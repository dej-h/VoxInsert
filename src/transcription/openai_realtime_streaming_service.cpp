#include "transcription/openai_realtime_streaming_service.h"

#include "config/app_config.h"
#include "observability/logging.h"
#include "security/api_credential_store.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
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

// OpenAI realtime transcription expects 24 kHz mono PCM16 in the audio buffer.
constexpr int kRealtimeSampleRate = 24000;
constexpr char kRealtimeUrl[] = "wss://api.openai.com/v1/realtime?intent=transcription";

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

// Streaming linear-interpolation resampler that remembers the last input sample
// across calls so chunk boundaries stay continuous.
class LinearResampler {
public:
    LinearResampler(int inputRate, int outputRate)
        : inputRate_(inputRate), outputRate_(outputRate) {}

    std::vector<int16_t> Process(std::span<const int16_t> input) {
        if (inputRate_ == outputRate_) {
            return std::vector<int16_t>(input.begin(), input.end());
        }

        std::vector<int16_t> output;
        if (input.empty() || inputRate_ <= 0 || outputRate_ <= 0) {
            return output;
        }

        const double step = static_cast<double>(inputRate_) / static_cast<double>(outputRate_);
        // position_ is expressed in input-sample space relative to the previous
        // tail sample (index -1 conceptually) so the first output sample blends
        // the carried tail with the first new sample.
        output.reserve(static_cast<size_t>(static_cast<double>(input.size()) / step) + 2);

        while (position_ < static_cast<double>(input.size())) {
            const double clamped = position_ < 0.0 ? 0.0 : position_;
            const size_t baseIndex = static_cast<size_t>(clamped);
            const double fraction = clamped - static_cast<double>(baseIndex);

            const int16_t a = (position_ < 0.0)
                ? lastSample_
                : input[baseIndex];
            const int16_t b = (baseIndex + 1 < input.size())
                ? input[baseIndex + 1]
                : input.back();

            const double interpolated =
                static_cast<double>(a) + (static_cast<double>(b) - static_cast<double>(a)) * fraction;
            output.push_back(static_cast<int16_t>(interpolated));
            position_ += step;
        }

        position_ -= static_cast<double>(input.size());
        lastSample_ = input.back();
        return output;
    }

private:
    int inputRate_;
    int outputRate_;
    double position_ = 0.0;
    int16_t lastSample_ = 0;
};

class OpenAiRealtimeSession final : public IStreamingTranscriptionSession {
public:
    OpenAiRealtimeSession(
        TranscriptionConfig config,
        StreamingSessionId sessionId,
        std::function<void(BackendTranscriptEvent)> onEvent,
        std::shared_ptr<spdlog::logger> logger)
        : config_(std::move(config)),
          sessionId_(std::move(sessionId)),
          onEvent_(std::move(onEvent)),
          logger_(std::move(logger)),
          resampler_(16000, kRealtimeSampleRate) {}

    ~OpenAiRealtimeSession() override {
        Close();
    }

    bool Start(std::wstring& failureReason) override {
        std::wstring apiKeyWide;
        if (!TryReadApiCredentialSecret(WideFromUtf8(config_.openAi.credentialTarget), apiKeyWide, failureReason)) {
            return false;
        }

        EnsureNetSystemInitialized();

        ix::WebSocketHttpHeaders headers;
        headers["Authorization"] = "Bearer " + Utf8FromWide(apiKeyWide);
        // The Realtime API is now GA. The legacy `OpenAI-Beta: realtime=v1`
        // header must NOT be sent: the GA endpoint drops the WebSocket upgrade
        // before returning any HTTP response when it sees the beta header,
        // which surfaces as "Failed reading HTTP status line". The official
        // OpenAI clients connect with only Authorization (+ OpenAI-Log-Session).
        headers["OpenAI-Log-Session"] = "1";

        webSocket_.setUrl(kRealtimeUrl);
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
        // connection result is delivered asynchronously via the Open/Error
        // messages. This keeps the F8 -> status-pill path off the network
        // latency: recording and the UI start instantly while the socket
        // finishes its TLS handshake. Audio captured before Open is buffered
        // in AppendAudio and flushed once the session is ready, and a failed
        // connection surfaces as a Failed event so the controller falls back
        // to file transcription (the retained PCM is never lost).
        connectStartedAt_ = std::chrono::steady_clock::now();
        webSocket_.start();
        return true;
    }

    bool AppendAudio(const StreamingBackendCommand& command, std::wstring& failureReason) override {
        if (command.pcm16.empty()) {
            return true;
        }

        std::vector<int16_t> resampled = resampler_.Process(command.pcm16);
        if (resampled.empty()) {
            return true;
        }

        const std::string base64Audio = Base64Encode(
            reinterpret_cast<const std::uint8_t*>(resampled.data()),
            resampled.size() * sizeof(int16_t));

        json payload;
        payload["type"] = "input_audio_buffer.append";
        payload["audio"] = base64Audio;
        return QueueOrSendSerialized(payload.dump(), failureReason, "input_audio_buffer.append");
    }

    bool CommitUtterance(const UtteranceId& /*utteranceId*/, std::wstring& failureReason) override {
        commitTime_.store(std::chrono::steady_clock::now());
        json payload;
        payload["type"] = "input_audio_buffer.commit";
        return SendJson(payload, failureReason, "input_audio_buffer.commit");
    }

    void CancelUtterance(const UtteranceId& /*utteranceId*/) noexcept override {
        try {
            json payload;
            payload["type"] = "input_audio_buffer.clear";
            std::wstring ignored;
            SendJson(payload, ignored, "input_audio_buffer.clear");
        }
        catch (...) {
        }
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
            reason = L"The OpenAI realtime transcription session failed.";
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
            logger_->warn("openai realtime: {}: {}", context, Utf8FromWide(reason));
        }
        stateCondition_.notify_all();
        EmitEvent(BackendTranscriptEventKind::Failed, {}, {}, std::move(reason));
    }

    bool SendSerializedNow(const std::string& serialized, std::wstring& failureReason, std::string_view phase) {
        const std::scoped_lock sendLock(sendMutex_);
        const ix::WebSocketSendInfo info = webSocket_.sendText(serialized);
        if (!info.success) {
            failureReason = L"Failed to send a message to the OpenAI realtime transcription endpoint.";
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
                    ? L"The OpenAI realtime connection failed before audio could be sent."
                    : connectFailureReason_;
                return false;
            }
            if (!opened_) {
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
        // GA Realtime transcription session shape (see OpenAI realtime
        // transcription guide and the openai-agents-python STT client):
        //   { type: session.update,
        //     session: { type: transcription,
        //                audio: { input: { format: {type: audio/pcm, rate},
        //                                  transcription: {model, language},
        //                                  turn_detection: null } } } }
        json transcription;
        transcription["model"] = config_.openAi.streamingModel;
        if (!config_.languageHint.empty()) {
            transcription["language"] = config_.languageHint;
        }

        json format;
        format["type"] = "audio/pcm";
        format["rate"] = kRealtimeSampleRate;

        json input;
        input["format"] = format;
        input["transcription"] = transcription;
        // Turn detection is disabled: VoxInsert owns commit boundaries and
        // commits the buffer manually on stop.
        input["turn_detection"] = nullptr;

        json audio;
        audio["input"] = input;

        json session;
        session["type"] = "transcription";
        session["audio"] = audio;

        json payload;
        payload["type"] = "session.update";
        payload["session"] = session;

        return payload.dump();
    }

    void SendSessionUpdateAfterTransportOpen() {
        size_t queuedAtOpen = 0;
        {
            const std::scoped_lock lock(stateMutex_);
            if (opened_ || sessionUpdateSent_ || connectFailed_) {
                return;
            }
            sessionUpdateSent_ = true;
            queuedAtOpen = pendingOutboundPayloads_.size();
        }

        if (logger_ != nullptr) {
            const auto openMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - connectStartedAt_)
                                    .count();
            logger_->debug(
                "openai realtime: websocket transport open in {}ms ({} queued message(s)); sending session.update",
                openMs,
                queuedAtOpen);
        }

        std::wstring failureReason;
        if (!SendSerializedNow(BuildSessionConfigurationPayload(), failureReason, "session.update")) {
            return;
        }
    }

    void CompleteStartupHandshake() {
        size_t queuedBeforeReady = 0;
        {
            const std::scoped_lock lock(stateMutex_);
            if (opened_ || connectFailed_) {
                return;
            }
            queuedBeforeReady = pendingOutboundPayloads_.size();
        }

        if (logger_ != nullptr) {
            const auto handshakeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - connectStartedAt_)
                                         .count();
            logger_->debug(
                "openai realtime: session.updated received in {}ms ({} queued message(s)); flushing startup audio",
                handshakeMs,
                queuedBeforeReady);
        }

        std::wstring failureReason;
        size_t flushedCount = 0;
        while (true) {
            std::vector<std::string> pending;
            {
                const std::scoped_lock lock(stateMutex_);
                if (opened_ || connectFailed_) {
                    return;
                }
                pending.swap(pendingOutboundPayloads_);
                if (pending.empty()) {
                    opened_ = true;
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
                "openai realtime: session ready in {}ms ({} queued message(s) flushed)",
                readyMs,
                flushedCount);
        }

        stateCondition_.notify_all();
        EmitEvent(BackendTranscriptEventKind::SessionReady, {}, {}, {});
    }

    std::wstring BuildCloseReason(const ix::WebSocketCloseInfo& closeInfo) const {
        std::wstring reason = L"OpenAI realtime websocket closed";
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
        case ix::WebSocketMessageType::Open: {
            SendSessionUpdateAfterTransportOpen();
            break;
        }
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
                        "openai realtime: websocket closed (code={}, remote={}, reason={})",
                        message->closeInfo.code,
                        message->closeInfo.remote,
                        closeReason);
                }
                else {
                    logger_->warn(
                        "openai realtime: websocket closed (code={}, remote={}, reason={})",
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
                logger_->warn("openai realtime: could not parse server message: {}", exception.what());
            }
            return;
        }

        const std::string type = parsed.value("type", std::string{});
        const std::string itemId = parsed.value("item_id", std::string{});

        if (type == "session.updated") {
            CompleteStartupHandshake();
        }
        else if (type == "conversation.item.input_audio_transcription.delta") {
            const std::string delta = parsed.value("delta", std::string{});
            if (!delta.empty()) {
                const int count = ++deltaCount_;
                deltaBytes_ += delta.size();
                if (logger_ != nullptr) {
                    if (count == 1) {
                        const auto sinceCommitMs = FirstDeltaLatencyMs();
                        logger_->debug(
                            "openai realtime: first transcription delta received ({} bytes, {}ms after commit)",
                            delta.size(),
                            sinceCommitMs);
                    }
                    else {
                        logger_->debug("openai realtime: transcription delta #{} ({} bytes)", count, delta.size());
                    }
                }
                EmitEvent(BackendTranscriptEventKind::AppendDelta, itemId, delta, {});
            }
        }
        else if (type == "conversation.item.input_audio_transcription.completed") {
            const std::string transcript = parsed.value("transcript", std::string{});
            if (logger_ != nullptr) {
                logger_->debug(
                    "openai realtime: transcription completed ({} bytes total) after {} delta(s) ({} delta bytes)",
                    transcript.size(),
                    deltaCount_.load(),
                    deltaBytes_.load());
            }
            EmitEvent(BackendTranscriptEventKind::UtteranceFinalized, itemId, transcript, {});
        }
        else if (type == "error") {
            std::string errorMessage = "OpenAI realtime transcription error.";
            const auto errorIt = parsed.find("error");
            if (errorIt != parsed.end() && errorIt->is_object()) {
                errorMessage = errorIt->value("message", errorMessage);
            }
            EmitEvent(BackendTranscriptEventKind::Failed, itemId, {}, WideFromUtf8(errorMessage));
        }
    }

    TranscriptionConfig config_;
    StreamingSessionId sessionId_;
    std::function<void(BackendTranscriptEvent)> onEvent_;
    std::shared_ptr<spdlog::logger> logger_;
    LinearResampler resampler_;

    ix::WebSocket webSocket_;
    std::mutex stateMutex_;
    std::mutex sendMutex_;
    std::condition_variable stateCondition_;
    bool opened_ = false;
    bool sessionUpdateSent_ = false;
    bool connectFailed_ = false;
    std::wstring connectFailureReason_;
    std::atomic<bool> closed_{false};

    // Set when the socket starts connecting; used to report how long the
    // asynchronous connection took once Open arrives.
    std::chrono::steady_clock::time_point connectStartedAt_{};
    // Messages queued before the realtime session is fully ready. This keeps
    // startup audio, stop/commit, and any late-arriving chunks in one ordered
    // stream so the callback thread can flush them serially after session.updated
    // confirms the session configuration. Guarded by stateMutex_.
    std::vector<std::string> pendingOutboundPayloads_;

    // Diagnostics for the delta/diff path: how many incremental transcription
    // deltas the server streamed and how long after commit the first arrived.
    std::atomic<int> deltaCount_{0};
    std::atomic<size_t> deltaBytes_{0};
    std::atomic<std::chrono::steady_clock::time_point> commitTime_{};
};

} // namespace

StreamingBackendCapabilities OpenAiRealtimeStreamingBackend::Capabilities(const TranscriptionConfig& /*config*/) const {
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

std::unique_ptr<IStreamingTranscriptionSession> OpenAiRealtimeStreamingBackend::CreateSession(
    const TranscriptionConfig& config,
    StreamingSessionId sessionId,
    std::function<void(BackendTranscriptEvent)> onEvent,
    std::shared_ptr<spdlog::logger> logger,
    std::wstring& /*failureReason*/) const {
    return std::make_unique<OpenAiRealtimeSession>(config, std::move(sessionId), std::move(onEvent), std::move(logger));
}

} // namespace voxinsert
