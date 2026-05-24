#pragma once

#include "config/app_config.h"

#include <windows.h>

namespace voxinsert {

struct SettingsDialogValues {
    HotkeyBinding toggleRecordingHotkey;
    HotkeyBinding cancelRecordingHotkey;
    std::wstring transcriptionProvider;
    std::wstring languageHint;
    std::wstring openAiModel;
    std::wstring openAiCredentialTarget;
    std::wstring openAiPrompt;
    std::wstring openAiCredentialStatus;
    std::wstring openAiApiKey;
    bool removeOpenAiApiKey = false;
    std::wstring mistralModel;
    std::wstring mistralCredentialTarget;
    std::wstring mistralContextBias;
    std::wstring mistralCredentialStatus;
    std::wstring mistralApiKey;
    bool removeMistralApiKey = false;
    StatusPillPlacement statusPillPlacement = StatusPillPlacement::TrayAnchor;
    bool autoStartWithWindows = false;
    bool showStatusPill = true;
    bool archiveEnabled = false;
    bool archivePersistTranscript = true;
    bool archivePersistAudio = true;
    std::wstring archiveFolderPath;
};

enum class SettingsDialogResult {
    Saved,
    Cancelled
};

SettingsDialogResult ShowSettingsDialog(HINSTANCE instance, HWND ownerWindow, SettingsDialogValues& values);

} // namespace voxinsert