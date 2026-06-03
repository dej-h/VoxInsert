#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spdlog {
class logger;
} // namespace spdlog

namespace voxinsert {

struct TranscriptionConfig;

// Stable correlation identifiers for a streaming run. Strings are used because
// provider item identifiers are strings and logs read more clearly.
using StreamingSessionId = std::string;
using UtteranceId = std::string;
using BackendItemId = std::string;

// VoxInsert's internal capture format. Provider adapters convert as needed.
struct PcmAudioFormat {
    int sampleRate = 16000;
    int channelCount = 1;
    int bitsPerSample = 16;
};

// The full retained PCM buffer. Source of truth for WAV writing and the
// finished-file fallback path.
struct RetainedRecordingAudio {
    PcmAudioFormat format;
    std::vector<int16_t> samples;
    bool streamingBackpressureObserved = false;
    bool inputOverflowObserved = false;
};

enum class StreamingBackendCommandKind {
    AppendAudio,
    CommitUtterance,
    CancelUtterance,
    CloseSession
};

// A command sent from the coordinator to a backend session. Internal to the
// coordinator/backend boundary; UI and persistence code never see it.
struct StreamingBackendCommand {
    StreamingBackendCommandKind kind = StreamingBackendCommandKind::AppendAudio;
    StreamingSessionId sessionId;
    UtteranceId utteranceId;
    uint64_t firstSequence = 0;
    uint64_t lastSequence = 0;
    PcmAudioFormat format;
    int64_t startOffsetMs = 0;
    int64_t durationMs = 0;
    // Non-owning payload valid only for the synchronous AppendAudio call.
    std::span<const int16_t> pcm16;
};

enum class BackendTranscriptEventKind {
    AppendDelta,
    Snapshot,
    SegmentFinalized,
    UtteranceFinalized,
    SessionReady,
    SessionClosed,
    Failed
};

// Provider-neutral backend event. Provider-specific JSON, WebSocket message
// names, item IDs, and ordering quirks stop at the backend adapter.
struct BackendTranscriptEvent {
    BackendTranscriptEventKind kind = BackendTranscriptEventKind::AppendDelta;
    StreamingSessionId sessionId;
    UtteranceId utteranceId;
    BackendItemId backendItemId;
    uint64_t revision = 0;
    std::string textUtf8;
    std::optional<int64_t> startMs;
    std::optional<int64_t> endMs;
    std::wstring failureReason;
};

enum class TranscriptPatchKind {
    AppendStableText,
    ReplaceUnstableTail,
    FinalizeUtterance,
    ResetUtterance,
    Error
};

// A change to VoxInsert's assembled transcript, emitted by the assembler.
//
// Consumer contract (visible = stable + unstable):
//   AppendStableText   : stable += textUtf8; unstable = "".
//   ReplaceUnstableTail: unstable = textUtf8.
//   FinalizeUtterance  : stable = textUtf8; unstable = ""; isFinal == true.
//   ResetUtterance     : stable = ""; unstable = "".
//   Error              : failureReason is set; visible text is unchanged.
struct TranscriptPatch {
    TranscriptPatchKind kind = TranscriptPatchKind::AppendStableText;
    StreamingSessionId sessionId;
    UtteranceId utteranceId;
    uint64_t revision = 0;
    size_t replaceStartUtf8 = 0;
    size_t replaceEndUtf8 = 0;
    std::string textUtf8;
    bool isFinal = false;
    float stability = 0.0f;
    std::wstring failureReason;
};

// Per-utterance assembled transcript state.
//   visible transcript = stablePrefixUtf8 + unstableTailUtf8
//   final transcript   = finalTextUtf8 once finalized == true
struct TranscriptAssemblyState {
    StreamingSessionId sessionId;
    UtteranceId utteranceId;
    std::string stablePrefixUtf8;
    std::string unstableTailUtf8;
    std::string finalTextUtf8;
    uint64_t revision = 0;
    bool finalized = false;
};

// Bounded diagnostic trace of emitted patches. The canonical transcript is
// always derived from assembled state, never from replaying this trace.
struct TranscriptPatchTrace {
    size_t maxEntries = 512;
    std::vector<TranscriptPatch> patches;
    bool truncated = false;
};

enum class StreamingWorkflowStatus {
    Succeeded,
    Cancelled,
    Failed,
    FallbackRequired
};

struct StreamingWorkflowResult {
    StreamingWorkflowStatus status = StreamingWorkflowStatus::Failed;
    RetainedRecordingAudio audio;
    std::string transcriptUtf8;
    std::vector<TranscriptPatch> patchTrace;
    bool transcriptFinalizedByStreaming = false;
    bool fallbackUsed = false;
    std::wstring errorTitle;
    std::wstring failureReason;
};

// Describes what a backend will do so the coordinator can pick the right
// assembly and commit behavior.
struct StreamingBackendCapabilities {
    bool emitsPartialsBeforeCommit = false;
    bool appendOnlyDeltas = false;
    bool revisingSnapshots = false;
    bool supportsManualCommit = true;
    bool supportsServerTurnDetection = false;
    int requiredSampleRate = 16000;
    int preferredAppendBatchMs = 20;
};

// A live streaming transcription session owned by the coordinator.
class IStreamingTranscriptionSession {
public:
    virtual ~IStreamingTranscriptionSession() = default;

    virtual bool Start(std::wstring& failureReason) = 0;
    virtual bool AppendAudio(const StreamingBackendCommand& command, std::wstring& failureReason) = 0;
    virtual bool CommitUtterance(const UtteranceId& utteranceId, std::wstring& failureReason) = 0;
    virtual void CancelUtterance(const UtteranceId& utteranceId) noexcept = 0;
    virtual void Close() noexcept = 0;
};

// A factory for streaming sessions. Provider selection stays behind this.
class IStreamingTranscriptionBackend {
public:
    virtual ~IStreamingTranscriptionBackend() = default;

    virtual std::string_view BackendId() const noexcept = 0;
    virtual StreamingBackendCapabilities Capabilities(const TranscriptionConfig& config) const = 0;

    virtual std::unique_ptr<IStreamingTranscriptionSession> CreateSession(
        const TranscriptionConfig& config,
        StreamingSessionId sessionId,
        std::function<void(BackendTranscriptEvent)> onEvent,
        std::shared_ptr<spdlog::logger> logger,
        std::wstring& failureReason) const = 0;
};

} // namespace voxinsert
