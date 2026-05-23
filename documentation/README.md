# VoxInsert Learning Documentation

This folder is the step-by-step learning map for building VoxInsert yourself. It is intentionally separate from the implementation so you can study the Windows concepts before turning each one into code.

The repo currently contains the first hidden-host baseline: a native Windows C++ executable that configures, builds, creates a hidden control window, adds a tray icon with Quit, enters a Win32 message loop, writes a file log, and exits cleanly on shutdown. The guides below explain the pieces you will add later.

## Reading Order

1. [Toolchain And Build System](01-toolchain-build-system.md)
2. [Win32 App Lifetime](02-win32-app-lifetime.md)
3. [Messages And Window Procedures](03-messages-and-window-procedures.md)
4. [Unicode, Strings, And Paths](04-unicode-strings-and-paths.md)
5. [Config Files And AppData](05-config-files-and-appdata.md)
6. [Debugging, Logging, And Observability](06-debugging-logging-observability.md)
7. [Global Hotkeys](07-global-hotkeys.md)
8. [Tray Icon Control Surface](08-tray-icon-control-surface.md)
9. [Threading And Shutdown](09-threading-and-shutdown.md)
10. [Audio Capture With PortAudio](10-audio-capture-portaudio.md)
11. [WAV, RIFF, And PCM](11-wav-riff-pcm.md)
12. [HTTP, JSON, And Transcription](12-http-json-transcription.md)
13. [Text Insertion, Clipboard, And SendInput](13-text-insertion-clipboard-sendinput.md)
14. [Credential Manager](14-credential-manager.md)
15. [Startup Registration](15-startup-registration.md)
16. [Development Workflow And Milestones](16-development-workflow-and-milestones.md)

## How To Use These Guides

For each topic, learn in this order:

1. Read the mental model.
2. Look at the official references.
3. Do the suggested tiny exercise.
4. Add only one small feature to the app.
5. Build and run a smoke test before moving on.

This project should grow like a notebook that compiles. When a concept is still fuzzy, keep the implementation tiny. When the concept is clear, turn it into a real module.

## Current Repo Anchors

- The build entry points are `scripts/build-debug.ps1` and `scripts/build-release.ps1`.
- The CMake project starts at `CMakeLists.txt`.
- The preset configuration is in `CMakePresets.json`.
- The vcpkg dependency list is in `vcpkg.json`.
- The executable entry point is in `src/main.cpp`.
- The logging setup lives in `src/logging.cpp` and `src/logging.h`.
- The hidden window, tray icon, and smoke-test host live in `src/app_host.cpp` and `src/app_host.h`.
- Runtime logs are written to `%LOCALAPPDATA%\VoxInsert\logs\voxinsert.log`.

## Reference Style

Most links point to official Microsoft, CMake, vcpkg, PortAudio, OpenAI, cpr, and nlohmann/json documentation. A few C++ standard library links point to cppreference because it is the most practical reference for modern C++ concurrency details.
