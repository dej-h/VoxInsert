#pragma once

#include <windows.h>

#include <string>

namespace voxinsert {

struct HotkeyBinding {
    UINT modifiers = MOD_NOREPEAT;
    UINT virtualKey = 0;
    std::wstring displayName;
};

enum class StatusPillPlacement {
    TrayAnchor,
    ScreenTopLeft,
    ScreenTopRight,
    ScreenBottomLeft,
    ScreenBottomRight
};

struct AudioConfig {
    int sampleRate = 16000;
    int channelCount = 1;
    unsigned long framesPerBuffer = 256;
    int maxRecordingSeconds = 300;
};

struct TranscriptionConfig {
    std::string provider = "openai";
    std::string model = "gpt-4o-transcribe";
    std::string credentialTarget = "VoiceAgentTyper/OpenAI";
    std::string languageHint = "en";
    std::string prompt;
};

struct InsertionConfig {
    std::string mode = "clipboard_paste";
    bool restoreClipboard = true;
    bool autoPressEnter = false;
};

struct UiConfig {
    bool showStatusPill = true;
    StatusPillPlacement statusPillPlacement = StatusPillPlacement::TrayAnchor;
};

struct SystemConfig {
    bool autoStartWithWindows = false;
    bool launchMinimized = true;
};

struct AppSettingsUpdate {
    HotkeyBinding toggleRecordingHotkey;
    HotkeyBinding cancelRecordingHotkey;
    UiConfig ui;
    SystemConfig system;
};

// Keeps the small amount of runtime configuration the current milestone actually uses.
struct AppConfig {
    std::wstring configFilePath;
    HotkeyBinding toggleRecordingHotkey;
    HotkeyBinding cancelRecordingHotkey;
    AudioConfig audio;
    TranscriptionConfig transcription;
    InsertionConfig insertion;
    UiConfig ui;
    SystemConfig system;
};

AppConfig DefaultAppConfig();
bool LoadAppConfig(AppConfig& config, std::wstring& failureReason);
bool SaveAppSettings(const AppConfig& config, const AppSettingsUpdate& settings, std::wstring& failureReason);
bool TryCreateHotkeyBinding(UINT modifiers, UINT virtualKey, HotkeyBinding& binding, std::wstring& failureReason);
std::wstring SerializeHotkeyBinding(const HotkeyBinding& binding);

} // namespace voxinsert