# VoiceAgentTyper — Updated Build Guide

A Windows-native C++ utility that records speech with a toggle hotkey, transcribes it, and inserts the final transcript into the currently focused text box.

This guide is written as a project brief for a coding agent. It intentionally avoids installer/updater/live-preview scope until the core loop works.

---

## 0. Project boundary

The v0.1 app does exactly this:

```text
F8 once  -> start recording
F8 again -> stop recording
Audio -> transcription
Transcript -> focused text box
User reviews manually
User sends manually
```

It does **not** do this yet:

```text
- control the IDE
- interpret commands
- rewrite prompts
- post tweets
- auto-send
- stream partial text
- install/update itself
- run local transcription
- support many transcription providers
```

Hard rule:

```text
voice -> transcript -> focused textbox
```

Everything else is later.

---

## 1. Current best-practice decisions

| Area | Decision | Reason |
|---|---|---|
| OS | Windows only | The useful parts depend on native Windows APIs. |
| Language | C++20 | Modern enough, stable MSVC support, no C++23 friction. |
| Build system | CMake + Ninja | Works cleanly in VS Code and CI later. |
| Compiler | MSVC via Visual Studio Build Tools 2022 | Native Windows API + common Windows C++ baseline. |
| Dependency manager | vcpkg manifest mode | Dependencies belong in the repo via `vcpkg.json`, not global install state. |
| HTTP | `cpr` | Simple C++ wrapper around libcurl; good enough for this app. |
| JSON | `nlohmann/json` | Simple, stable, and common. |
| Audio | PortAudio | Faster than raw WASAPI for the first working version. |
| Audio archive compression | `libopusenc` | High-level Ogg Opus writer for compact local clip archives. |
| Secrets | Windows Credential Manager | Correct pattern for user-entered API keys in a Windows UI app. |
| Config | JSON under `%APPDATA%` | Normal settings belong in app data, not install/build directory. |
| Text insertion | Clipboard paste + `Ctrl+V` | Fast and reliable for long prompts. |
| Hotkeys | `RegisterHotKey` | Enough for global toggle/cancel without low-level hooks. |
| Startup | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` | Per-user, no admin needed. |
| UI | Tray icon + tiny status pill + minimal settings dialog | Functional without becoming a UI project. |
| Transcription model | `gpt-4o-transcribe` | High-quality OpenAI speech-to-text. |
| API response format | `json` | Safest option for `gpt-4o-transcribe` and `gpt-4o-mini-transcribe`. |

---

## 2. Done definition

The project is done when this works on a normal Windows user account:

```text
1. Run VoiceAgentTyper.exe.
2. Enter/save OpenAI API key once in the app.
3. Click into Notepad.
4. Press F8.
5. Speak.
6. Press F8 again.
7. Transcript appears in Notepad.
8. Existing text clipboard is restored if configured.
9. The app never presses Enter.
10. App can run from tray.
11. App can auto-start with Windows.
```

Secondary manual tests:

```text
- VS Code Copilot Chat textbox
- Claude Code terminal/input if used
- Browser ChatGPT textbox
- X/LinkedIn/browser post textbox
- Normal Notepad text area
```

---

## 3. Development environment rule

This project should be built and run as a **native Windows app**.

Do **not** build it inside WSL.

Use WSL for Linux/backend/server work. Use native Windows tooling for this repo.

Recommended repo location:

```text
C:\dev\VoiceAgentTyper
```

Open it in normal VS Code, not `Remote - WSL`.

---

## 4. Install toolchain

### 4.1 Install Visual Studio Build Tools with C++ workload

A plain Build Tools install can be incomplete. Install with the Visual C++ workload explicitly.

Run PowerShell as a normal user first. If winget/Visual Studio installer asks for elevation, approve it.

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --source winget `
  --accept-package-agreements `
  --accept-source-agreements `
  --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --norestart"
```

After installation, open a fresh terminal.

If `windows.h`, `cl.exe`, or the Windows SDK is missing, open **Visual Studio Installer**, modify **Build Tools 2022**, and make sure these are installed:

```text
- Desktop development with C++
- MSVC v143 C++ build tools
- Windows 10 or Windows 11 SDK
- C++ CMake tools for Windows
```

### 4.2 Install CMake, Ninja, and Git

```powershell
winget install --id Kitware.CMake --source winget
winget install --id Ninja-build.Ninja --source winget
winget install --id Git.Git --source winget
```

### 4.3 Install vcpkg

```powershell
cd C:\dev
git clone https://github.com/microsoft/vcpkg.git
cd C:\dev\vcpkg
.\bootstrap-vcpkg.bat
setx VCPKG_ROOT C:\dev\vcpkg
```

Close and reopen the terminal after `setx`.

Check:

```powershell
$env:VCPKG_ROOT
C:\dev\vcpkg\vcpkg version
```

---

## 5. Terminal choice

For command-line builds with Ninja + MSVC, use one of these:

```text
Option A: "Developer PowerShell for VS 2022"
Option B: VS Code CMake Tools with the Visual Studio Build Tools kit selected
```

If using normal PowerShell and `cl.exe` is not found, launch the Visual Studio developer shell first.

Common Build Tools path:

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64
```

Full Visual Studio Community path, if applicable:

```powershell
& "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64
```

---

## 6. Repo layout

```text
VoiceAgentTyper/
  CMakeLists.txt
  CMakePresets.json
  vcpkg.json
  README.md
  LICENSE
  config.example.json

  src/
    main.cpp

    App.hpp
    App.cpp

    HotkeyManager.hpp
    HotkeyManager.cpp

    AudioRecorder.hpp
    AudioRecorder.cpp

    WavWriter.hpp
    WavWriter.cpp

    SilenceTrimmer.hpp
    SilenceTrimmer.cpp

    TranscriptionClient.hpp
    TranscriptionClient.cpp

    TextInjector.hpp
    TextInjector.cpp

    Clipboard.hpp
    Clipboard.cpp

    CredentialStore.hpp
    CredentialStore.cpp

    Config.hpp
    Config.cpp

    TrayIcon.hpp
    TrayIcon.cpp

    StatusWindow.hpp
    StatusWindow.cpp

    StartupManager.hpp
    StartupManager.cpp

    SettingsWindow.hpp
    SettingsWindow.cpp

    Utf.hpp
    Utf.cpp

  assets/
    icon.ico

  scripts/
    build-debug.ps1
    build-release.ps1
```

---

## 7. vcpkg manifest

Use manifest mode. Do not require users or coding agents to mutate global vcpkg state for this project.

`vcpkg.json`:

```json
{
  "name": "voice-agent-typer",
  "version-string": "0.1.0",
  "dependencies": [
    "portaudio",
    "cpr",
    "libopusenc",
    "nlohmann-json",
    "spdlog"
  ]
}
```

Manifest mode means vcpkg resolves dependencies for this project when CMake configures through the vcpkg toolchain.

Optional later improvement:

```text
Add a builtin-baseline to lock dependency versions once the first build works.
```

Do not block v0.1 on dependency pinning.

---

## 8. CMake presets

Use `CMakePresets.json` so the coding agent does not invent configure commands.

`CMakePresets.json`:

```json
{
  "version": 6,
  "cmakeMinimumRequired": {
    "major": 3,
    "minor": 25,
    "patch": 0
  },
  "configurePresets": [
    {
      "name": "windows-msvc-debug",
      "displayName": "Windows MSVC Debug",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/out/build/${presetName}",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
        "VCPKG_TARGET_TRIPLET": "x64-windows"
      }
    },
    {
      "name": "windows-msvc-release",
      "displayName": "Windows MSVC Release",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/out/build/${presetName}",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Release",
        "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
        "VCPKG_TARGET_TRIPLET": "x64-windows"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "debug",
      "configurePreset": "windows-msvc-debug"
    },
    {
      "name": "release",
      "configurePreset": "windows-msvc-release"
    }
  ]
}
```

Build:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset debug
```

Release build:

```powershell
cmake --preset windows-msvc-release
cmake --build --preset release
```

---

## 9. CMakeLists.txt

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)

project(VoiceAgentTyper
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(VoiceAgentTyper WIN32
    src/main.cpp

    src/App.cpp
    src/HotkeyManager.cpp
    src/AudioRecorder.cpp
    src/WavWriter.cpp
    src/SilenceTrimmer.cpp
    src/TranscriptionClient.cpp
    src/TextInjector.cpp
    src/Clipboard.cpp
    src/CredentialStore.cpp
    src/Config.cpp
    src/TrayIcon.cpp
    src/StatusWindow.cpp
    src/StartupManager.cpp
    src/SettingsWindow.cpp
    src/Utf.cpp
)

target_compile_definitions(VoiceAgentTyper PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(portaudio CONFIG REQUIRED)

target_link_libraries(VoiceAgentTyper PRIVATE
    cpr::cpr
    nlohmann_json::nlohmann_json
    portaudio
    user32
    shell32
    advapi32
    ole32
)

if (MSVC)
    target_compile_options(VoiceAgentTyper PRIVATE
        /W4
        /permissive-
        /utf-8
    )
endif()
```

PortAudio note:

```text
The PortAudio CMake target from vcpkg is usually `portaudio` for dynamic and `portaudio_static` for static.
If configure fails on the PortAudio target, inspect the vcpkg usage output or:
%VCPKG_ROOT%\installed\x64-windows\share\portaudio
```

For v0.1, use `x64-windows` dynamic linking. If packaging later becomes annoying because of DLLs, consider `x64-windows-static` or a package script that copies required DLLs.

---

## 10. Config

Runtime config path:

```text
%APPDATA%\VoiceAgentTyper\config.json
```

First launch behavior:

```text
if config does not exist:
    create %APPDATA%\VoiceAgentTyper
    copy config.example.json to config.json
```

`config.example.json`:

```json
{
  "hotkeys": {
    "toggle_recording": "F8",
    "cancel_recording": "Escape"
  },
  "transcription": {
    "provider": "openai",
    "language_hint": "en",
    "openai": {
      "model": "gpt-4o-transcribe",
      "credential_target": "VoiceAgentTyper/OpenAI",
      "prompt": "The user is dictating technical prompts for coding agents, IDEs, VS Code, GitHub Copilot, Claude Code, Codex, Python, C++, FastAPI, LangChain, OpenAI, Docker, GitHub, APIs, terminals, embeddings, rerankers, and software engineering workflows."
    },
    "mistral": {
      "model": "voxtral-mini-latest",
      "credential_target": "VoiceAgentTyper/Mistral",
      "context_bias": "VoxInsert,VS Code,GitHub Copilot,Claude Code,Codex,Python,C++,FastAPI,LangChain,OpenAI,Docker,GitHub,APIs,terminals,embeddings,rerankers"
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
    "frames_per_buffer": 512,
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
}
```

Config rules:

```text
- Do not store API key in config.json.
- Store only Credential Manager target name.
- Config parse failure should show a clear error, not crash silently.
- Missing fields should fall back to defaults where reasonable.
```

---

## 11. API key storage

Use Windows Credential Manager.

Credential:

```text
Type:        CRED_TYPE_GENERIC
TargetName:  VoiceAgentTyper/OpenAI
UserName:    openai
Blob:        API key bytes
Persist:     CRED_PERSIST_LOCAL_MACHINE
```

Public artifact behavior:

```text
Settings UI
-> user pastes API key
-> Save
-> app writes to Credential Manager
-> config.json still contains no secret
```

Class shape:

```cpp
class CredentialStore {
public:
    void saveApiKey(const std::wstring& targetName, const std::string& apiKey);
    std::optional<std::string> readApiKey(const std::wstring& targetName) const;
    std::string readRequired(const std::wstring& targetName) const;
    void deleteApiKey(const std::wstring& targetName);
};
```

Use:

```text
CredWriteW
CredReadW
CredDeleteW
CredFree
```

Minimal implementation:

```cpp
void CredentialStore::saveApiKey(
    const std::wstring& targetName,
    const std::string& apiKey
) {
    CREDENTIALW credential = {};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(targetName.c_str());
    credential.UserName = const_cast<LPWSTR>(L"openai");
    credential.CredentialBlobSize = static_cast<DWORD>(apiKey.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(
        const_cast<char*>(apiKey.data())
    );
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;

    if (!CredWriteW(&credential, 0)) {
        throw std::runtime_error("CredWriteW failed: " + std::to_string(GetLastError()));
    }
}
```

Remember:

```text
- Call CredFree after CredReadW succeeds.
- Do not log the credential blob.
- If credential is missing, open settings or show setup error.
```

---

## 12. App state machine

```text
Idle
  F8 -> Recording

Recording
  F8  -> Transcribing
  Esc -> Cancelled -> Idle

Transcribing
  Success -> Inserting
  Failure -> Error -> Idle

Inserting
  Success -> Idle
  Failure -> Error -> Idle
```

Rules:

```text
- F8 starts only when Idle.
- F8 stops only when Recording.
- Esc cancels only when Recording.
- Transcription runs on worker thread.
- Audio recording runs on worker thread.
- UI message loop stays responsive.
- App never presses Enter.
```

---

## 13. Component responsibilities

### 13.1 `App`

Owns:

```text
- process lifecycle
- message loop routing
- state machine
- component construction
- cross-component error handling
```

Shape:

```cpp
enum class AppState {
    Idle,
    Recording,
    Transcribing,
    Inserting
};

class App {
public:
    explicit App(HINSTANCE instance);
    int run();

private:
    void handleHotkey(WPARAM hotkeyId);
    void startRecording();
    void stopRecordingAndTranscribe();
    void cancelRecording();
    void setState(AppState nextState);
};
```

---

### 13.2 `HotkeyManager`

Owns:

```text
- RegisterHotKey
- UnregisterHotKey
- hotkey ID mapping
```

Default IDs:

```cpp
constexpr int HOTKEY_TOGGLE_RECORDING = 1;
constexpr int HOTKEY_CANCEL_RECORDING = 2;
```

Use `MOD_NOREPEAT` where possible.

If F8 is already registered by another app, show:

```text
Could not register F8. Choose another hotkey in config.
```

Do not use low-level keyboard hooks for v0.1.

---

### 13.3 `AudioRecorder`

Owns:

```text
- PortAudio init/terminate
- default microphone stream
- blocking read loop
- recording worker thread
- PCM16 sample buffer
```

Audio format:

```text
sample rate: 16000 Hz
channels: 1
format: signed 16-bit PCM
frames per buffer: 512
```

Thread behavior:

```text
start()
    clear samples
    set recording flag true
    start worker thread
    open default input stream
    while recording flag true:
        Pa_ReadStream(...)
        append samples
    close stream

stop()
    set recording flag false
    join worker
    return samples

cancel()
    set recording flag false
    join worker
    discard samples
```

For v0.1, use blocking `Pa_ReadStream`. Callback mode is not necessary.

---

### 13.4 `WavWriter`

Owns:

```text
- write valid RIFF/WAVE header
- write PCM16 data
```

Output path:

```text
%TEMP%\VoiceAgentTyper\recording-{timestamp}.wav
```

Shape:

```cpp
class WavWriter {
public:
    std::filesystem::path writePcm16Mono(
        const std::vector<int16_t>& samples,
        int sampleRate
    );
};
```

Test manually:

```text
Record -> write WAV -> open it in Windows Media Player or Audacity.
```

Do this before API work.

---

### 13.5 `SilenceTrimmer`

Owns:

```text
- simple RMS-based silence trimming
- leading/trailing silence removal
- optional long internal silence removal
```

Keep this crude. Do not make VAD a project.

Algorithm:

```text
1. Split PCM into 20 ms frames.
2. Calculate RMS per frame.
3. Mark frame as speech if RMS > threshold.
4. Keep 250 ms before first speech.
5. Keep 500 ms after last speech.
6. Optionally remove internal silent gaps longer than 1000 ms.
7. Write trimmed WAV.
```

If this becomes annoying, implement only leading/trailing trim and move on.

---

### 13.6 `TranscriptionClient`

Owns:

```text
- read API key from CredentialStore
- multipart POST to OpenAI
- parse JSON response
```

Endpoint:

```text
POST https://api.openai.com/v1/audio/transcriptions
```

Fields:

```text
model           = gpt-4o-transcribe
file            = recording.wav
language        = en
prompt          = config prompt
response_format = json
```

Use JSON response and parse `text`.

Suggested implementation:

```cpp
std::string TranscriptionClient::transcribe(const std::filesystem::path& wavPath) {
    const std::string apiKey = credentialStore.readRequired(
        utf8ToWide(config.transcription.credentialTarget)
    );

    cpr::Response response = cpr::Post(
        cpr::Url{"https://api.openai.com/v1/audio/transcriptions"},
        cpr::Header{
            {"Authorization", "Bearer " + apiKey}
        },
        cpr::Multipart{
            {"model", config.transcription.model},
            {"language", config.transcription.languageHint},
            {"prompt", config.transcription.prompt},
            {"response_format", "json"},
            {"file", cpr::File{wavPath.string()}}
        },
        cpr::Timeout{120000}
    );

    if (response.status_code < 200 || response.status_code >= 300) {
        throw std::runtime_error("Transcription failed: " + response.text);
    }

    nlohmann::json body = nlohmann::json::parse(response.text);
    return body.at("text").get<std::string>();
}
```

Before implementing in C++, test with curl:

```powershell
curl https://api.openai.com/v1/audio/transcriptions `
  -H "Authorization: Bearer $env:OPENAI_API_KEY" `
  -F "model=gpt-4o-transcribe" `
  -F "response_format=json" `
  -F "language=en" `
  -F "file=@recording.wav"
```

For v0.1, it is acceptable to temporarily read `OPENAI_API_KEY` from the environment during Milestone 4. Final app must use Credential Manager.

---

### 13.7 `Clipboard`

Owns:

```text
- reading current Unicode text clipboard
- setting Unicode text clipboard
- RAII wrapper around OpenClipboard/CloseClipboard
```

Use `CF_UNICODETEXT`.

v0.1 only restores text clipboard content.

Known limitation:

```text
If the clipboard previously contained an image/file/rich text, v0.1 does not preserve that full format set.
```

This is acceptable for the first version.

---

### 13.8 `TextInjector`

Owns:

```text
- insert final transcript into focused textbox
- never auto-send
```

Default insertion:

```text
1. Read current Unicode text clipboard if available.
2. Set clipboard to transcript.
3. Send Ctrl+V using SendInput.
4. Sleep briefly.
5. Restore previous Unicode text clipboard if configured.
```

Shape:

```cpp
class TextInjector {
public:
    void insertText(const std::wstring& text);

private:
    void sendCtrlV();
};
```

Do not implement character-by-character typing for v0.1 unless clipboard paste fails badly.

Important limitation:

```text
SendInput can fail or be ignored for elevated/admin target apps because Windows blocks lower-integrity apps from injecting input into higher-integrity apps.
```

That is fine. Document it.

---

### 13.9 `StatusWindow`

Owns:

```text
- tiny non-intrusive overlay
- status text
- error display
```

Window styles:

```text
WS_POPUP
WS_EX_TOPMOST
WS_EX_TOOLWINDOW
WS_EX_NOACTIVATE
```

States:

| State | Text |
|---|---|
| Idle | hidden |
| Recording | `Recording — F8 to stop, Esc to cancel` |
| Transcribing | `Transcribing...` |
| Inserting | `Inserting text...` |
| Cancelled | `Cancelled` |
| Error | short error |

Rules:

```text
- Does not steal focus.
- Does not appear in taskbar.
- Small bottom-right by default.
- Hidden when idle.
- No fancy animation required.
```

---

### 13.10 `TrayIcon`

Owns:

```text
- tray icon
- right-click menu
- menu command routing
```

Menu:

```text
Start / stop recording
Cancel recording
Open settings
Reload config
Enable startup
Disable startup
Exit
```

Use `Shell_NotifyIcon`.

---

### 13.11 `SettingsWindow`

Keep it minimal.

Required v0.1 UI:

```text
- API key masked input
- Save API key
- Clear API key
- Open config file
- Close
```

Nice but optional:

```text
- Test key button
- Hotkey field
- Model dropdown
```

Do not turn settings into the project.

If no API key exists on startup:

```text
- launch app
- show settings window
- ask user to enter key
```

---

### 13.12 `StartupManager`

Owns:

```text
- enable startup
- disable startup
- check startup state
```

Registry key:

```text
HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run
```

Value:

```text
VoiceAgentTyper = "C:\path\to\VoiceAgentTyper.exe"
```

No admin required.

---

## 14. Main pipeline

Expected application flow:

```cpp
void App::startRecording() {
    if (state != AppState::Idle) {
        return;
    }

    setState(AppState::Recording);
    statusWindow.show(L"Recording — F8 to stop, Esc to cancel");
    audioRecorder.start();
}

void App::stopRecordingAndTranscribe() {
    if (state != AppState::Recording) {
        return;
    }

    setState(AppState::Transcribing);
    statusWindow.show(L"Transcribing...");

    std::thread worker([this]() {
        try {
            std::vector<int16_t> samples = audioRecorder.stop();

            std::filesystem::path rawWavPath = wavWriter.writePcm16Mono(
                samples,
                config.audio.sampleRate
            );

            std::filesystem::path uploadWavPath = rawWavPath;

            if (config.audio.useSilenceTrimming) {
                uploadWavPath = silenceTrimmer.trim(rawWavPath);
            }

            std::string transcriptUtf8 = transcriptionClient.transcribe(uploadWavPath);

            setState(AppState::Inserting);
            statusWindow.show(L"Inserting text...");

            std::wstring transcriptWide = utf8ToWide(transcriptUtf8);
            textInjector.insertText(transcriptWide);

            setState(AppState::Idle);
            statusWindow.hideAfter(800);
        }
        catch (const std::exception& error) {
            setState(AppState::Idle);
            statusWindow.showError(utf8ToWide(error.what()));
        }
    });

    worker.detach();
}

void App::cancelRecording() {
    if (state != AppState::Recording) {
        return;
    }

    audioRecorder.cancel();
    setState(AppState::Idle);
    statusWindow.showTemporary(L"Cancelled", 800);
}
```

For v0.1, a detached worker is acceptable if shutdown is handled carefully. Cleaner later:

```text
- own a std::jthread
- disable exit while transcription is active
- or wait for worker on shutdown
```

---

## 15. Implementation milestones

### Milestone 1 — buildable skeleton

Done when:

```text
- CMake configures.
- Project builds.
- App starts and exits cleanly.
```

Build:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset debug
```

---

### Milestone 2 — hotkey state machine

Done when:

```text
- Press F8 -> Recording state.
- Press F8 again -> Transcribing placeholder state.
- Press Esc while recording -> Idle.
```

Implement:

```text
- App
- HotkeyManager
- message loop
- simple debug output or MessageBox/status text
```

Do not implement audio yet.

---

### Milestone 3 — recording to WAV

Done when:

```text
- Press F8 to start.
- Press F8 to stop.
- Valid WAV file appears in temp folder.
- You can play the WAV manually.
```

Implement:

```text
- AudioRecorder
- WavWriter
```

Do not touch OpenAI until WAV playback works.

---

### Milestone 4 — transcription

Done when:

```text
- WAV uploads to OpenAI.
- Transcript prints to debug output.
```

Implement:

```text
- temporary env var fallback or CredentialStore
- TranscriptionClient
- error handling for API failures
```

Test API with curl before debugging C++ HTTP code.

---

### Milestone 5 — focused text insertion

Done when:

```text
- Click into Notepad.
- F8 start.
- Speak.
- F8 stop.
- Transcript appears in Notepad.
```

Implement:

```text
- Clipboard
- TextInjector
- Ctrl+V via SendInput
```

This is the first true success.

---

### Milestone 6 — Credential Manager

Done when:

```text
- API key is saved through app/UI.
- API key is read from Credential Manager.
- Environment variable is no longer required.
```

Implement:

```text
- CredentialStore
- minimal SettingsWindow or setup dialog
```

---

### Milestone 7 — status pill

Done when:

```text
- Recording/transcribing/inserting/error states are visible.
- Status pill does not steal focus.
```

Implement:

```text
- StatusWindow
```

---

### Milestone 8 — tray icon

Done when:

```text
- App lives in system tray.
- Exit works from tray menu.
- Start/stop/cancel menu commands work.
```

Implement:

```text
- TrayIcon
```

---

### Milestone 9 — auto-start

Done when:

```text
- Enable startup writes HKCU Run key.
- Disable startup removes it.
- App starts after Windows login.
```

Implement:

```text
- StartupManager
```

---

### Milestone 10 — silence trimming

Done when:

```text
- Leading/trailing silence is removed.
- Speech is not cut off.
```

Implement only after the basic app works.

---

## 16. Error handling

User-facing messages should be short.

| Failure | Message |
|---|---|
| Missing API key | `Missing OpenAI API key. Open settings.` |
| Invalid API key | `Invalid OpenAI API key.` |
| F8 unavailable | `F8 is already used by another app.` |
| No microphone | `No input microphone found.` |
| Recording failed | `Could not record audio.` |
| API/network fail | `Transcription failed. Check connection.` |
| Clipboard failed | `Could not insert text.` |
| Elevated target app | `Target app may block simulated input.` |
| Config invalid | `Invalid config. Open config file.` |

Log internal details separately.

Never log:

```text
- API key
- full credential blobs
```

---

## 17. Manual test checklist

| Test | Expected |
|---|---|
| App starts | Tray icon appears |
| No API key | Settings/setup opens |
| Save API key | Credential saved |
| F8 idle | Recording starts |
| F8 recording | Recording stops |
| Esc recording | Recording cancels |
| Esc idle | Nothing disruptive |
| Notepad focused | Transcript appears |
| Browser textbox focused | Transcript appears |
| VS Code textbox focused | Transcript appears |
| Clipboard had text | Text clipboard restored |
| Invalid key | Clear error |
| Network disabled | Clear error |
| No mic | Clear error |
| Very short recording | Either transcribes or clear no-speech error |
| Long recording | Max duration respected |
| App exit | Tray cleanup and hotkeys unregistered |

---

## 18. Debug checklist

| Problem | Likely cause |
|---|---|
| `cmake` cannot find compiler | Not in Developer PowerShell / MSVC not installed |
| `windows.h` missing | Windows SDK missing |
| `find_package(portaudio)` fails | vcpkg manifest/toolchain not active |
| PortAudio target missing | Inspect vcpkg usage output; target may be `portaudio`/`portaudio_static` |
| F8 does nothing | Hotkey registration failed |
| Text inserts twice | hotkey repeat not suppressed |
| Recording empty | default mic issue or stream not started |
| WAV will not play | bad WAV header |
| API returns 401 | bad/missing key |
| API returns 400 | bad multipart fields or model/response format mismatch |
| JSON parse fails | error body parsed as success |
| Text inserts nowhere | focus changed before Ctrl+V |
| Clipboard not restored | timing or clipboard ownership issue |
| Admin app target fails | Windows UIPI/integrity level blocking input |
| App freezes | API call on UI thread |

---

## 19. Scripts

`scripts/build-debug.ps1`:

```powershell
$ErrorActionPreference = "Stop"

cmake --preset windows-msvc-debug
cmake --build --preset debug
```

`scripts/build-release.ps1`:

```powershell
$ErrorActionPreference = "Stop"

cmake --preset windows-msvc-release
cmake --build --preset release
```

---

## 20. README skeleton

```markdown
# VoiceAgentTyper

A small Windows tray utility that records speech, transcribes it, and inserts the transcript into the currently focused text box.

It is designed for dictating prompts into AI agents, editors, browsers, terminals, and normal text fields.

## Features

- Toggle recording with F8
- Cancel recording with Esc
- OpenAI transcription
- Inserts text into the focused textbox
- Does not press Enter
- Stores API key in Windows Credential Manager
- Small status overlay
- Windows auto-start support

## Setup

1. Run VoiceAgentTyper.exe.
2. Enter your OpenAI API key in settings.
3. Click into any text box.
4. Press F8.
5. Speak.
6. Press F8 again.
7. Review inserted text.
8. Send manually.

## Known limitations

- Windows only.
- Uses clipboard paste internally.
- Restores only plain text clipboard content in v0.1.
- Requires an OpenAI API key.
- No live preview in v0.1.
- Some elevated/admin apps may reject simulated paste input.
```

---

## 21. Known limitations

Add this to the public README:

```markdown
## Known limitations

- Windows only.
- Uses clipboard paste internally.
- Restores only plain text clipboard content in v0.1.
- Requires an OpenAI API key.
- No live transcription preview in v0.1.
- Some elevated/admin apps may reject simulated paste input.
- Long recordings may take a few seconds to transcribe.
- No local/offline transcription.
```

These limitations are acceptable. Do not hide them.

---

## 22. What not to build yet

Do not build these before the end-to-end loop works:

```text
- installer
- auto-updater
- GitHub release automation
- live streaming preview
- realtime WebSocket transcription
- local Whisper/faster-whisper
- multi-provider abstraction
- tweet/post mode
- coding-agent cleanup mode
- voice commands
- OS assistant
- fancy UI
- plugin system
- database
```

The first success target remains:

```text
F8 -> speak -> F8 -> text appears in Notepad
```

---

## 23. First coding-agent prompt

Use this prompt to start:

```text
We are building VoiceAgentTyper, a Windows-only C++20 tray utility.

The v0.1 goal is:
F8 toggles recording on/off.
When stopped, recorded microphone audio is saved as WAV, sent to OpenAI transcription, and the final transcript is inserted into the currently focused text box.
The app never presses Enter.
Esc cancels recording.
API key is stored in Windows Credential Manager.
Config is JSON.
Dependencies are managed through vcpkg manifest mode.
Build is CMake + Ninja + MSVC on Windows.

Start by creating the CMake project skeleton and implementing:
- CMakeLists.txt
- CMakePresets.json
- vcpkg.json
- main Win32 message loop
- App state machine
- HotkeyManager using RegisterHotKey for F8 and Esc
- placeholder AudioRecorder/TranscriptionClient/TextInjector classes
- debug logging for state transitions

Do not implement audio or API yet. First make F8/Esc state transitions work cleanly.
```

---

## 24. Reference links

Use these docs when implementing the related pieces:

```text
OpenAI Speech-to-Text:
https://developers.openai.com/api/docs/guides/speech-to-text

OpenAI Create Transcription API reference:
https://developers.openai.com/api/reference/resources/audio/subresources/transcriptions/methods/create/

Visual Studio Build Tools workload/component IDs:
https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools

Visual Studio command-line install parameters:
https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio

vcpkg manifest mode:
https://learn.microsoft.com/en-us/vcpkg/concepts/manifest-mode

vcpkg.json reference:
https://learn.microsoft.com/en-us/vcpkg/reference/vcpkg-json

CMake Presets:
https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html

Visual Studio CMake Presets:
https://learn.microsoft.com/en-us/cpp/build/cmake-presets-vs

Win32 RegisterHotKey:
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey

Win32 WM_HOTKEY:
https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-hotkey

Win32 SendInput:
https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput

Win32 Clipboard:
https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard

Windows Credential Manager CredWrite:
https://learn.microsoft.com/en-us/windows/win32/api/wincred/nf-wincred-credwritew

Windows Credential Manager CredRead:
https://learn.microsoft.com/en-us/windows/win32/api/wincred/nf-wincred-credreadw

PortAudio API overview:
https://portaudio.com/docs/v19-doxydocs/api_overview.html

cpr HTTP library:
https://github.com/libcpr/cpr

nlohmann/json:
https://github.com/nlohmann/json
```

---

## 25. Final reminder

Keep the first build dumb.

```text
Record.
Transcribe.
Insert.
Done.
```

Everything else is later.
