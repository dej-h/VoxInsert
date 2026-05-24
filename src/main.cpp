#include "observability/logging.h"
#include "runtime/app_host.h"

#include <commctrl.h>
#include <windows.h>

#include <string>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_WIN95_CLASSES;
    if (InitCommonControlsEx(&commonControls) == FALSE) {
        const std::wstring failureReason = L"InitCommonControlsEx failed: " + voxinsert::FormatWin32Error(GetLastError());
        OutputDebugStringW(failureReason.c_str());
        MessageBoxW(nullptr, failureReason.c_str(), L"VoxInsert startup failed", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::wstring logFilePath;
    std::wstring failureReason;
    if (!voxinsert::BuildLogFilePath(logFilePath, failureReason)) {
        OutputDebugStringW(failureReason.c_str());
        MessageBoxW(nullptr, failureReason.c_str(), L"VoxInsert startup failed", MB_OK | MB_ICONERROR);
        return 1;
    }

    auto logger = voxinsert::CreateLogger(logFilePath, failureReason);
    if (logger == nullptr) {
        OutputDebugStringW(failureReason.c_str());
        MessageBoxW(nullptr, failureReason.c_str(), L"VoxInsert startup failed", MB_OK | MB_ICONERROR);
        return 1;
    }

    voxinsert::SpdlogLifetimeGuard spdlogLifetimeGuard;

    // Keep process bootstrap thin so the host owns the runtime behavior.
    logger->info("VoxInsert starting");
    logger->info("log file: {}", voxinsert::Utf8FromWide(logFilePath));

    const voxinsert::AppHostOptions options = voxinsert::ParseAppHostOptions();
    const int exitCode = voxinsert::RunAppHost(instance, showCommand, logger, options);

    logger->info("VoxInsert exiting");
    return exitCode;
}