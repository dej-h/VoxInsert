#pragma once

#include <windows.h>

#include "config/app_config.h"

#include <string>

namespace voxinsert {

// Owns the small v0.1 global hotkey surface so the host only handles state transitions.
class HotkeyManager {
public:
    static constexpr int kToggleRecordingHotkeyId = 1;
    static constexpr int kCancelRecordingHotkeyId = 2;

    bool RegisterHotkeys(
        HWND window,
        const HotkeyBinding& toggleRecordingHotkey,
        const HotkeyBinding& cancelRecordingHotkey,
        std::wstring& failureReason) noexcept;
    void UnregisterAll(HWND window) noexcept;

private:
    bool RegisterHotkey(
        HWND window,
        int hotkeyId,
        const HotkeyBinding& binding,
        std::wstring& failureReason) noexcept;

    bool toggleRegistered_ = false;
    bool cancelRegistered_ = false;
};

} // namespace voxinsert