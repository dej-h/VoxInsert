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
            if (!QueueOrSendSerialized(payload.dump(), failureReason, "input_audio.append")) {
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
        if (!SendJson(flush, failureReason, "input_audio.flush")) {
            return false;
        }

        json end;
        end["type"] = "input_audio.end";
        return SendJson(end, failureReason, "input_audio.end");
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

    void FailSession(std::wstring reason, std::string_view context) {
        if (reason.empty()) {
            reason = L"The Mistral realtime transcription session failed.";
        }

        bool shouldEmit = false;
        {
            const std::scoped_lock lock(stateMutex_);
            if (!connectFailed_) {
                connectFailed_ = true;
                connectFailureReason_ = reason;
                shouldEmit = true;
            }
            else if (connectFailureReason_.empty()) {
                connectFailureReason_ = reason;
            }
        }

        if (!shouldEmit) {
            return;
        }

        if (logger_ != nullptr) {
            logger_->warn("mistral realtime: {}: {}", context, Utf8FromWide(reason));
        }
        stateCondition_.notify_all();
        EmitEvent(BackendTranscriptEventKind::Failed, {}, {}, std::move(reason));
    }

    bool SendSerializedNow(const std::string& serialized, std::wstring& failureReason, std::string_view phase) {
        const std::scoped_lock sendLock(sendMutex_);
        const ix::WebSocketSendInfo info = webSocket_.sendText(serialized);
        if (!info.success) {
            failureReason = L"Failed to send a message to the Mistral realtime transcription endpoint.";
            FailSession(failureReason, phase);
            return false;
        }
        return true;
    }

    bool QueueOrSendSerialized(std::string serialized, std::wstring& failureReason, std::string_view phase) {
        {
            std::scoped_lock lock(stateMutex_);
            if (connectFailed_) {
                failureReason = connectFailureReason_.empty()
                    ? L"The Mistral realtime connection failed before audio could be sent."
                    : connectFailureReason_;
                return false;
            }
            if (!ready_) {
                pendingOutboundPayloads_.push_back(std::move(serialized));
                return true;
            }
        }

        return SendSerializedNow(serialized, failureReason, phase);
    }

    bool SendJson(const json& payload, std::wstring& failureReason, std::string_view phase) {
        return QueueOrSendSerialized(payload.dump(), failureReason, phase);
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

    std::string BuildSessionConfigurationPayload() const {
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

        return payload.dump();
    }

    void CompleteSessionReadyHandshake() {
        size_t queuedBeforeReady = 0;
        {
            const std::scoped_lock lock(stateMutex_);
            queuedBeforeReady = pendingOutboundPayloads_.size();
        }

        if (logger_ != nullptr) {
            const auto handshakeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - connectStartedAt_)
                                         .count();
            logger_->debug(
                "mistral realtime: session.created received in {}ms ({} queued message(s)); sending session.update",
                handshakeMs,
                queuedBeforeReady);
        }

        std::wstring failureReason;
        if (!SendSerializedNow(BuildSessionConfigurationPayload(), failureReason, "session.update")) {
            return;
        }

        size_t flushedCount = 0;
        while (true) {
            std::vector<std::string> pending;
            {
                const std::scoped_lock lock(stateMutex_);
                pending.swap(pendingOutboundPayloads_);
                if (pending.empty()) {
                    ready_ = true;
                    break;
                }
            }

            for (const std::string& serialized : pending) {
                if (!SendSerializedNow(serialized, failureReason, "startup_flush")) {
                    return;
                }
                ++flushedCount;
            }
        }

        if (logger_ != nullptr) {
            const auto readyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - connectStartedAt_)
                                     .count();
            logger_->debug(
                "mistral realtime: session ready in {}ms ({} queued message(s) flushed)",
                readyMs,
                flushedCount);
        }

        stateCondition_.notify_all();
        EmitEvent(BackendTranscriptEventKind::SessionReady, {}, {}, {});
    }

    std::wstring BuildCloseReason(const ix::WebSocketCloseInfo& closeInfo) const {
        std::wstring reason = L"Mistral realtime websocket closed";
        if (closeInfo.code != 0) {
            reason += L" (code ";
            reason += std::to_wstring(closeInfo.code);
            reason += L")";
        }
        if (!closeInfo.reason.empty()) {
            reason += L": ";
            reason += WideFromUtf8(closeInfo.reason);
        }
        return reason;
    }

    void HandleSocketMessage(const ix::WebSocketMessagePtr& message) {
        switch (message->type) {
        case ix::WebSocketMessageType::Open:
            // The application-level handshake (session.created) is what gates
            // audio; nothing to do on the raw WebSocket open beyond keeping
            // buffering. connectStartedAt_ was captured in Start().
            if (logger_ != nullptr) {
                const auto openMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - connectStartedAt_)
                                        .count();
                logger_->debug(
                    "mistral realtime: websocket transport open in {}ms; waiting for session.created",
                    openMs);
            }
            break;
        case ix::WebSocketMessageType::Message:
            HandleTextMessage(message->str);
            break;
        case ix::WebSocketMessageType::Error: {
            const std::wstring reason = WideFromUtf8(message->errorInfo.reason);
            FailSession(reason, "websocket error");
            break;
        }
        case ix::WebSocketMessageType::Close: {
            const bool localShutdown = closed_.load();
            const std::wstring reason = BuildCloseReason(message->closeInfo);
            if (logger_ != nullptr) {
                const std::string closeReason = message->closeInfo.reason.empty()
                    ? std::string{"<none>"}
                    : message->closeInfo.reason;
                if (localShutdown) {
                    logger_->debug(
                        "mistral realtime: websocket closed (code={}, remote={}, reason={})",
                        message->closeInfo.code,
                        message->closeInfo.remote,
                        closeReason);
                }
                else {
                    logger_->warn(
                        "mistral realtime: websocket closed (code={}, remote={}, reason={})",
                        message->closeInfo.code,
                        message->closeInfo.remote,
                        closeReason);
                }
            }
            if (!localShutdown) {
                {
                    const std::scoped_lock lock(stateMutex_);
                    if (!connectFailed_) {
                        connectFailed_ = true;
                        connectFailureReason_ = reason;
                    }
                }
                stateCondition_.notify_all();
            }
            EmitEvent(BackendTranscriptEventKind::SessionClosed, {}, {}, localShutdown ? std::wstring{} : reason);
            break;
        }
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
            CompleteSessionReadyHandshake();
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
    std::mutex sendMutex_;
    std::condition_variable stateCondition_;
    bool ready_ = false;
    bool connectFailed_ = false;
    std::wstring connectFailureReason_;
    std::atomic<bool> closed_{false};

    // Set when the socket starts connecting; used to report how long the
    // asynchronous session negotiation took once session.created arrives.
    std::chrono::steady_clock::time_point connectStartedAt_{};
    // Messages queued before the realtime session is fully ready. This keeps
    // startup audio, stop/flush/end, and any late-arriving chunks in one
    // ordered stream so the callback thread can flush them serially after
    // session.update. Guarded by stateMutex_.
    std::vector<std::string> pendingOutboundPayloads_;

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
    capabilities.preferredAppendBatchMs = 20;
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
