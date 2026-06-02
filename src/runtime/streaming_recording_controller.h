#pragma once

#include "audio/audio_recorder.h"
#include "audio/wav_writer.h"
#include "config/app_config.h"
#include "insertion/text_injector.h"
#include "runtime/bounded_queue.h"
#include "runtime/post_recording_workflow.h"
#include "transcription/streaming_transcription_service.h"
#include "transcription/transcript_assembler.h"
#include "transcription/transcription_client.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace spdlog {
class logger;
}

namespace voxinsert {

class ArchiveService;

struct StreamingRecordingRequest {
    AudioRecorder* audioRecorder = nullptr;
    WavWriter* wavWriter = nullptr;
    TextInjector* textInjector = nullptr;
    ArchiveService* archiveService = nullptr;
    TranscriptionClient* fallbackClient = nullptr;
    const AppConfig* config = nullptr;
    std::shared_ptr<spdlog::logger> logger;
    HWND ownerWindow = nullptr;
    bool smokeTest = false;
    AudioRecorder::AmplitudeCallback amplitudeCallback;
    std::function<bool()> isShutdownRequested;
    std::function<void(PostRecordingPhase)> onPhaseChanged;
};

struct StreamingRecordingResult {
    bool success = false;
    bool showDone = false;
    bool hasRecordingPath = false;
    bool hasTranscript = false;
    bool fallbackUsed = false;
    std::filesystem::path recordingPath;
    std::wstring transcript;
    std::wstring errorTitle;
    std::wstring failureReason;
};

// Coordinates the live WebSocket streaming transcription path: it owns the
// backend session, the capture-to-backend pump, the transcript assembler, and
// the final persistence/insertion bridge. Construction is cheap; the network
// session is only created in Start.
class StreamingRecordingController {
public:
    StreamingRecordingController();
    ~StreamingRecordingController();

    StreamingRecordingController(const StreamingRecordingController&) = delete;
    StreamingRecordingController& operator=(const StreamingRecordingController&) = delete;

    // Connects the backend session and starts capturing/streaming audio. On
    // failure nothing is left running so the caller can fall back to the
    // one-shot recording path.
    bool Start(const StreamingRecordingRequest& request, std::wstring& failureReason);

    // Stops capture, commits the utterance, waits for the final transcript,
    // writes the WAV, inserts the text, and enqueues the archive. Call once
    // after a successful Start.
    StreamingRecordingResult Finish() noexcept;

    // Aborts streaming without inserting any text.
    void Cancel() noexcept;

    bool IsActive() const noexcept { return active_.load(); }

private:
    void AudioPumpMain(std::stop_token stopToken) noexcept;
    void EventPumpMain(std::stop_token stopToken) noexcept;
    void OnBackendEvent(BackendTranscriptEvent event);
    void StopWorkers() noexcept;
    void FlushAppendBatch(std::vector<int16_t>& batch) noexcept;

    StreamingRecordingRequest request_;
    std::unique_ptr<IStreamingTranscriptionBackend> backend_;
    std::unique_ptr<IStreamingTranscriptionSession> session_;
    TranscriptAssembler assembler_;
    std::mutex assemblerMutex_;

    BlockingBoundedQueue<CapturedAudioChunk> audioQueue_;
    BlockingBoundedQueue<BackendTranscriptEvent> eventQueue_;

    std::jthread audioPump_;
    std::jthread eventPump_;

    std::mutex finalizeMutex_;
    std::condition_variable finalizeCondition_;
    bool finalized_ = false;
    bool failed_ = false;
    std::wstring backendFailureReason_;

    std::atomic<bool> active_{false};
    std::atomic<bool> backpressureObserved_{false};
    std::atomic<bool> sessionSendFailed_{false};
    std::atomic<std::uint64_t> nextSequence_{0};
    StreamingSessionId sessionId_;
    UtteranceId utteranceId_;
    PcmAudioFormat captureFormat_;
    int appendBatchMs_ = 80;
};

} // namespace voxinsert
