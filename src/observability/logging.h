#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

namespace voxinsert {

std::string Utf8FromWide(std::wstring_view text);
std::wstring WideFromUtf8(std::string_view text);
std::wstring FormatWin32Error(DWORD errorCode);

bool BuildLogFilePath(std::wstring& logFilePath, std::wstring& failureReason);
std::shared_ptr<spdlog::logger> CreateLogger(const std::wstring& logFilePath, std::wstring& failureReason);

class SpdlogLifetimeGuard {
public:
    ~SpdlogLifetimeGuard();
};

} // namespace voxinsert