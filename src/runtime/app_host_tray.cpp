#include "runtime/app_host_internal.h"

#include "observability/logging.h"
#include "resource.h"

#include <commctrl.h>

namespace voxinsert {
namespace {

HICON LoadOwnedTrayIcon(HINSTANCE instance, int width, int height) {
    (void)width;
    (void)height;

    HICON iconHandle = nullptr;
    HRESULT result = LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_TRAY_ICON), LIM_SMALL, &iconHandle);
    if (FAILED(result) || iconHandle == nullptr) {
        iconHandle = nullptr;
        result = LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_APP_ICON), LIM_SMALL, &iconHandle);
    }

    if (iconHandle == nullptr) {
        iconHandle = reinterpret_cast<HICON>(LoadImageW(
            nullptr,
            IDI_APPLICATION,
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR));
    }

    return iconHandle;
}

} // namespace

void CopyLastTranscriptFromTray(AppContext& context) {
    std::wstring transcript;
    if (!TryGetLastTranscript(context, transcript)) {
        ShowRuntimeInfo(
            context,
            L"VoxInsert transcript",
            L"No transcript is available yet. Record something first.");
        return;
    }

    std::wstring failureReason;
    if (!context.textInjector.CopyTextToClipboard(context.window, transcript, failureReason)) {
        ShowRuntimeError(context, L"VoxInsert clipboard error", failureReason);
        return;
    }

    context.logger->info("last transcript copied to the clipboard from the tray menu");
}

void ReinsertLastTranscriptFromTray(AppContext& context) {
    if (context.state != AppState::Idle) {
        ShowRuntimeInfo(
            context,
            L"VoxInsert transcript",
            L"The last transcript can only be re-inserted while VoxInsert is idle.");
        return;
    }

    std::wstring transcript;
    if (!TryGetLastTranscript(context, transcript)) {
        ShowRuntimeInfo(
            context,
            L"VoxInsert transcript",
            L"No transcript is available yet. Record something first.");
        return;
    }

    std::wstring failureReason;
    if (!context.textInjector.InsertText(context.window, context.config.insertion, transcript, failureReason)) {
        ShowRuntimeError(context, L"VoxInsert insertion error", failureReason);
        return;
    }

    context.logger->info("last transcript re-inserted from the tray menu");
    context.statusPill.SetState(StatusPillState::Done);
    SetTransientTrayStatus(context, TrayStatus::Inserted, {}, kInsertedTrayStatusMs);
}

void OpenLastRecordingFolderFromTray(AppContext& context) {
    std::filesystem::path recordingPath;
    if (!TryGetLastRecordingPath(context, recordingPath)) {
        ShowRuntimeInfo(
            context,
            L"VoxInsert recordings",
            L"No recording has been written yet.");
        return;
    }

    std::filesystem::path folderPath = recordingPath.parent_path();
    if (folderPath.empty()) {
        folderPath = recordingPath;
    }

    std::error_code errorCode;
    if (!std::filesystem::exists(folderPath, errorCode) || errorCode) {
        std::wstring failureReason = L"The last recording folder could not be found: ";
        failureReason += folderPath.wstring();
        ShowRuntimeError(context, L"VoxInsert recordings", failureReason);
        return;
    }

    std::wstring failureReason;
    if (!OpenPathInExplorer(context.window, folderPath, failureReason)) {
        ShowRuntimeError(context, L"VoxInsert recordings", failureReason);
        return;
    }

    context.logger->info("opened last recording folder from the tray menu: {}", Utf8FromWide(folderPath.wstring()));
}

void RemoveTrayIcon(AppContext& context) {
    if (context.trayIconAdded) {
        if (Shell_NotifyIconW(NIM_DELETE, &context.trayIconData) == 0) {
            context.logger->warn(
                "Shell_NotifyIconW(NIM_DELETE) failed: {}",
                Utf8FromWide(FormatWin32Error(GetLastError())));
        }
        else {
            context.logger->info("tray icon removed");
        }
        context.trayIconAdded = false;
    }

    if (context.trayIcon != nullptr) {
        DestroyIcon(context.trayIcon);
        context.trayIcon = nullptr;
    }
}

bool AddTrayIcon(AppContext& context) {
    RemoveTrayIcon(context);

    context.trayIcon = LoadOwnedTrayIcon(context.instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (context.trayIcon == nullptr) {
        context.logger->error("LoadImageW failed for tray icon: {}", Utf8FromWide(FormatWin32Error(GetLastError())));
        return false;
    }

    NOTIFYICONDATAW trayIconData{};
    trayIconData.cbSize = sizeof(trayIconData);
    trayIconData.hWnd = context.window;
    trayIconData.uID = kTrayIconId;
    trayIconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    trayIconData.uCallbackMessage = kTrayCallbackMessage;
    trayIconData.hIcon = context.trayIcon;
    const std::wstring tooltip = BuildTrayTooltip(context);
    wcsncpy_s(trayIconData.szTip, ARRAYSIZE(trayIconData.szTip), tooltip.c_str(), _TRUNCATE);

    if (Shell_NotifyIconW(NIM_ADD, &trayIconData) == 0) {
        context.logger->error("Shell_NotifyIconW(NIM_ADD) failed: {}", Utf8FromWide(FormatWin32Error(GetLastError())));
        DestroyIcon(context.trayIcon);
        context.trayIcon = nullptr;
        return false;
    }

    trayIconData.uVersion = NOTIFYICON_VERSION_4;
    if (Shell_NotifyIconW(NIM_SETVERSION, &trayIconData) == 0) {
        context.logger->warn("Shell_NotifyIconW(NIM_SETVERSION) failed: {}", Utf8FromWide(FormatWin32Error(GetLastError())));
    }
    else {
        context.logger->info("tray icon version set to NOTIFYICON_VERSION_4");
    }

    context.trayIconData = trayIconData;
    context.trayIconAdded = true;
    context.logger->info("tray icon added");
    return true;
}

void ShowTrayMenu(AppContext& context) {
    HMENU menuHandle = CreatePopupMenu();
    if (menuHandle == nullptr) {
        context.logger->error("CreatePopupMenu failed: {}", Utf8FromWide(FormatWin32Error(GetLastError())));
        return;
    }

    const std::wstring menuTitle = BuildTrayMenuTitle(context);
    const bool hasLastTranscript = HasLastTranscript(context);
    const bool hasLastRecordingPath = HasLastRecordingPath(context);
    AppendMenuW(menuHandle, MF_STRING | MF_DISABLED, 0, menuTitle.c_str());
    AppendMenuW(menuHandle, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(
        menuHandle,
        context.state == AppState::Idle ? MF_STRING : (MF_STRING | MF_GRAYED),
        kTrayMenuCommandSettings,
        L"Settings...");
    AppendMenuW(
        menuHandle,
        context.state == AppState::Idle ? MF_STRING : (MF_STRING | MF_GRAYED),
        kTrayMenuCommandReloadConfig,
        L"Reload config");
    AppendMenuW(menuHandle, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        menuHandle,
        hasLastTranscript ? MF_STRING : (MF_STRING | MF_GRAYED),
        kTrayMenuCommandCopyLastTranscript,
        L"Copy Last Transcript");
    AppendMenuW(
        menuHandle,
        (context.state == AppState::Idle && hasLastTranscript) ? MF_STRING : (MF_STRING | MF_GRAYED),
        kTrayMenuCommandReinsertLastTranscript,
        L"Re-insert Last Transcript");
    AppendMenuW(
        menuHandle,
        hasLastRecordingPath ? MF_STRING : (MF_STRING | MF_GRAYED),
        kTrayMenuCommandOpenLastRecordingFolder,
        L"Open Last Recording Folder");
    AppendMenuW(menuHandle, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menuHandle, MF_STRING, kTrayMenuCommandQuit, L"Quit");

    POINT cursorPosition{};
    GetCursorPos(&cursorPosition);

    SetForegroundWindow(context.window);
    TrackPopupMenu(
        menuHandle,
        TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
        cursorPosition.x,
        cursorPosition.y,
        0,
        context.window,
        nullptr);
    PostMessageW(context.window, WM_NULL, 0, 0);
    Shell_NotifyIconW(NIM_SETFOCUS, &context.trayIconData);

    DestroyMenu(menuHandle);
}

} // namespace voxinsert