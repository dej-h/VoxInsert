#pragma once

#include "transcription/streaming_transcription_service.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace voxinsert {

// Streaming backend that talks to the Mistral realtime transcription API over a
// secure WebSocket. Audio is appended as base64 PCM16 at the API's native
// 16 kHz sample rate (so no resampling is required) and transcription
// deltas/finals are surfaced as provider-neutral BackendTranscriptEvent values.
//
// The adapter contains all Mistral-specific JSON, event names, and the I/O
// thread; the rest of VoxInsert only sees the IStreamingTranscriptionBackend
// abstraction, exactly mirroring the OpenAI realtime adapter.
class MistralRealtimeStreamingBackend final : public IStreamingTranscriptionBackend {
public:
    MistralRealtimeStreamingBackend() = default;

    std::string_view BackendId() const noexcept override { return "mistral_realtime"; }

    StreamingBackendCapabilities Capabilities(const TranscriptionConfig& config) const override;

    std::unique_ptr<IStreamingTranscriptionSession> CreateSession(
        const TranscriptionConfig& config,
        StreamingSessionId sessionId,
        std::function<void(BackendTranscriptEvent)> onEvent,
        std::shared_ptr<spdlog::logger> logger,
        std::wstring& failureReason) const override;
};

} // namespace voxinsert
