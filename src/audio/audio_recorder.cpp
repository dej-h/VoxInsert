#include "audio/audio_recorder.h"

#include "observability/logging.h"

#include <portaudio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace voxinsert {
namespace {

constexpr wchar_t kMicrophonePermissionHelp[] =
    L"If this is a desktop microphone-permission problem, open Settings > Privacy & security > Microphone and turn on both 'Microphone access' and 'Let desktop apps access your microphone'.";
constexpr unsigned long kMaxInteractiveFramesPerBuffer = 256;

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

    recordingRequested_ = true;
    worker_ = std::thread(&AudioRecorder::RecordingThreadMain, this, config, std::move(amplitudeCallback));

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
    if (!recordingRequested_ && !worker_.joinable()) {
        failureReason = L"AudioRecorder::Stop called while no recording is in progress.";
        return false;
    }

    recordingRequested_ = false;
    JoinWorker();

    std::scoped_lock lock(mutex_);
    if (!workerFailureReason_.empty()) {
        failureReason = workerFailureReason_;
        samples_.clear();
        return false;
    }

    samples = samples_;
    samples_.clear();
    return true;
}

void AudioRecorder::Cancel() noexcept {
    recordingRequested_ = false;
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

void AudioRecorder::RecordingThreadMain(AudioConfig config, AmplitudeCallback amplitudeCallback) noexcept {
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

    while (recordingRequested_) {
        const PaError readError = Pa_ReadStream(stream, readBuffer.data(), framesPerRead);
        if (readError == paInputOverflowed) {
            continue;
        }

        if (readError != paNoError) {
            const std::scoped_lock lock(mutex_);
            workerFailureReason_ = L"Pa_ReadStream failed while recording: ";
            workerFailureReason_ += WideFromPortAudioError(Pa_GetErrorText(readError));
            break;
        }

        capturedSamples.insert(capturedSamples.end(), readBuffer.begin(), readBuffer.end());

        if (amplitudeCallback) {
            amplitudeCallback(ComputeRmsAmplitude(readBuffer));
        }

        const uint64_t capturedFrames = static_cast<uint64_t>(capturedSamples.size()) / static_cast<uint64_t>(config.channelCount);
        if (capturedFrames >= maxFrames) {
            recordingRequested_ = false;
            const std::scoped_lock lock(mutex_);
            workerFailureReason_ = L"Maximum recording length reached before stop was requested.";
            break;
        }
    }

    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    recordingRequested_ = false;

    const std::scoped_lock lock(mutex_);
    if (workerFailureReason_.empty()) {
        samples_ = std::move(capturedSamples);
    }
}

} // namespace voxinsert