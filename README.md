# VoxInsert

VoxInsert is a Windows tray app for push-to-talk dictation. Press `F8`, speak, press `F8` again, and the transcript is pasted into the app you were already using.

It is designed as a local-first desktop utility: API keys stay in Windows Credential Manager, settings live in AppData, recordings are temporary by default, and optional archiving stays on your machine.

## Why VoxInsert?

VoxInsert is meant for people who want fast dictation without changing their writing workflow. It does not open a big editor or ask you to copy text around. It sits in the notification area, listens only during an active hotkey recording session, transcribes through your configured provider, and inserts the result into the focused text field.

Out of the box it supports:

- push-to-talk recording with `F8`
- cancel while recording with `Escape`
- OpenAI or Mistral transcription
- automatic clipboard paste into the focused app
- a small status pill while recording, transcribing, and inserting
- Settings UI for provider choice, hotkeys, credentials, startup, and archive options
- optional local archives with Opus audio, transcript text, and metadata

## Quick Start

Download the latest release zip from [GitHub Releases](https://github.com/dej-h/VoxInsert/releases), extract it, then run:

```powershell
.\install.ps1
```

You can also double-click `Install VoxInsert.bat`.

The installer asks where to install VoxInsert, whether to create shortcuts, whether to launch after install, and whether to open Settings on first launch. By default it installs to:

```text
%LOCALAPPDATA%\Programs\VoxInsert
```

On first launch, open `Settings...` from the tray icon and add an OpenAI API key, a Mistral API key, or both. Then:

1. Focus the editor, browser field, chat box, or other text field where the transcript should go.
2. Press `F8` to start recording.
3. Speak.
4. Press `F8` again to stop, transcribe, and insert.
5. Press `Escape` while recording to cancel without transcribing.

For automated installs or testing, skip installer prompts with:

```powershell
.\install.ps1 -NonInteractive
```

To uninstall later:

```powershell
& "$env:LOCALAPPDATA\Programs\VoxInsert\uninstall.ps1"
```

The uninstall flow removes the installed program files and shortcuts, then optionally removes config, logs, archive data, and stored provider credentials.

## Requirements

For a packaged release you need:

- Windows 10 or Windows 11
- PowerShell
- an OpenAI API key, a Mistral API key, or both

For a source build you also need:

- Visual Studio Build Tools 2022 with the C++ workload
- CMake
- Ninja
- Git
- vcpkg, normally at `C:\dev\vcpkg`

If PowerShell blocks local scripts in your current shell, run this once before using the repo scripts:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
```

## Settings

VoxInsert creates `%APPDATA%\VoxInsert\config.json` from [config.example.json](config.example.json) on first launch.

Most users should change settings through the tray menu instead of editing JSON directly. The Settings dialog can:

- choose OpenAI or Mistral as the transcription provider
- set provider models, prompts, and context bias terms
- save, replace, or remove provider API keys
- edit the language hint and hotkeys
- enable or disable Windows startup
- enable, hide, or reposition the status pill
- enable local archive output and choose what gets saved

Settings are applied when the app is idle. The tray menu can also reload config from disk.

## Privacy And Local Data

API keys are stored in Windows Credential Manager, not in `config.json`. The default credential targets are:

- OpenAI: `VoiceAgentTyper/OpenAI`
- Mistral: `VoiceAgentTyper/Mistral`

Runtime files live here:

```text
%APPDATA%\VoxInsert\config.json
%LOCALAPPDATA%\VoxInsert\logs\voxinsert.log
%TEMP%\VoxInsert\recording-*.wav
```

Archiving is disabled by default. When enabled, completed dictations are written under:

```text
%LOCALAPPDATA%\VoxInsert\Archive\YYYY\MM\DD\clip-*.opus
%LOCALAPPDATA%\VoxInsert\Archive\YYYY\MM\DD\clip-*.txt
%LOCALAPPDATA%\VoxInsert\Archive\YYYY\MM\DD\clip-*.json
```

Classic Win32 desktop apps do not show an in-app microphone consent dialog. If recording fails immediately, check:

```text
Settings > Privacy & security > Microphone
```

Make sure microphone access and desktop app microphone access are both enabled.

## Build From Source

Clone the repo, then bootstrap the native toolchain:

```powershell
git clone https://github.com/dej-h/VoxInsert.git VoxInsert
cd VoxInsert
.\scripts\bootstrap-toolchain.ps1
```

Build a debug executable:

```powershell
.\scripts\build-debug.ps1
```

Build a release executable:

```powershell
.\scripts\build-release.ps1
```

Run the debug app:

```powershell
& .\out\build\windows-msvc-debug\VoxInsert.exe
```

Run the release app:

```powershell
& .\out\build\windows-msvc-release\VoxInsert.exe
```

The build uses vcpkg manifest mode. Project dependencies are listed in [vcpkg.json](vcpkg.json): PortAudio, cpr, libopusenc, nlohmann/json, and spdlog.

To create a GitHub-distributable release zip locally:

```powershell
.\scripts\package-release.ps1
```

This builds the release executable, stages the runtime files, runs smoke tests from the staged package, and writes a zip under `out\release`.

## Test And Debug

Run the host smoke test:

```powershell
& .\out\build\windows-msvc-debug\VoxInsert.exe --smoke-test
$LASTEXITCODE
```

Run the archive smoke test:

```powershell
& .\out\build\windows-msvc-debug\VoxInsert.exe --archive-smoke-test
$LASTEXITCODE
```

The host smoke test exercises tray startup, shutdown, and recording state transitions without making a real transcription API call. The archive smoke test synthesizes PCM audio, writes an Opus archive clip plus transcript and metadata sidecars, verifies the files, and cleans up after itself.

Inspect recent logs with:

```powershell
Get-Content "$env:LOCALAPPDATA\VoxInsert\logs\voxinsert.log" -Tail 40
```

If the build fails with `LNK1168: cannot open VoxInsert.exe for writing`, close the running app from the tray or stop it:

```powershell
Get-Process VoxInsert -ErrorAction SilentlyContinue | Stop-Process -Force
```

If you replace [assets/VoxInsertIcon.png](assets/VoxInsertIcon.png), regenerate the Windows icons:

```powershell
pwsh -File .\scripts\generate-app-icon.ps1
```

## Current Limits

- VoxInsert is Windows only.
- Microphone selection uses the OS default input device; there is no device picker yet.
- Text insertion currently uses clipboard paste only.
- Mistral's dedicated offline transcription endpoint currently uses `voxtral-mini-latest`; Voxtral Small would require a separate chat-with-audio implementation path.
- Optional archive files are local filesystem files, not a searchable history UI yet.

## Project Map

The core runtime is split by responsibility:

- [src/runtime](src/runtime): hidden Win32 host, tray menu, settings integration, startup registration, and post-recording workflow
- [src/audio](src/audio): PortAudio capture and temporary WAV writing
- [src/transcription](src/transcription): provider contract, OpenAI provider, Mistral provider, and provider dispatch
- [src/insertion](src/insertion): clipboard paste insertion
- [src/security](src/security): Windows Credential Manager helpers
- [src/config](src/config): AppData config loading, validation, defaults, and saving
- [src/ui](src/ui): native settings dialog and status pill overlay
- [src/archive](src/archive): optional transcript and Ogg Opus audio archive service

## More Documentation

- [VoxInsert_Updated_Build_Guide.md](VoxInsert_Updated_Build_Guide.md)
- [documentation/reference/README.md](documentation/reference/README.md)
- [documentation/reference/openai-transcription-api.md](documentation/reference/openai-transcription-api.md)
- [documentation/reference/mistral-transcription-api.md](documentation/reference/mistral-transcription-api.md)
- [documentation/reference/transcript-post-processing.md](documentation/reference/transcript-post-processing.md)
