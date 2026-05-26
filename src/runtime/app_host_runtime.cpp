#include "runtime/app_host_internal.h"

#include "observability/logging.h"
#include "runtime/startup_registration.h"

#include <utility>

namespace voxinsert {

bool SameHotkeyBinding(const HotkeyBinding& left, const HotkeyBinding& right) {
    return left.virtualKey == right.virtualKey &&
        (left.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) ==
            (right.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN));
}

std::wstring_view StateLabel(AppState state) {
    switch (state) {
    case AppState::Idle:
        return L"Idle";
    case AppState::Recording:
        return L"Recording";
    case AppState::SavingRecording:
        return L"Saving recording";
    case AppState::Transcribing:
        return L"Transcribing";
    case AppState::Inserting:
        return L"Inserting";
    }

    return L"Unknown";
}

TrayStatus TrayStatusForAppState(AppState state) {
    switch (state) {
    case AppState::Idle:
        return TrayStatus::Idle;
    case AppState::Recording:
        return TrayStatus::Recording;
    case AppState::SavingRecording:
        return TrayStatus::SavingRecording;
    case AppState::Transcribing:
        return TrayStatus::Transcribing;
    case AppState::Inserting:
        return TrayStatus::Inserting;
    }

    return TrayStatus::Idle;
}

bool IsTransientTrayStatus(TrayStatus status) {
    return status == TrayStatus::Inserted || status == TrayStatus::Error;
}

std::wstring_view TrayStatusLabel(TrayStatus status) {
    switch (status) {
    case TrayStatus::Idle:
        return L"";
    case TrayStatus::Recording:
        return L"recording";
    case TrayStatus::SavingRecording:
        return L"saving recording";
    case TrayStatus::Transcribing:
        return L"transcribing";
    case TrayStatus::Inserting:
        return L"inserting";
    case TrayStatus::Inserted:
        return L"inserted";
    case TrayStatus::Error:
        return L"error";
    }

    return L"";
}

std::wstring TrayStatusText(const AppContext& context, bool includeErrorDetail) {
    if (context.trayStatus == TrayStatus::Error && includeErrorDetail && !context.trayStatusDetail.empty()) {
        return context.trayStatusDetail;
    }

    return std::wstring(TrayStatusLabel(context.trayStatus));
}

std::wstring BuildTrayTooltip(const AppContext& context) {
    std::wstring tooltip(kTrayTooltip);
    const std::wstring statusText = TrayStatusText(context, true);
    if (!statusText.empty()) {
        tooltip += L" - ";
        tooltip += statusText;
    }
    return tooltip;
}

std::wstring BuildTrayMenuTitle(const AppContext& context) {
    std::wstring title(kTrayTooltip);
    const std::wstring statusText = TrayStatusText(context, false);
    if (!statusText.empty()) {
        title += L" - ";
        title += statusText;
    }
    return title;
}

bool UpdateTrayTooltip(AppContext& context) {
    if (!context.trayIconAdded) {
        return true;
    }

    NOTIFYICONDATAW trayIconData = context.trayIconData;
    trayIconData.uFlags = NIF_TIP | NIF_SHOWTIP;

    const std::wstring tooltip = BuildTrayTooltip(context);
    wcsncpy_s(trayIconData.szTip, ARRAYSIZE(trayIconData.szTip), tooltip.c_str(), _TRUNCATE);

    if (Shell_NotifyIconW(NIM_MODIFY, &trayIconData) == 0) {
        context.logger->warn(
            "Shell_NotifyIconW(NIM_MODIFY) failed while updating tray tooltip: {}",
            Utf8FromWide(FormatWin32Error(GetLastError())));
        return false;
    }

    context.trayIconData = trayIconData;
    return true;
}

void SetTrayStatus(AppContext& context, TrayStatus status, std::wstring detail) {
    if (context.window != nullptr) {
        KillTimer(context.window, kTrayStatusResetTimerId);
    }

    context.trayStatus = status;
    context.trayStatusDetail = std::move(detail);
    UpdateTrayTooltip(context);
}

void SetTransientTrayStatus(AppContext& context, TrayStatus status, std::wstring detail, UINT durationMs) {
    context.trayStatus = status;
    context.trayStatusDetail = std::move(detail);
    UpdateTrayTooltip(context);

    if (context.window != nullptr) {
        SetTimer(context.window, kTrayStatusResetTimerId, durationMs, nullptr);
    }
}

void ResetTrayStatusToCurrentState(AppContext& context) {
    SetTrayStatus(context, TrayStatusForAppState(context.state));
}

void SetState(AppContext& context, AppState nextState) {
    if (context.state == nextState) {
        return;
    }

    context.logger->info(
        "state transition: {} -> {}",
        Utf8FromWide(StateLabel(context.state)),
        Utf8FromWide(StateLabel(nextState)));

    context.state = nextState;
    context.smtcController.SyncPlaybackActive(nextState == AppState::Recording);

    switch (nextState) {
    case AppState::Idle:
        context.statusPill.SetState(StatusPillState::Idle);
        if (!IsTransientTrayStatus(context.trayStatus)) {
            SetTrayStatus(context, TrayStatus::Idle);
        }
        break;
    case AppState::Recording:
        SetTrayStatus(context, TrayStatus::Recording);
        context.statusPill.SetState(StatusPillState::Recording);
        break;
    case AppState::SavingRecording:
        SetTrayStatus(context, TrayStatus::SavingRecording);
        context.statusPill.SetState(StatusPillState::Working);
        break;
    case AppState::Transcribing:
        SetTrayStatus(context, TrayStatus::Transcribing);
        context.statusPill.SetState(StatusPillState::Transcribing);
        break;
    case AppState::Inserting:
        SetTrayStatus(context, TrayStatus::Inserting);
        context.statusPill.SetState(StatusPillState::Working);
        break;
    }
}

void ShowRuntimeError(AppContext& context, const wchar_t* title, const std::wstring& message) {
    context.logger->error("{}", Utf8FromWide(message));
    context.statusPill.SetState(StatusPillState::Error, message);
    SetTransientTrayStatus(context, TrayStatus::Error, message, kErrorTrayStatusMs);
    if (!context.options.smokeTest) {
        MessageBoxW(context.window, message.c_str(), title, MB_OK | MB_ICONERROR);
    }
}

void ShowRuntimeInfo(AppContext& context, const wchar_t* title, const std::wstring& message) {
    context.logger->info("{}", Utf8FromWide(message));
    if (!context.options.smokeTest) {
        MessageBoxW(context.window, message.c_str(), title, MB_OK | MB_ICONINFORMATION);
    }
}

void JoinPostRecordingWorker(AppContext& context) noexcept {
    if (context.postRecordingWorker.joinable()) {
        context.postRecordingWorker.join();
    }
}

void StorePostRecordingResult(AppContext& context, PostRecordingResult result) {
    const std::scoped_lock lock(context.postRecordingResultMutex);
    context.postRecordingResult = std::move(result);
    context.hasPostRecordingResult = true;
}

bool TakePostRecordingResult(AppContext& context, PostRecordingResult& result) {
    const std::scoped_lock lock(context.postRecordingResultMutex);
    if (!context.hasPostRecordingResult) {
        return false;
    }

    result = std::move(context.postRecordingResult);
    context.postRecordingResult = {};
    context.hasPostRecordingResult = false;
    return true;
}

void StoreLastRecordingPath(AppContext& context, std::filesystem::path recordingPath) {
    if (recordingPath.empty()) {
        return;
    }

    const std::scoped_lock lock(context.recentResultMutex);
    context.lastRecordingPath = std::move(recordingPath);
}

bool TryGetLastRecordingPath(AppContext& context, std::filesystem::path& recordingPath) {
    const std::scoped_lock lock(context.recentResultMutex);
    if (context.lastRecordingPath.empty()) {
        return false;
    }

    recordingPath = context.lastRecordingPath;
    return true;
}

bool HasLastRecordingPath(AppContext& context) {
    const std::scoped_lock lock(context.recentResultMutex);
    return !context.lastRecordingPath.empty();
}

void StoreLastTranscript(AppContext& context, std::wstring transcript) {
    if (transcript.empty()) {
        return;
    }

    const std::scoped_lock lock(context.recentResultMutex);
    context.lastTranscript = std::move(transcript);
    context.hasLastTranscript = true;
}

bool TryGetLastTranscript(AppContext& context, std::wstring& transcript) {
    const std::scoped_lock lock(context.recentResultMutex);
    if (!context.hasLastTranscript) {
        return false;
    }

    transcript = context.lastTranscript;
    return true;
}

bool HasLastTranscript(AppContext& context) {
    const std::scoped_lock lock(context.recentResultMutex);
    return context.hasLastTranscript;
}

bool OpenPathInExplorer(HWND ownerWindow, const std::filesystem::path& path, std::wstring& failureReason) {
    const std::wstring pathString = path.wstring();
    const INT_PTR shellResult = reinterpret_cast<INT_PTR>(
        ShellExecuteW(ownerWindow, L"open", pathString.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (shellResult > 32) {
        return true;
    }

    failureReason = L"Could not open '";
    failureReason += pathString;
    failureReason += L"' in Explorer (ShellExecuteW code ";
    failureReason += std::to_wstring(shellResult);
    failureReason += L").";
    return false;
}

void CompletePostRecordingWithError(AppContext& context, std::wstring errorTitle, std::wstring failureReason) {
    StorePostRecordingResult(
        context,
        PostRecordingResult{
            .success = false,
            .showDone = false,
            .errorTitle = std::move(errorTitle),
            .failureReason = std::move(failureReason)});
    PostMessageW(context.window, kPostRecordingCompleteMessage, 0, 0);
}

void CompletePostRecordingSuccessfully(AppContext& context, bool showDone) {
    StorePostRecordingResult(
        context,
        PostRecordingResult{
            .success = true,
            .showDone = showDone,
            .errorTitle = {},
            .failureReason = {}});
    PostMessageW(context.window, kPostRecordingCompleteMessage, 0, 0);
}

bool ApplyStartupRegistrationFromConfig(AppContext& context, std::wstring& failureReason) {
    if (context.options.smokeTest) {
        return true;
    }

    return ApplyStartupRegistration(context.config.system, failureReason);
}

} // namespace voxinsert