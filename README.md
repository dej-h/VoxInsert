# VoxInsert

This repo is set up for the first small buildable slices from [VoiceAgentTyper_Updated_Build_Guide.md](VoiceAgentTyper_Updated_Build_Guide.md): a buildable Windows-native skeleton plus a basic Win32 app lifetime.

For the step-by-step learning path behind the Windows APIs and build system, start with [documentation/README.md](documentation/README.md).

The current code intentionally does only this:

- CMake configures
- the project builds with MSVC + Ninja
- the executable creates a hidden Win32 control window
- the app enters a normal message loop
- the app adds a dedicated notification-area icon with a Quit menu
- the app loads configurable hotkeys from `%APPDATA%\VoxInsert\config.json`
- the default hotkeys are `F8` for toggle and `Escape` for cancel
- the toggle hotkey records from the current default microphone and writes a WAV file on stop
- after the WAV is written, the app uploads it to OpenAI transcription and pastes the returned text into the currently focused text field from a background worker
- while recording or processing, the app shows a click-through floating status pill above the tray icon
- the status pill shows live microphone activity while recording, a spinner while working, and short done/error states
- on startup, the app checks whether the configured OpenAI credential exists and offers to create it if it is missing
- the tray menu now includes `Set OpenAI Key...` and `Remove OpenAI Key`
- the cancel hotkey returns `Recording` back to `Idle`
- choosing Quit removes the tray icon and exits cleanly
- startup and shutdown are logged to `%LOCALAPPDATA%\VoxInsert\logs\voxinsert.log`
- `--smoke-test` drives a short start/cancel/start/stop recording sequence, skips the OpenAI upload, and exits automatically with code `0`

It currently uses clipboard paste for insertion and does not implement richer insertion modes yet.

## Toolchain bootstrap

Run this once in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\bootstrap-toolchain.ps1
```

The bootstrap script installs or validates the native Windows toolchain needed to start building the project yourself:

- Visual Studio Build Tools 2022 with the C++ workload
- CMake
- Ninja
- Git if `git.exe` is not already available
- PowerShell 7 if `pwsh.exe` is not already available
- `vcpkg` in `C:\dev\vcpkg`

It also sets the user-level `VCPKG_ROOT` environment variable.

## Build

Debug build:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-debug.ps1
```

Release build:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
```

## Run

Run the debug app normally:

```powershell
& .\out\build\windows-msvc-debug\VoiceAgentTyper.exe
```

The app starts hidden and adds a dedicated tray icon. Right-click the tray icon and choose `Quit` to exit cleanly.

On first launch, the app creates `%APPDATA%\VoxInsert\config.json` from [config.example.json](config.example.json) if that file does not already exist.

By default, press `F8` once to start recording from the current default microphone, press `Escape` to cancel back to idle, or press `F8` again to stop and write a WAV file under `%TEMP%\VoxInsert`. After transcription succeeds, the app pastes the transcript into whichever text field is currently focused by setting the clipboard temporarily and sending `Ctrl+V`. You can change those bindings and insertion options in `%APPDATA%\VoxInsert\config.json`. The current capture state is logged and also reflected in the tray tooltip/menu title.

For a classic desktop Win32 app like this one, Windows does not show an in-app microphone consent dialog. The app opens the OS default input device through PortAudio. If microphone privacy is blocked, recording start will fail and the error points you to Settings > Privacy & security > Microphone, where both device microphone access and desktop app microphone access must be enabled.

For the current transcription milestone, the app reads the OpenAI API key from Windows Credential Manager only. By default it looks for a Generic Credential with the target name `VoiceAgentTyper/OpenAI`, which matches `transcription.credential_target` in `%APPDATA%\VoxInsert\config.json`. After the WAV is written, a background worker uploads it to `https://api.openai.com/v1/audio/transcriptions` using the configured model, language hint, and prompt from `%APPDATA%\VoxInsert\config.json`, then pastes the returned transcript into the currently focused text field. The hidden Win32 window remains free to process tray, hotkey, and status-pill animation messages while that worker runs.

Insertion is currently implemented through `insertion.mode = clipboard_paste`. If `insertion.restore_clipboard` is `true`, the app restores the previous Unicode text clipboard contents after a short paste delay. If `insertion.auto_press_enter` is `true`, it sends `Enter` after the paste, but the default remains `false`.

The status pill follows [documentation/statuspill-design-spec.md](documentation/statuspill-design-spec.md). It is a passive, topmost, click-through overlay. During recording it consumes live RMS amplitude samples from the microphone stream and renders five waveform bars. During transcription and insertion it renders a spinner, then briefly shows success or error feedback before hiding.

If that credential is missing on startup, the app now prompts you to create it through a small built-in setup dialog. You can also open the tray menu later and choose `Set OpenAI Key...` or `Remove OpenAI Key`.

One-time setup in Windows Credential Manager:

- open Credential Manager
- choose Windows Credentials
- add a Generic Credential
- set the target name to `VoiceAgentTyper/OpenAI`
- set any username you want, such as `openai`
- paste the OpenAI API key into the password field

Run the automated lifetime smoke test:

```powershell
& .\out\build\windows-msvc-debug\VoiceAgentTyper.exe --smoke-test
$LASTEXITCODE
Get-Content "$env:LOCALAPPDATA\VoxInsert\logs\voxinsert.log" -Tail 12
```

If you replace [assets/VoxInsertIcon.png](assets/VoxInsertIcon.png), regenerate the Windows icon first:

```powershell
pwsh -File .\scripts\generate-app-icon.ps1
```

That script now produces both [assets/VoxInsertIcon.ico](assets/VoxInsertIcon.ico) for the executable/app resource and [assets/VoxInsertTray.ico](assets/VoxInsertTray.ico) for the notification-area tray icon.

The current tray-host implementation also follows Microsoft's recommended loading path for notification-area icons: the executable embeds a Common Controls v6 + DPI-awareness manifest, loads the tray icon with `LoadIconMetric`, and sets `NOTIFYICON_VERSION_4` after `NIM_ADD`.

If your local execution policy already allows local scripts, you can also run the build scripts directly as `./scripts/build-debug.ps1` and `./scripts/build-release.ps1`. The longer `powershell -ExecutionPolicy Bypass -File ...` form is just the most robust copy-paste wrapper for setup instructions.

## What is in the repo

- [CMakeLists.txt](CMakeLists.txt)
- [CMakePresets.json](CMakePresets.json)
- [vcpkg.json](vcpkg.json)
- [config.example.json](config.example.json)
- [documentation/README.md](documentation/README.md)
- [scripts/bootstrap-toolchain.ps1](scripts/bootstrap-toolchain.ps1)
- [scripts/build-debug.ps1](scripts/build-debug.ps1)
- [scripts/build-release.ps1](scripts/build-release.ps1)
- [scripts/generate-app-icon.ps1](scripts/generate-app-icon.ps1)
- [src/audio/audio_recorder.cpp](src/audio/audio_recorder.cpp)
- [src/audio/audio_recorder.h](src/audio/audio_recorder.h)
- [src/audio/wav_writer.cpp](src/audio/wav_writer.cpp)
- [src/audio/wav_writer.h](src/audio/wav_writer.h)
- [src/config/app_config.cpp](src/config/app_config.cpp)
- [src/config/app_config.h](src/config/app_config.h)
- [src/input/hotkey_manager.cpp](src/input/hotkey_manager.cpp)
- [src/input/hotkey_manager.h](src/input/hotkey_manager.h)
- [src/insertion/text_injector.cpp](src/insertion/text_injector.cpp)
- [src/insertion/text_injector.h](src/insertion/text_injector.h)
- [src/main.cpp](src/main.cpp)
- [src/observability/logging.cpp](src/observability/logging.cpp)
- [src/observability/logging.h](src/observability/logging.h)
- [src/resource.h](src/resource.h)
- [src/runtime/app_host.cpp](src/runtime/app_host.cpp)
- [src/runtime/app_host.h](src/runtime/app_host.h)
- [src/security/openai_credential_store.cpp](src/security/openai_credential_store.cpp)
- [src/security/openai_credential_store.h](src/security/openai_credential_store.h)
- [src/testing/app_host_smoke_test.cpp](src/testing/app_host_smoke_test.cpp)
- [src/testing/app_host_smoke_test.h](src/testing/app_host_smoke_test.h)
- [src/transcription/transcription_client.cpp](src/transcription/transcription_client.cpp)
- [src/transcription/transcription_client.h](src/transcription/transcription_client.h)
- [src/ui/status_pill.cpp](src/ui/status_pill.cpp)
- [src/ui/status_pill.h](src/ui/status_pill.h)
- [src/VoiceAgentTyper.rc](src/VoiceAgentTyper.rc)
- [assets/VoxInsertIcon.ico](assets/VoxInsertIcon.ico)
- [assets/VoxInsertIcon.png](assets/VoxInsertIcon.png)
- [assets/VoxInsertTray.ico](assets/VoxInsertTray.ico)

## Next step

The next incremental slice after this is a small reload-config action so prompt, insertion, and UI settings can be changed without restarting the app.
