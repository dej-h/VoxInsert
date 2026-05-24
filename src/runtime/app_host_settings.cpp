#include "runtime/app_host_internal.h"
#include "runtime/runtime_config_applier.h"

#include "observability/logging.h"
#include "security/api_credential_store.h"
#include "ui/settings_dialog.h"

#include <algorithm>
#include <utility>

namespace voxinsert {
namespace {

constexpr wchar_t kOpenAiCredentialUserName[] = L"openai";
constexpr wchar_t kMistralCredentialUserName[] = L"mistral";

std::wstring ProviderDisplayName(std::string_view provider) {
    if (provider == "mistral") {
        return L"Mistral";
    }

    return L"OpenAI";
}

std::wstring CredentialTargetForProvider(const TranscriptionConfig& config, std::string_view provider) {
    if (provider == "mistral") {
        return WideFromUtf8(config.mistral.credentialTarget);
    }

    return WideFromUtf8(config.openAi.credentialTarget);
}

std::wstring SelectedCredentialTarget(const TranscriptionConfig& config) {
    return CredentialTargetForProvider(config, config.provider);
}

std::wstring CredentialStatusText(
    std::wstring_view providerName,
    std::wstring_view targetName,
    const std::shared_ptr<spdlog::logger>& logger) {
    bool exists = false;
    std::wstring failureReason;
    if (!CheckApiCredentialExists(targetName, exists, failureReason)) {
        if (logger != nullptr) {
            logger->warn(
                "{} credential status check failed for target {}: {}",
                Utf8FromWide(providerName),
                Utf8FromWide(std::wstring(targetName)),
                Utf8FromWide(failureReason));
        }

        return L"Unavailable.";
    }

    return exists ? L"Present." : L"Missing.";
}

bool RemoveCredentialIfPresent(std::wstring_view targetName, std::wstring& failureReason) {
    if (targetName.empty()) {
        return true;
    }

    bool removed = false;
    return RemoveApiCredential(targetName, removed, failureReason);
}

bool ApplyProviderCredentialChanges(
    const wchar_t* providerName,
    std::wstring_view previousTarget,
    std::wstring_view nextTarget,
    std::wstring_view credentialUserName,
    std::wstring_view newApiKey,
    bool removeCredential,
    std::wstring& failureReason) {
    if (removeCredential) {
        if (!previousTarget.empty() && !RemoveCredentialIfPresent(previousTarget, failureReason)) {
            return false;
        }

        if (!nextTarget.empty() && nextTarget != previousTarget && !RemoveCredentialIfPresent(nextTarget, failureReason)) {
            return false;
        }

        return true;
    }

    if (!newApiKey.empty()) {
        if (!SaveApiCredential(nextTarget, credentialUserName, newApiKey, failureReason)) {
            failureReason.insert(0, std::wstring(providerName) + L": ");
            return false;
        }

        if (!previousTarget.empty() && previousTarget != nextTarget) {
            std::wstring removeFailureReason;
            if (!RemoveCredentialIfPresent(previousTarget, removeFailureReason)) {
                failureReason = providerName;
                failureReason += L": the API key was saved, but the old credential target could not be cleaned up: ";
                failureReason += removeFailureReason;
                return false;
            }
        }

        return true;
    }

    if (previousTarget.empty() || previousTarget == nextTarget) {
        return true;
    }

    bool previousCredentialExists = false;
    if (!CheckApiCredentialExists(previousTarget, previousCredentialExists, failureReason)) {
        failureReason.insert(0, std::wstring(providerName) + L": ");
        return false;
    }

    if (!previousCredentialExists) {
        return true;
    }

    std::wstring existingSecret;
    if (!TryReadApiCredentialSecret(previousTarget, existingSecret, failureReason)) {
        failureReason.insert(0, std::wstring(providerName) + L": ");
        return false;
    }

    const bool saveSucceeded = SaveApiCredential(nextTarget, credentialUserName, existingSecret, failureReason);
    std::fill(existingSecret.begin(), existingSecret.end(), L'\0');
    existingSecret.clear();
    if (!saveSucceeded) {
        failureReason.insert(0, std::wstring(providerName) + L": ");
        return false;
    }

    std::wstring removeFailureReason;
    if (!RemoveCredentialIfPresent(previousTarget, removeFailureReason)) {
        failureReason = providerName;
        failureReason += L": the API key was moved to the new credential target, but the old target could not be cleaned up: ";
        failureReason += removeFailureReason;
        return false;
    }

    return true;
}

bool ApplyCredentialChangesFromDialog(
    const AppConfig& previousConfig,
    const AppConfig& nextConfig,
    const SettingsDialogValues& values,
    std::wstring& failureReason) {
    if (!ApplyProviderCredentialChanges(
            L"OpenAI",
            WideFromUtf8(previousConfig.transcription.openAi.credentialTarget),
            WideFromUtf8(nextConfig.transcription.openAi.credentialTarget),
            kOpenAiCredentialUserName,
            values.openAiApiKey,
            values.removeOpenAiApiKey,
            failureReason)) {
        return false;
    }

    if (!ApplyProviderCredentialChanges(
            L"Mistral",
            WideFromUtf8(previousConfig.transcription.mistral.credentialTarget),
            WideFromUtf8(nextConfig.transcription.mistral.credentialTarget),
            kMistralCredentialUserName,
            values.mistralApiKey,
            values.removeMistralApiKey,
            failureReason)) {
        return false;
    }

    return true;
}

SettingsDialogValues BuildSettingsDialogValues(const AppContext& context) {
    SettingsDialogValues values{};
    values.toggleRecordingHotkey = context.config.toggleRecordingHotkey;
    values.cancelRecordingHotkey = context.config.cancelRecordingHotkey;
    values.transcriptionProvider = WideFromUtf8(context.config.transcription.provider);
    values.languageHint = WideFromUtf8(context.config.transcription.languageHint);
    values.openAiModel = WideFromUtf8(context.config.transcription.openAi.model);
    values.openAiCredentialTarget = WideFromUtf8(context.config.transcription.openAi.credentialTarget);
    values.openAiPrompt = WideFromUtf8(context.config.transcription.openAi.prompt);
    values.openAiCredentialStatus = CredentialStatusText(
        L"OpenAI",
        WideFromUtf8(context.config.transcription.openAi.credentialTarget),
        context.logger);
    values.mistralModel = WideFromUtf8(context.config.transcription.mistral.model);
    values.mistralCredentialTarget = WideFromUtf8(context.config.transcription.mistral.credentialTarget);
    values.mistralContextBias = WideFromUtf8(context.config.transcription.mistral.contextBias);
    values.mistralCredentialStatus = CredentialStatusText(
        L"Mistral",
        WideFromUtf8(context.config.transcription.mistral.credentialTarget),
        context.logger);
    values.statusPillPlacement = context.config.ui.statusPillPlacement;
    values.autoStartWithWindows = context.config.system.autoStartWithWindows;
    values.showStatusPill = context.config.ui.showStatusPill;
    values.archiveEnabled = context.config.archive.enabled;
    values.archivePersistTranscript = context.config.archive.persistTranscript;
    values.archivePersistAudio = context.config.archive.persistAudio;
    values.archiveFolderPath = context.config.archive.folderPath;
    return values;
}

AppConfig BuildConfigFromSettings(const AppConfig& currentConfig, const SettingsDialogValues& values) {
    AppConfig nextConfig = currentConfig;
    nextConfig.toggleRecordingHotkey = values.toggleRecordingHotkey;
    nextConfig.cancelRecordingHotkey = values.cancelRecordingHotkey;
    nextConfig.transcription.provider = Utf8FromWide(values.transcriptionProvider);
    nextConfig.transcription.languageHint = Utf8FromWide(values.languageHint);
    nextConfig.transcription.openAi.model = Utf8FromWide(values.openAiModel);
    nextConfig.transcription.openAi.credentialTarget = Utf8FromWide(values.openAiCredentialTarget);
    nextConfig.transcription.openAi.prompt = Utf8FromWide(values.openAiPrompt);
    nextConfig.transcription.mistral.model = Utf8FromWide(values.mistralModel);
    nextConfig.transcription.mistral.credentialTarget = Utf8FromWide(values.mistralCredentialTarget);
    nextConfig.transcription.mistral.contextBias = NormalizeMistralContextBias(Utf8FromWide(values.mistralContextBias));
    nextConfig.ui.showStatusPill = values.showStatusPill;
    nextConfig.ui.statusPillPlacement = values.statusPillPlacement;
    nextConfig.system.autoStartWithWindows = values.autoStartWithWindows;
    nextConfig.archive.enabled = values.archiveEnabled;
    nextConfig.archive.persistTranscript = values.archivePersistTranscript;
    nextConfig.archive.persistAudio = values.archivePersistAudio;
    nextConfig.archive.folderPath = values.archiveFolderPath.empty() ? DefaultArchiveFolderPath() : values.archiveFolderPath;
    return nextConfig;
}

AppSettingsUpdate BuildSettingsUpdate(const AppConfig& config) {
    AppSettingsUpdate settings{};
    settings.toggleRecordingHotkey = config.toggleRecordingHotkey;
    settings.cancelRecordingHotkey = config.cancelRecordingHotkey;
    settings.transcription = config.transcription;
    settings.ui = config.ui;
    settings.system = config.system;
    settings.archive = config.archive;
    return settings;
}

bool RestoreCurrentHotkeys(AppContext& context, std::wstring& failureReason) {
    return context.hotkeyManager.RegisterHotkeys(
        context.window,
        context.config.toggleRecordingHotkey,
        context.config.cancelRecordingHotkey,
        failureReason);
}

bool LoadAndApplyConfig(AppContext& context, std::wstring& failureReason) {
    AppConfig nextConfig;
    if (!LoadAppConfig(nextConfig, failureReason)) {
        return false;
    }

    return ApplyRuntimeConfig(context, std::move(nextConfig), failureReason);
}

bool ApplySettingsFromDialog(AppContext& context, const SettingsDialogValues& values, std::wstring& failureReason) {
    const AppConfig previousConfig = context.config;
    const AppConfig nextConfig = BuildConfigFromSettings(previousConfig, values);
    if (!ApplyCredentialChangesFromDialog(previousConfig, nextConfig, values, failureReason)) {
        return false;
    }

    if (!ApplyRuntimeConfig(context, nextConfig, failureReason)) {
        return false;
    }

    if (SaveAppSettings(context.config, BuildSettingsUpdate(context.config), failureReason)) {
        return true;
    }

    std::wstring restoreFailureReason;
    if (!ApplyRuntimeConfig(context, previousConfig, restoreFailureReason)) {
        failureReason += L" The previous settings could not be restored: ";
        failureReason += restoreFailureReason;
    }

    return false;
}

} // namespace

void CheckTranscriptionCredentialOnStartup(AppContext& context) {
    if (context.options.smokeTest) {
        return;
    }

    bool exists = false;
    std::wstring failureReason;
    const std::wstring credentialTarget = SelectedCredentialTarget(context.config.transcription);
    const std::wstring providerName = ProviderDisplayName(context.config.transcription.provider);
    if (!CheckApiCredentialExists(credentialTarget, exists, failureReason)) {
        ShowRuntimeError(context, L"VoxInsert transcription setup failed", failureReason);
        return;
    }

    if (exists) {
        context.logger->info(
            "{} credential found for target {}",
            Utf8FromWide(providerName),
            Utf8FromWide(credentialTarget));
        return;
    }

    std::wstring message = L"No ";
    message += providerName;
    message += L" API key is stored for credential target '";
    message += credentialTarget;
    message += L"'.\n\nWould you like to open Settings now?";

    context.logger->warn(
        "{} credential missing for target {}",
        Utf8FromWide(providerName),
        Utf8FromWide(credentialTarget));

    if (MessageBoxW(
            context.window,
            message.c_str(),
            L"VoxInsert transcription setup",
            MB_YESNO | MB_ICONWARNING) == IDYES) {
        ShowSettingsDialogFromTray(context);
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

    std::wstring failureReason;
    if (!LoadAndApplyConfig(context, failureReason)) {
        ShowRuntimeError(context, L"VoxInsert config reload failed", failureReason);
        return;
    }

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

    SettingsDialogValues values = BuildSettingsDialogValues(context);

    context.hotkeyManager.UnregisterAll(context.window);
    context.settingsDialogOpen = true;
    const SettingsDialogResult dialogResult = ShowSettingsDialog(context.instance, context.window, values);
    context.settingsDialogOpen = false;

    if (dialogResult != SettingsDialogResult::Saved) {
        std::wstring restoreFailureReason;
        if (!RestoreCurrentHotkeys(context, restoreFailureReason)) {
            ShowRuntimeError(context, L"VoxInsert settings", restoreFailureReason);
            return;
        }

        context.logger->info("settings dialog cancelled");
        return;
    }

    std::wstring failureReason;
    if (!ApplySettingsFromDialog(context, values, failureReason)) {
        ShowRuntimeError(context, L"VoxInsert settings", failureReason);
        return;
    }

    context.logger->info("settings saved to {}", Utf8FromWide(context.config.configFilePath));
    ShowRuntimeInfo(context, L"VoxInsert settings", L"Settings saved.");
}

} // namespace voxinsert