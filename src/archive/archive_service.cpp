#include "archive/archive_service.h"

#include "observability/logging.h"

#include <windows.h>

#include <nlohmann/json.hpp>
#include <opus/opus.h>
#include <opus/opusenc.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace voxinsert {
namespace {

using json = nlohmann::json;

struct ClipClock {
    SYSTEMTIME local{};
    SYSTEMTIME utc{};
};

struct ArchivePaths {
    std::string clipIdUtf8;
    std::wstring clipIdWide;
    std::filesystem::path dayDirectory;
    std::filesystem::path audioPath;
    std::filesystem::path transcriptPath;
    std::filesystem::path metadataPath;
};

std::string ProviderModel(const TranscriptionConfig& transcription) {
    if (transcription.provider == "mistral") {
        return transcription.mistral.model;
    }

    return transcription.openAi.model;
}

std::string FormatSystemTimeUtc(const SYSTEMTIME& time) {
    char buffer[40]{};
    sprintf_s(
        buffer,
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds);
    return buffer;
}

std::wstring FormatClipId(const SYSTEMTIME& time) {
    wchar_t buffer[64]{};
    swprintf_s(
        buffer,
        L"clip-%04u%02u%02u-%02u%02u%02u-%03u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds);
    return buffer;
}

ClipClock CurrentClipClock() {
    ClipClock clock{};
    GetLocalTime(&clock.local);
    GetSystemTime(&clock.utc);
    return clock;
}

ArchivePaths BuildArchivePaths(const ArchiveConfig& archive, const ClipClock& clock) {
    std::filesystem::path baseDirectory = archive.folderPath.empty()
        ? std::filesystem::path(DefaultArchiveFolderPath())
        : std::filesystem::path(archive.folderPath);
    if (baseDirectory.empty()) {
        throw std::runtime_error("archive folder is empty");
    }

    wchar_t year[8]{};
    wchar_t month[4]{};
    wchar_t day[4]{};
    swprintf_s(year, L"%04u", clock.local.wYear);
    swprintf_s(month, L"%02u", clock.local.wMonth);
    swprintf_s(day, L"%02u", clock.local.wDay);

    ArchivePaths paths{};
    paths.clipIdWide = FormatClipId(clock.local);
    paths.clipIdUtf8 = Utf8FromWide(paths.clipIdWide);
    paths.dayDirectory = baseDirectory / year / month / day;
    paths.audioPath = paths.dayDirectory / (paths.clipIdWide + L".opus");
    paths.transcriptPath = paths.dayDirectory / (paths.clipIdWide + L".txt");
    paths.metadataPath = paths.dayDirectory / (paths.clipIdWide + L".json");
    return paths;
}

std::string RelativeFileNameUtf8(const std::filesystem::path& path) {
    return Utf8FromWide(path.filename().wstring());
}

std::wstring OpeFailureMessage(std::string_view operation, int errorCode) {
    std::wstring message = WideFromUtf8(operation);
    message += L" failed: ";
    message += WideFromUtf8(ope_strerror(errorCode));
    message += L" (";
    message += std::to_wstring(errorCode);
    message += L")";
    return message;
}

void CheckOpeStatus(std::string_view operation, int status) {
    if (status == OPE_OK) {
        return;
    }

    throw std::runtime_error(Utf8FromWide(OpeFailureMessage(operation, status)));
}

void AddOpusComment(OggOpusComments* comments, const char* tag, const std::string& value) {
    CheckOpeStatus("ope_comments_add", ope_comments_add(comments, tag, value.c_str()));
}

void WriteOpusFile(
    const std::filesystem::path& outputPath,
    const ArchiveRequest& request,
    const ArchivePaths& paths) {
    if (request.audio.sampleRate <= 0 || request.audio.channelCount != 1) {
        throw std::runtime_error("archive audio requires positive mono sample-rate config");
    }

    if (request.samples.empty()) {
        throw std::runtime_error("archive audio request has no PCM samples");
    }

    if (request.samples.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("archive audio request has too many samples for libopusenc");
    }

    OggOpusComments* rawComments = ope_comments_create();
    if (rawComments == nullptr) {
        throw std::runtime_error("ope_comments_create failed");
    }

    std::unique_ptr<OggOpusComments, decltype(&ope_comments_destroy)> comments(rawComments, ope_comments_destroy);
    AddOpusComment(comments.get(), "TITLE", paths.clipIdUtf8);
    AddOpusComment(comments.get(), "ENCODER", "VoxInsert");
    AddOpusComment(comments.get(), "TRANSCRIPTION_PROVIDER", request.transcription.provider);
    AddOpusComment(comments.get(), "TRANSCRIPTION_MODEL", ProviderModel(request.transcription));

    const std::string outputPathUtf8 = Utf8FromWide(outputPath.wstring());
    int createError = OPE_OK;
    OggOpusEnc* rawEncoder = ope_encoder_create_file(
        outputPathUtf8.c_str(),
        comments.get(),
        request.audio.sampleRate,
        request.audio.channelCount,
        0,
        &createError);
    if (rawEncoder == nullptr) {
        throw std::runtime_error(Utf8FromWide(OpeFailureMessage("ope_encoder_create_file", createError)));
    }

    std::unique_ptr<OggOpusEnc, decltype(&ope_encoder_destroy)> encoder(rawEncoder, ope_encoder_destroy);
    CheckOpeStatus("ope_encoder_ctl(OPUS_SET_BITRATE)", ope_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(request.archive.opusBitrateBps)));
    CheckOpeStatus("ope_encoder_ctl(OPUS_SET_SIGNAL)", ope_encoder_ctl(encoder.get(), OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)));
    CheckOpeStatus(
        "ope_encoder_write",
        ope_encoder_write(encoder.get(), request.samples.data(), static_cast<int>(request.samples.size())));
    CheckOpeStatus("ope_encoder_drain", ope_encoder_drain(encoder.get()));
}

void WriteTextFile(const std::filesystem::path& outputPath, const std::string& transcriptUtf8) {
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to open transcript file for writing");
    }

    output << transcriptUtf8;
    if (!output.good()) {
        throw std::runtime_error("failed while writing transcript file");
    }
}

void WriteMetadataFile(
    const std::filesystem::path& outputPath,
    const ArchiveRequest& request,
    const ArchivePaths& paths,
    const ClipClock& clock) {
    const int durationMs = request.audio.sampleRate > 0
        ? static_cast<int>((static_cast<long long>(request.samples.size()) * 1000LL) / request.audio.sampleRate)
        : 0;

    json metadata;
    metadata["schema_version"] = 1;
    metadata["clip_id"] = paths.clipIdUtf8;
    metadata["created_at_utc"] = FormatSystemTimeUtc(clock.utc);
    metadata["provider"] = request.transcription.provider;
    metadata["model"] = ProviderModel(request.transcription);
    metadata["language_hint"] = request.transcription.languageHint;
    metadata["insertion_succeeded"] = request.insertionSucceeded;

    metadata["audio"]["persisted"] = request.archive.persistAudio;
    metadata["audio"]["format"] = request.archive.persistAudio ? "ogg_opus" : "none";
    metadata["audio"]["relative_path"] = request.archive.persistAudio ? RelativeFileNameUtf8(paths.audioPath) : "";
    metadata["audio"]["sample_rate_hz"] = request.audio.sampleRate;
    metadata["audio"]["channels"] = request.audio.channelCount;
    metadata["audio"]["duration_ms"] = durationMs;
    metadata["audio"]["opus_bitrate_bps"] = request.archive.opusBitrateBps;

    metadata["transcript"]["persisted"] = request.archive.persistTranscript;
    metadata["transcript"]["relative_path"] = request.archive.persistTranscript ? RelativeFileNameUtf8(paths.transcriptPath) : "";
    if (request.archive.persistTranscript) {
        metadata["transcript"]["text"] = request.transcriptUtf8;
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to open archive metadata file for writing");
    }

    output << metadata.dump(2);
    output << '\n';
    if (!output.good()) {
        throw std::runtime_error("failed while writing archive metadata file");
    }
}

void WriteArchive(const ArchiveRequest& request) {
    if (!request.archive.enabled || (!request.archive.persistTranscript && !request.archive.persistAudio)) {
        return;
    }

    const ClipClock clock = CurrentClipClock();
    const ArchivePaths paths = BuildArchivePaths(request.archive, clock);

    std::error_code errorCode;
    std::filesystem::create_directories(paths.dayDirectory, errorCode);
    if (errorCode) {
        throw std::runtime_error("failed to create archive folder: " + errorCode.message());
    }

    if (request.archive.persistAudio) {
        WriteOpusFile(paths.audioPath, request, paths);
    }

    if (request.archive.persistTranscript) {
        WriteTextFile(paths.transcriptPath, request.transcriptUtf8);
    }

    WriteMetadataFile(paths.metadataPath, request, paths, clock);
}

} // namespace

ArchiveService::~ArchiveService() {
    Shutdown();
}

void ArchiveService::EnsureWorkerStarted() {
    if (worker_.joinable()) {
        return;
    }

    worker_ = std::thread([this]() {
        WorkerLoop();
    });
}

void ArchiveService::Enqueue(ArchiveRequest request) noexcept {
    if (!request.archive.enabled || (!request.archive.persistTranscript && !request.archive.persistAudio)) {
        return;
    }

    try {
        std::scoped_lock lock(mutex_);
        if (stopping_) {
            return;
        }

        queue_.push(std::move(request));
        EnsureWorkerStarted();
        condition_.notify_one();
    }
    catch (const std::exception& exception) {
        if (request.logger != nullptr) {
            request.logger->warn("archive request could not be queued: {}", exception.what());
        }
    }
    catch (...) {
        if (request.logger != nullptr) {
            request.logger->warn("archive request could not be queued: unknown failure");
        }
    }
}

void ArchiveService::Shutdown() noexcept {
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }

    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ArchiveService::WorkerLoop() noexcept {
    for (;;) {
        ArchiveRequest request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this]() {
                return stopping_ || !queue_.empty();
            });

            if (queue_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }

            request = std::move(queue_.front());
            queue_.pop();
        }

        ProcessRequest(request);
    }
}

void ArchiveService::ProcessRequest(const ArchiveRequest& request) noexcept {
    try {
        WriteArchive(request);
        if (request.logger != nullptr) {
            request.logger->info("transcription archive written");
        }
    }
    catch (const std::exception& exception) {
        if (request.logger != nullptr) {
            request.logger->warn("transcription archive failed: {}", exception.what());
        }
    }
    catch (...) {
        if (request.logger != nullptr) {
            request.logger->warn("transcription archive failed: unknown failure");
        }
    }
}

} // namespace voxinsert