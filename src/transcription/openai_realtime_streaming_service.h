#pragma once

#include "transcription/streaming_transcription_service.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace voxinsert {

// Streaming backend that talks to the OpenAI Realtime transcription API over a
// secure WebSocket. Audio is appended as base64 PCM16 (resampled to the rate
// the API requires) and transcription deltas/finals are surfaced as
// provider-neutral BackendTranscriptEvent values.
//
// The adapter contains all OpenAI-specific JSON, event names, and the I/O
// thread; the rest of VoxInsert only sees the IStreamingTranscriptionBackend
// abstraction.
class OpenAiRealtimeStreamingBackend final : public IStreamingTranscriptionBackend {
public:
    OpenAiRealtimeStreamingBackend() = default;

    std::string_view BackendId() const noexcept override { return "openai_realtime"; }

    StreamingBackendCapabilities Capabilities(const TranscriptionConfig& config) const override;

    std::unique_ptr<IStreamingTranscriptionSession> CreateSession(
        const TranscriptionConfig& config,
        StreamingSessionId sessionId,
        std::function<void(BackendTranscriptEvent)> onEvent,
        std::shared_ptr<spdlog::logger> logger,
        std::wstring& failureReason) const override;
};

} // namespace voxinsert
