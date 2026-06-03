#pragma once

#include "config/app_config.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace voxinsert {

// Captures PCM16 audio from the current default microphone using PortAudio's blocking API.
class AudioRecorder {
public:
    using AmplitudeCallback = std::function<void(float)>;
    // Invoked on the recording thread for each captured buffer. Used by the
    // streaming path to forward audio without blocking the audio thread; the
    // callback must be cheap and non-blocking (e.g. enqueue and return).
    using FrameSink = std::function<void(const int16_t* data, size_t sampleCount)>;

    AudioRecorder() = default;
    ~AudioRecorder();

    bool Start(const AudioConfig& config, AmplitudeCallback amplitudeCallback, std::wstring& failureReason);
    bool Start(const AudioConfig& config, AmplitudeCallback amplitudeCallback, FrameSink frameSink, std::wstring& failureReason);
    bool Stop(std::vector<int16_t>& samples, std::wstring& failureReason);
    void Cancel() noexcept;
    bool IsRecording() const noexcept;
    std::wstring ActiveDeviceName() const;

private:
    void RecordingThreadMain(AudioConfig config, AmplitudeCallback amplitudeCallback, FrameSink frameSink) noexcept;
    bool EnsureInitialized(std::wstring& failureReason);
    void JoinWorker() noexcept;
    std::wstring BuildMicrophoneError(std::wstring_view action, std::string_view portAudioErrorText) const;

    std::atomic<bool> recordingRequested_{false};
    mutable std::mutex mutex_;
    std::thread worker_;
    std::vector<int16_t> samples_;
    std::wstring workerFailureReason_;
    std::wstring activeDeviceName_;
    std::condition_variable startupCondition_;
    std::atomic<int64_t> stopRequestedAtTicks_{0};
    bool startupCompleted_ = false;
    bool startupSucceeded_ = false;
    bool portAudioInitialized_ = false;
};

} // namespace voxinsert
