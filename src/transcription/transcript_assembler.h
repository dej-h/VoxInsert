#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "transcription/streaming_transcription_service.h"

namespace voxinsert {

// Converts provider-neutral BackendTranscriptEvent values into TranscriptPatch
// values and maintains the assembled transcript for each utterance.
//
// Supported backend behaviors:
//   - Append-only deltas: each AppendDelta is treated as confirmed text and
//     appended to the stable prefix.
//   - Revising snapshots: each Snapshot is the full current hypothesis. The
//     longest common prefix with the previous snapshot (on UTF-8 code point
//     boundaries) is promoted to stable text; the remainder is the unstable
//     tail. A prefix becomes stable after appearing unchanged in two
//     consecutive snapshots.
//
// The assembler never splits a UTF-8 code point and never shrinks the stable
// prefix once promoted.
class TranscriptAssembler {
public:
    using PatchSink = std::function<void(const TranscriptPatch&)>;

    explicit TranscriptAssembler(size_t maxTraceEntries = 512);

    // Applies one backend event, emitting zero or more patches to the sink.
    void Apply(const BackendTranscriptEvent& event, const PatchSink& sink);

    // Convenience overload that records patches into the internal trace only.
    void Apply(const BackendTranscriptEvent& event);

    // Visible transcript for one utterance: stable prefix + unstable tail, or
    // the final text once finalized.
    std::string VisibleTextFor(const UtteranceId& utteranceId) const;

    // Combined transcript across all utterances in first-seen order. Finalized
    // utterances contribute their final text; others contribute visible text.
    std::string FinalTranscriptUtf8() const;

    bool IsFinalized(const UtteranceId& utteranceId) const;

    const TranscriptPatchTrace& Trace() const { return trace_; }

    void Reset() noexcept;

private:
    TranscriptAssemblyState& StateFor(const StreamingSessionId& sessionId, const UtteranceId& utteranceId);
    void Emit(TranscriptPatch patch, const PatchSink& sink);

    void ApplyAppendDelta(const BackendTranscriptEvent& event, const PatchSink& sink);
    void ApplySnapshot(const BackendTranscriptEvent& event, const PatchSink& sink);
    void ApplyFinalize(const BackendTranscriptEvent& event, const PatchSink& sink);
    void ApplyFailure(const BackendTranscriptEvent& event, const PatchSink& sink);

    std::map<UtteranceId, TranscriptAssemblyState> states_;
    std::map<UtteranceId, std::string> lastSnapshot_;
    std::vector<UtteranceId> order_;
    TranscriptPatchTrace trace_;
};

// Returns the length in bytes of the longest common prefix of a and b that ends
// on a UTF-8 code point boundary. Exposed for testing.
size_t LongestCommonPrefixUtf8(const std::string& a, const std::string& b);

} // namespace voxinsert
