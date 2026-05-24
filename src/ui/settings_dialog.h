#pragma once

#include "config/app_config.h"

#include <windows.h>

namespace voxinsert {

struct SettingsDialogValues {
    HotkeyBinding toggleRecordingHotkey;
    HotkeyBinding cancelRecordingHotkey;
    StatusPillPlacement statusPillPlacement = StatusPillPlacement::TrayAnchor;
    bool autoStartWithWindows = false;
    bool showStatusPill = true;
};

enum class SettingsDialogResult {
    Saved,
    Cancelled
};

SettingsDialogResult ShowSettingsDialog(HINSTANCE instance, HWND ownerWindow, SettingsDialogValues& values);

} // namespace voxinsert