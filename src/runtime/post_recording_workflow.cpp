#include "runtime/post_recording_workflow.h"

#include "archive/archive_service.h"
#include "observability/logging.h"

#include <chrono>
#include <exception>
#include <vector>

namespace voxinsert {
namespace {

using WorkflowClock = std::chrono::steady_clock;

int64_t ElapsedMilliseconds(WorkflowClock::time_point startedAt, WorkflowClock::time_point finishedAt) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(finishedAt - startedAt).count();
}

void LogPostRecordingLatency(
    const std::shared_ptr<spdlog::logger>& logger,
    std::string_view outcome,
    int64_t stopRecordingMs,
    int64_t writeWavMs,
    int64_t transcriptionMs,
    int64_t insertionMs,
    int64_t totalMs,
    size_t transcriptUtf8Bytes) {
    if (logger == nullptr) {
        return;
    }

    logger->debug(
        "post-recording latency outcome={} stop_recording={}ms wav_write={}ms transcription={}ms insert={}ms total={}ms transcript_utf8_bytes={}",
        outcome,
        stopRecordingMs,
        writeWavMs,
        transcriptionMs,
        insertionMs,
        totalMs,
        transcriptUtf8Bytes);
}

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

        const auto workflowStartedAt = WorkflowClock::now();
        PostRecordingWorkflowResult result;
        std::vector<int16_t> samples;
        std::wstring failureReason;

        const auto stopRecordingStartedAt = WorkflowClock::now();
        if (!request.audioRecorder->Stop(samples, failureReason)) {
            const auto failedAt = WorkflowClock::now();
            LogPostRecordingLatency(
                request.logger,
                "stop_recording_failed",
                ElapsedMilliseconds(stopRecordingStartedAt, failedAt),
                0,
                0,
                0,
                ElapsedMilliseconds(workflowStartedAt, failedAt),
                0);
            return FailedWorkflowResult(L"VoxInsert recording error", std::move(failureReason));
        }
        const auto stopRecordingFinishedAt = WorkflowClock::now();
        const int64_t stopRecordingMs = ElapsedMilliseconds(stopRecordingStartedAt, stopRecordingFinishedAt);

        std::filesystem::path wavPath;

        const auto writeWavStartedAt = WorkflowClock::now();
        if (!request.wavWriter->WritePcm16Mono(samples, request.audioConfig->sampleRate, wavPath, failureReason)) {
            const auto failedAt = WorkflowClock::now();
            LogPostRecordingLatency(
                request.logger,
                "wav_write_failed",
                stopRecordingMs,
                ElapsedMilliseconds(writeWavStartedAt, failedAt),
                0,
                0,
                ElapsedMilliseconds(workflowStartedAt, failedAt),
                0);
            return FailedWorkflowResult(L"VoxInsert WAV error", std::move(failureReason));
        }
        const auto writeWavFinishedAt = WorkflowClock::now();
        const int64_t writeWavMs = ElapsedMilliseconds(writeWavStartedAt, writeWavFinishedAt);

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

        const auto transcriptionStartedAt = WorkflowClock::now();
        if (!request.transcriptionClient->Transcribe(*request.transcriptionConfig, wavPath, transcript, failureReason)) {
            const auto failedAt = WorkflowClock::now();
            LogPostRecordingLatency(
                request.logger,
                "transcription_failed",
                stopRecordingMs,
                writeWavMs,
                ElapsedMilliseconds(transcriptionStartedAt, failedAt),
                0,
                ElapsedMilliseconds(workflowStartedAt, failedAt),
                0);
            return FailedWorkflowResult(L"VoxInsert transcription error", std::move(failureReason));
        }
        const auto transcriptionFinishedAt = WorkflowClock::now();
        const int64_t transcriptionMs = ElapsedMilliseconds(transcriptionStartedAt, transcriptionFinishedAt);

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

        const auto insertionStartedAt = WorkflowClock::now();
        if (!request.textInjector->InsertText(
                request.ownerWindow,
                *request.insertionConfig,
                result.transcript,
                failureReason)) {
            const auto failedAt = WorkflowClock::now();
            enqueueArchive(false);
            LogPostRecordingLatency(
                request.logger,
                "insert_failed",
                stopRecordingMs,
                writeWavMs,
                transcriptionMs,
                ElapsedMilliseconds(insertionStartedAt, failedAt),
                ElapsedMilliseconds(workflowStartedAt, failedAt),
                transcript.size());
            return FailedWorkflowResult(L"VoxInsert insertion error", std::move(failureReason));
        }
        const auto insertionFinishedAt = WorkflowClock::now();
        const int64_t insertionMs = ElapsedMilliseconds(insertionStartedAt, insertionFinishedAt);

        enqueueArchive(true);

        LogPostRecordingLatency(
            request.logger,
            "success",
            stopRecordingMs,
            writeWavMs,
            transcriptionMs,
            insertionMs,
            ElapsedMilliseconds(workflowStartedAt, insertionFinishedAt),
            transcript.size());

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