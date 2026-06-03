#include "transcription/fake_streaming_transcription_service.h"

#include <utility>

namespace voxinsert {

namespace {

// Fills correlation IDs from the live session when the script left them blank,
// so test scripts can stay terse.
BackendTranscriptEvent Resolve(
    BackendTranscriptEvent event,
    const StreamingSessionId& sessionId,
    const UtteranceId& utteranceId) {
    if (event.sessionId.empty()) {
        event.sessionId = sessionId;
    }
    if (event.utteranceId.empty()) {
        event.utteranceId = utteranceId;
    }
    return event;
}

class FakeStreamingSession : public IStreamingTranscriptionSession {
public:
    FakeStreamingSession(
        FakeStreamingScript script,
        StreamingSessionId sessionId,
        std::function<void(BackendTranscriptEvent)> onEvent,
        std::shared_ptr<FakeStreamingStats> stats)
        : script_(std::move(script)),
          sessionId_(std::move(sessionId)),
          onEvent_(std::move(onEvent)),
          stats_(std::move(stats)),
          pendingAppendEvents_(script_.eventsDuringAppend.begin(), script_.eventsDuringAppend.end()) {}

    bool Start(std::wstring& failureReason) override {
        if (stats_) {
            stats_->startCalls.fetch_add(1);
        }
        if (script_.failOnStart) {
            failureReason = script_.startFailureReason.empty()
                ? L"Fake streaming backend was configured to fail on start."
                : script_.startFailureReason;
            return false;
        }
        if (script_.emitSessionReadyOnStart && onEvent_) {
            BackendTranscriptEvent ready;
            ready.kind = BackendTranscriptEventKind::SessionReady;
            ready.sessionId = sessionId_;
            onEvent_(std::move(ready));
        }
        return true;
    }

    bool AppendAudio(const StreamingBackendCommand& command, std::wstring& failureReason) override {
        if (command.pcm16.empty()) {
            // Empty audio appends are a contract violation; surface it so tests
            // can assert local gating suppressed them.
            if (stats_) {
                stats_->rejectedEmptyAppend.store(true);
            }
            failureReason = L"Fake streaming backend received an empty audio append.";
            return false;
        }
        if (stats_) {
            stats_->appendCalls.fetch_add(1);
            stats_->appendedSamples.fetch_add(command.pcm16.size());
        }
        if (script_.failOnAppend) {
            failureReason = script_.appendFailureReason.empty()
                ? L"Fake streaming backend was configured to fail on append."
                : script_.appendFailureReason;
            return false;
        }
        if (!pendingAppendEvents_.empty() && onEvent_) {
            BackendTranscriptEvent event = pendingAppendEvents_.front();
            pendingAppendEvents_.pop_front();
            onEvent_(Resolve(std::move(event), sessionId_, command.utteranceId));
        }
        return true;
    }

    bool CommitUtterance(const UtteranceId& utteranceId, std::wstring& failureReason) override {
        if (stats_) {
            stats_->commitCalls.fetch_add(1);
        }
        if (script_.failOnCommit) {
            failureReason = script_.commitFailureReason.empty()
                ? L"Fake streaming backend was configured to fail on commit."
                : script_.commitFailureReason;
            return false;
        }
        if (onEvent_) {
            for (const BackendTranscriptEvent& scripted : script_.eventsOnCommit) {
                onEvent_(Resolve(scripted, sessionId_, utteranceId));
            }
        }
        return true;
    }

    void CancelUtterance(const UtteranceId&) noexcept override {
        if (stats_) {
            stats_->cancelCalls.fetch_add(1);
        }
    }

    void Close() noexcept override {
        if (stats_) {
            stats_->closeCalls.fetch_add(1);
        }
        if (onEvent_) {
            BackendTranscriptEvent closed;
            closed.kind = BackendTranscriptEventKind::SessionClosed;
            closed.sessionId = sessionId_;
            onEvent_(std::move(closed));
        }
    }

private:
    FakeStreamingScript script_;
    StreamingSessionId sessionId_;
    std::function<void(BackendTranscriptEvent)> onEvent_;
    std::shared_ptr<FakeStreamingStats> stats_;
    std::deque<BackendTranscriptEvent> pendingAppendEvents_;
};

} // namespace

FakeStreamingBackend::FakeStreamingBackend(FakeStreamingScript script)
    : script_(std::move(script)),
      stats_(std::make_shared<FakeStreamingStats>()) {}

StreamingBackendCapabilities FakeStreamingBackend::Capabilities(const TranscriptionConfig&) const {
    return script_.capabilities;
}

std::unique_ptr<IStreamingTranscriptionSession> FakeStreamingBackend::CreateSession(
    const TranscriptionConfig&,
    StreamingSessionId sessionId,
    std::function<void(BackendTranscriptEvent)> onEvent,
    std::shared_ptr<spdlog::logger>,
    std::wstring&) const {
    stats_->sessionsCreated.fetch_add(1);
    return std::make_unique<FakeStreamingSession>(
        script_,
        std::move(sessionId),
        std::move(onEvent),
        stats_);
}

} // namespace voxinsert
