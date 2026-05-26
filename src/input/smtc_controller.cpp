#include "input/smtc_controller.h"

#include "observability/logging.h"

#include <systemmediatransportcontrolsinterop.h>

#include <cstdio>

namespace voxinsert {
namespace {

std::wstring FormatHResult(HRESULT result) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(result));
    return buffer;
}

std::wstring BuildFailureReason(HRESULT result, std::wstring_view detail) {
    std::wstring message(detail);
    message += L" (HRESULT ";
    message += FormatHResult(result);
    message += L").";
    return message;
}

} // namespace

SmtcController::~SmtcController() {
    Shutdown();
}

bool SmtcController::Apply(
    HWND ownerWindow,
    UINT toggleMessage,
    bool enabled,
    const std::shared_ptr<spdlog::logger>& logger,
    std::wstring& failureReason) {
    logger_ = logger;

    if (!enabled) {
        Shutdown();
        return true;
    }

    if (smtc_ != nullptr && ownerWindow_ == ownerWindow && toggleMessage_ == toggleMessage) {
        return true;
    }

    Shutdown();

    if (ownerWindow == nullptr) {
        failureReason = L"Cannot enable headset/media Play/Pause toggle before the hidden window exists.";
        return false;
    }

    try {
        auto interop = winrt::get_activation_factory<winrt::Windows::Media::SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();

        winrt::Windows::Media::SystemMediaTransportControls smtc{nullptr};
        const HRESULT result = interop->GetForWindow(
            ownerWindow,
            winrt::guid_of<winrt::Windows::Media::ISystemMediaTransportControls>(),
            reinterpret_cast<void**>(winrt::put_abi(smtc)));
        if (FAILED(result)) {
            failureReason = BuildFailureReason(result, L"Could not acquire SystemMediaTransportControls for the VoxInsert window");
            return false;
        }

        smtc.IsEnabled(true);
        smtc.IsPlayEnabled(true);
        smtc.IsPauseEnabled(true);
        smtc.IsStopEnabled(false);
        smtc.IsNextEnabled(false);
        smtc.IsPreviousEnabled(false);

        auto updater = smtc.DisplayUpdater();
        updater.Type(winrt::Windows::Media::MediaPlaybackType::Music);
        updater.MusicProperties().Title(L"VoxInsert");
        updater.MusicProperties().Artist(L"Dictation control");
        updater.Update();

        buttonPressedToken_ = smtc.ButtonPressed(
            [windowHandle = ownerWindow, messageId = toggleMessage](
                const winrt::Windows::Media::SystemMediaTransportControls&,
                const winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs& args) {
                const auto button = args.Button();
                if (button != winrt::Windows::Media::SystemMediaTransportControlsButton::Play &&
                    button != winrt::Windows::Media::SystemMediaTransportControlsButton::Pause) {
                    return;
                }

                PostMessageW(windowHandle, messageId, 0, 0);
            });

        smtc_ = std::move(smtc);
        ownerWindow_ = ownerWindow;
        toggleMessage_ = toggleMessage;
        buttonPressedRegistered_ = true;

        if (logger_ != nullptr) {
            logger_->info("SMTC media Play/Pause toggle enabled");
        }

        return true;
    }
    catch (const winrt::hresult_error& error) {
        failureReason = L"Could not enable headset/media Play/Pause toggle: ";
        failureReason += error.message().c_str();
        if (logger_ != nullptr) {
            logger_->warn("SMTC initialization failed: {}", Utf8FromWide(failureReason));
        }
        Shutdown();
        return false;
    }
}

void SmtcController::SyncPlaybackActive(bool recordingActive) {
    if (smtc_ == nullptr) {
        return;
    }

    smtc_.PlaybackStatus(
        recordingActive
            ? winrt::Windows::Media::MediaPlaybackStatus::Playing
            : winrt::Windows::Media::MediaPlaybackStatus::Paused);
}

void SmtcController::Shutdown() {
    if (smtc_ != nullptr) {
        if (buttonPressedRegistered_) {
            smtc_.ButtonPressed(buttonPressedToken_);
            buttonPressedRegistered_ = false;
        }

        smtc_.IsEnabled(false);
        smtc_ = nullptr;
    }

    ownerWindow_ = nullptr;
    toggleMessage_ = 0;
}

} // namespace voxinsert