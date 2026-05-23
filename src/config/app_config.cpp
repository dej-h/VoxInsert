#include "config/app_config.h"

#include "observability/logging.h"

#include <shlobj_core.h>

#include <nlohmann/json.hpp>

#include <algorithm>
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
    "model": "gpt-4o-transcribe",
    "credential_target": "VoiceAgentTyper/OpenAI",
    "language_hint": "en",
    "prompt": "The user is dictating technical prompts for coding agents, IDEs, VS Code, GitHub Copilot, Claude Code, Codex, Python, C++, FastAPI, LangChain, OpenAI, Docker, GitHub, APIs, terminals, embeddings, rerankers, and software engineering workflows."
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
    "status_pill_position": "bottom_right"
  },
  "system": {
    "auto_start_with_windows": true,
    "launch_minimized": true
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

bool ParseHotkeyBinding(std::wstring_view text, HotkeyBinding& binding, std::wstring& failureReason) {
    HotkeyBinding parsedBinding{};
    parsedBinding.modifiers = MOD_NOREPEAT;

    bool foundPrimaryKey = false;
    std::vector<std::wstring> displayParts;
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
            displayParts.push_back(L"Ctrl");
        }
        else if (upperToken == L"ALT") {
            parsedBinding.modifiers |= MOD_ALT;
            displayParts.push_back(L"Alt");
        }
        else if (upperToken == L"SHIFT") {
            parsedBinding.modifiers |= MOD_SHIFT;
            displayParts.push_back(L"Shift");
        }
        else if (upperToken == L"WIN" || upperToken == L"WINDOWS") {
            parsedBinding.modifiers |= MOD_WIN;
            displayParts.push_back(L"Win");
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
            displayParts.push_back(displayName);
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

    parsedBinding.displayName.clear();
    for (size_t index = 0; index < displayParts.size(); ++index) {
        if (index > 0) {
            parsedBinding.displayName += L"+";
        }
        parsedBinding.displayName += displayParts[index];
    }

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

    return true;
}

bool LoadConfiguredTranscriptionSettings(const json& root, TranscriptionConfig& transcription, std::wstring& failureReason) {
    if (!LoadConfiguredString(root, "transcription", "provider", transcription.provider, failureReason)) {
        return false;
    }

    if (!LoadConfiguredString(root, "transcription", "model", transcription.model, failureReason)) {
        return false;
    }

    if (!LoadConfiguredString(root, "transcription", "credential_target", transcription.credentialTarget, failureReason)) {
        return false;
    }

    if (!LoadConfiguredString(root, "transcription", "language_hint", transcription.languageHint, failureReason)) {
        return false;
    }

    if (!LoadConfiguredString(root, "transcription", "prompt", transcription.prompt, failureReason)) {
        return false;
    }

    return true;
}

} // namespace

AppConfig DefaultAppConfig() {
    AppConfig config{};
    config.toggleRecordingHotkey = HotkeyBinding{MOD_NOREPEAT, VK_F8, L"F8"};
    config.cancelRecordingHotkey = HotkeyBinding{MOD_NOREPEAT, VK_ESCAPE, L"Escape"};
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

    return true;
}

} // namespace voxinsert