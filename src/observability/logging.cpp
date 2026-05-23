#include "observability/logging.h"

#include <objbase.h>
#include <shlobj_core.h>

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <cwchar>
#include <vector>

namespace voxinsert {
namespace {

constexpr size_t kLogFileMaxSizeBytes = 1024 * 1024;
constexpr size_t kLogFileMaxCount = 3;

spdlog::filename_t ToSpdlogFilename(const std::wstring& path) {
#ifdef SPDLOG_WCHAR_FILENAMES
    return path;
#else
    return Utf8FromWide(path);
#endif
}

bool EnsureDirectoryExists(const std::wstring& directoryPath, DWORD& errorCode) {
    if (CreateDirectoryW(directoryPath.c_str(), nullptr) != 0) {
        return true;
    }

    errorCode = GetLastError();
    if (errorCode != ERROR_ALREADY_EXISTS) {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(directoryPath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return true;
    }

    errorCode = ERROR_ALREADY_EXISTS;
    return false;
}

} // namespace

std::string Utf8FromWide(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    const int requiredBytes = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (requiredBytes <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(requiredBytes), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        requiredBytes,
        nullptr,
        nullptr);

    return result;
}

std::wstring WideFromUtf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int requiredCharacters = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);

    if (requiredCharacters <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(requiredCharacters), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        requiredCharacters);

    return result;
}

std::wstring FormatWin32Error(DWORD errorCode) {
    wchar_t* messageBuffer = nullptr;
    const DWORD characterCount = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        reinterpret_cast<LPWSTR>(&messageBuffer),
        0,
        nullptr);

    std::wstring message = std::to_wstring(errorCode);

    if (characterCount > 0 && messageBuffer != nullptr) {
        std::wstring systemMessage(messageBuffer, characterCount);

        while (!systemMessage.empty() && (systemMessage.back() == L'\r' || systemMessage.back() == L'\n' || systemMessage.back() == L'.')) {
            systemMessage.pop_back();
        }

        message += L" (";
        message += systemMessage;
        message += L")";
    }

    if (messageBuffer != nullptr) {
        LocalFree(messageBuffer);
    }

    return message;
}

bool BuildLogFilePath(std::wstring& logFilePath, std::wstring& failureReason) {
    PWSTR localAppDataPath = nullptr;
    const HRESULT knownFolderResult = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppDataPath);
    if (FAILED(knownFolderResult)) {
        failureReason = L"SHGetKnownFolderPath(FOLDERID_LocalAppData) failed with HRESULT 0x";
        wchar_t codeBuffer[16]{};
        swprintf_s(codeBuffer, L"%08X", static_cast<unsigned int>(knownFolderResult));
        failureReason += codeBuffer;
        return false;
    }

    const std::wstring appDirectory = std::wstring(localAppDataPath) + L"\\VoxInsert";
    const std::wstring logDirectory = appDirectory + L"\\logs";
    CoTaskMemFree(localAppDataPath);

    DWORD errorCode = ERROR_SUCCESS;
    if (!EnsureDirectoryExists(appDirectory, errorCode)) {
        failureReason = L"CreateDirectoryW failed for app data directory: ";
        failureReason += FormatWin32Error(errorCode);
        return false;
    }

    if (!EnsureDirectoryExists(logDirectory, errorCode)) {
        failureReason = L"CreateDirectoryW failed for log directory: ";
        failureReason += FormatWin32Error(errorCode);
        return false;
    }

    logFilePath = logDirectory + L"\\voxinsert.log";
    return true;
}

std::shared_ptr<spdlog::logger> CreateLogger(const std::wstring& logFilePath, std::wstring& failureReason) {
    try {
        auto rotatingFileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            ToSpdlogFilename(logFilePath),
            kLogFileMaxSizeBytes,
            kLogFileMaxCount);
        auto debuggerSink = std::make_shared<spdlog::sinks::msvc_sink_mt>();

        std::vector<spdlog::sink_ptr> sinks{rotatingFileSink, debuggerSink};
        auto logger = std::make_shared<spdlog::logger>("voxinsert", sinks.begin(), sinks.end());
        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::info);
        logger->set_pattern("%Y-%m-%dT%H:%M:%S.%eZ [%l] %v", spdlog::pattern_time_type::utc);

        spdlog::set_default_logger(logger);
        return logger;
    }
    catch (const spdlog::spdlog_ex& exception) {
        failureReason = L"spdlog initialization failed: ";
        failureReason += WideFromUtf8(exception.what());
        return nullptr;
    }
}

SpdlogLifetimeGuard::~SpdlogLifetimeGuard() {
    spdlog::shutdown();
}

} // namespace voxinsert