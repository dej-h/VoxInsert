#pragma once

#include "audio/audio_recorder.h"
#include "audio/wav_writer.h"
#include "config/app_config.h"
#include "insertion/text_injector.h"
#include "transcription/transcription_client.h"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace spdlog {
class logger;
}

namespace voxinsert {

class ArchiveService;

enum class PostRecordingPhase {
    Transcribing,
    Inserting
};

struct PostRecordingWorkflowRequest {
    AudioRecorder* audioRecorder = nullptr;
    WavWriter* wavWriter = nullptr;
    const AudioConfig* audioConfig = nullptr;
    TranscriptionClient* transcriptionClient = nullptr;
    const TranscriptionConfig* transcriptionConfig = nullptr;
    TextInjector* textInjector = nullptr;
    const InsertionConfig* insertionConfig = nullptr;
    ArchiveService* archiveService = nullptr;
    const ArchiveConfig* archiveConfig = nullptr;
    std::shared_ptr<spdlog::logger> logger;
    HWND ownerWindow = nullptr;
    bool smokeTest = false;
    std::function<bool()> isShutdownRequested;
    std::function<void(PostRecordingPhase)> onPhaseChanged;
};

enum class PostRecordingWorkflowStatus {
    Succeeded,
    Cancelled,
    Failed
};

struct PostRecordingWorkflowResult {
    PostRecordingWorkflowStatus status = PostRecordingWorkflowStatus::Failed;
    bool showDone = false;
    bool hasRecordingPath = false;
    bool hasTranscript = false;
    std::filesystem::path recordingPath;
    std::wstring transcript;
    std::wstring errorTitle;
    std::wstring failureReason;
};

PostRecordingWorkflowResult RunPostRecordingWorkflow(const PostRecordingWorkflowRequest& request) noexcept;

} // namespace voxinsert