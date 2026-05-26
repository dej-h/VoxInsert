#pragma once

#include <windows.h>

#include <memory>
#include <string>

#include <spdlog/logger.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>

namespace voxinsert {

class SmtcController {
public:
    SmtcController() = default;
    SmtcController(const SmtcController&) = delete;
    SmtcController& operator=(const SmtcController&) = delete;
    SmtcController(SmtcController&&) = delete;
    SmtcController& operator=(SmtcController&&) = delete;
    ~SmtcController();

    bool Apply(
        HWND ownerWindow,
        UINT toggleMessage,
        bool enabled,
        const std::shared_ptr<spdlog::logger>& logger,
        std::wstring& failureReason);

    void SyncPlaybackActive(bool recordingActive);
    void Shutdown();

private:
    winrt::Windows::Media::SystemMediaTransportControls smtc_{nullptr};
    winrt::event_token buttonPressedToken_{};
    std::shared_ptr<spdlog::logger> logger_;
    HWND ownerWindow_ = nullptr;
    UINT toggleMessage_ = 0;
    bool buttonPressedRegistered_ = false;
};

} // namespace voxinsert