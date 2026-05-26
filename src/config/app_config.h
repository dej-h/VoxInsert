#pragma once

#include <windows.h>

#include <string>
#include <string_view>

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

struct OpenAiTranscriptionProviderConfig {
    std::string model = "gpt-4o-transcribe";
    std::string credentialTarget = "VoiceAgentTyper/OpenAI";
    std::string prompt = "The user is dictating technical prompts for coding agents, IDEs, VS Code, GitHub Copilot, Claude Code, Codex, Python, C++, FastAPI, LangChain, OpenAI, Docker, GitHub, APIs, terminals, embeddings, rerankers, and software engineering workflows.";
};

struct MistralTranscriptionProviderConfig {
    std::string model = "voxtral-mini-latest";
    std::string credentialTarget = "VoiceAgentTyper/Mistral";
    std::string contextBias = "VoxInsert,VS_Code,GitHub_Copilot,Claude_Code,Codex,Python,C++,FastAPI,LangChain,OpenAI,Docker,GitHub,APIs,terminals,embeddings,rerankers";
};

struct TranscriptionConfig {
    std::string provider = "openai";
    std::string languageHint = "en";
    OpenAiTranscriptionProviderConfig openAi;
    MistralTranscriptionProviderConfig mistral;
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
    bool useMediaPlayPauseToggle = false;
    bool launchMinimized = true;
};

struct ArchiveConfig {
    bool enabled = false;
    bool persistTranscript = true;
    bool persistAudio = true;
    std::wstring folderPath;
    int opusBitrateBps = 24000;
};

struct AppSettingsUpdate {
    HotkeyBinding toggleRecordingHotkey;
    HotkeyBinding cancelRecordingHotkey;
    TranscriptionConfig transcription;
    UiConfig ui;
    SystemConfig system;
    ArchiveConfig archive;
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
    ArchiveConfig archive;
};

AppConfig DefaultAppConfig();
std::wstring DefaultArchiveFolderPath();
std::string NormalizeMistralContextBias(std::string_view rawContextBias);
bool LoadAppConfig(AppConfig& config, std::wstring& failureReason);
bool SaveAppSettings(const AppConfig& config, const AppSettingsUpdate& settings, std::wstring& failureReason);
bool TryCreateHotkeyBinding(UINT modifiers, UINT virtualKey, HotkeyBinding& binding, std::wstring& failureReason);
std::wstring SerializeHotkeyBinding(const HotkeyBinding& binding);

} // namespace voxinsert