#include "runtime/app_host_internal.h"

#include "observability/logging.h"
#include "security/openai_credential_store.h"
#include "ui/settings_dialog.h"

namespace voxinsert {

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

void RecreateStatusPillIfNeeded(AppContext& context, const UiConfig& previousUiConfig) {
    if (previousUiConfig.showStatusPill == context.config.ui.showStatusPill &&
        previousUiConfig.statusPillPlacement == context.config.ui.statusPillPlacement) {
        return;
    }

    context.statusPill.Destroy();
    if (!context.config.ui.showStatusPill) {
        context.logger->info("status pill disabled by reloaded config");
        return;
    }

    std::wstring statusPillFailureReason;
    if (!context.statusPill.Create(
            context.instance,
            context.window,
            kTrayIconId,
            context.config.ui.showStatusPill,
            context.config.ui.statusPillPlacement,
            context.logger,
            statusPillFailureReason)) {
        context.logger->warn("status pill disabled after config reload: {}", Utf8FromWide(statusPillFailureReason));
        ShowRuntimeInfo(context, L"VoxInsert config", L"Config reloaded, but the status pill could not be enabled.");
    }
}

void ReloadConfigFromTray(AppContext& context, const wchar_t* successMessage) {
    if (context.state != AppState::Idle) {
        ShowRuntimeInfo(
            context,
            L"VoxInsert config",
            L"Config can only be reloaded while VoxInsert is idle.");
        return;
    }

    AppConfig nextConfig;
    std::wstring failureReason;
    if (!LoadAppConfig(nextConfig, failureReason)) {
        ShowRuntimeError(context, L"VoxInsert config reload failed", failureReason);
        return;
    }

    const AppConfig previousConfig = context.config;
    if (!context.hotkeyManager.RegisterHotkeys(
            context.window,
            nextConfig.toggleRecordingHotkey,
            nextConfig.cancelRecordingHotkey,
            failureReason)) {
        std::wstring restoreFailureReason;
        if (!context.hotkeyManager.RegisterHotkeys(
                context.window,
                previousConfig.toggleRecordingHotkey,
                previousConfig.cancelRecordingHotkey,
                restoreFailureReason)) {
            failureReason += L" The previous hotkeys could not be restored: ";
            failureReason += restoreFailureReason;
        }

        ShowRuntimeError(context, L"VoxInsert config reload failed", failureReason);
        return;
    }

    context.config = std::move(nextConfig);
    RecreateStatusPillIfNeeded(context, previousConfig.ui);

    if (!ApplyStartupRegistrationFromConfig(context, failureReason)) {
        ShowRuntimeError(context, L"VoxInsert startup settings", failureReason);
        return;
    }

    ResetTrayStatusToCurrentState(context);

    context.logger->info(
        "config reloaded from {}",
        Utf8FromWide(context.config.configFilePath));
    context.logger->info(
        "global hotkeys reloaded: toggle={}, cancel={}",
        Utf8FromWide(context.config.toggleRecordingHotkey.displayName),
        Utf8FromWide(context.config.cancelRecordingHotkey.displayName));

    ShowRuntimeInfo(context, L"VoxInsert config", successMessage);
}

void ShowSettingsDialogFromTray(AppContext& context) {
    if (context.state != AppState::Idle) {
        ShowRuntimeInfo(
            context,
            L"VoxInsert settings",
            L"Settings can only be changed while VoxInsert is idle.");
        return;
    }

    SettingsDialogValues values{};
    values.toggleRecordingHotkey = context.config.toggleRecordingHotkey;
    values.cancelRecordingHotkey = context.config.cancelRecordingHotkey;
    values.statusPillPlacement = context.config.ui.statusPillPlacement;
    values.autoStartWithWindows = context.config.system.autoStartWithWindows;
    values.showStatusPill = context.config.ui.showStatusPill;

    context.hotkeyManager.UnregisterAll(context.window);
    context.settingsDialogOpen = true;
    const SettingsDialogResult dialogResult = ShowSettingsDialog(context.instance, context.window, values);
    context.settingsDialogOpen = false;

    if (dialogResult != SettingsDialogResult::Saved) {
        std::wstring restoreFailureReason;
        if (!context.hotkeyManager.RegisterHotkeys(
                context.window,
                context.config.toggleRecordingHotkey,
                context.config.cancelRecordingHotkey,
                restoreFailureReason)) {
            ShowRuntimeError(context, L"VoxInsert settings", restoreFailureReason);
            return;
        }

        context.logger->info("settings dialog cancelled");
        return;
    }

    AppSettingsUpdate settings{};
    settings.toggleRecordingHotkey = values.toggleRecordingHotkey;
    settings.cancelRecordingHotkey = values.cancelRecordingHotkey;
    settings.ui = context.config.ui;
    settings.system = context.config.system;
    settings.ui.showStatusPill = values.showStatusPill;
    settings.ui.statusPillPlacement = values.statusPillPlacement;
    settings.system.autoStartWithWindows = values.autoStartWithWindows;

    const bool hotkeysChanged =
        !SameHotkeyBinding(settings.toggleRecordingHotkey, context.config.toggleRecordingHotkey) ||
        !SameHotkeyBinding(settings.cancelRecordingHotkey, context.config.cancelRecordingHotkey);

    std::wstring failureReason;
    if (hotkeysChanged &&
        !context.hotkeyManager.RegisterHotkeys(
            context.window,
            settings.toggleRecordingHotkey,
            settings.cancelRecordingHotkey,
            failureReason)) {
        std::wstring restoreFailureReason;
        if (!context.hotkeyManager.RegisterHotkeys(
                context.window,
                context.config.toggleRecordingHotkey,
                context.config.cancelRecordingHotkey,
                restoreFailureReason)) {
            failureReason += L" The previous hotkeys could not be restored: ";
            failureReason += restoreFailureReason;
        }

        ShowRuntimeError(context, L"VoxInsert settings", failureReason);
        return;
    }

    if (!SaveAppSettings(context.config, settings, failureReason)) {
        std::wstring restoreFailureReason;
        if (!context.hotkeyManager.RegisterHotkeys(
                context.window,
                context.config.toggleRecordingHotkey,
                context.config.cancelRecordingHotkey,
                restoreFailureReason)) {
            failureReason += L" The previous hotkeys could not be restored: ";
            failureReason += restoreFailureReason;
        }

        ShowRuntimeError(context, L"VoxInsert settings", failureReason);
        return;
    }

    context.logger->info("settings saved to {}", Utf8FromWide(context.config.configFilePath));
    ReloadConfigFromTray(context, L"Settings saved.");
}

} // namespace voxinsert