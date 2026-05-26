#include "runtime/runtime_config_applier.h"

#include "runtime/app_host_internal.h"

namespace voxinsert {
namespace {

bool RestoreRegisteredHotkeys(AppContext& context, const AppConfig& config, std::wstring& failureReason) {
    return context.hotkeyManager.RegisterHotkeys(
        context.window,
        config.toggleRecordingHotkey,
        config.cancelRecordingHotkey,
        failureReason);
}

void RestoreUiConfig(AppContext& context, const UiConfig& previousUiConfig, const AppConfig& restoredConfig) {
    context.config = restoredConfig;
    RecreateStatusPillIfNeeded(context, previousUiConfig);
    ResetTrayStatusToCurrentState(context);
}

} // namespace

bool ApplyRuntimeConfig(AppContext& context, AppConfig nextConfig, std::wstring& failureReason) {
    const AppConfig previousConfig = context.config;
    if (!context.hotkeyManager.RegisterHotkeys(
            context.window,
            nextConfig.toggleRecordingHotkey,
            nextConfig.cancelRecordingHotkey,
            failureReason)) {
        std::wstring restoreFailureReason;
        if (!RestoreRegisteredHotkeys(context, previousConfig, restoreFailureReason)) {
            failureReason += L" The previous hotkeys could not be restored: ";
            failureReason += restoreFailureReason;
        }

        return false;
    }

    const UiConfig appliedUiConfig = nextConfig.ui;
    context.config = std::move(nextConfig);
    RecreateStatusPillIfNeeded(context, previousConfig.ui);

    if (!ApplyStartupRegistrationFromConfig(context, failureReason)) {
        std::wstring restoreFailureReason;
        if (!RestoreRegisteredHotkeys(context, previousConfig, restoreFailureReason)) {
            failureReason += L" The previous hotkeys could not be restored: ";
            failureReason += restoreFailureReason;
        }

        RestoreUiConfig(context, appliedUiConfig, previousConfig);

        std::wstring restoreStartupFailureReason;
        if (!ApplyStartupRegistrationFromConfig(context, restoreStartupFailureReason)) {
            failureReason += L" The previous startup registration could not be restored: ";
            failureReason += restoreStartupFailureReason;
        }

        return false;
    }

    if (!context.smtcController.Apply(
            context.window,
            kSmtcToggleMessage,
            context.config.system.useMediaPlayPauseToggle,
            context.logger,
            failureReason)) {
        std::wstring restoreFailureReason;
        if (!RestoreRegisteredHotkeys(context, previousConfig, restoreFailureReason)) {
            failureReason += L" The previous hotkeys could not be restored: ";
            failureReason += restoreFailureReason;
        }

        RestoreUiConfig(context, appliedUiConfig, previousConfig);

        std::wstring restoreStartupFailureReason;
        if (!ApplyStartupRegistrationFromConfig(context, restoreStartupFailureReason)) {
            failureReason += L" The previous startup registration could not be restored: ";
            failureReason += restoreStartupFailureReason;
        }

        std::wstring restoreSmtcFailureReason;
        if (!context.smtcController.Apply(
                context.window,
                kSmtcToggleMessage,
                context.config.system.useMediaPlayPauseToggle,
                context.logger,
                restoreSmtcFailureReason)) {
            failureReason += L" The previous media Play/Pause integration could not be restored: ";
            failureReason += restoreSmtcFailureReason;
        }
        else {
            context.smtcController.SyncPlaybackActive(context.state == AppState::Recording);
        }

        return false;
    }

    context.smtcController.SyncPlaybackActive(context.state == AppState::Recording);

    ResetTrayStatusToCurrentState(context);
    return true;
}

} // namespace voxinsert