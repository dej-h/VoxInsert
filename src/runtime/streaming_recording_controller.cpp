#include "runtime/streaming_recording_controller.h"

#include "archive/archive_service.h"
#include "observability/logging.h"
#include "transcription/mistral_realtime_streaming_service.h"
#include "transcription/openai_realtime_streaming_service.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace voxinsert {
namespace {

using StreamingClock = std::chrono::steady_clock;

constexpr size_t kAudioQueueCapacity = 512;
constexpr size_t kEventQueueCapacity = 512;

int64_t ElapsedMilliseconds(StreamingClock::time_point startedAt, StreamingClock::time_point finishedAt) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(finishedAt - startedAt).count();
}

std::unique_ptr<IStreamingTranscriptionBackend> CreateBackend(const std::string& provider) {
    if (provider == "openai_realtime") {
        return std::make_unique<OpenAiRealtimeStreamingBackend>();
    }
    if (provider == "mistral_realtime") {
        return std::make_unique<MistralRealtimeStreamingBackend>();
    }
    return nullptr;
}

} // namespace

StreamingRecordingController::StreamingRecordingController()
    : audioQueue_(kAudioQueueCapacity), eventQueue_(kEventQueueCapacity) {}

StreamingRecordingController::~StreamingRecordingController() {
    Cancel();
}

bool StreamingRecordingController::Start(const StreamingRecordingRequest& request, std::wstring& failureReason) {
    if (active_.load()) {
        failureReason = L"Streaming recording is already in progress.";
        return false;
    }

    if (request.audioRecorder == nullptr || request.config == nullptr) {
        failureReason = L"Streaming recording received an incomplete request.";
        return false;
    }

    request_ = request;

    const StreamingTranscriptionConfig& streaming = request_.config->transcription.streaming;
    appendBatchMs_ = streaming.appendBatchMs;

    backend_ = CreateBackend(streaming.provider);
    if (backend_ == nullptr) {
        failureReason = L"Unsupported streaming transcription provider '";
        failureReason += WideFromUtf8(streaming.provider);
        failureReason += L"'.";
        return false;
    }

    captureFormat_.sampleRate = request_.config->audio.sampleRate;
    captureFormat_.channelCount = request_.config->audio.channelCount;
    captureFormat_.bitsPerSample = 16;

    const auto now = std::chrono::system_clock::now().time_since_epoch();
    sessionId_ = "stream-" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    utteranceId_ = sessionId_ + "-u0";

    {
        std::scoped_lock lock(assemblerMutex_);
        assembler_.Reset();
    }
    finalized_ = false;
    failed_ = false;
    backendFailureReason_.clear();
    backpressureObserved_.store(false);
    sessionSendFailed_.store(false);
    nextSequence_.store(0);

    // The queues are reused across recordings; a previous Finish()/Cancel()
    // left them closed. Re-open and drain them so this session's audio and
    // backend events flow again. Without this, only the first recording after
    // launch works and every later one stalls (no audio sent, finalize event
    // dropped) until the finalize timeout falls back to file transcription.
    audioQueue_.Reset();
    eventQueue_.Reset();

    session_ = backend_->CreateSession(
        request_.config->transcription,
        sessionId_,
        [this](BackendTranscriptEvent event) { OnBackendEvent(std::move(event)); },
        request_.logger,
        failureReason);
    if (session_ == nullptr) {
        backend_.reset();
        return false;
    }

    if (!session_->Start(failureReason)) {
        session_.reset();
        backend_.reset();
        return false;
    }

    eventPump_ = std::jthread([this](std::stop_token stopToken) { EventPumpMain(std::move(stopToken)); });
    audioPump_ = std::jthread([this](std::stop_token stopToken) { AudioPumpMain(std::move(stopToken)); });

    const bool recorderStarted = request_.audioRecorder->Start(
        request_.config->audio,
        request_.amplitudeCallback,
        [this](const int16_t* data, size_t sampleCount) {
            CapturedAudioChunk chunk;
            chunk.sessionId = sessionId_;
            chunk.utteranceId = utteranceId_;
            chunk.sequence = nextSequence_.fetch_add(1);
            chunk.format = captureFormat_;
            chunk.pcm16.assign(data, data + sampleCount);
            if (!audioQueue_.TryPush(std::move(chunk))) {
                backpressureObserved_.store(true);
            }
        },
        failureReason);

    if (!recorderStarted) {
        StopWorkers();
        if (session_ != nullptr) {
            session_->Close();
            session_.reset();
        }
        backend_.reset();
        return false;
    }

    active_.store(true);
    if (request_.logger != nullptr) {
        request_.logger->info("streaming transcription session started (provider={})", streaming.provider);
    }
    return true;
}

void StreamingRecordingController::FlushAppendBatch(std::vector<int16_t>& batch) noexcept {
    if (batch.empty() || session_ == nullptr) {
        batch.clear();
        return;
    }

    StreamingBackendCommand command;
    command.kind = StreamingBackendCommandKind::AppendAudio;
    command.sessionId = sessionId_;
    command.utteranceId = utteranceId_;
    command.format = captureFormat_;
    command.pcm16 = std::move(batch);
    batch.clear();

    std::wstring failureReason;
    if (!session_->AppendAudio(command, failureReason)) {
        // The socket is dead. Mark the session so Finish skips the finalize
        // wait, and log only the first failure to avoid flooding the log.
        if (!sessionSendFailed_.exchange(true) && request_.logger != nullptr) {
            request_.logger->warn("streaming append failed: {}", Utf8FromWide(failureReason));
        }
    }
}

void StreamingRecordingController::AudioPumpMain(std::stop_token stopToken) noexcept {
    const int sampleRate = captureFormat_.sampleRate > 0 ? captureFormat_.sampleRate : 16000;
    const size_t batchThreshold =
        static_cast<size_t>(static_cast<int64_t>(sampleRate) * std::max(appendBatchMs_, 10) / 1000);

    std::vector<int16_t> batch;
    CapturedAudioChunk chunk;
    while (audioQueue_.WaitPop(chunk, stopToken)) {
        batch.insert(batch.end(), chunk.pcm16.begin(), chunk.pcm16.end());
        if (batch.size() >= batchThreshold) {
            FlushAppendBatch(batch);
        }
    }

    FlushAppendBatch(batch);
}

void StreamingRecordingController::EventPumpMain(std::stop_token stopToken) noexcept {
    BackendTranscriptEvent event;
    while (eventQueue_.WaitPop(event, stopToken)) {
        {
            std::scoped_lock lock(assemblerMutex_);
            assembler_.Apply(event);
        }

        if (event.kind == BackendTranscriptEventKind::UtteranceFinalized) {
            {
                std::scoped_lock lock(finalizeMutex_);
                finalized_ = true;
            }
            finalizeCondition_.notify_all();
        }
        else if (event.kind == BackendTranscriptEventKind::Failed) {
            {
                std::scoped_lock lock(finalizeMutex_);
                failed_ = true;
                backendFailureReason_ = event.failureReason;
            }
            finalizeCondition_.notify_all();
        }
    }
}

void StreamingRecordingController::OnBackendEvent(BackendTranscriptEvent event) {
    if (!eventQueue_.TryPush(std::move(event))) {
        if (!backpressureObserved_.exchange(true) && request_.logger != nullptr) {
            request_.logger->warn("streaming event queue full; dropping backend events");
        }
    }
}

void StreamingRecordingController::StopWorkers() noexcept {
    audioQueue_.Close();
    eventQueue_.Close();
    if (audioPump_.joinable()) {
        audioPump_.request_stop();
        audioPump_.join();
    }
    if (eventPump_.joinable()) {
        eventPump_.request_stop();
        eventPump_.join();
    }
}

StreamingRecordingResult StreamingRecordingController::Finish() noexcept {
    StreamingRecordingResult result;

    try {
        if (!active_.load()) {
            result.errorTitle = L"VoxInsert transcription error";
            result.failureReason = L"StreamingRecordingController::Finish called without an active session.";
            return result;
        }

        const StreamingTranscriptionConfig& streaming = request_.config->transcription.streaming;
        const auto workflowStartedAt = StreamingClock::now();

        // 1. Stop capture and collect the retained PCM (source of truth).
        std::vector<int16_t> samples;
        std::wstring failureReason;
        const auto stopRecordingStartedAt = StreamingClock::now();
        const bool stopped = request_.audioRecorder->Stop(samples, failureReason);
        const int64_t stopRecordingMs = ElapsedMilliseconds(stopRecordingStartedAt, StreamingClock::now());

        // 2. Drain queued audio to the backend, then commit the utterance.
        audioQueue_.Close();
        if (audioPump_.joinable()) {
            audioPump_.join();
        }

        if (request_.onPhaseChanged) {
            request_.onPhaseChanged(PostRecordingPhase::Transcribing);
        }

        const auto transcriptionStartedAt = StreamingClock::now();
        bool commitSucceeded = false;
        if (session_ != nullptr) {
            std::wstring commitFailure;
            commitSucceeded = session_->CommitUtterance(utteranceId_, commitFailure);
            if (!commitSucceeded && request_.logger != nullptr) {
                request_.logger->warn("streaming commit failed: {}", Utf8FromWide(commitFailure));
            }
        }

        // 3. Wait for the final transcript only while the backend session is
        // still healthy. A dead socket (failed appends or commit) will never
        // deliver a final transcript, so skip the timeout and fall back
        // immediately instead of stalling for finalizeTimeoutMs.
        const bool sessionAlive = commitSucceeded && !sessionSendFailed_.load();
        if (sessionAlive) {
            std::unique_lock<std::mutex> lock(finalizeMutex_);
            finalizeCondition_.wait_for(
                lock,
                std::chrono::milliseconds(std::max(streaming.finalizeTimeoutMs, 250)),
                [this]() { return finalized_ || failed_; });
        }

        if (session_ != nullptr) {
            session_->Close();
        }
        StopWorkers();
        session_.reset();
        backend_.reset();
        active_.store(false);
        const int64_t transcriptionMs = ElapsedMilliseconds(transcriptionStartedAt, StreamingClock::now());

        bool streamingFailed = false;
        std::wstring streamingFailureReason;
        {
            std::scoped_lock lock(finalizeMutex_);
            streamingFailed = failed_ || !finalized_;
            streamingFailureReason = backendFailureReason_;
        }

        std::string transcriptUtf8;
        {
            std::scoped_lock lock(assemblerMutex_);
            transcriptUtf8 = assembler_.FinalTranscriptUtf8();
        }

        if (!stopped) {
            result.errorTitle = L"VoxInsert recording error";
            result.failureReason = failureReason;
            return result;
        }

        // 4. Persist the WAV from retained PCM.
        std::filesystem::path wavPath;
        const auto writeWavStartedAt = StreamingClock::now();
        if (request_.wavWriter != nullptr &&
            request_.wavWriter->WritePcm16Mono(samples, request_.config->audio.sampleRate, wavPath, failureReason)) {
            result.hasRecordingPath = true;
            result.recordingPath = wavPath;
            if (request_.logger != nullptr) {
                request_.logger->info("recording written to {}", Utf8FromWide(wavPath.wstring()));
            }
        }
        else if (request_.logger != nullptr) {
            request_.logger->warn("streaming path could not write the WAV file: {}", Utf8FromWide(failureReason));
        }
        const int64_t writeWavMs = ElapsedMilliseconds(writeWavStartedAt, StreamingClock::now());

        if (request_.smokeTest) {
            if (request_.logger != nullptr) {
                request_.logger->info("smoke-test: skipping streaming insertion");
            }
            result.success = true;
            result.showDone = false;
            return result;
        }

        // 5. Fall back to the one-shot file transcription if streaming did not
        //    produce a usable transcript.
        const bool transcriptUsable = !streamingFailed && !transcriptUtf8.empty();
        if (!transcriptUsable) {
            if (streaming.fallbackToFileTranscription && result.hasRecordingPath && request_.fallbackClient != nullptr) {
                if (request_.logger != nullptr) {
                    request_.logger->warn(
                        "streaming transcript unavailable ({}); falling back to file transcription",
                        streamingFailureReason.empty() ? "empty result" : Utf8FromWide(streamingFailureReason));
                }
                std::string fallbackTranscript;
                std::wstring fallbackFailure;
                if (request_.fallbackClient->Transcribe(
                        request_.config->transcription, wavPath, fallbackTranscript, fallbackFailure)) {
                    transcriptUtf8 = std::move(fallbackTranscript);
                    result.fallbackUsed = true;
                }
                else {
                    result.errorTitle = L"VoxInsert transcription error";
                    result.failureReason = fallbackFailure;
                    return result;
                }
            }
            else {
                result.errorTitle = L"VoxInsert transcription error";
                result.failureReason = streamingFailureReason.empty()
                    ? L"The streaming transcription returned no text."
                    : streamingFailureReason;
                return result;
            }
        }

        result.hasTranscript = true;
        result.transcript = WideFromUtf8(transcriptUtf8);

        const auto enqueueArchive = [this, &samples, &transcriptUtf8](bool insertionSucceeded) {
            if (request_.archiveService == nullptr || !request_.config->archive.enabled) {
                return;
            }
            request_.archiveService->Enqueue(ArchiveRequest{
                .archive = request_.config->archive,
                .audio = request_.config->audio,
                .transcription = request_.config->transcription,
                .samples = samples,
                .transcriptUtf8 = transcriptUtf8,
                .insertionSucceeded = insertionSucceeded,
                .logger = request_.logger,
            });
        };

        if (request_.onPhaseChanged) {
            request_.onPhaseChanged(PostRecordingPhase::Inserting);
        }

        // 6. Insert the final transcript.
        const auto insertionStartedAt = StreamingClock::now();
        if (request_.textInjector != nullptr &&
            !request_.textInjector->InsertText(
                request_.ownerWindow, request_.config->insertion, result.transcript, failureReason)) {
            enqueueArchive(false);
            result.errorTitle = L"VoxInsert insertion error";
            result.failureReason = failureReason;
            return result;
        }
        const int64_t insertionMs = ElapsedMilliseconds(insertionStartedAt, StreamingClock::now());

        enqueueArchive(true);

        if (request_.logger != nullptr) {
            request_.logger->debug(
                "post-recording latency outcome={} stop_recording={}ms wav_write={}ms transcription={}ms insert={}ms total={}ms transcript_utf8_bytes={}",
                result.fallbackUsed ? "streaming_fallback" : "streaming_success",
                stopRecordingMs,
                writeWavMs,
                transcriptionMs,
                insertionMs,
                ElapsedMilliseconds(workflowStartedAt, StreamingClock::now()),
                transcriptUtf8.size());
            request_.logger->info("streaming transcription inserted into the focused text field via clipboard paste");
        }

        result.success = true;
        result.showDone = true;
        return result;
    }
    catch (const std::exception& exception) {
        result.errorTitle = L"VoxInsert transcription error";
        result.failureReason = L"Unexpected streaming transcription failure: " + WideFromUtf8(exception.what());
        return result;
    }
    catch (...) {
        result.errorTitle = L"VoxInsert transcription error";
        result.failureReason = L"Unexpected streaming transcription failure.";
        return result;
    }
}

void StreamingRecordingController::Cancel() noexcept {
    if (request_.audioRecorder != nullptr) {
        request_.audioRecorder->Cancel();
    }

    if (session_ != nullptr) {
        session_->CancelUtterance(utteranceId_);
        session_->Close();
    }

    StopWorkers();

    session_.reset();
    backend_.reset();
    active_.store(false);
}

} // namespace voxinsert
