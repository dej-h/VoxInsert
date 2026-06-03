#include "audio/audio_recorder.h"

#include "observability/logging.h"

#include <portaudio.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace voxinsert {
namespace {

using AudioClock = std::chrono::steady_clock;

constexpr wchar_t kMicrophonePermissionHelp[] =
    L"If this is a desktop microphone-permission problem, open Settings > Privacy & security > Microphone and turn on both 'Microphone access' and 'Let desktop apps access your microphone'.";
constexpr unsigned long kMaxInteractiveFramesPerBuffer = 256;

int64_t AudioClockTicks() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(AudioClock::now().time_since_epoch()).count();
}

int64_t ElapsedMilliseconds(AudioClock::time_point startedAt, AudioClock::time_point finishedAt) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(finishedAt - startedAt).count();
}

int64_t ElapsedMillisecondsSinceTick(int64_t startedAtTicks) {
    if (startedAtTicks <= 0) {
        return -1;
    }

    return AudioClockTicks() - startedAtTicks;
}

std::wstring WideFromPortAudioError(std::string_view errorText) {
    return WideFromUtf8(errorText);
}

float ComputeRmsAmplitude(const std::vector<int16_t>& samples) {
    if (samples.empty()) {
        return 0.0f;
    }

    double sumOfSquares = 0.0;
    for (const int16_t sample : samples) {
        const double normalizedSample = static_cast<double>(sample) / 32768.0;
        sumOfSquares += normalizedSample * normalizedSample;
    }

    return static_cast<float>(std::sqrt(sumOfSquares / static_cast<double>(samples.size())));
}

} // namespace

AudioRecorder::~AudioRecorder() {
    Cancel();

    if (portAudioInitialized_) {
        Pa_Terminate();
        portAudioInitialized_ = false;
    }
}

bool AudioRecorder::EnsureInitialized(std::wstring& failureReason) {
    if (portAudioInitialized_) {
        return true;
    }

    const PaError error = Pa_Initialize();
    if (error != paNoError) {
        failureReason = L"Pa_Initialize failed: ";
        failureReason += WideFromPortAudioError(Pa_GetErrorText(error));
        return false;
    }

    portAudioInitialized_ = true;
    return true;
}

std::wstring AudioRecorder::BuildMicrophoneError(std::wstring_view action, std::string_view portAudioErrorText) const {
    std::wstring message(action);
    message += L": ";
    message += WideFromPortAudioError(portAudioErrorText);
    message += L" ";
    message += kMicrophonePermissionHelp;
    return message;
}

bool AudioRecorder::Start(const AudioConfig& config, AmplitudeCallback amplitudeCallback, std::wstring& failureReason) {
    return Start(config, std::move(amplitudeCallback), FrameSink{}, failureReason);
}

bool AudioRecorder::Start(const AudioConfig& config, AmplitudeCallback amplitudeCallback, FrameSink frameSink, std::wstring& failureReason) {
    if (recordingRequested_) {
        failureReason = L"AudioRecorder::Start called while a recording is already in progress.";
        return false;
    }

    if (config.sampleRate <= 0 || config.channelCount <= 0 || config.framesPerBuffer == 0) {
        failureReason = L"AudioRecorder received invalid audio settings.";
        return false;
    }

    if (!EnsureInitialized(failureReason)) {
        return false;
    }

    {
        const std::scoped_lock lock(mutex_);
        samples_.clear();
        workerFailureReason_.clear();
        activeDeviceName_.clear();
        startupCompleted_ = false;
        startupSucceeded_ = false;
    }
    stopRequestedAtTicks_.store(0, std::memory_order_release);

    recordingRequested_ = true;
    worker_ = std::thread(&AudioRecorder::RecordingThreadMain, this, config, std::move(amplitudeCallback), std::move(frameSink));

    std::unique_lock lock(mutex_);
    startupCondition_.wait(lock, [this]() { return startupCompleted_; });
    if (!startupSucceeded_) {
        failureReason = workerFailureReason_;
        lock.unlock();
        JoinWorker();
        return false;
    }

    return true;
}

bool AudioRecorder::Stop(std::vector<int16_t>& samples, std::wstring& failureReason) {
    const auto stopStartedAt = AudioClock::now();
    if (!recordingRequested_ && !worker_.joinable()) {
        failureReason = L"AudioRecorder::Stop called while no recording is in progress.";
        return false;
    }

    const bool workerWasJoinable = worker_.joinable();
    stopRequestedAtTicks_.store(AudioClockTicks(), std::memory_order_release);
    recordingRequested_.store(false, std::memory_order_release);
    const auto stopSignalSentAt = AudioClock::now();
    JoinWorker();
    const auto workerJoinedAt = AudioClock::now();

    const auto lockStartedAt = AudioClock::now();
    std::unique_lock lock(mutex_);
    const auto lockAcquiredAt = AudioClock::now();
    if (!workerFailureReason_.empty()) {
        failureReason = workerFailureReason_;
        samples_.clear();
        if (auto* logger = spdlog::default_logger_raw()) {
            logger->debug(
                "audio recorder stop latency outcome=worker_failure stop_signal={}ms join_wait={}ms lock_wait={}ms total={}ms worker_joinable={}",
                ElapsedMilliseconds(stopStartedAt, stopSignalSentAt),
                ElapsedMilliseconds(stopSignalSentAt, workerJoinedAt),
                ElapsedMilliseconds(lockStartedAt, lockAcquiredAt),
                ElapsedMilliseconds(stopStartedAt, AudioClock::now()),
                workerWasJoinable);
        }
        return false;
    }

    const auto sampleMoveStartedAt = AudioClock::now();
    samples = std::move(samples_);
    const size_t returnedSampleCount = samples.size();
    samples_.clear();
    const auto finishedAt = AudioClock::now();
    if (auto* logger = spdlog::default_logger_raw()) {
        logger->debug(
            "audio recorder stop latency outcome=success stop_signal={}ms join_wait={}ms lock_wait={}ms sample_move={}ms total={}ms worker_joinable={} returned_samples={}",
            ElapsedMilliseconds(stopStartedAt, stopSignalSentAt),
            ElapsedMilliseconds(stopSignalSentAt, workerJoinedAt),
            ElapsedMilliseconds(lockStartedAt, lockAcquiredAt),
            ElapsedMilliseconds(sampleMoveStartedAt, finishedAt),
            ElapsedMilliseconds(stopStartedAt, finishedAt),
            workerWasJoinable,
            returnedSampleCount);
    }
    return true;
}

void AudioRecorder::Cancel() noexcept {
    stopRequestedAtTicks_.store(AudioClockTicks(), std::memory_order_release);
    recordingRequested_.store(false, std::memory_order_release);
    JoinWorker();

    const std::scoped_lock lock(mutex_);
    samples_.clear();
    workerFailureReason_.clear();
    activeDeviceName_.clear();
}

bool AudioRecorder::IsRecording() const noexcept {
    return recordingRequested_;
}

std::wstring AudioRecorder::ActiveDeviceName() const {
    const std::scoped_lock lock(mutex_);
    return activeDeviceName_;
}

void AudioRecorder::JoinWorker() noexcept {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void AudioRecorder::RecordingThreadMain(AudioConfig config, AmplitudeCallback amplitudeCallback, FrameSink frameSink) noexcept {
    PaStream* stream = nullptr;
    std::vector<int16_t> capturedSamples;

    const PaDeviceIndex deviceIndex = Pa_GetDefaultInputDevice();
    if (deviceIndex == paNoDevice) {
        const std::scoped_lock lock(mutex_);
        workerFailureReason_ = BuildMicrophoneError(L"No default microphone input device is available", "No default input device");
        startupCompleted_ = true;
        startupSucceeded_ = false;
        recordingRequested_ = false;
        startupCondition_.notify_one();
        return;
    }

    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(deviceIndex);
    if (deviceInfo == nullptr) {
        const std::scoped_lock lock(mutex_);
        workerFailureReason_ = BuildMicrophoneError(L"Pa_GetDeviceInfo failed for the default microphone", "Could not query device info");
        startupCompleted_ = true;
        startupSucceeded_ = false;
        recordingRequested_ = false;
        startupCondition_.notify_one();
        return;
    }

    {
        const std::scoped_lock lock(mutex_);
        activeDeviceName_ = WideFromUtf8(deviceInfo->name != nullptr ? deviceInfo->name : "Unknown device");
    }

    PaStreamParameters inputParameters{};
    inputParameters.device = deviceIndex;
    inputParameters.channelCount = config.channelCount;
    inputParameters.sampleFormat = paInt16;
    inputParameters.suggestedLatency = deviceInfo->defaultLowInputLatency;
    inputParameters.hostApiSpecificStreamInfo = nullptr;

    const unsigned long framesPerRead = std::min(config.framesPerBuffer, kMaxInteractiveFramesPerBuffer);

    const PaError openError = Pa_OpenStream(
        &stream,
        &inputParameters,
        nullptr,
        static_cast<double>(config.sampleRate),
        framesPerRead,
        paClipOff,
        nullptr,
        nullptr);
    if (openError != paNoError) {
        const std::scoped_lock lock(mutex_);
        workerFailureReason_ = BuildMicrophoneError(L"Pa_OpenStream failed for the default microphone", Pa_GetErrorText(openError));
        startupCompleted_ = true;
        startupSucceeded_ = false;
        recordingRequested_ = false;
        startupCondition_.notify_one();
        return;
    }

    const PaError startError = Pa_StartStream(stream);
    if (startError != paNoError) {
        Pa_CloseStream(stream);
        const std::scoped_lock lock(mutex_);
        workerFailureReason_ = BuildMicrophoneError(L"Pa_StartStream failed for the default microphone", Pa_GetErrorText(startError));
        startupCompleted_ = true;
        startupSucceeded_ = false;
        recordingRequested_ = false;
        startupCondition_.notify_one();
        return;
    }

    {
        const std::scoped_lock lock(mutex_);
        startupCompleted_ = true;
        startupSucceeded_ = true;
    }
    startupCondition_.notify_one();

    const uint64_t maxFrames = static_cast<uint64_t>(config.sampleRate) * static_cast<uint64_t>(std::max(config.maxRecordingSeconds, 1));
    std::vector<int16_t> readBuffer(static_cast<size_t>(framesPerRead) * static_cast<size_t>(config.channelCount));

    uint64_t readCount = 0;
    uint64_t overflowCount = 0;
    int64_t finalReadMs = 0;
    int64_t maxReadMs = 0;
    bool stoppedAfterRead = false;

    while (recordingRequested_) {
        const auto readStartedAt = AudioClock::now();
        const PaError readError = Pa_ReadStream(stream, readBuffer.data(), framesPerRead);
        const auto readFinishedAt = AudioClock::now();
        finalReadMs = ElapsedMilliseconds(readStartedAt, readFinishedAt);
        maxReadMs = std::max(maxReadMs, finalReadMs);
        ++readCount;
        stoppedAfterRead = !recordingRequested_.load(std::memory_order_acquire);
        if (readError == paInputOverflowed) {
            ++overflowCount;
            continue;
        }

        if (readError != paNoError) {
            const std::scoped_lock lock(mutex_);
            workerFailureReason_ = L"Pa_ReadStream failed while recording: ";
            workerFailureReason_ += WideFromPortAudioError(Pa_GetErrorText(readError));
            break;
        }

        if (amplitudeCallback) {
            amplitudeCallback(ComputeRmsAmplitude(readBuffer));
        }

        capturedSamples.insert(capturedSamples.end(), readBuffer.begin(), readBuffer.end());

        if (frameSink) {
            frameSink(readBuffer.data(), readBuffer.size());
        }

        const uint64_t capturedFrames = static_cast<uint64_t>(capturedSamples.size()) / static_cast<uint64_t>(config.channelCount);
        if (capturedFrames >= maxFrames) {
            recordingRequested_ = false;
            const std::scoped_lock lock(mutex_);
            workerFailureReason_ = L"Maximum recording length reached before stop was requested.";
            break;
        }
    }

    const int64_t stopObservedAfterRequestMs =
        ElapsedMillisecondsSinceTick(stopRequestedAtTicks_.load(std::memory_order_acquire));
    const auto paStopStartedAt = AudioClock::now();
    const PaError stopError = Pa_StopStream(stream);
    const auto paStopFinishedAt = AudioClock::now();
    const auto paCloseStartedAt = AudioClock::now();
    const PaError closeError = Pa_CloseStream(stream);
    const auto paCloseFinishedAt = AudioClock::now();
    recordingRequested_.store(false, std::memory_order_release);

    const auto sampleHandoffStartedAt = AudioClock::now();
    const std::scoped_lock lock(mutex_);
    const size_t capturedSampleCount = capturedSamples.size();
    if (workerFailureReason_.empty()) {
        samples_ = std::move(capturedSamples);
    }
    const auto sampleHandoffFinishedAt = AudioClock::now();

    if (auto* logger = spdlog::default_logger_raw()) {
        logger->debug(
            "audio recorder worker stop latency stop_observed_after_request={}ms final_read={}ms max_read={}ms read_count={} overflow_count={} stopped_after_read={} pa_stop={}ms pa_stop_error={} pa_close={}ms pa_close_error={} sample_handoff={}ms captured_samples={}",
            stopObservedAfterRequestMs,
            finalReadMs,
            maxReadMs,
            readCount,
            overflowCount,
            stoppedAfterRead,
            ElapsedMilliseconds(paStopStartedAt, paStopFinishedAt),
            static_cast<int>(stopError),
            ElapsedMilliseconds(paCloseStartedAt, paCloseFinishedAt),
            static_cast<int>(closeError),
            ElapsedMilliseconds(sampleHandoffStartedAt, sampleHandoffFinishedAt),
            capturedSampleCount);
    }
}

} // namespace voxinsert
