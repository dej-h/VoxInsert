#include "transcription/transcript_assembler.h"

#include <algorithm>

namespace voxinsert {

namespace {

bool IsUtf8ContinuationByte(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

// Moves the byte length back to the nearest UTF-8 code point boundary so a
// multi-byte sequence is never split.
size_t ClampToCodePointBoundary(const std::string& text, size_t length) {
    length = std::min(length, text.size());
    while (length > 0 && length < text.size() && IsUtf8ContinuationByte(static_cast<unsigned char>(text[length]))) {
        --length;
    }
    return length;
}

} // namespace

size_t LongestCommonPrefixUtf8(const std::string& a, const std::string& b) {
    const size_t limit = std::min(a.size(), b.size());
    size_t matched = 0;
    while (matched < limit && a[matched] == b[matched]) {
        ++matched;
    }
    return ClampToCodePointBoundary(a, matched);
}

TranscriptAssembler::TranscriptAssembler(size_t maxTraceEntries) {
    trace_.maxEntries = maxTraceEntries;
}

void TranscriptAssembler::Reset() noexcept {
    states_.clear();
    lastSnapshot_.clear();
    order_.clear();
    trace_.patches.clear();
    trace_.truncated = false;
}

TranscriptAssemblyState& TranscriptAssembler::StateFor(
    const StreamingSessionId& sessionId,
    const UtteranceId& utteranceId) {
    auto it = states_.find(utteranceId);
    if (it == states_.end()) {
        TranscriptAssemblyState state;
        state.sessionId = sessionId;
        state.utteranceId = utteranceId;
        it = states_.emplace(utteranceId, std::move(state)).first;
        order_.push_back(utteranceId);
    }
    return it->second;
}

void TranscriptAssembler::Emit(TranscriptPatch patch, const PatchSink& sink) {
    if (trace_.patches.size() < trace_.maxEntries) {
        trace_.patches.push_back(patch);
    } else {
        trace_.truncated = true;
    }
    if (sink) {
        sink(patch);
    }
}

void TranscriptAssembler::Apply(const BackendTranscriptEvent& event, const PatchSink& sink) {
    switch (event.kind) {
    case BackendTranscriptEventKind::AppendDelta:
        ApplyAppendDelta(event, sink);
        break;
    case BackendTranscriptEventKind::Snapshot:
        ApplySnapshot(event, sink);
        break;
    case BackendTranscriptEventKind::SegmentFinalized:
    case BackendTranscriptEventKind::UtteranceFinalized:
        ApplyFinalize(event, sink);
        break;
    case BackendTranscriptEventKind::Failed:
        ApplyFailure(event, sink);
        break;
    case BackendTranscriptEventKind::SessionReady:
    case BackendTranscriptEventKind::SessionClosed:
        // Lifecycle events do not change assembled text.
        break;
    }
}

void TranscriptAssembler::Apply(const BackendTranscriptEvent& event) {
    Apply(event, PatchSink{});
}

void TranscriptAssembler::ApplyAppendDelta(const BackendTranscriptEvent& event, const PatchSink& sink) {
    if (event.textUtf8.empty()) {
        return; // Empty deltas are suppressed locally.
    }

    TranscriptAssemblyState& state = StateFor(event.sessionId, event.utteranceId);
    if (state.finalized) {
        return; // Ignore late deltas after finalization.
    }

    state.stablePrefixUtf8 += event.textUtf8;
    state.revision += 1;
    lastSnapshot_[event.utteranceId] = state.stablePrefixUtf8 + state.unstableTailUtf8;

    TranscriptPatch patch;
    patch.kind = TranscriptPatchKind::AppendStableText;
    patch.sessionId = state.sessionId;
    patch.utteranceId = state.utteranceId;
    patch.revision = state.revision;
    patch.replaceStartUtf8 = state.stablePrefixUtf8.size() - event.textUtf8.size();
    patch.replaceEndUtf8 = patch.replaceStartUtf8;
    patch.textUtf8 = event.textUtf8;
    patch.stability = 1.0f;
    Emit(std::move(patch), sink);
}

void TranscriptAssembler::ApplySnapshot(const BackendTranscriptEvent& event, const PatchSink& sink) {
    TranscriptAssemblyState& state = StateFor(event.sessionId, event.utteranceId);
    if (state.finalized) {
        return;
    }

    const std::string& newText = event.textUtf8;
    const std::string& previousSnapshot = lastSnapshot_[event.utteranceId];
    const std::string previousUnstable = state.unstableTailUtf8;

    const size_t oldStableLen = state.stablePrefixUtf8.size();
    const size_t lcp = LongestCommonPrefixUtf8(previousSnapshot, newText);

    // Stable text never shrinks; promote only what stayed identical across two
    // consecutive snapshots.
    size_t newStableLen = std::max(oldStableLen, lcp);
    newStableLen = ClampToCodePointBoundary(newText, newStableLen);

    const bool promoted = newStableLen > oldStableLen;
    if (promoted) {
        const std::string appended = newText.substr(oldStableLen, newStableLen - oldStableLen);
        state.stablePrefixUtf8 = newText.substr(0, newStableLen);
        state.revision += 1;

        TranscriptPatch patch;
        patch.kind = TranscriptPatchKind::AppendStableText;
        patch.sessionId = state.sessionId;
        patch.utteranceId = state.utteranceId;
        patch.revision = state.revision;
        patch.replaceStartUtf8 = oldStableLen;
        patch.replaceEndUtf8 = oldStableLen;
        patch.textUtf8 = appended;
        patch.stability = 1.0f;
        Emit(std::move(patch), sink);
    }

    std::string newUnstable = newText.substr(newStableLen);
    if (promoted || newUnstable != previousUnstable) {
        state.unstableTailUtf8 = newUnstable;
        state.revision += 1;

        TranscriptPatch patch;
        patch.kind = TranscriptPatchKind::ReplaceUnstableTail;
        patch.sessionId = state.sessionId;
        patch.utteranceId = state.utteranceId;
        patch.revision = state.revision;
        patch.replaceStartUtf8 = state.stablePrefixUtf8.size();
        patch.replaceEndUtf8 = state.stablePrefixUtf8.size() + previousUnstable.size();
        patch.textUtf8 = newUnstable;
        patch.stability = 0.0f;
        Emit(std::move(patch), sink);
    } else {
        state.unstableTailUtf8 = std::move(newUnstable);
    }

    lastSnapshot_[event.utteranceId] = newText;
}

void TranscriptAssembler::ApplyFinalize(const BackendTranscriptEvent& event, const PatchSink& sink) {
    TranscriptAssemblyState& state = StateFor(event.sessionId, event.utteranceId);
    if (state.finalized) {
        return;
    }

    std::string finalText = !event.textUtf8.empty()
        ? event.textUtf8
        : state.stablePrefixUtf8 + state.unstableTailUtf8;

    state.finalTextUtf8 = finalText;
    state.stablePrefixUtf8 = finalText;
    state.unstableTailUtf8.clear();
    state.finalized = true;
    state.revision += 1;
    lastSnapshot_[event.utteranceId] = finalText;

    TranscriptPatch patch;
    patch.kind = TranscriptPatchKind::FinalizeUtterance;
    patch.sessionId = state.sessionId;
    patch.utteranceId = state.utteranceId;
    patch.revision = state.revision;
    patch.replaceStartUtf8 = 0;
    patch.replaceEndUtf8 = 0;
    patch.textUtf8 = std::move(finalText);
    patch.isFinal = true;
    patch.stability = 1.0f;
    Emit(std::move(patch), sink);
}

void TranscriptAssembler::ApplyFailure(const BackendTranscriptEvent& event, const PatchSink& sink) {
    TranscriptAssemblyState& state = StateFor(event.sessionId, event.utteranceId);
    state.revision += 1;

    TranscriptPatch patch;
    patch.kind = TranscriptPatchKind::Error;
    patch.sessionId = state.sessionId;
    patch.utteranceId = state.utteranceId;
    patch.revision = state.revision;
    patch.failureReason = event.failureReason;
    Emit(std::move(patch), sink);
}

std::string TranscriptAssembler::VisibleTextFor(const UtteranceId& utteranceId) const {
    const auto it = states_.find(utteranceId);
    if (it == states_.end()) {
        return {};
    }
    const TranscriptAssemblyState& state = it->second;
    if (state.finalized) {
        return state.finalTextUtf8;
    }
    return state.stablePrefixUtf8 + state.unstableTailUtf8;
}

bool TranscriptAssembler::IsFinalized(const UtteranceId& utteranceId) const {
    const auto it = states_.find(utteranceId);
    return it != states_.end() && it->second.finalized;
}

std::string TranscriptAssembler::FinalTranscriptUtf8() const {
    std::string combined;
    for (const UtteranceId& id : order_) {
        const auto it = states_.find(id);
        if (it == states_.end()) {
            continue;
        }
        const TranscriptAssemblyState& state = it->second;
        const std::string text = state.finalized
            ? state.finalTextUtf8
            : state.stablePrefixUtf8 + state.unstableTailUtf8;
        if (text.empty()) {
            continue;
        }
        if (!combined.empty()) {
            combined += ' ';
        }
        combined += text;
    }
    return combined;
}

} // namespace voxinsert
