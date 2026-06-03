#include "runtime/streaming_recording_controller.h"

#include "archive/archive_service.h"
#include "observability/logging.h"
#include "transcription/mistral_realtime_streaming_service.h"
#include "transcription/openai_realtime_streaming_service.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iterator>
#include <utility>

namespace voxinsert {
namespace {

using StreamingClock = std::chrono::steady_clock;

constexpr size_t kAudioQueueCapacity = 512;
constexpr size_t kEventQueueCapacity = 512;
constexpr int kMinimumAppendBatchMs = 10;
constexpr unsigned long kMaxInteractiveFramesPerBuffer = 256;

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

bool IsSupportedStreamingProvider(const std::string& provider) {
    return provider == "openai_realtime" || provider == "mistral_realtime";
}

int EffectiveAppendBatchMs(int configuredAppendBatchMs, const StreamingBackendCapabilities& capabilities) {
    const int configured = std::max(configuredAppendBatchMs, kMinimumAppendBatchMs);
    const int providerPreferred = std::max(capabilities.preferredAppendBatchMs, kMinimumAppendBatchMs);
    return std::clamp(configured, kMinimumAppendBatchMs, providerPreferred);
}

size_t CaptureSlotSampleCapacity(const AudioConfig& audio) {
    const unsigned long framesPerRead = std::min(audio.framesPerBuffer, kMaxInteractiveFramesPerBuffer);
    const int channelCount = std::max(audio.channelCount, 1);
    return static_cast<size_t>(framesPerRead) * static_cast<size_t>(channelCount);
}

} // namespace

StreamingRecordingController::StreamingRecordingController()
    : freeAudioSlots_(kAudioQueueCapacity),
      readyAudioSlots_(kAudioQueueCapacity),
      eventQueue_(kEventQueueCapacity) {}

StreamingRecordingController::~StreamingRecordingController() {
    Cancel();
}

bool StreamingRecordingController::Start(const StreamingRecordingRequest& request, std::wstring& failureReason) {
    const auto startupStartedAt = StreamingClock::now();

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
    appendBatchMs_ = std::max(streaming.appendBatchMs, kMinimumAppendBatchMs);

    if (!IsSupportedStreamingProvider(streaming.provider)) {
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
    audioForwardingEnabled_.store(false);
    audioRingClosed_.store(true, std::memory_order_release);
    backpressureObserved_.store(false);
    sessionSendFailed_.store(false);
    nextSequence_.store(0);
    ResetAudioSlots(CaptureSlotSampleCapacity(request_.config->audio));

    // The event queue is reused across recordings; a previous Finish()/Cancel()
    // left them closed. Re-open and drain them so this session's audio and
    // backend events flow again. Without this, only the first recording after
    // launch works and every later one stalls (no audio sent, finalize event
    // dropped) until the finalize timeout falls back to file transcription.
    eventQueue_.Reset();

    audioForwardingEnabled_.store(true);
    const auto captureStartStartedAt = StreamingClock::now();
    const bool recorderStarted = request_.audioRecorder->Start(
        request_.config->audio,
        request_.amplitudeCallback,
        [this](const int16_t* data, size_t sampleCount) {
            if (!audioForwardingEnabled_.load()) {
                return;
            }

            if (data == nullptr || sampleCount == 0) {
                return;
            }

            size_t slotIndex = 0;
            if (!freeAudioSlots_.TryPop(slotIndex)) {
                MarkStreamingTranscriptUntrusted("streaming audio slot ring full; dropping live audio");
                return;
            }

            if (slotIndex >= audioSlots_.size()) {
                MarkStreamingTranscriptUntrusted("streaming audio slot ring produced an invalid slot index");
                return;
            }

            CapturedAudioSlot& slot = audioSlots_[slotIndex];
            if (sampleCount > slot.pcm16.size()) {
                const bool returned = freeAudioSlots_.TryPush(slotIndex);
                (void)returned;
                MarkStreamingTranscriptUntrusted("streaming audio callback exceeded the preallocated slot size");
                return;
            }

            slot.sequence = nextSequence_.fetch_add(1, std::memory_order_relaxed);
            slot.sampleCount = sampleCount;
            std::copy_n(data, sampleCount, slot.pcm16.data());

            if (!readyAudioSlots_.TryPush(slotIndex)) {
                const bool returned = freeAudioSlots_.TryPush(slotIndex);
                (void)returned;
                MarkStreamingTranscriptUntrusted("streaming ready audio slot ring full; dropping live audio");
                return;
            }

            readyAudioSignal_.release();
        },
        failureReason);
    const auto captureStartedAt = StreamingClock::now();
    const int64_t captureStartMs = ElapsedMilliseconds(captureStartStartedAt, captureStartedAt);

    if (!recorderStarted) {
        audioForwardingEnabled_.store(false);
        audioRingClosed_.store(true, std::memory_order_release);
        readyAudioSignal_.release();
        backend_.reset();
        return false;
    }

    active_.store(true);

    const auto recordBackendStartupFailure = [this](std::wstring reason) {
        audioForwardingEnabled_.store(false);
        audioRingClosed_.store(true, std::memory_order_release);
        readyAudioSignal_.release();
        sessionSendFailed_.store(true);
        {
            std::scoped_lock lock(finalizeMutex_);
            failed_ = true;
            backendFailureReason_ = std::move(reason);
        }
    };

    const auto backendSetupStartedAt = StreamingClock::now();
    try {
        backend_ = CreateBackend(streaming.provider);
        if (backend_ == nullptr) {
            failureReason = L"Unsupported streaming transcription provider '";
            failureReason += WideFromUtf8(streaming.provider);
            failureReason += L"'.";
            recordBackendStartupFailure(failureReason);
            return true;
        }

        const StreamingBackendCapabilities capabilities = backend_->Capabilities(request_.config->transcription);
        appendBatchMs_ = EffectiveAppendBatchMs(streaming.appendBatchMs, capabilities);
        if (request_.logger != nullptr) {
            request_.logger->debug(
                "streaming append batching configured_append_batch_ms={} provider_preferred_append_batch_ms={} effective_append_batch_ms={} provider={}",
                streaming.appendBatchMs,
                capabilities.preferredAppendBatchMs,
                appendBatchMs_,
                streaming.provider);
        }

        session_ = backend_->CreateSession(
            request_.config->transcription,
            sessionId_,
            [this](BackendTranscriptEvent event) { OnBackendEvent(std::move(event)); },
            request_.logger,
            failureReason);
        if (session_ == nullptr) {
            backend_.reset();
            recordBackendStartupFailure(failureReason);
            if (request_.logger != nullptr) {
                request_.logger->warn(
                    "streaming backend session could not be created after capture started ({}); recording will continue and use fallback transcription on stop",
                    Utf8FromWide(failureReason));
                request_.logger->debug(
                    "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
                    captureStartMs,
                    ElapsedMilliseconds(backendSetupStartedAt, StreamingClock::now()),
                    ElapsedMilliseconds(startupStartedAt, StreamingClock::now()),
                    streaming.provider,
                    ReadyAudioSlotCount());
            }
            return true;
        }

        if (!session_->Start(failureReason)) {
            session_->Close();
            session_.reset();
            backend_.reset();
            recordBackendStartupFailure(failureReason);
            if (request_.logger != nullptr) {
                request_.logger->warn(
                    "streaming backend session could not start after capture started ({}); recording will continue and use fallback transcription on stop",
                    Utf8FromWide(failureReason));
                request_.logger->debug(
                    "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
                    captureStartMs,
                    ElapsedMilliseconds(backendSetupStartedAt, StreamingClock::now()),
                    ElapsedMilliseconds(startupStartedAt, StreamingClock::now()),
                    streaming.provider,
                    ReadyAudioSlotCount());
            }
            return true;
        }
    }
    catch (const std::exception& exception) {
        failureReason = L"Unexpected streaming backend startup failure after capture started: " + WideFromUtf8(exception.what());
        if (session_ != nullptr) {
            session_->Close();
            session_.reset();
        }
        backend_.reset();
        recordBackendStartupFailure(failureReason);
        if (request_.logger != nullptr) {
            request_.logger->warn(
                "streaming backend startup threw after capture started ({}); recording will continue and use fallback transcription on stop",
                Utf8FromWide(failureReason));
            request_.logger->debug(
                "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
                captureStartMs,
                ElapsedMilliseconds(backendSetupStartedAt, StreamingClock::now()),
                ElapsedMilliseconds(startupStartedAt, StreamingClock::now()),
                streaming.provider,
                ReadyAudioSlotCount());
        }
        return true;
    }
    catch (...) {
        failureReason = L"Unexpected streaming backend startup failure after capture started.";
        if (session_ != nullptr) {
            session_->Close();
            session_.reset();
        }
        backend_.reset();
        recordBackendStartupFailure(failureReason);
        if (request_.logger != nullptr) {
            request_.logger->warn(
                "streaming backend startup threw after capture started; recording will continue and use fallback transcription on stop");
            request_.logger->debug(
                "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
                captureStartMs,
                ElapsedMilliseconds(backendSetupStartedAt, StreamingClock::now()),
                ElapsedMilliseconds(startupStartedAt, StreamingClock::now()),
                streaming.provider,
                ReadyAudioSlotCount());
        }
        return true;
    }

    const auto backendReadyAt = StreamingClock::now();
    const int64_t backendSetupAfterCaptureMs = ElapsedMilliseconds(backendSetupStartedAt, backendReadyAt);
    const size_t queuedAudioChunksBeforePumps = ReadyAudioSlotCount();

    try {
        eventPump_ = std::jthread([this](std::stop_token stopToken) { EventPumpMain(std::move(stopToken)); });
        audioPump_ = std::jthread([this](std::stop_token stopToken) { AudioPumpMain(std::move(stopToken)); });
    }
    catch (const std::exception& exception) {
        failureReason = L"Could not start streaming worker threads after capture started: " + WideFromUtf8(exception.what());
        if (session_ != nullptr) {
            session_->Close();
            session_.reset();
        }
        backend_.reset();
        StopWorkers();
        recordBackendStartupFailure(failureReason);
        if (request_.logger != nullptr) {
            request_.logger->warn(
                "streaming worker startup failed after capture started ({}); recording will continue and use fallback transcription on stop",
                Utf8FromWide(failureReason));
            request_.logger->debug(
                "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
                captureStartMs,
                backendSetupAfterCaptureMs,
                ElapsedMilliseconds(startupStartedAt, StreamingClock::now()),
                streaming.provider,
                queuedAudioChunksBeforePumps);
        }
        return true;
    }
    catch (...) {
        failureReason = L"Could not start streaming worker threads after capture started.";
        if (session_ != nullptr) {
            session_->Close();
            session_.reset();
        }
        backend_.reset();
        StopWorkers();
        recordBackendStartupFailure(failureReason);
        if (request_.logger != nullptr) {
            request_.logger->warn(
                "streaming worker startup failed after capture started; recording will continue and use fallback transcription on stop");
            request_.logger->debug(
                "streaming startup latency order=capture_first backend_ready=false capture_start={}ms backend_setup_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
                captureStartMs,
                backendSetupAfterCaptureMs,
                ElapsedMilliseconds(startupStartedAt, StreamingClock::now()),
                streaming.provider,
                queuedAudioChunksBeforePumps);
        }
        return true;
    }
    const auto pumpsStartedAt = StreamingClock::now();

    if (request_.logger != nullptr) {
        request_.logger->info("streaming transcription session started (provider={})", streaming.provider);
        request_.logger->debug(
            "streaming startup latency order=capture_first backend_ready=true capture_start={}ms backend_setup_after_capture={}ms pumps_start_after_capture={}ms total={}ms provider={} queued_audio_chunks={}",
            captureStartMs,
            backendSetupAfterCaptureMs,
            ElapsedMilliseconds(captureStartedAt, pumpsStartedAt),
            ElapsedMilliseconds(startupStartedAt, pumpsStartedAt),
            streaming.provider,
            queuedAudioChunksBeforePumps);
    }
    return true;
}

void StreamingRecordingController::FlushAppendBatch(
    std::vector<int16_t>& batch,
    std::uint64_t firstSequence,
    std::uint64_t lastSequence) noexcept {
    if (batch.empty() || session_ == nullptr) {
        batch.clear();
        return;
    }

    StreamingBackendCommand command;
    command.kind = StreamingBackendCommandKind::AppendAudio;
    command.sessionId = sessionId_;
    command.utteranceId = utteranceId_;
    command.firstSequence = firstSequence;
    command.lastSequence = lastSequence;
    command.format = captureFormat_;
    command.pcm16 = std::span<const int16_t>(batch.data(), batch.size());

    std::wstring failureReason;
    if (!session_->AppendAudio(command, failureReason)) {
        // The socket is dead. Mark the session so Finish skips the finalize
        // wait, and log only the first failure to avoid flooding the log.
        if (!sessionSendFailed_.exchange(true) && request_.logger != nullptr) {
            request_.logger->warn("streaming append failed: {}", Utf8FromWide(failureReason));
        }
        MarkStreamingTranscriptUntrusted("streaming append failed; live transcript is untrusted");
    }
    batch.clear();
}

void StreamingRecordingController::AudioPumpMain(std::stop_token stopToken) noexcept {
    const int sampleRate = captureFormat_.sampleRate > 0 ? captureFormat_.sampleRate : 16000;
    const size_t batchThreshold =
        static_cast<size_t>(static_cast<int64_t>(sampleRate) * std::max(appendBatchMs_, kMinimumAppendBatchMs) / 1000);

    std::vector<int16_t> batch;
    batch.reserve(batchThreshold + audioSlotSampleCapacity_);
    bool batchHasSequence = false;
    std::uint64_t firstBatchSequence = 0;
    std::uint64_t lastBatchSequence = 0;

    for (;;) {
        bool drainedAny = false;
        size_t slotIndex = 0;
        while (readyAudioSlots_.TryPop(slotIndex)) {
            drainedAny = true;
            if (slotIndex >= audioSlots_.size()) {
                MarkStreamingTranscriptUntrusted("streaming audio pump received an invalid slot index");
                continue;
            }

            CapturedAudioSlot& slot = audioSlots_[slotIndex];
            if (!batchHasSequence) {
                firstBatchSequence = slot.sequence;
                batchHasSequence = true;
            }
            lastBatchSequence = slot.sequence;
            batch.insert(batch.end(), slot.pcm16.begin(), std::next(slot.pcm16.begin(), static_cast<std::ptrdiff_t>(slot.sampleCount)));
            slot.sampleCount = 0;

            if (!freeAudioSlots_.TryPush(slotIndex)) {
                MarkStreamingTranscriptUntrusted("streaming free audio slot ring full");
            }

            if (batch.size() >= batchThreshold) {
                FlushAppendBatch(batch, firstBatchSequence, lastBatchSequence);
                batchHasSequence = false;
            }
        }

        if (audioRingClosed_.load(std::memory_order_acquire) || stopToken.stop_requested()) {
            break;
        }

        if (!drainedAny) {
            const bool acquired = readyAudioSignal_.try_acquire_for(std::chrono::milliseconds(5));
            (void)acquired;
        }
    }

    FlushAppendBatch(batch, firstBatchSequence, lastBatchSequence);
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
        else if (event.kind == BackendTranscriptEventKind::SessionClosed) {
            std::wstring closeReason;
            bool notifyClosedFailure = false;
            {
                std::scoped_lock lock(finalizeMutex_);
                if (!finalized_ && !failed_) {
                    failed_ = true;
                    backendFailureReason_ = event.failureReason.empty()
                        ? L"Streaming transcription session closed before a final transcript arrived."
                        : event.failureReason;
                    closeReason = backendFailureReason_;
                    notifyClosedFailure = true;
                }
            }
            if (notifyClosedFailure) {
                if (request_.logger != nullptr) {
                    request_.logger->warn("streaming session closed before finalize: {}", Utf8FromWide(closeReason));
                }
                finalizeCondition_.notify_all();
            }
        }
    }
}

void StreamingRecordingController::OnBackendEvent(BackendTranscriptEvent event) {
    if (!eventQueue_.TryPush(std::move(event))) {
        MarkStreamingTranscriptUntrusted("streaming event queue full; dropping backend events");
    }
}

void StreamingRecordingController::StopWorkers() noexcept {
    audioForwardingEnabled_.store(false);
    audioRingClosed_.store(true, std::memory_order_release);
    readyAudioSignal_.release();
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
        audioForwardingEnabled_.store(false);
        const auto stopRecordingStartedAt = StreamingClock::now();
        const bool stopped = request_.audioRecorder->Stop(samples, failureReason);
        const int64_t stopRecordingMs = ElapsedMilliseconds(stopRecordingStartedAt, StreamingClock::now());

        // 2. Drain queued audio to the backend, then commit the utterance.
        audioRingClosed_.store(true, std::memory_order_release);
        readyAudioSignal_.release();
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
            if (!commitSucceeded) {
                sessionSendFailed_.store(true, std::memory_order_release);
                {
                    std::scoped_lock lock(finalizeMutex_);
                    if (backendFailureReason_.empty()) {
                        backendFailureReason_ = commitFailure.empty()
                            ? L"Streaming commit failed before a trusted final transcript arrived."
                            : commitFailure;
                    }
                }
                if (request_.logger != nullptr) {
                    request_.logger->warn("streaming commit failed: {}", Utf8FromWide(commitFailure));
                }
            }
        }

        // 3. Wait for the final transcript only while the backend session is
        // still healthy. A dead socket (failed appends or commit) will never
        // deliver a final transcript, so skip the timeout and fall back
        // immediately instead of stalling for finalizeTimeoutMs.
        const bool sessionAlive = commitSucceeded && !sessionSendFailed_.load();
        const int finalizeWaitMs = std::max(streaming.finalizeTimeoutMs, 250);
        bool finalizeCompleted = true;
        if (sessionAlive) {
            std::unique_lock<std::mutex> lock(finalizeMutex_);
            finalizeCompleted = finalizeCondition_.wait_for(
                lock,
                std::chrono::milliseconds(finalizeWaitMs),
                [this]() { return finalized_ || failed_; });
            if (!finalizeCompleted && backendFailureReason_.empty()) {
                backendFailureReason_ = L"Streaming finalize timed out waiting for a backend final event.";
            }
        }
        if (sessionAlive && !finalizeCompleted && request_.logger != nullptr) {
            request_.logger->warn(
                "streaming finalize timed out after {}ms waiting for backend final event (provider={})",
                finalizeWaitMs,
                streaming.provider);
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
            streamingFailed = failed_ || !finalized_ || backpressureObserved_.load() || sessionSendFailed_.load();
            streamingFailureReason = backendFailureReason_;
        }
        if (backpressureObserved_.load() && streamingFailureReason.empty()) {
            streamingFailureReason = L"Streaming audio or backend-event backpressure made the live transcript untrusted.";
        }
        if (sessionSendFailed_.load() && streamingFailureReason.empty()) {
            streamingFailureReason = L"Streaming backend send failed before a trusted final transcript arrived.";
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
    audioForwardingEnabled_.store(false);

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

void StreamingRecordingController::ResetAudioSlots(size_t sampleCapacity) {
    sampleCapacity = std::max(sampleCapacity, static_cast<size_t>(1));
    while (readyAudioSignal_.try_acquire()) {
    }

    audioSlotSampleCapacity_ = sampleCapacity;
    audioSlots_.resize(kAudioQueueCapacity);
    freeAudioSlots_.Reset();
    readyAudioSlots_.Reset();

    for (size_t index = 0; index < audioSlots_.size(); ++index) {
        CapturedAudioSlot& slot = audioSlots_[index];
        slot.sequence = 0;
        slot.sampleCount = 0;
        slot.pcm16.resize(audioSlotSampleCapacity_);
        const bool pushed = freeAudioSlots_.TryPush(index);
        (void)pushed;
    }

    audioRingClosed_.store(false, std::memory_order_release);
}

void StreamingRecordingController::MarkStreamingTranscriptUntrusted(const char* reason) noexcept {
    audioForwardingEnabled_.store(false, std::memory_order_release);
    sessionSendFailed_.store(true, std::memory_order_release);
    if (!backpressureObserved_.exchange(true, std::memory_order_acq_rel) && request_.logger != nullptr) {
        request_.logger->warn("{}; retained PCM fallback will be used", reason);
    }
}

size_t StreamingRecordingController::ReadyAudioSlotCount() const noexcept {
    return readyAudioSlots_.Size();
}

} // namespace voxinsert
