#include "runtime/app_host_internal.h"

#include "observability/logging.h"
#include "resource.h"

#include <winrt/base.h>

#include <shellapi.h>

#include <cwchar>
#include <filesystem>
#include <string_view>
#include <vector>

namespace voxinsert {
namespace {

constexpr wchar_t kHiddenWindowClassName[] = L"VoxInsert.HiddenWindow";
constexpr wchar_t kHiddenWindowTitle[] = L"VoxInsert.HiddenWindow";
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
                    context->config.ui.statusPillPlacement,
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

            std::wstring smtcFailureReason;
            if (!context->smtcController.Apply(
                    window,
                    kSmtcToggleMessage,
                    context->config.system.useMediaPlayPauseToggle,
                    context->logger,
                    smtcFailureReason)) {
                context->logger->warn("SMTC media Play/Pause toggle unavailable: {}", Utf8FromWide(smtcFailureReason));
                ShowRuntimeInfo(*context, L"VoxInsert media button toggle", smtcFailureReason);
            }
            context->smtcController.SyncPlaybackActive(context->state == AppState::Recording);

            std::wstring startupRegistrationFailureReason;
            if (!ApplyStartupRegistrationFromConfig(*context, startupRegistrationFailureReason)) {
                ShowRuntimeError(*context, L"VoxInsert startup settings", startupRegistrationFailureReason);
            }

            if (context->options.openSettingsOnStartup) {
                context->logger->info("startup requested settings dialog");
                PostMessageW(window, kOpenSettingsMessage, 0, 0);
            }
            else {
                CheckTranscriptionCredentialOnStartup(*context);
            }

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
        if (wordParam == kTrayStatusResetTimerId) {
            KillTimer(window, kTrayStatusResetTimerId);
            if (context != nullptr) {
                ResetTrayStatusToCurrentState(*context);
            }
            return 0;
        }
        break;

    case WM_HOTKEY:
        if (context != nullptr && context->logger != nullptr) {
            if (context->settingsDialogOpen) {
                return 0;
            }

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

    case kOpenSettingsMessage:
        if (context != nullptr && context->logger != nullptr) {
            ShowSettingsDialogFromTray(*context);
        }
        return 0;

    case kSmtcToggleMessage:
        if (context != nullptr && context->logger != nullptr) {
            HandleSmtcToggle(*context);
        }
        return 0;

    case WM_COMMAND:
        if (context != nullptr && context->logger != nullptr) {
            switch (LOWORD(wordParam)) {
            case kTrayMenuCommandSettings:
                context->logger->info("tray Settings selected");
                ShowSettingsDialogFromTray(*context);
                return 0;

            case kTrayMenuCommandReloadConfig:
                context->logger->info("tray Reload config selected");
                ReloadConfigFromTray(*context);
                return 0;

            case kTrayMenuCommandCopyLastTranscript:
                context->logger->info("tray Copy Last Transcript selected");
                CopyLastTranscriptFromTray(*context);
                return 0;

            case kTrayMenuCommandReinsertLastTranscript:
                context->logger->info("tray Re-insert Last Transcript selected");
                ReinsertLastTranscriptFromTray(*context);
                return 0;

            case kTrayMenuCommandOpenLastRecordingFolder:
                context->logger->info("tray Open Last Recording Folder selected");
                OpenLastRecordingFolderFromTray(*context);
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
            KillTimer(window, kTrayStatusResetTimerId);
            context->shutdownRequested = true;
            if (context->state == AppState::Recording) {
                context->audioRecorder.Cancel();
            }
            JoinPostRecordingWorker(*context);
            context->archiveService.Shutdown();
            context->statusPill.Destroy();
            context->smtcController.Shutdown();
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

bool IsNonEmptyFile(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::is_regular_file(path, errorCode) &&
           !errorCode &&
           std::filesystem::file_size(path, errorCode) > 0 &&
           !errorCode;
}

bool RunArchiveSmokeTest(const std::shared_ptr<spdlog::logger>& logger) {
    std::error_code errorCode;
    const std::filesystem::path root = std::filesystem::temp_directory_path(errorCode) / L"VoxInsertArchiveSmoke";
    if (errorCode) {
        logger->error("archive-smoke-test: temp_directory_path failed: {}", errorCode.message());
        return false;
    }

    std::filesystem::remove_all(root, errorCode);
    std::filesystem::create_directories(root, errorCode);
    if (errorCode) {
        logger->error("archive-smoke-test: create_directories failed: {}", errorCode.message());
        return false;
    }

    AudioConfig audio{};
    audio.sampleRate = 16000;
    audio.channelCount = 1;

    ArchiveConfig archive{};
    archive.enabled = true;
    archive.persistAudio = true;
    archive.persistTranscript = true;
    archive.folderPath = root.wstring();
    archive.opusBitrateBps = 24000;

    TranscriptionConfig transcription{};
    transcription.provider = "openai";
    transcription.openAi.model = "archive-smoke-test";

    std::vector<int16_t> samples(static_cast<size_t>(audio.sampleRate));
    for (size_t index = 0; index < samples.size(); ++index) {
        samples[index] = ((index / 80) % 2 == 0) ? 1200 : -1200;
    }

    ArchiveService service;
    service.Enqueue(ArchiveRequest{
        .archive = archive,
        .audio = audio,
        .transcription = transcription,
        .samples = std::move(samples),
        .transcriptUtf8 = "archive smoke test transcript",
        .insertionSucceeded = true,
        .logger = logger,
    });
    service.Shutdown();

    bool foundOpus = false;
    bool foundText = false;
    bool foundJson = false;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, errorCode)) {
        if (errorCode) {
            break;
        }

        const std::filesystem::path path = entry.path();
        if (path.extension() == L".opus" && IsNonEmptyFile(path)) {
            foundOpus = true;
        }
        else if (path.extension() == L".txt" && IsNonEmptyFile(path)) {
            foundText = true;
        }
        else if (path.extension() == L".json" && IsNonEmptyFile(path)) {
            foundJson = true;
        }
    }

    std::filesystem::remove_all(root, errorCode);
    if (!foundOpus || !foundText || !foundJson) {
        logger->error(
            "archive-smoke-test failed: opus={}, text={}, json={}",
            foundOpus,
            foundText,
            foundJson);
        return false;
    }

    logger->info("archive-smoke-test passed");
    return true;
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
        else if (argument == L"--archive-smoke-test") {
            options.archiveSmokeTest = true;
        }
        else if (argument == L"--settings") {
            options.openSettingsOnStartup = true;
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

    if (context.options.archiveSmokeTest) {
        logger->info("running archive smoke test");
        return RunArchiveSmokeTest(logger) ? 0 : 1;
    }

    std::wstring configFailureReason;
    if (!LoadAppConfig(context.config, configFailureReason)) {
        logger->error("LoadAppConfig failed: {}", Utf8FromWide(configFailureReason));
        MessageBoxW(nullptr, configFailureReason.c_str(), L"VoxInsert startup failed", MB_OK | MB_ICONERROR);
        logger->info("VoxInsert exiting after config load failure");
        return 1;
    }

    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    }
    catch (const winrt::hresult_error& error) {
        std::wstring failureReason = L"winrt::init_apartment failed: ";
        failureReason += error.message().c_str();
        logger->error("{}", Utf8FromWide(failureReason));
        MessageBoxW(nullptr, failureReason.c_str(), L"VoxInsert startup failed", MB_OK | MB_ICONERROR);
        return 1;
    }

    struct ApartmentGuard {
        ~ApartmentGuard() {
            winrt::uninit_apartment();
        }
    } apartmentGuard;

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