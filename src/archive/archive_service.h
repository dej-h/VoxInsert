#pragma once

#include "config/app_config.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace spdlog {
class logger;
}

namespace voxinsert {

struct ArchiveRequest {
    ArchiveConfig archive;
    AudioConfig audio;
    TranscriptionConfig transcription;
    std::vector<int16_t> samples;
    std::string transcriptUtf8;
    bool insertionSucceeded = false;
    std::shared_ptr<spdlog::logger> logger;
};

class ArchiveService {
public:
    ArchiveService() = default;
    ArchiveService(const ArchiveService&) = delete;
    ArchiveService& operator=(const ArchiveService&) = delete;
    ~ArchiveService();

    void Enqueue(ArchiveRequest request) noexcept;
    void Shutdown() noexcept;

private:
    void EnsureWorkerStarted();
    void WorkerLoop() noexcept;
    void ProcessRequest(const ArchiveRequest& request) noexcept;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<ArchiveRequest> queue_;
    std::thread worker_;
    bool stopping_ = false;
};

} // namespace voxinsert