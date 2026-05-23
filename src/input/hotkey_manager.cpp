#include "input/hotkey_manager.h"

#include "observability/logging.h"

namespace voxinsert {
namespace {

} // namespace

bool HotkeyManager::RegisterHotkey(
    HWND window,
    int hotkeyId,
    const HotkeyBinding& binding,
    std::wstring& failureReason) noexcept {
    if (RegisterHotKey(window, hotkeyId, binding.modifiers, binding.virtualKey) == 0) {
        failureReason = L"Could not register ";
        failureReason += binding.displayName;
        failureReason += L". Choose another hotkey in config. Win32 error: ";
        failureReason += FormatWin32Error(GetLastError());
        return false;
    }

    if (hotkeyId == kToggleRecordingHotkeyId) {
        toggleRegistered_ = true;
    }
    else if (hotkeyId == kCancelRecordingHotkeyId) {
        cancelRegistered_ = true;
    }

    return true;
}

bool HotkeyManager::RegisterHotkeys(
    HWND window,
    const HotkeyBinding& toggleRecordingHotkey,
    const HotkeyBinding& cancelRecordingHotkey,
    std::wstring& failureReason) noexcept {
    UnregisterAll(window);

    if (!RegisterHotkey(window, kToggleRecordingHotkeyId, toggleRecordingHotkey, failureReason)) {
        return false;
    }

    if (!RegisterHotkey(window, kCancelRecordingHotkeyId, cancelRecordingHotkey, failureReason)) {
        UnregisterAll(window);
        return false;
    }

    return true;
}

void HotkeyManager::UnregisterAll(HWND window) noexcept {
    if (toggleRegistered_) {
        UnregisterHotKey(window, kToggleRecordingHotkeyId);
        toggleRegistered_ = false;
    }

    if (cancelRegistered_) {
        UnregisterHotKey(window, kCancelRecordingHotkeyId);
        cancelRegistered_ = false;
    }
}

} // namespace voxinsert