#include "transcription/mistral_realtime_streaming_service.h"

#include "config/app_config.h"
#include "observability/logging.h"
#include "security/api_credential_store.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace voxinsert {
namespace {

using json = nlohmann::json;

// Mistral realtime transcription consumes 16 kHz mono PCM16 natively, which is
// exactly what VoxInsert captures, so unlike the OpenAI adapter no resampling
// is required. The model is selected via a URL query parameter rather than the
// session.update payload.
constexpr int kRealtimeSampleRate = 16000;
constexpr char kRealtimeBaseUrl[] = "wss://api.mistral.ai/v1/audio/transcriptions/realtime?model=";

// Mistral rejects input_audio.append messages whose decoded PCM payload exceeds
// 262144 bytes. VoxInsert's append batches are far smaller than this, but the
// adapter still chunks defensively so an unusually large batch (e.g. the
// pre-connect backlog) can never trip the limit.
constexpr size_t kMaxAppendDecodedBytes = 262144;

void EnsureNetSystemInitialized() {
    static const bool initialized = []() {
        ix::initNetSystem();
        return true;
    }();
    (void)initialized;
}

std::string Base64Encode(const std::uint8_t* data, size_t length) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((length + 2) / 3) * 4);

    size_t index = 0;
    while (index + 3 <= length) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(data[index]) << 16) |
            (static_cast<std::uint32_t>(data[index + 1]) << 8) |
            static_cast<std::uint32_t>(data[index + 2]);
        encoded.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        encoded.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        encoded.push_back(kAlphabet[triple & 0x3F]);
        index += 3;
    }

    const size_t remaining = length - index;
    if (remaining == 1) {
        const std::uint32_t triple = static_cast<std::uint32_t>(data[index]) << 16;
        encoded.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        encoded.push_back('=');
        encoded.push_back('=');
    }
    else if (remaining == 2) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(data[index]) << 16) |
            (static_cast<std::uint32_t>(data[index + 1]) << 8);
        encoded.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        encoded.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        encoded.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        encoded.push_back('=');
    }

    return encoded;
}

class MistralRealtimeSession final : public IStreamingTranscriptionSession {
public:
    MistralRealtimeSession(
        TranscriptionConfig config,
        StreamingSessionId sessionId,
        std::function<void(BackendTranscriptEvent)> onEvent,
        std::shared_ptr<spdlog::logger> logger)
        : config_(std::move(config)),
          sessionId_(std::move(sessionId)),
          onEvent_(std::move(onEvent)),
          logger_(std::move(logger)) {}

    ~MistralRealtimeSession() override {
        Close();
    }

    bool Start(std::wstring& failureReason) override {
        std::wstring apiKeyWide;
        if (!TryReadApiCredentialSecret(WideFromUtf8(config_.mistral.credentialTarget), apiKeyWide, failureReason)) {
            return false;
        }

        EnsureNetSystemInitialized();

        ix::WebSocketHttpHeaders headers;
        headers["Authorization"] = "Bearer " + Utf8FromWide(apiKeyWide);

        webSocket_.setUrl(kRealtimeBaseUrl + config_.mistral.streamingModel);
        webSocket_.setExtraHeaders(headers);

        // A streaming session is bound to a single recording; if the socket
        // drops we want to fail fast and fall back, not retry in the
        // background (which would spam the log and never recover the take).
        webSocket_.disableAutomaticReconnection();

        ix::SocketTLSOptions tlsOptions;
        tlsOptions.caFile = "SYSTEM";
        webSocket_.setTLSOptions(tlsOptions);

        webSocket_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& message) {
            HandleSocketMessage(message);
        });

        // Connect without blocking the caller. webSocket_.start() spins up
        // ixwebsocket's own background thread and returns immediately; the
        // connection result is delivered asynchronously. Recording and the UI
        // start instantly while the socket finishes its TLS handshake. Audio
        // captured before the session is ready is buffered in AppendAudio and
        // flushed once Mistral sends session.created (the point at which the
        // audio_format has been negotiated), and a failed connection surfaces
        // as a Failed event so the controller falls back to file transcription.
        connectStartedAt_ = std::chrono::steady_clock::now();
        webSocket_.start();
        return true;
    }

    bool AppendAudio(const StreamingBackendCommand& command, std::wstring& failureReason) override {
        if (command.pcm16.empty()) {
            return true;
        }

        // Mistral consumes 16 kHz PCM16 directly, so the captured samples are
        // sent as-is. Split into messages that stay under Mistral's decoded
        // payload cap.
        const size_t maxSamplesPerMessage = kMaxAppendDecodedBytes / sizeof(int16_t);
        for (size_t offset = 0; offset < command.pcm16.size(); offset += maxSamplesPerMessage) {
            const size_t count = std::min(maxSamplesPerMessage, command.pcm16.size() - offset);
            const std::string base64Audio = Base64Encode(
                reinterpret_cast<const std::uint8_t*>(command.pcm16.data() + offset),
                count * sizeof(int16_t));

            json payload;
            payload["type"] = "input_audio.append";
            payload["audio"] = base64Audio;
            std::string serialized = payload.dump();

            {
                std::scoped_lock lock(stateMutex_);
                if (connectFailed_) {
                    failureReason = connectFailureReason_.empty()
                        ? L"The Mistral realtime connection failed before audio could be sent."
                        : connectFailureReason_;
                    return false;
                }
                if (!ready_) {
                    // The session has not negotiated audio_format yet; retain
                    // this chunk in order so no leading audio is lost.
                    // OnSessionReady() flushes these once session.created
                    // arrives.
                    pendingAudioPayloads_.push_back(std::move(serialized));
                    continue;
                }
            }

            const ix::WebSocketSendInfo info = webSocket_.sendText(serialized);
            if (!info.success) {
                failureReason = L"Failed to send a message to the Mistral realtime transcription endpoint.";
                return false;
            }
        }

        return true;
    }

    bool CommitUtterance(const UtteranceId& /*utteranceId*/, std::wstring& failureReason) override {
        // Mistral has no explicit "commit": flushing forces transcription of
        // the buffered audio and end closes the input stream, after which the
        // server emits transcription.done with the final transcript.
        commitTime_.store(std::chrono::steady_clock::now());

        json flush;
        flush["type"] = "input_audio.flush";
        if (!SendJson(flush, failureReason)) {
            return false;
        }

        json end;
        end["type"] = "input_audio.end";
        return SendJson(end, failureReason);
    }

    void CancelUtterance(const UtteranceId& /*utteranceId*/) noexcept override {
        // Mistral exposes no clear/cancel control message; discarding the take
        // is handled by Close() stopping the socket so the buffered audio is
        // never transcribed into a final.
    }

    void Close() noexcept override {
        if (closed_.exchange(true)) {
            return;
        }

        try {
            webSocket_.stop();
        }
        catch (...) {
        }
    }

private:
    void EmitEvent(BackendTranscriptEventKind kind, const std::string& itemId, std::string textUtf8, std::wstring failure) {
        if (!onEvent_) {
            return;
        }

        BackendTranscriptEvent event;
        event.kind = kind;
        event.sessionId = sessionId_;
        event.utteranceId = itemId.empty() ? sessionId_ : itemId;
        event.backendItemId = itemId;
        event.textUtf8 = std::move(textUtf8);
        event.failureReason = std::move(failure);
        onEvent_(std::move(event));
    }

    bool SendJson(const json& payload, std::wstring& failureReason) {
        const std::string serialized = payload.dump();
        const ix::WebSocketSendInfo info = webSocket_.sendText(serialized);
        if (!info.success) {
            failureReason = L"Failed to send a message to the Mistral realtime transcription endpoint.";
            return false;
        }
        return true;
    }

    // Milliseconds between the most recent commit and now; used to report how
    // long the server took to emit the first transcription delta. Returns -1 if
    // no commit has been issued yet (e.g. deltas arriving mid-recording).
    int64_t FirstDeltaLatencyMs() const {
        const auto commitAt = commitTime_.load();
        if (commitAt.time_since_epoch().count() == 0) {
            return -1;
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - commitAt)
            .count();
    }

    void SendSessionConfiguration() {
        // Mistral realtime session shape (see mistralai/client-python realtime
        // connection): the audio_format must be declared before any audio is
        // sent. The model is supplied via the URL query parameter, not here.
        json audioFormat;
        audioFormat["encoding"] = "pcm_s16le";
        audioFormat["sample_rate"] = kRealtimeSampleRate;

        json session;
        session["audio_format"] = audioFormat;

        json payload;
        payload["type"] = "session.update";
        payload["session"] = session;

        std::wstring ignored;
        if (!SendJson(payload, ignored) && logger_ != nullptr) {
            logger_->warn("mistral realtime: failed to send session.update");
        }
    }

    void OnSessionReady() {
        // session.created has arrived: declare the audio_format, then flush any
        // audio captured while the socket was still connecting, in capture
        // order and after session.update so Mistral accepts the format.
        SendSessionConfiguration();

        std::vector<std::string> pending;
        {
            const std::scoped_lock lock(stateMutex_);
            ready_ = true;
            pending.swap(pendingAudioPayloads_);
        }
        for (const std::string& serialized : pending) {
            webSocket_.sendText(serialized);
        }
        if (logger_ != nullptr) {
            const auto connectMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - connectStartedAt_)
                                       .count();
            logger_->debug(
                "mistral realtime: session ready in {}ms ({} buffered append(s) flushed)",
                connectMs,
                pending.size());
        }
        stateCondition_.notify_all();
        EmitEvent(BackendTranscriptEventKind::SessionReady, {}, {}, {});
    }

    void HandleSocketMessage(const ix::WebSocketMessagePtr& message) {
        switch (message->type) {
        case ix::WebSocketMessageType::Open:
            // The application-level handshake (session.created) is what gates
            // audio; nothing to do on the raw WebSocket open beyond keeping
            // buffering. connectStartedAt_ was captured in Start().
            break;
        case ix::WebSocketMessageType::Message:
            HandleTextMessage(message->str);
            break;
        case ix::WebSocketMessageType::Error: {
            const std::wstring reason = WideFromUtf8(message->errorInfo.reason);
            {
                const std::scoped_lock lock(stateMutex_);
                if (!ready_) {
                    connectFailed_ = true;
                    connectFailureReason_ = reason;
                }
            }
            stateCondition_.notify_all();
            EmitEvent(BackendTranscriptEventKind::Failed, {}, {}, reason);
            break;
        }
        case ix::WebSocketMessageType::Close:
            EmitEvent(BackendTranscriptEventKind::SessionClosed, {}, {}, {});
            break;
        default:
            break;
        }
    }

    void HandleTextMessage(const std::string& text) {
        json parsed;
        try {
            parsed = json::parse(text);
        }
        catch (const std::exception& exception) {
            if (logger_ != nullptr) {
                logger_->warn("mistral realtime: could not parse server message: {}", exception.what());
            }
            return;
        }

        const std::string type = parsed.value("type", std::string{});

        if (type == "session.created") {
            OnSessionReady();
        }
        else if (type == "transcription.text.delta") {
            const std::string delta = parsed.value("text", std::string{});
            if (!delta.empty()) {
                const int count = ++deltaCount_;
                deltaBytes_ += delta.size();
                if (logger_ != nullptr) {
                    if (count == 1) {
                        const auto sinceCommitMs = FirstDeltaLatencyMs();
                        logger_->debug(
                            "mistral realtime: first transcription delta received ({} bytes, {}ms after commit)",
                            delta.size(),
                            sinceCommitMs);
                    }
                    else {
                        logger_->debug("mistral realtime: transcription delta #{} ({} bytes)", count, delta.size());
                    }
                }
                EmitEvent(BackendTranscriptEventKind::AppendDelta, {}, delta, {});
            }
        }
        else if (type == "transcription.done") {
            const std::string transcript = parsed.value("text", std::string{});
            if (logger_ != nullptr) {
                logger_->debug(
                    "mistral realtime: transcription done ({} bytes total) after {} delta(s) ({} delta bytes)",
                    transcript.size(),
                    deltaCount_.load(),
                    deltaBytes_.load());
            }
            EmitEvent(BackendTranscriptEventKind::UtteranceFinalized, {}, transcript, {});
        }
        else if (type == "error") {
            std::string errorMessage = "Mistral realtime transcription error.";
            const auto errorIt = parsed.find("error");
            if (errorIt != parsed.end() && errorIt->is_object()) {
                errorMessage = errorIt->value("message", errorMessage);
            }
            else {
                errorMessage = parsed.value("message", errorMessage);
            }
            EmitEvent(BackendTranscriptEventKind::Failed, {}, {}, WideFromUtf8(errorMessage));
        }
        // session.updated, transcription.language, and transcription.segment are
        // informational for VoxInsert's append-only assembler and are ignored.
    }

    TranscriptionConfig config_;
    StreamingSessionId sessionId_;
    std::function<void(BackendTranscriptEvent)> onEvent_;
    std::shared_ptr<spdlog::logger> logger_;

    ix::WebSocket webSocket_;
    std::mutex stateMutex_;
    std::condition_variable stateCondition_;
    bool ready_ = false;
    bool connectFailed_ = false;
    std::wstring connectFailureReason_;
    std::atomic<bool> closed_{false};

    // Set when the socket starts connecting; used to report how long the
    // asynchronous session negotiation took once session.created arrives.
    std::chrono::steady_clock::time_point connectStartedAt_{};
    // Audio appended before the session was ready, retained in capture order and
    // flushed by OnSessionReady() so no leading speech is dropped. Guarded by
    // stateMutex_.
    std::vector<std::string> pendingAudioPayloads_;

    // Diagnostics for the delta path: how many incremental transcription deltas
    // the server streamed and how long after commit the first arrived.
    std::atomic<int> deltaCount_{0};
    std::atomic<size_t> deltaBytes_{0};
    std::atomic<std::chrono::steady_clock::time_point> commitTime_{};
};

} // namespace

StreamingBackendCapabilities MistralRealtimeStreamingBackend::Capabilities(const TranscriptionConfig& /*config*/) const {
    StreamingBackendCapabilities capabilities;
    capabilities.emitsPartialsBeforeCommit = true;
    capabilities.appendOnlyDeltas = true;
    capabilities.revisingSnapshots = false;
    capabilities.supportsManualCommit = true;
    capabilities.supportsServerTurnDetection = false;
    capabilities.requiredSampleRate = kRealtimeSampleRate;
    capabilities.preferredAppendBatchMs = 80;
    return capabilities;
}

std::unique_ptr<IStreamingTranscriptionSession> MistralRealtimeStreamingBackend::CreateSession(
    const TranscriptionConfig& config,
    StreamingSessionId sessionId,
    std::function<void(BackendTranscriptEvent)> onEvent,
    std::shared_ptr<spdlog::logger> logger,
    std::wstring& /*failureReason*/) const {
    return std::make_unique<MistralRealtimeSession>(config, std::move(sessionId), std::move(onEvent), std::move(logger));
}

} // namespace voxinsert
