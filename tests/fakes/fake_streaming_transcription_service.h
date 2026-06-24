#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "transcription/streaming_transcription_service.h"

namespace voxinsert {

// Counters and observed state shared between the fake backend and the sessions
// it creates, so tests can assert what the coordinator did.
struct FakeStreamingStats {
    std::atomic<int> sessionsCreated{0};
    std::atomic<int> startCalls{0};
    std::atomic<int> appendCalls{0};
    std::atomic<int> commitCalls{0};
    std::atomic<int> cancelCalls{0};
    std::atomic<int> closeCalls{0};
    std::atomic<uint64_t> appendedSamples{0};
    std::atomic<bool> rejectedEmptyAppend{false};
};

// Deterministic backend for tests. It accepts audio without a network or
// microphone and emits scripted transcript events.
struct FakeStreamingScript {
    StreamingBackendCapabilities capabilities;
    bool emitSessionReadyOnStart = true;

    bool failOnStart = false;
    std::wstring startFailureReason;
    bool failOnAppend = false;
    std::wstring appendFailureReason;
    bool failOnCommit = false;
    std::wstring commitFailureReason;

    // Emitted one event per successful AppendAudio call, in order, while events
    // remain. Use this to simulate true-live-partial backends.
    std::vector<BackendTranscriptEvent> eventsDuringAppend;

    // Emitted, in order, when CommitUtterance is called. Use this for
    // append-before-commit and final-on-commit backends.
    std::vector<BackendTranscriptEvent> eventsOnCommit;
};

class FakeStreamingBackend : public IStreamingTranscriptionBackend {
public:
    explicit FakeStreamingBackend(FakeStreamingScript script);

    std::string_view BackendId() const noexcept override { return "fake_streaming"; }
    StreamingBackendCapabilities Capabilities(const TranscriptionConfig& config) const override;

    std::unique_ptr<IStreamingTranscriptionSession> CreateSession(
        const TranscriptionConfig& config,
        StreamingSessionId sessionId,
        std::function<void(BackendTranscriptEvent)> onEvent,
        std::shared_ptr<spdlog::logger> logger,
        std::wstring& failureReason) const override;

    const std::shared_ptr<FakeStreamingStats>& Stats() const { return stats_; }

private:
    FakeStreamingScript script_;
    std::shared_ptr<FakeStreamingStats> stats_;
};

} // namespace voxinsert
