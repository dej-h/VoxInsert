#include "config/app_config.h"

#include "observability/logging.h"

#include <shlobj_core.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace voxinsert {
namespace {

using json = nlohmann::json;

constexpr wchar_t kConfigDirectoryName[] = L"VoxInsert";
constexpr wchar_t kConfigFileName[] = L"config.json";
constexpr wchar_t kConfigExampleFileName[] = L"config.example.json";

constexpr char kFallbackConfigJson[] = R"json({
  "hotkeys": {
    "toggle_recording": "F8",
    "cancel_recording": "Escape"
  },
  "transcription": {
    "provider": "openai",
    "language_hint": "en",
        "openai": {
            "model": "gpt-4o-transcribe",
            "streaming_model": "gpt-realtime-whisper",
            "credential_target": "VoiceAgentTyper/OpenAI",
            "prompt": "The user is dictating technical prompts for coding agents, IDEs, VS Code, GitHub Copilot, Claude Code, Codex, Python, C++, FastAPI, LangChain, OpenAI, Docker, GitHub, APIs, terminals, embeddings, rerankers, and software engineering workflows."
        },
        "mistral": {
            "model": "voxtral-mini-latest",
            "streaming_model": "voxtral-mini-transcribe-realtime-2602",
            "credential_target": "VoiceAgentTyper/Mistral",
            "context_bias": "VoxInsert,VS_Code,GitHub_Copilot,Claude_Code,Codex,Python,C++,FastAPI,LangChain,OpenAI,Docker,GitHub,APIs,terminals,embeddings,rerankers"
        },
        "streaming": {
            "enabled": true,
            "provider": "openai_realtime",
            "append_batch_ms": 20,
            "finalize_timeout_ms": 8000,
            "fallback_to_file_transcription": true
        }
  },
  "insertion": {
    "mode": "clipboard_paste",
    "restore_clipboard": true,
    "auto_press_enter": false
  },
  "audio": {
    "sample_rate": 16000,
    "channels": 1,
        "frames_per_buffer": 256,
    "use_silence_trimming": true,
    "remove_leading_trailing_silence": true,
    "remove_internal_silence_longer_than_ms": 1000,
    "max_recording_seconds": 300
  },
  "ui": {
    "show_status_pill": true,
        "status_pill_position": "tray_anchor"
  },
  "system": {
                "auto_start_with_windows": false,
        "use_media_play_pause_toggle": false,
    "launch_minimized": true
    },
    "archive": {
        "enabled": false,
        "persist_transcript": true,
        "persist_audio": true,
        "folder": "",
        "opus_bitrate_bps": 24000
  }
})json";

bool EnsureDirectoryExists(const std::wstring& directoryPath, DWORD& errorCode) {
    if (CreateDirectoryW(directoryPath.c_str(), nullptr) != 0) {
        return true;
    }

    errorCode = GetLastError();
    if (errorCode != ERROR_ALREADY_EXISTS) {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(directoryPath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return true;
    }

    errorCode = ERROR_ALREADY_EXISTS;
    return false;
}

bool FileExists(const std::filesystem::path& path) {
    std::error_code errorCode;
    return std::filesystem::exists(path, errorCode) && !errorCode;
}

std::wstring ToUpperAscii(std::wstring_view value) {
    std::wstring normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t character) {
        if (character >= L'a' && character <= L'z') {
            return static_cast<wchar_t>(character - (L'a' - L'A'));
        }

        return character;
    });
    return normalized;
}

std::wstring Trim(std::wstring_view value) {
    size_t start = 0;
    while (start < value.size() && iswspace(value[start]) != 0) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && iswspace(value[end - 1]) != 0) {
        --end;
    }

    return std::wstring(value.substr(start, end - start));
}

std::string NormalizeMistralContextBiasEntry(std::string_view entry) {
    std::string normalized;
    normalized.reserve(entry.size());

    bool needsUnderscore = false;
    for (char character : entry) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            if (!normalized.empty()) {
                needsUnderscore = true;
            }
            continue;
        }

        if (needsUnderscore) {
            normalized.push_back('_');
            needsUnderscore = false;
        }

        normalized.push_back(character);
    }

    return normalized;
}

std::string NormalizeMistralContextBiasImpl(std::string_view rawContextBias) {
    std::vector<std::string> entries;
    std::string currentEntry;

    const auto flushEntry = [&entries, &currentEntry]() {
        const std::string normalizedEntry = NormalizeMistralContextBiasEntry(currentEntry);
        if (!normalizedEntry.empty()) {
            entries.push_back(normalizedEntry);
        }
        currentEntry.clear();
    };

    for (char character : rawContextBias) {
        if (character == ',' || character == ';' || character == '\r' || character == '\n') {
            flushEntry();
            continue;
        }

        currentEntry.push_back(character);
    }

    flushEntry();

    std::string normalized;
    for (size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) {
            normalized.push_back(',');
        }
        normalized += entries[index];
    }

    return normalized;
}

std::wstring BuildDefaultArchiveFolderPath() {
    PWSTR localAppDataPath = nullptr;
    const HRESULT knownFolderResult = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppDataPath);
    if (FAILED(knownFolderResult)) {
        return {};
    }

    std::wstring archivePath = localAppDataPath;
    CoTaskMemFree(localAppDataPath);
    archivePath += L"\\VoxInsert\\Archive";
    return archivePath;
}

std::filesystem::path GetExecutableDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD characterCount = 0;

    for (;;) {
        characterCount = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (characterCount == 0) {
            return {};
        }

        if (characterCount < buffer.size() - 1) {
            buffer.resize(characterCount);
            return std::filesystem::path(buffer).parent_path();
        }

        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path FindConfigTemplatePath() {
    std::vector<std::filesystem::path> rootsToCheck;
    rootsToCheck.emplace_back(std::filesystem::current_path());

    const std::filesystem::path executableDirectory = GetExecutableDirectory();
    if (!executableDirectory.empty()) {
        rootsToCheck.push_back(executableDirectory);
    }

    for (const std::filesystem::path& root : rootsToCheck) {
        std::filesystem::path candidate = root;
        while (!candidate.empty()) {
            const std::filesystem::path filePath = candidate / kConfigExampleFileName;
            if (FileExists(filePath)) {
                return filePath;
            }

            const std::filesystem::path parent = candidate.parent_path();
            if (parent == candidate) {
                break;
            }
            candidate = parent;
        }
    }

    return {};
}

bool CreateDefaultConfigFile(const std::wstring& configFilePath, std::wstring& failureReason) {
    const std::filesystem::path templatePath = FindConfigTemplatePath();
    if (!templatePath.empty()) {
        if (CopyFileW(templatePath.c_str(), configFilePath.c_str(), TRUE) != 0) {
            return true;
        }

        failureReason = L"CopyFileW failed while creating config.json: ";
        failureReason += FormatWin32Error(GetLastError());
        return false;
    }

    std::ofstream configFile(std::filesystem::path(configFilePath), std::ios::binary | std::ios::trunc);
    if (!configFile) {
        failureReason = L"Failed to create default config file at ";
        failureReason += configFilePath;
        return false;
    }

    configFile << kFallbackConfigJson;
    if (!configFile.good()) {
        failureReason = L"Failed to write default config file at ";
        failureReason += configFilePath;
        return false;
    }

    return true;
}

bool EnsureConfigFileExists(const std::wstring& configFilePath, std::wstring& failureReason) {
    if (FileExists(std::filesystem::path(configFilePath))) {
        return true;
    }

    return CreateDefaultConfigFile(configFilePath, failureReason);
}

bool ParsePrimaryKeyToken(
    std::wstring_view token,
    UINT& virtualKey,
    std::wstring& displayName) {
    const std::wstring upperToken = ToUpperAscii(token);

    if (upperToken == L"ESC" || upperToken == L"ESCAPE") {
        virtualKey = VK_ESCAPE;
        displayName = L"Escape";
        return true;
    }

    if (upperToken.size() == 1) {
        const wchar_t character = upperToken[0];
        if (character >= L'A' && character <= L'Z') {
            virtualKey = static_cast<UINT>(character);
            displayName.assign(1, character);
            return true;
        }

        if (character >= L'0' && character <= L'9') {
            virtualKey = static_cast<UINT>(character);
            displayName.assign(1, character);
            return true;
        }

    }

    if (upperToken.size() >= 2 && upperToken[0] == L'F') {
        try {
            const int functionKeyNumber = std::stoi(std::wstring(upperToken.substr(1)));
            if (functionKeyNumber >= 1 && functionKeyNumber <= 24) {
                virtualKey = static_cast<UINT>(VK_F1 + (functionKeyNumber - 1));
                displayName = L"F" + std::to_wstring(functionKeyNumber);
                return true;
            }
        }
        catch (...) {
        }
    }

    return false;
}

bool TryDescribePrimaryKey(UINT virtualKey, std::wstring& displayName) {
    if (virtualKey == VK_ESCAPE) {
        displayName = L"Escape";
        return true;
    }

    if ((virtualKey >= L'A' && virtualKey <= L'Z') ||
        (virtualKey >= L'0' && virtualKey <= L'9')) {
        displayName.assign(1, static_cast<wchar_t>(virtualKey));
        return true;
    }

    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        displayName = L"F" + std::to_wstring((virtualKey - VK_F1) + 1);
        return true;
    }

    return false;
}

std::wstring BuildHotkeyDisplayName(const HotkeyBinding& binding) {
    std::wstring displayName;
    const UINT modifiers = binding.modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    if ((modifiers & MOD_CONTROL) != 0) {
        displayName += L"Ctrl+";
    }

    if ((modifiers & MOD_ALT) != 0) {
        displayName += L"Alt+";
    }

    if ((modifiers & MOD_SHIFT) != 0) {
        displayName += L"Shift+";
    }

    if ((modifiers & MOD_WIN) != 0) {
        displayName += L"Win+";
    }

    std::wstring primaryKeyName;
    if (TryDescribePrimaryKey(binding.virtualKey, primaryKeyName)) {
        displayName += primaryKeyName;
        return displayName;
    }

    if (!binding.displayName.empty()) {
        return binding.displayName;
    }

    return displayName;
}

bool ParseStatusPillPlacement(
    std::string_view text,
    StatusPillPlacement& placement,
    std::wstring& failureReason) {
    if (text.empty() || text == "tray_anchor" || text == "bottom_right") {
        placement = StatusPillPlacement::TrayAnchor;
        return true;
    }

    if (text == "screen_top_left") {
        placement = StatusPillPlacement::ScreenTopLeft;
        return true;
    }

    if (text == "screen_top_right") {
        placement = StatusPillPlacement::ScreenTopRight;
        return true;
    }

    if (text == "screen_bottom_left") {
        placement = StatusPillPlacement::ScreenBottomLeft;
        return true;
    }

    if (text == "screen_bottom_right") {
        placement = StatusPillPlacement::ScreenBottomRight;
        return true;
    }

    failureReason = L"Unsupported ui.status_pill_position value '";
    failureReason += WideFromUtf8(std::string(text));
    failureReason += L"'. Supported values are tray_anchor, screen_top_left, screen_top_right, screen_bottom_left, and screen_bottom_right.";
    return false;
}

std::string_view StatusPillPlacementToConfigString(StatusPillPlacement placement) {
    switch (placement) {
    case StatusPillPlacement::ScreenTopLeft:
        return "screen_top_left";
    case StatusPillPlacement::ScreenTopRight:
        return "screen_top_right";
    case StatusPillPlacement::ScreenBottomLeft:
        return "screen_bottom_left";
    case StatusPillPlacement::ScreenBottomRight:
        return "screen_bottom_right";
    case StatusPillPlacement::TrayAnchor:
    default:
        return "tray_anchor";
    }
}

bool ParseHotkeyBinding(std::wstring_view text, HotkeyBinding& binding, std::wstring& failureReason) {
    HotkeyBinding parsedBinding{};
    parsedBinding.modifiers = MOD_NOREPEAT;

    bool foundPrimaryKey = false;
    size_t currentOffset = 0;

    while (currentOffset <= text.size()) {
        const size_t separatorOffset = text.find(L'+', currentOffset);
        const size_t tokenLength = separatorOffset == std::wstring_view::npos
            ? text.size() - currentOffset
            : separatorOffset - currentOffset;
        const std::wstring token = Trim(text.substr(currentOffset, tokenLength));

        if (token.empty()) {
            failureReason = L"Invalid hotkey string: empty token in '";
            failureReason += std::wstring(text);
            failureReason += L"'.";
            return false;
        }

        const std::wstring upperToken = ToUpperAscii(token);
        if (upperToken == L"CTRL" || upperToken == L"CONTROL") {
            parsedBinding.modifiers |= MOD_CONTROL;
        }
        else if (upperToken == L"ALT") {
            parsedBinding.modifiers |= MOD_ALT;
        }
        else if (upperToken == L"SHIFT") {
            parsedBinding.modifiers |= MOD_SHIFT;
        }
        else if (upperToken == L"WIN" || upperToken == L"WINDOWS") {
            parsedBinding.modifiers |= MOD_WIN;
        }
        else {
            if (foundPrimaryKey) {
                failureReason = L"Invalid hotkey string: multiple primary keys in '";
                failureReason += std::wstring(text);
                failureReason += L"'.";
                return false;
            }

            std::wstring displayName;
            if (!ParsePrimaryKeyToken(token, parsedBinding.virtualKey, displayName)) {
                failureReason = L"Unsupported hotkey token '";
                failureReason += token;
                failureReason += L"' in '";
                failureReason += std::wstring(text);
                failureReason += L"'.";
                return false;
            }

            foundPrimaryKey = true;
        }

        if (separatorOffset == std::wstring_view::npos) {
            break;
        }

        currentOffset = separatorOffset + 1;
    }

    if (!foundPrimaryKey) {
        failureReason = L"Invalid hotkey string: missing primary key in '";
        failureReason += std::wstring(text);
        failureReason += L"'.";
        return false;
    }

    parsedBinding.displayName = BuildHotkeyDisplayName(parsedBinding);

    binding = parsedBinding;
    return true;
}

bool LoadJsonFile(const std::wstring& path, json& value, std::wstring& failureReason) {
    try {
        std::ifstream inputFile(std::filesystem::path(path), std::ios::binary);
        if (!inputFile) {
            failureReason = L"Failed to open config file at ";
            failureReason += path;
            return false;
        }

        value = json::parse(inputFile);
        return true;
    }
    catch (const json::exception& exception) {
        failureReason = L"Failed to parse config.json: ";
        failureReason += WideFromUtf8(exception.what());
        return false;
    }
}

bool SaveJsonFile(const std::wstring& path, const json& value, std::wstring& failureReason) {
    try {
        std::ofstream outputFile(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
        if (!outputFile) {
            failureReason = L"Failed to open config file for writing at ";
            failureReason += path;
            return false;
        }

        outputFile << value.dump(2) << '\n';
        if (!outputFile.good()) {
            failureReason = L"Failed to write config file at ";
            failureReason += path;
            return false;
        }

        return true;
    }
    catch (const json::exception& exception) {
        failureReason = L"Failed to serialize config.json: ";
        failureReason += WideFromUtf8(exception.what());
        return false;
    }
}

bool EnsureConfigSection(json& root, const char* sectionName, std::wstring& failureReason) {
    auto sectionIt = root.find(sectionName);
    if (sectionIt == root.end()) {
        root[sectionName] = json::object();
        return true;
    }

    if (sectionIt->is_object()) {
        return true;
    }

    failureReason = L"Config section ";
    failureReason += WideFromUtf8(sectionName);
    failureReason += L" must be an object.";
    return false;
}

bool EnsureNestedConfigSection(json& root, const char* sectionName, const char* nestedSectionName, std::wstring& failureReason) {
    if (!EnsureConfigSection(root, sectionName, failureReason)) {
        return false;
    }

    auto& section = root[sectionName];
    auto nestedIt = section.find(nestedSectionName);
    if (nestedIt == section.end()) {
        section[nestedSectionName] = json::object();
        return true;
    }

    if (nestedIt->is_object()) {
        return true;
    }

    failureReason = L"Config section ";
    failureReason += WideFromUtf8(sectionName);
    failureReason += L".";
    failureReason += WideFromUtf8(nestedSectionName);
    failureReason += L" must be an object.";
    return false;
}

bool LoadConfiguredHotkey(
    const json& root,
    const char* fieldName,
    HotkeyBinding& binding,
    std::wstring& failureReason) {
    const auto hotkeysIt = root.find("hotkeys");
    if (hotkeysIt == root.end() || !hotkeysIt->is_object()) {
        return true;
    }

    const auto valueIt = hotkeysIt->find(fieldName);
    if (valueIt == hotkeysIt->end()) {
        return true;
    }

    if (!valueIt->is_string()) {
        failureReason = L"Config value hotkeys.";
        failureReason += WideFromUtf8(fieldName);
        failureReason += L" must be a string.";
        return false;
    }

    return ParseHotkeyBinding(WideFromUtf8(valueIt->get<std::string>()), binding, failureReason);
}

bool LoadConfiguredPositiveInteger(
    const json& root,
    const char* sectionName,
    const char* fieldName,
    int minimumValue,
    int& value,
    std::wstring& failureReason) {
    const auto sectionIt = root.find(sectionName);
    if (sectionIt == root.end() || !sectionIt->is_object()) {
        return true;
    }

    const auto valueIt = sectionIt->find(fieldName);
    if (valueIt == sectionIt->end()) {
        return true;
    }

    if (!valueIt->is_number_integer()) {
        failureReason = L"Config value ";
        failureReason += WideFromUtf8(sectionName);
        failureReason += L".";
        failureReason += WideFromUtf8(fieldName);
        failureReason += L" must be an integer.";
        return false;
    }

    const int parsedValue = valueIt->get<int>();
    if (parsedValue < minimumValue) {
        failureReason = L"Config value ";
        failureReason += WideFromUtf8(sectionName);
        failureReason += L".";
        failureReason += WideFromUtf8(fieldName);
        failureReason += L" must be at least ";
        failureReason += std::to_wstring(minimumValue);
        failureReason += L".";
        return false;
    }

    value = parsedValue;
    return true;
}

bool LoadConfiguredAudioSettings(const json& root, AudioConfig& audio, std::wstring& failureReason) {
    int framesPerBuffer = static_cast<int>(audio.framesPerBuffer);

    if (!LoadConfiguredPositiveInteger(root, "audio", "sample_rate", 1, audio.sampleRate, failureReason)) {
        return false;
    }

    if (!LoadConfiguredPositiveInteger(root, "audio", "channels", 1, audio.channelCount, failureReason)) {
        return false;
    }

    if (audio.channelCount != 1) {
        failureReason = L"Only mono microphone capture is supported right now. Set audio.channels to 1.";
        return false;
    }

    if (!LoadConfiguredPositiveInteger(root, "audio", "frames_per_buffer", 1, framesPerBuffer, failureReason)) {
        return false;
    }

    if (!LoadConfiguredPositiveInteger(root, "audio", "max_recording_seconds", 1, audio.maxRecordingSeconds, failureReason)) {
        return false;
    }

    audio.framesPerBuffer = static_cast<unsigned long>(framesPerBuffer);
    return true;
}

bool LoadConfiguredString(
    const json& root,
    const char* sectionName,
    const char* fieldName,
    std::string& value,
    std::wstring& failureReason) {
    const auto sectionIt = root.find(sectionName);
    if (sectionIt == root.end() || !sectionIt->is_object()) {
        return true;
    }

    const auto valueIt = sectionIt->find(fieldName);
    if (valueIt == sectionIt->end()) {
        return true;
    }

    if (!valueIt->is_string()) {
        failureReason = L"Config value ";
        failureReason += WideFromUtf8(sectionName);
        failureReason += L".";
        failureReason += WideFromUtf8(fieldName);
        failureReason += L" must be a string.";
        return false;
    }

    value = valueIt->get<std::string>();
    return true;
}

bool LoadConfiguredWideString(
    const json& root,
    const char* sectionName,
    const char* fieldName,
    std::wstring& value,
    std::wstring& failureReason) {
    std::string utf8Value = Utf8FromWide(value);
    if (!LoadConfiguredString(root, sectionName, fieldName, utf8Value, failureReason)) {
        return false;
    }

    value = WideFromUtf8(utf8Value);
    return true;
}

bool LoadConfiguredNestedString(
    const json& root,
    const char* sectionName,
    const char* nestedSectionName,
    const char* fieldName,
    std::string& value,
    std::wstring& failureReason) {
    const auto sectionIt = root.find(sectionName);
    if (sectionIt == root.end() || !sectionIt->is_object()) {
        return true;
    }

    const auto nestedIt = sectionIt->find(nestedSectionName);
    if (nestedIt == sectionIt->end()) {
        return true;
    }

    if (!nestedIt->is_object()) {
        failureReason = L"Config section ";
        failureReason += WideFromUtf8(sectionName);
        failureReason += L".";
        failureReason += WideFromUtf8(nestedSectionName);
        failureReason += L" must be an object.";
        return false;
    }

    const auto valueIt = nestedIt->find(fieldName);
    if (valueIt == nestedIt->end()) {
        return true;
    }

    if (!valueIt->is_string()) {
        failureReason = L"Config value ";
        failureReason += WideFromUtf8(sectionName);
        failureReason += L".";
        failureReason += WideFromUtf8(nestedSectionName);
        failureReason += L".";
        failureReason += WideFromUtf8(fieldName);
        failureReason += L" must be a string.";
        return false;
    }

    value = valueIt->get<std::string>();
    return true;
}

bool LoadConfiguredBoolean(
    const json& root,
    const char* sectionName,
    const char* fieldName,
    bool& value,
    std::wstring& failureReason) {
    const auto sectionIt = root.find(sectionName);
    if (sectionIt == root.end() || !sectionIt->is_object()) {
        return true;
    }

    const auto valueIt = sectionIt->find(fieldName);
    if (valueIt == sectionIt->end()) {
        return true;
    }

    if (!valueIt->is_boolean()) {
        failureReason = L"Config value ";
        failureReason += WideFromUtf8(sectionName);
        failureReason += L".";
        failureReason += WideFromUtf8(fieldName);
        failureReason += L" must be a boolean.";
        return false;
    }

    value = valueIt->get<bool>();
    return true;
}

bool LoadConfiguredInsertionSettings(const json& root, InsertionConfig& insertion, std::wstring& failureReason) {
    if (!LoadConfiguredString(root, "insertion", "mode", insertion.mode, failureReason)) {
        return false;
    }

    if (insertion.mode != "clipboard_paste") {
        failureReason = L"Only insertion.mode = clipboard_paste is supported right now.";
        return false;
    }

    if (!LoadConfiguredBoolean(root, "insertion", "restore_clipboard", insertion.restoreClipboard, failureReason)) {
        return false;
    }

    if (!LoadConfiguredBoolean(root, "insertion", "auto_press_enter", insertion.autoPressEnter, failureReason)) {
        return false;
    }

    return true;
}

bool LoadConfiguredUiSettings(const json& root, UiConfig& ui, std::wstring& failureReason) {
    if (!LoadConfiguredBoolean(root, "ui", "show_status_pill", ui.showStatusPill, failureReason)) {
        return false;
    }

    std::string position = std::string(StatusPillPlacementToConfigString(ui.statusPillPlacement));
    if (!LoadConfiguredString(root, "ui", "status_pill_position", position, failureReason)) {
        return false;
    }

    if (!ParseStatusPillPlacement(position, ui.statusPillPlacement, failureReason)) {
        return false;
    }

    return true;
}

bool LoadConfiguredSystemSettings(const json& root, SystemConfig& system, std::wstring& failureReason) {
    if (!LoadConfiguredBoolean(root, "system", "auto_start_with_windows", system.autoStartWithWindows, failureReason)) {
        return false;
    }

    if (!LoadConfiguredBoolean(root, "system", "use_media_play_pause_toggle", system.useMediaPlayPauseToggle, failureReason)) {
        return false;
    }

    if (!LoadConfiguredBoolean(root, "system", "launch_minimized", system.launchMinimized, failureReason)) {
        return false;
    }

    return true;
}

bool LoadConfiguredArchiveSettings(const json& root, ArchiveConfig& archive, std::wstring& failureReason) {
    if (!LoadConfiguredBoolean(root, "archive", "enabled", archive.enabled, failureReason)) {
        return false;
    }

    if (!LoadConfiguredBoolean(root, "archive", "persist_transcript", archive.persistTranscript, failureReason)) {
        return false;
    }

    if (!LoadConfiguredBoolean(root, "archive", "persist_audio", archive.persistAudio, failureReason)) {
        return false;
    }

    if (!LoadConfiguredWideString(root, "archive", "folder", archive.folderPath, failureReason)) {
        return false;
    }

    if (!LoadConfiguredPositiveInteger(root, "archive", "opus_bitrate_bps", 6000, archive.opusBitrateBps, failureReason)) {
        return false;
    }

    if (archive.folderPath.empty()) {
        archive.folderPath = DefaultArchiveFolderPath();
    }

    return true;
}

bool LoadConfiguredTranscriptionSettings(const json& root, TranscriptionConfig& transcription, std::wstring& failureReason) {
    if (!LoadConfiguredString(root, "transcription", "provider", transcription.provider, failureReason)) {
        return false;
    }

    if (transcription.provider != "openai" && transcription.provider != "mistral") {
        failureReason = L"Unsupported transcription.provider value '";
        failureReason += WideFromUtf8(transcription.provider);
        failureReason += L"'. Supported values are openai and mistral.";
        return false;
    }

    if (!LoadConfiguredString(root, "transcription", "language_hint", transcription.languageHint, failureReason)) {
        return false;
    }

    if (!LoadConfiguredNestedString(root, "transcription", "openai", "model", transcription.openAi.model, failureReason)) {
        return false;
    }

    if (!LoadConfiguredNestedString(root, "transcription", "openai", "streaming_model", transcription.openAi.streamingModel, failureReason)) {
        return false;
    }

    if (!LoadConfiguredNestedString(root, "transcription", "openai", "credential_target", transcription.openAi.credentialTarget, failureReason)) {
        return false;
    }

    if (!LoadConfiguredNestedString(root, "transcription", "openai", "prompt", transcription.openAi.prompt, failureReason)) {
        return false;
    }

    if (!LoadConfiguredNestedString(root, "transcription", "mistral", "model", transcription.mistral.model, failureReason)) {
        return false;
    }

    if (!LoadConfiguredNestedString(root, "transcription", "mistral", "streaming_model", transcription.mistral.streamingModel, failureReason)) {
        return false;
    }

    if (!LoadConfiguredNestedString(root, "transcription", "mistral", "credential_target", transcription.mistral.credentialTarget, failureReason)) {
        return false;
    }

    if (!LoadConfiguredNestedString(root, "transcription", "mistral", "context_bias", transcription.mistral.contextBias, failureReason)) {
        return false;
    }

    transcription.mistral.contextBias = NormalizeMistralContextBias(transcription.mistral.contextBias);

    // The audio/transcriptions endpoint currently supports Voxtral Mini Transcribe, not Voxtral Small.
    if (transcription.mistral.model == "voxtral-small-latest") {
        transcription.mistral.model = "voxtral-mini-latest";
    }

    const auto transcriptionIt = root.find("transcription");
    if (transcriptionIt != root.end() && transcriptionIt->is_object()) {
        const auto streamingIt = transcriptionIt->find("streaming");
        if (streamingIt != transcriptionIt->end()) {
            if (!streamingIt->is_object()) {
                failureReason = L"Config section transcription.streaming must be an object.";
                return false;
            }

            const json streamingSection{{"streaming", *streamingIt}};
            if (!LoadConfiguredBoolean(streamingSection, "streaming", "enabled", transcription.streaming.enabled, failureReason)) {
                return false;
            }

            if (!LoadConfiguredString(streamingSection, "streaming", "provider", transcription.streaming.provider, failureReason)) {
                return false;
            }

            if (transcription.streaming.provider != "openai_realtime" &&
                transcription.streaming.provider != "mistral_realtime") {
                failureReason = L"Unsupported transcription.streaming.provider value '";
                failureReason += WideFromUtf8(transcription.streaming.provider);
                failureReason += L"'. Supported values are openai_realtime and mistral_realtime.";
                return false;
            }

            if (!LoadConfiguredPositiveInteger(streamingSection, "streaming", "append_batch_ms", 10, transcription.streaming.appendBatchMs, failureReason)) {
                return false;
            }

            if (!LoadConfiguredPositiveInteger(streamingSection, "streaming", "finalize_timeout_ms", 250, transcription.streaming.finalizeTimeoutMs, failureReason)) {
                return false;
            }

            if (!LoadConfiguredBoolean(streamingSection, "streaming", "fallback_to_file_transcription", transcription.streaming.fallbackToFileTranscription, failureReason)) {
                return false;
            }
        }
    }

    // The realtime streaming backend always follows the selected transcription
    // provider so the single provider choice drives both the file and streaming
    // paths and the fallback stays vendor-consistent.
    transcription.streaming.provider =
        (transcription.provider == "mistral") ? "mistral_realtime" : "openai_realtime";

    return true;
}

} // namespace

std::string NormalizeMistralContextBias(std::string_view rawContextBias) {
    return NormalizeMistralContextBiasImpl(rawContextBias);
}

std::wstring DefaultArchiveFolderPath() {
    return BuildDefaultArchiveFolderPath();
}

bool TryCreateHotkeyBinding(UINT modifiers, UINT virtualKey, HotkeyBinding& binding, std::wstring& failureReason) {
    if (virtualKey == 0) {
        failureReason = L"Choose a hotkey before saving settings.";
        return false;
    }

    HotkeyBinding parsedBinding{};
    parsedBinding.modifiers = (modifiers & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) | MOD_NOREPEAT;
    parsedBinding.virtualKey = virtualKey;

    std::wstring primaryKeyName;
    if (!TryDescribePrimaryKey(virtualKey, primaryKeyName)) {
        failureReason = L"Only A-Z, 0-9, F1-F24, and Escape hotkeys are supported right now.";
        return false;
    }

    parsedBinding.displayName = BuildHotkeyDisplayName(parsedBinding);
    binding = parsedBinding;
    return true;
}

std::wstring SerializeHotkeyBinding(const HotkeyBinding& binding) {
    return BuildHotkeyDisplayName(binding);
}

AppConfig DefaultAppConfig() {
    AppConfig config{};
    config.toggleRecordingHotkey = HotkeyBinding{MOD_NOREPEAT, VK_F8, L"F8"};
    config.cancelRecordingHotkey = HotkeyBinding{MOD_NOREPEAT, VK_ESCAPE, L"Escape"};
    config.archive.folderPath = DefaultArchiveFolderPath();
    return config;
}

bool LoadAppConfig(AppConfig& config, std::wstring& failureReason) {
    config = DefaultAppConfig();

    PWSTR roamingAppDataPath = nullptr;
    const HRESULT knownFolderResult = SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &roamingAppDataPath);
    if (FAILED(knownFolderResult)) {
        failureReason = L"SHGetKnownFolderPath(FOLDERID_RoamingAppData) failed with HRESULT 0x";
        wchar_t codeBuffer[16]{};
        swprintf_s(codeBuffer, L"%08X", static_cast<unsigned int>(knownFolderResult));
        failureReason += codeBuffer;
        return false;
    }

    const std::wstring configDirectory = std::wstring(roamingAppDataPath) + L"\\" + kConfigDirectoryName;
    CoTaskMemFree(roamingAppDataPath);

    DWORD errorCode = ERROR_SUCCESS;
    if (!EnsureDirectoryExists(configDirectory, errorCode)) {
        failureReason = L"CreateDirectoryW failed for config directory: ";
        failureReason += FormatWin32Error(errorCode);
        return false;
    }

    config.configFilePath = configDirectory + L"\\" + kConfigFileName;
    if (!EnsureConfigFileExists(config.configFilePath, failureReason)) {
        return false;
    }

    json root;
    if (!LoadJsonFile(config.configFilePath, root, failureReason)) {
        return false;
    }

    if (!LoadConfiguredHotkey(root, "toggle_recording", config.toggleRecordingHotkey, failureReason)) {
        return false;
    }

    if (!LoadConfiguredHotkey(root, "cancel_recording", config.cancelRecordingHotkey, failureReason)) {
        return false;
    }

    if (!LoadConfiguredAudioSettings(root, config.audio, failureReason)) {
        return false;
    }

    if (!LoadConfiguredTranscriptionSettings(root, config.transcription, failureReason)) {
        return false;
    }

    if (!LoadConfiguredInsertionSettings(root, config.insertion, failureReason)) {
        return false;
    }

    if (!LoadConfiguredUiSettings(root, config.ui, failureReason)) {
        return false;
    }

    if (!LoadConfiguredSystemSettings(root, config.system, failureReason)) {
        return false;
    }

    if (!LoadConfiguredArchiveSettings(root, config.archive, failureReason)) {
        return false;
    }

    return true;
}

bool SaveAppSettings(const AppConfig& config, const AppSettingsUpdate& settings, std::wstring& failureReason) {
    if (config.configFilePath.empty()) {
        failureReason = L"Cannot save settings because config.configFilePath is empty.";
        return false;
    }

    json root;
    if (!LoadJsonFile(config.configFilePath, root, failureReason)) {
        return false;
    }

    if (!EnsureConfigSection(root, "ui", failureReason)) {
        return false;
    }

    if (!EnsureConfigSection(root, "system", failureReason)) {
        return false;
    }

    if (!EnsureConfigSection(root, "hotkeys", failureReason)) {
        return false;
    }

    if (!EnsureConfigSection(root, "archive", failureReason)) {
        return false;
    }

    if (!EnsureConfigSection(root, "transcription", failureReason)) {
        return false;
    }

    if (!EnsureNestedConfigSection(root, "transcription", "openai", failureReason)) {
        return false;
    }

    if (!EnsureNestedConfigSection(root, "transcription", "mistral", failureReason)) {
        return false;
    }

    if (!EnsureNestedConfigSection(root, "transcription", "streaming", failureReason)) {
        return false;
    }

    root["hotkeys"]["toggle_recording"] = Utf8FromWide(SerializeHotkeyBinding(settings.toggleRecordingHotkey));
    root["hotkeys"]["cancel_recording"] = Utf8FromWide(SerializeHotkeyBinding(settings.cancelRecordingHotkey));
    root["transcription"]["provider"] = settings.transcription.provider;
    root["transcription"]["language_hint"] = settings.transcription.languageHint;
    root["transcription"].erase("model");
    root["transcription"].erase("credential_target");
    root["transcription"].erase("prompt");
    root["transcription"]["openai"]["model"] = settings.transcription.openAi.model;
    root["transcription"]["openai"]["streaming_model"] = settings.transcription.openAi.streamingModel;
    root["transcription"]["openai"]["credential_target"] = settings.transcription.openAi.credentialTarget;
    root["transcription"]["openai"]["prompt"] = settings.transcription.openAi.prompt;
    root["transcription"]["mistral"]["model"] = settings.transcription.mistral.model;
    root["transcription"]["mistral"]["streaming_model"] = settings.transcription.mistral.streamingModel;
    root["transcription"]["mistral"]["credential_target"] = settings.transcription.mistral.credentialTarget;
    root["transcription"]["mistral"]["context_bias"] = NormalizeMistralContextBias(settings.transcription.mistral.contextBias);
    root["transcription"]["streaming"]["enabled"] = settings.transcription.streaming.enabled;
    root["transcription"]["streaming"]["provider"] = settings.transcription.streaming.provider;
    root["transcription"]["streaming"]["append_batch_ms"] = settings.transcription.streaming.appendBatchMs;
    root["transcription"]["streaming"]["finalize_timeout_ms"] = settings.transcription.streaming.finalizeTimeoutMs;
    root["transcription"]["streaming"]["fallback_to_file_transcription"] = settings.transcription.streaming.fallbackToFileTranscription;
    root["ui"]["show_status_pill"] = settings.ui.showStatusPill;
    root["ui"]["status_pill_position"] = StatusPillPlacementToConfigString(settings.ui.statusPillPlacement);
    root["system"]["auto_start_with_windows"] = settings.system.autoStartWithWindows;
    root["system"]["use_media_play_pause_toggle"] = settings.system.useMediaPlayPauseToggle;
    root["system"]["launch_minimized"] = settings.system.launchMinimized;
    root["archive"]["enabled"] = settings.archive.enabled;
    root["archive"]["persist_transcript"] = settings.archive.persistTranscript;
    root["archive"]["persist_audio"] = settings.archive.persistAudio;
    root["archive"]["folder"] = Utf8FromWide(settings.archive.folderPath);
    root["archive"]["opus_bitrate_bps"] = settings.archive.opusBitrateBps;

    return SaveJsonFile(config.configFilePath, root, failureReason);
}

} // namespace voxinsert
