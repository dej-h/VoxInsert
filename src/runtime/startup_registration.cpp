#include "runtime/startup_registration.h"

#include "observability/logging.h"

#include <windows.h>

#include <utility>

namespace voxinsert {
namespace {

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"VoxInsert";

class RegistryKey {
public:
    RegistryKey() = default;
    ~RegistryKey() {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    HKEY* OutParam() {
        return &key_;
    }

    HKEY Get() const {
        return key_;
    }

private:
    HKEY key_ = nullptr;
};

bool GetExecutablePath(std::wstring& executablePath, std::wstring& failureReason) {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD characterCount = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (characterCount == 0) {
            failureReason = L"GetModuleFileNameW failed while preparing startup registration: ";
            failureReason += FormatWin32Error(GetLastError());
            return false;
        }

        if (characterCount < buffer.size() - 1) {
            buffer.resize(characterCount);
            executablePath = std::move(buffer);
            return true;
        }

        buffer.resize(buffer.size() * 2);
    }
}

std::wstring QuoteCommandPath(const std::wstring& executablePath) {
    std::wstring commandLine = L"\"";
    commandLine += executablePath;
    commandLine += L"\"";
    return commandLine;
}

bool EnableStartupRegistration(std::wstring& failureReason) {
    std::wstring executablePath;
    if (!GetExecutablePath(executablePath, failureReason)) {
        return false;
    }

    const std::wstring commandLine = QuoteCommandPath(executablePath);

    RegistryKey key;
    const LSTATUS createStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kRunKeyPath,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        key.OutParam(),
        nullptr);
    if (createStatus != ERROR_SUCCESS) {
        failureReason = L"RegCreateKeyExW failed for Windows startup registration: ";
        failureReason += FormatWin32Error(static_cast<DWORD>(createStatus));
        return false;
    }

    const DWORD byteCount = static_cast<DWORD>((commandLine.size() + 1) * sizeof(wchar_t));
    const LSTATUS setStatus = RegSetValueExW(
        key.Get(),
        kRunValueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(commandLine.c_str()),
        byteCount);
    if (setStatus == ERROR_SUCCESS) {
        return true;
    }

    failureReason = L"RegSetValueExW failed for Windows startup registration: ";
    failureReason += FormatWin32Error(static_cast<DWORD>(setStatus));
    return false;
}

bool DisableStartupRegistration(std::wstring& failureReason) {
    RegistryKey key;
    const LSTATUS openStatus = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, key.OutParam());
    if (openStatus == ERROR_FILE_NOT_FOUND) {
        return true;
    }

    if (openStatus != ERROR_SUCCESS) {
        failureReason = L"RegOpenKeyExW failed for Windows startup registration: ";
        failureReason += FormatWin32Error(static_cast<DWORD>(openStatus));
        return false;
    }

    const LSTATUS deleteStatus = RegDeleteValueW(key.Get(), kRunValueName);
    if (deleteStatus == ERROR_SUCCESS || deleteStatus == ERROR_FILE_NOT_FOUND) {
        return true;
    }

    failureReason = L"RegDeleteValueW failed for Windows startup registration: ";
    failureReason += FormatWin32Error(static_cast<DWORD>(deleteStatus));
    return false;
}

} // namespace

bool ApplyStartupRegistration(const SystemConfig& config, std::wstring& failureReason) {
    if (config.autoStartWithWindows) {
        return EnableStartupRegistration(failureReason);
    }

    return DisableStartupRegistration(failureReason);
}

} // namespace voxinsert