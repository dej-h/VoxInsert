#include "runtime/post_recording_workflow.h"

#include "archive/archive_service.h"
#include "observability/logging.h"

#include <exception>
#include <vector>

namespace voxinsert {
namespace {

PostRecordingWorkflowResult FailedWorkflowResult(std::wstring errorTitle, std::wstring failureReason) {
    PostRecordingWorkflowResult result;
    result.status = PostRecordingWorkflowStatus::Failed;
    result.errorTitle = std::move(errorTitle);
    result.failureReason = std::move(failureReason);
    return result;
}

bool IsValidRequest(const PostRecordingWorkflowRequest& request) {
    return request.audioRecorder != nullptr &&
           request.wavWriter != nullptr &&
           request.audioConfig != nullptr &&
           request.transcriptionClient != nullptr &&
           request.transcriptionConfig != nullptr &&
           request.textInjector != nullptr &&
           request.insertionConfig != nullptr;
}

} // namespace

PostRecordingWorkflowResult RunPostRecordingWorkflow(const PostRecordingWorkflowRequest& request) noexcept {
    try {
        if (!IsValidRequest(request)) {
            return FailedWorkflowResult(
                L"VoxInsert transcription error",
                L"PostRecordingWorkflow received an incomplete request.");
        }

        const auto shutdownRequested = [&request]() {
            return request.isShutdownRequested && request.isShutdownRequested();
        };

        PostRecordingWorkflowResult result;
        std::vector<int16_t> samples;
        std::wstring failureReason;
        if (!request.audioRecorder->Stop(samples, failureReason)) {
            return FailedWorkflowResult(L"VoxInsert recording error", std::move(failureReason));
        }

        std::filesystem::path wavPath;
        if (!request.wavWriter->WritePcm16Mono(samples, request.audioConfig->sampleRate, wavPath, failureReason)) {
            return FailedWorkflowResult(L"VoxInsert WAV error", std::move(failureReason));
        }

        result.hasRecordingPath = true;
        result.recordingPath = wavPath;

        if (request.logger != nullptr) {
            request.logger->info("recording written to {}", Utf8FromWide(wavPath.wstring()));
        }

        if (request.smokeTest) {
            if (request.logger != nullptr) {
                request.logger->info("smoke-test: skipping transcription upload");
            }

            result.status = PostRecordingWorkflowStatus::Succeeded;
            result.showDone = false;
            return result;
        }

        if (shutdownRequested()) {
            result.status = PostRecordingWorkflowStatus::Cancelled;
            return result;
        }

        if (request.onPhaseChanged) {
            request.onPhaseChanged(PostRecordingPhase::Transcribing);
        }

        std::string transcript;
        if (!request.transcriptionClient->Transcribe(*request.transcriptionConfig, wavPath, transcript, failureReason)) {
            return FailedWorkflowResult(L"VoxInsert transcription error", std::move(failureReason));
        }

        if (request.logger != nullptr) {
            request.logger->info("transcription completed ({} UTF-8 bytes)", transcript.size());
        }

        result.hasTranscript = true;
        result.transcript = WideFromUtf8(transcript);

        const auto enqueueArchive = [&request, &samples, &transcript](bool insertionSucceeded) {
            if (request.archiveService == nullptr || request.archiveConfig == nullptr || !request.archiveConfig->enabled) {
                return;
            }

            request.archiveService->Enqueue(ArchiveRequest{
                .archive = *request.archiveConfig,
                .audio = *request.audioConfig,
                .transcription = *request.transcriptionConfig,
                .samples = samples,
                .transcriptUtf8 = transcript,
                .insertionSucceeded = insertionSucceeded,
                .logger = request.logger,
            });
        };

        if (shutdownRequested()) {
            result.status = PostRecordingWorkflowStatus::Cancelled;
            return result;
        }

        if (request.onPhaseChanged) {
            request.onPhaseChanged(PostRecordingPhase::Inserting);
        }

        if (!request.textInjector->InsertText(
                request.ownerWindow,
                *request.insertionConfig,
                result.transcript,
                failureReason)) {
            enqueueArchive(false);
            return FailedWorkflowResult(L"VoxInsert insertion error", std::move(failureReason));
        }

        enqueueArchive(true);

        if (request.logger != nullptr) {
            request.logger->info("transcription inserted into the focused text field via clipboard paste");
        }

        result.status = PostRecordingWorkflowStatus::Succeeded;
        result.showDone = true;
        return result;
    }
    catch (const std::exception& exception) {
        return FailedWorkflowResult(
            L"VoxInsert transcription error",
            L"Unexpected background transcription failure: " + WideFromUtf8(exception.what()));
    }
    catch (...) {
        return FailedWorkflowResult(
            L"VoxInsert transcription error",
            L"Unexpected background transcription failure.");
    }
}

} // namespace voxinsert