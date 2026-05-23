#include "runtime/app_host.h"

#include "audio/audio_recorder.h"
#include "audio/wav_writer.h"
#include "config/app_config.h"
#include "input/hotkey_manager.h"
#include "insertion/text_injector.h"
#include "observability/logging.h"
#include "security/openai_credential_store.h"
#include "testing/app_host_smoke_test.h"
#include "transcription/transcription_client.h"
#include "ui/status_pill.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace voxinsert {
namespace {

constexpr wchar_t kHiddenWindowClassName[] = L"VoxInsert.HiddenWindow";
constexpr wchar_t kHiddenWindowTitle[] = L"VoxInsert.HiddenWindow";
constexpr wchar_t kTrayTooltip[] = L"VoxInsert";
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kPostRecordingPhaseMessage = WM_APP + 2;
constexpr UINT kPostRecordingCompleteMessage = WM_APP + 3;
constexpr UINT kTrayMenuCommandSetOpenAiKey = 1001;
constexpr UINT kTrayMenuCommandRemoveOpenAiKey = 1002;
constexpr UINT kTrayMenuCommandQuit = 1003;

enum class AppState {
    Idle,
    Recording,
    SavingRecording,
    Transcribing,
    Inserting
};

struct PostRecordingResult {
    bool success = false;
    bool showDone = false;
    std::wstring errorTitle;
    std::wstring failureReason;
};

// Groups the long-lived state owned by the hidden host window and its helper modules.
struct AppContext {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    std::shared_ptr<spdlog::logger> logger;
    AppHostOptions options{};
    AppConfig config;
    AudioRecorder audioRecorder;
    AppHostSmokeTest smokeTest;
    HotkeyManager hotkeyManager;
    TextInjector textInjector;
    TranscriptionClient transcriptionClient;
    StatusPill statusPill;
    WavWriter wavWriter;
    std::thread postRecordingWorker;
    std::mutex postRecordingResultMutex;
    PostRecordingResult postRecordingResult;
    bool hasPostRecordingResult = false;
    std::atomic<bool> shutdownRequested = false;
    NOTIFYICONDATAW trayIconData{};
    HICON trayIcon = nullptr;
    bool trayIconAdded = false;
    AppState state = AppState::Idle;
    std::filesystem::path lastRecordingPath;
    UINT taskbarCreatedMessage = 0;
    int exitCode = 0;
};

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

std::wstring BuildTrayTooltip(AppState state) {
    std::wstring tooltip(kTrayTooltip);
    tooltip += L" - ";
    tooltip += StateLabel(state);
    return tooltip;
}

bool UpdateTrayTooltip(AppContext& context) {
    if (!context.trayIconAdded) {
        return true;
    }

    NOTIFYICONDATAW trayIconData = context.trayIconData;
    trayIconData.uFlags = NIF_TIP | NIF_SHOWTIP;

    const std::wstring tooltip = BuildTrayTooltip(context.state);
    wcscpy_s(trayIconData.szTip, ARRAYSIZE(trayIconData.szTip), tooltip.c_str());

    if (Shell_NotifyIconW(NIM_MODIFY, &trayIconData) == 0) {
        context.logger->warn(
            "Shell_NotifyIconW(NIM_MODIFY) failed while updating tray tooltip: {}",
            Utf8FromWide(FormatWin32Error(GetLastError())));
        return false;
    }

    context.trayIconData = trayIconData;
    return true;
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
    UpdateTrayTooltip(context);

    switch (nextState) {
    case AppState::Idle:
        context.statusPill.SetState(StatusPillState::Idle);
        break;
    case AppState::Recording:
        context.statusPill.SetState(StatusPillState::Recording);
        break;
    case AppState::SavingRecording:
    case AppState::Transcribing:
    case AppState::Inserting:
        context.statusPill.SetState(StatusPillState::Working);
        break;
    }
}

void ShowRuntimeError(AppContext& context, const wchar_t* title, const std::wstring& message) {
    context.logger->error("{}", Utf8FromWide(message));
    context.statusPill.SetState(StatusPillState::Error, message);
    if (!context.options.smokeTest) {
        MessageBoxW(context.window, message.c_str(), title, MB_OK | MB_ICONERROR);
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

void CompletePostRecordingWithError(
    AppContext& context,
    std::wstring errorTitle,
    std::wstring failureReason) {
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

std::wstring CredentialTargetLabel(const AppContext& context) {
    return WideFromUtf8(context.config.transcription.credentialTarget);
}

void ShowRuntimeInfo(AppContext& context, const wchar_t* title, const std::wstring& message) {
    context.logger->info("{}", Utf8FromWide(message));
    if (!context.options.smokeTest) {
        MessageBoxW(context.window, message.c_str(), title, MB_OK | MB_ICONINFORMATION);
    }
}

void ConfigureOpenAiCredential(AppContext& context) {
    std::wstring failureReason;
    switch (PromptForOpenAiCredential(context.instance, context.window, context.config.transcription, failureReason)) {
    case OpenAiCredentialPromptResult::Saved:
        ShowRuntimeInfo(
            context,
            L"VoxInsert OpenAI setup",
            L"The OpenAI API key was saved to Windows Credential Manager.");
        return;

    case OpenAiCredentialPromptResult::Cancelled:
        context.logger->info("OpenAI credential onboarding cancelled");
        return;

    case OpenAiCredentialPromptResult::Failed:
        ShowRuntimeError(context, L"VoxInsert OpenAI setup failed", failureReason);
        return;
    }
}

void RemoveOpenAiCredentialFromStore(AppContext& context) {
    std::wstring prompt = L"Remove the stored OpenAI API key for credential target '";
    prompt += CredentialTargetLabel(context);
    prompt += L"'?";

    if (!context.options.smokeTest) {
        const int response = MessageBoxW(
            context.window,
            prompt.c_str(),
            L"VoxInsert OpenAI setup",
            MB_YESNO | MB_ICONQUESTION);
        if (response != IDYES) {
            context.logger->info("OpenAI credential removal cancelled");
            return;
        }
    }

    bool removed = false;
    std::wstring failureReason;
    if (!RemoveOpenAiCredential(context.config.transcription, removed, failureReason)) {
        ShowRuntimeError(context, L"VoxInsert OpenAI setup failed", failureReason);
        return;
    }

    if (removed) {
        ShowRuntimeInfo(
            context,
            L"VoxInsert OpenAI setup",
            L"The stored OpenAI API key was removed from Windows Credential Manager.");
        return;
    }

    std::wstring message = L"No stored OpenAI API key was found for credential target '";
    message += CredentialTargetLabel(context);
    message += L"'.";
    ShowRuntimeInfo(context, L"VoxInsert OpenAI setup", message);
}

void CheckTranscriptionCredentialOnStartup(AppContext& context) {
    if (context.options.smokeTest) {
        return;
    }

    bool exists = false;
    std::wstring failureReason;
    if (!CheckOpenAiCredentialExists(context.config.transcription, exists, failureReason)) {
        ShowRuntimeError(context, L"VoxInsert OpenAI setup failed", failureReason);
        return;
    }

    if (exists) {
        context.logger->info(
            "OpenAI credential found for target {}",
            Utf8FromWide(CredentialTargetLabel(context)));
        return;
    }

    std::wstring message = L"No OpenAI API key is stored for credential target '";
    message += CredentialTargetLabel(context);
    message += L"'.\n\nWould you like to set it up now?";

    context.logger->warn(
        "OpenAI credential missing for target {}",
        Utf8FromWide(CredentialTargetLabel(context)));

    if (MessageBoxW(
            context.window,
            message.c_str(),
            L"VoxInsert OpenAI setup",
            MB_YESNO | MB_ICONWARNING) == IDYES) {
        ConfigureOpenAiCredential(context);
    }
}

void StartRecording(AppContext& context) {
    std::wstring failureReason;
    if (!context.audioRecorder.Start(
            context.config.audio,
            [&context](float rms) {
                context.statusPill.PostAmplitudeSample(rms);
            },
            failureReason)) {
        ShowRuntimeError(context, L"VoxInsert microphone error", failureReason);
        SetState(context, AppState::Idle);
        return;
    }

    context.logger->info(
        "recording started using the current default microphone: {}",
        Utf8FromWide(context.audioRecorder.ActiveDeviceName()));
    SetState(context, AppState::Recording);
}

void RunPostRecordingPipeline(AppContext& context) noexcept {
    try {
        std::vector<int16_t> samples;
        std::wstring failureReason;
        if (!context.audioRecorder.Stop(samples, failureReason)) {
            CompletePostRecordingWithError(context, L"VoxInsert recording error", failureReason);
            return;
        }

        std::filesystem::path wavPath;
        if (!context.wavWriter.WritePcm16Mono(samples, context.config.audio.sampleRate, wavPath, failureReason)) {
            CompletePostRecordingWithError(context, L"VoxInsert WAV error", failureReason);
            return;
        }

        context.lastRecordingPath = wavPath;
        context.logger->info("recording written to {}", Utf8FromWide(wavPath.wstring()));

        if (context.options.smokeTest) {
            context.logger->info("smoke-test: skipping transcription upload");
            CompletePostRecordingSuccessfully(context, false);
            return;
        }

        if (context.shutdownRequested) {
            return;
        }

        PostMessageW(context.window, kPostRecordingPhaseMessage, static_cast<WPARAM>(static_cast<int>(AppState::Transcribing)), 0);

        std::string transcript;
        if (!context.transcriptionClient.Transcribe(context.config.transcription, wavPath, transcript, failureReason)) {
            CompletePostRecordingWithError(context, L"VoxInsert transcription error", failureReason);
            return;
        }

        context.logger->info("transcription text: {}", transcript);

        if (context.shutdownRequested) {
            return;
        }

        PostMessageW(context.window, kPostRecordingPhaseMessage, static_cast<WPARAM>(static_cast<int>(AppState::Inserting)), 0);

        const std::wstring transcriptWide = WideFromUtf8(transcript);
        if (!context.textInjector.InsertText(context.window, context.config.insertion, transcriptWide, failureReason)) {
            CompletePostRecordingWithError(context, L"VoxInsert insertion error", failureReason);
            return;
        }

        context.logger->info("transcription inserted into the focused text field via clipboard paste");
        CompletePostRecordingSuccessfully(context, true);
    }
    catch (const std::exception& exception) {
        CompletePostRecordingWithError(
            context,
            L"VoxInsert transcription error",
            L"Unexpected background transcription failure: " + WideFromUtf8(exception.what()));
    }
    catch (...) {
        CompletePostRecordingWithError(
            context,
            L"VoxInsert transcription error",
            L"Unexpected background transcription failure.");
    }
}

void StopRecordingAndWriteWav(AppContext& context) {
    SetState(context, AppState::SavingRecording);
    JoinPostRecordingWorker(context);

    try {
        context.postRecordingWorker = std::thread([&context]() {
            RunPostRecordingPipeline(context);
        });
    }
    catch (const std::exception& exception) {
        context.audioRecorder.Cancel();
        std::wstring failureReason = L"Could not start the background transcription worker: ";
        failureReason += WideFromUtf8(exception.what());
        ShowRuntimeError(context, L"VoxInsert transcription error", failureReason);
        SetState(context, AppState::Idle);
    }
}

void HandlePostRecordingPhase(AppContext& context, WPARAM phaseParam) {
    const AppState phase = static_cast<AppState>(static_cast<int>(phaseParam));
    if (phase != AppState::Transcribing && phase != AppState::Inserting) {
        context.logger->warn("unknown post-recording phase received: {}", static_cast<int>(phaseParam));
        return;
    }

    if (context.state == AppState::Idle) {
        context.logger->info("post-recording phase ignored because the app is already idle");
        return;
    }

    SetState(context, phase);
}

void HandlePostRecordingComplete(AppContext& context) {
    PostRecordingResult result;
    if (!TakePostRecordingResult(context, result)) {
        context.logger->warn("post-recording completion message received without a result");
        JoinPostRecordingWorker(context);
        SetState(context, AppState::Idle);
        return;
    }

    JoinPostRecordingWorker(context);

    if (!result.success) {
        ShowRuntimeError(context, result.errorTitle.c_str(), result.failureReason);
        SetState(context, AppState::Idle);
        return;
    }

    if (result.showDone) {
        context.statusPill.SetState(StatusPillState::Done);
    }

    SetState(context, AppState::Idle);
}

void CancelRecording(AppContext& context) {
    context.audioRecorder.Cancel();
    context.logger->info("recording cancelled");
    SetState(context, AppState::Idle);
}

void HandleHotkey(AppContext& context, WPARAM hotkeyId) {
    switch (hotkeyId) {
    case HotkeyManager::kToggleRecordingHotkeyId:
        if (context.state == AppState::Idle) {
            StartRecording(context);
        }
        else if (context.state == AppState::Recording) {
            StopRecordingAndWriteWav(context);
        }
        else {
            context.logger->info("toggle hotkey ignored while state is {}", Utf8FromWide(StateLabel(context.state)));
        }
        return;

    case HotkeyManager::kCancelRecordingHotkeyId:
        if (context.state == AppState::Recording) {
            CancelRecording(context);
        }
        else {
            context.logger->info("cancel hotkey ignored while state is {}", Utf8FromWide(StateLabel(context.state)));
        }
        return;
    }

    context.logger->warn("unknown hotkey id received: {}", hotkeyId);
}

HICON LoadSharedClassIcon(HINSTANCE instance, int width, int height) {
    HICON iconHandle = reinterpret_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(IDI_APP_ICON),
        IMAGE_ICON,
        width,
        height,
        LR_DEFAULTCOLOR | LR_SHARED));

    if (iconHandle == nullptr) {
        iconHandle = reinterpret_cast<HICON>(LoadImageW(
            nullptr,
            IDI_APPLICATION,
            IMAGE_ICON,
            width,
            height,
            LR_DEFAULTCOLOR | LR_SHARED));
    }

    return iconHandle;
}

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
    const std::wstring tooltip = BuildTrayTooltip(context.state);
    wcscpy_s(trayIconData.szTip, ARRAYSIZE(trayIconData.szTip), tooltip.c_str());

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

    const std::wstring menuTitle = std::wstring(L"VoxInsert - ") + std::wstring(StateLabel(context.state));
    AppendMenuW(menuHandle, MF_STRING | MF_DISABLED, 0, menuTitle.c_str());
    AppendMenuW(menuHandle, MF_SEPARATOR, 0, nullptr);

    bool credentialExists = false;
    std::wstring failureReason;
    if (!CheckOpenAiCredentialExists(context.config.transcription, credentialExists, failureReason)) {
        context.logger->warn("OpenAI credential check failed while building tray menu: {}", Utf8FromWide(failureReason));
    }

    AppendMenuW(menuHandle, MF_STRING, kTrayMenuCommandSetOpenAiKey, L"Set OpenAI Key...");
    AppendMenuW(
        menuHandle,
        credentialExists ? MF_STRING : (MF_STRING | MF_GRAYED),
        kTrayMenuCommandRemoveOpenAiKey,
        L"Remove OpenAI Key");
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

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wordParam, LPARAM longParam) {
    AppContext* context = reinterpret_cast<AppContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* createStruct = reinterpret_cast<const CREATESTRUCTW*>(longParam);
        context = static_cast<AppContext*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        context->window = window;
    }

    if (context != nullptr && context->taskbarCreatedMessage != 0 && message == context->taskbarCreatedMessage) {
        context->logger->info("TaskbarCreated received; restoring tray icon");
        if (!AddTrayIcon(*context)) {
            context->exitCode = 1;
            DestroyWindow(window);
        }
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        if (context != nullptr && context->logger != nullptr) {
            context->logger->info("hidden control window created");
            context->logger->info("config file: {}", Utf8FromWide(context->config.configFilePath));

            if (!AddTrayIcon(*context)) {
                context->exitCode = 1;
                return -1;
            }

            std::wstring statusPillFailureReason;
            if (!context->statusPill.Create(
                    context->instance,
                    window,
                    kTrayIconId,
                    context->config.ui.showStatusPill,
                    context->logger,
                    statusPillFailureReason)) {
                context->logger->warn("status pill disabled: {}", Utf8FromWide(statusPillFailureReason));
            }

            std::wstring hotkeyFailureReason;
            if (!context->hotkeyManager.RegisterHotkeys(
                    window,
                    context->config.toggleRecordingHotkey,
                    context->config.cancelRecordingHotkey,
                    hotkeyFailureReason)) {
                context->exitCode = 1;
                context->logger->error("RegisterHotKey failed: {}", Utf8FromWide(hotkeyFailureReason));
                MessageBoxW(window, hotkeyFailureReason.c_str(), L"VoxInsert startup failed", MB_OK | MB_ICONERROR);
                RemoveTrayIcon(*context);
                return -1;
            }

            context->logger->info(
                "global hotkeys registered: toggle={}, cancel={}",
                Utf8FromWide(context->config.toggleRecordingHotkey.displayName),
                Utf8FromWide(context->config.cancelRecordingHotkey.displayName));

            CheckTranscriptionCredentialOnStartup(*context);

            if (context->options.smokeTest) {
                if (!context->smokeTest.ArmInitialTimer(window, context->logger)) {
                    context->exitCode = 1;
                    context->hotkeyManager.UnregisterAll(window);
                    RemoveTrayIcon(*context);
                    return -1;
                }

                context->logger->info("smoke-test timer armed");
            }
        }
        return 0;

    case WM_TIMER:
        if (wordParam == AppHostSmokeTest::kTimerId) {
            KillTimer(window, AppHostSmokeTest::kTimerId);
            if (context != nullptr) {
                if (!context->smokeTest.HandleTimer(window, context->logger)) {
                    context->exitCode = 1;
                    DestroyWindow(window);
                }
            }
            return 0;
        }
        break;

    case WM_HOTKEY:
        if (context != nullptr && context->logger != nullptr) {
            HandleHotkey(*context, wordParam);
        }
        return 0;

    case kPostRecordingPhaseMessage:
        if (context != nullptr && context->logger != nullptr) {
            HandlePostRecordingPhase(*context, wordParam);
        }
        return 0;

    case kPostRecordingCompleteMessage:
        if (context != nullptr && context->logger != nullptr) {
            HandlePostRecordingComplete(*context);
        }
        return 0;

    case WM_COMMAND:
        if (context != nullptr && context->logger != nullptr) {
            switch (LOWORD(wordParam)) {
            case kTrayMenuCommandSetOpenAiKey:
                context->logger->info("tray Set OpenAI Key selected");
                ConfigureOpenAiCredential(*context);
                return 0;

            case kTrayMenuCommandRemoveOpenAiKey:
                context->logger->info("tray Remove OpenAI Key selected");
                RemoveOpenAiCredentialFromStore(*context);
                return 0;

            case kTrayMenuCommandQuit:
                context->logger->info("tray Quit selected");
                DestroyWindow(window);
                return 0;
            }
        }
        break;

    case kTrayCallbackMessage:
        if (context != nullptr) {
            const UINT trayEvent = LOWORD(static_cast<DWORD>(longParam));
            switch (trayEvent) {
            case WM_CONTEXTMENU:
            case WM_RBUTTONUP:
                context->logger->info("tray icon context menu requested");
                ShowTrayMenu(*context);
                return 0;

            case NIN_SELECT:
            case NIN_KEYSELECT:
            case WM_LBUTTONDBLCLK:
                context->logger->info("tray icon double-clicked");
                ShowTrayMenu(*context);
                return 0;
            }
        }
        break;

    case WM_CLOSE:
        if (context != nullptr && context->logger != nullptr) {
            context->logger->info("WM_CLOSE received; destroying hidden window");
        }
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        if (context != nullptr && context->logger != nullptr) {
            context->logger->info("WM_DESTROY received; cleaning up tray icon");
            KillTimer(window, AppHostSmokeTest::kTimerId);
            context->shutdownRequested = true;
            if (context->state == AppState::Recording) {
                context->audioRecorder.Cancel();
            }
            JoinPostRecordingWorker(*context);
            context->statusPill.Destroy();
            context->hotkeyManager.UnregisterAll(window);
            context->logger->info("global hotkeys unregistered");
            RemoveTrayIcon(*context);
            context->logger->info("posting quit message");
        }
        PostQuitMessage(context != nullptr ? context->exitCode : 0);
        return 0;
    }

    return DefWindowProcW(window, message, wordParam, longParam);
}

bool RegisterHiddenWindowClass(HINSTANCE instance, const std::shared_ptr<spdlog::logger>& logger) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadSharedClassIcon(instance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    windowClass.hIconSm = LoadSharedClassIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    windowClass.lpszClassName = kHiddenWindowClassName;

    if (RegisterClassExW(&windowClass) != 0) {
        logger->info("hidden window class registered");
        return true;
    }

    const DWORD errorCode = GetLastError();
    logger->error("RegisterClassExW failed: {}", Utf8FromWide(FormatWin32Error(errorCode)));
    return false;
}

HWND CreateHiddenWindow(AppContext& context) {
    HWND windowHandle = CreateWindowExW(
        0,
        kHiddenWindowClassName,
        kHiddenWindowTitle,
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        context.instance,
        &context);

    if (windowHandle == nullptr) {
        context.logger->error("CreateWindowExW failed: {}", Utf8FromWide(FormatWin32Error(GetLastError())));
        return nullptr;
    }

    return windowHandle;
}

int RunMessageLoop(const std::shared_ptr<spdlog::logger>& logger) {
    MSG message{};

    for (;;) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);

        if (result > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }

        if (result == 0) {
            const int exitCode = static_cast<int>(message.wParam);
            logger->info("message loop exited with code {}", exitCode);
            return exitCode;
        }

        logger->error("GetMessageW failed: {}", Utf8FromWide(FormatWin32Error(GetLastError())));
        return 1;
    }
}

} // namespace

AppHostOptions ParseAppHostOptions() {
    AppHostOptions options{};

    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return options;
    }

    for (int argumentIndex = 1; argumentIndex < argumentCount; ++argumentIndex) {
        const std::wstring_view argument(arguments[argumentIndex]);
        if (argument == L"--smoke-test") {
            options.smokeTest = true;
        }
    }

    LocalFree(arguments);
    return options;
}

int RunAppHost(
    HINSTANCE instance,
    int showCommand,
    const std::shared_ptr<spdlog::logger>& logger,
    const AppHostOptions& options) {
    (void)showCommand;

    AppContext context{};
    context.instance = instance;
    context.logger = logger;
    context.options = options;
    context.taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    if (context.options.smokeTest) {
        logger->info("running in smoke-test mode");
    }

    std::wstring configFailureReason;
    if (!LoadAppConfig(context.config, configFailureReason)) {
        logger->error("LoadAppConfig failed: {}", Utf8FromWide(configFailureReason));
        MessageBoxW(nullptr, configFailureReason.c_str(), L"VoxInsert startup failed", MB_OK | MB_ICONERROR);
        logger->info("VoxInsert exiting after config load failure");
        return 1;
    }

    if (!RegisterHiddenWindowClass(instance, logger)) {
        logger->info("VoxInsert exiting after startup failure");
        return 1;
    }

    if (CreateHiddenWindow(context) == nullptr) {
        logger->info("VoxInsert exiting after hidden window creation failure");
        return 1;
    }

    return RunMessageLoop(logger);
}

} // namespace voxinsert