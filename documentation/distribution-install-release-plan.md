# VoxInsert Distribution And Release Notes

This document describes the current zip-based install and release flow for VoxInsert. It is intentionally lightweight: the project ships as a per-user Windows utility, not as a full installer product.

## User Install Flow

Users download a release zip from GitHub Releases, extract it, then run:

```powershell
.\install.ps1
```

They can also double-click:

```text
Install VoxInsert.bat
```

The installer copies VoxInsert into:

```text
%LOCALAPPDATA%\Programs\VoxInsert
```

It can create Start Menu and Desktop shortcuts, launch VoxInsert after install, and open Settings on first launch so the user can enter provider credentials.

For automation or package testing:

```powershell
.\install.ps1 -NonInteractive
```

## Upgrade Behavior

Running `install.ps1` again refreshes the installed program files while preserving user-owned data:

- `%APPDATA%\VoxInsert\config.json`
- `%LOCALAPPDATA%\VoxInsert\logs`
- optional archive folders
- provider credentials stored in Windows Credential Manager

If VoxInsert is running, the installer stops it before replacing files.

## Uninstall Flow

The installed app includes:

```powershell
%LOCALAPPDATA%\Programs\VoxInsert\uninstall.ps1
```

The uninstaller removes program files and shortcuts. It asks before removing user data, archive files, or stored provider credentials.

The default uninstall behavior is conservative: remove the app, keep the user's config and credentials unless they explicitly choose full cleanup.

## Package Contents

A release zip is expected to contain the files needed to install and run VoxInsert without the source tree:

```text
VoxInsert-vX.Y.Z-win-x64/
  VoxInsert.exe
  *.dll
  config.example.json
  README.md
  LICENSE
  install.ps1
  uninstall.ps1
  Install VoxInsert.bat
```

The package should not require Visual Studio, CMake, Ninja, vcpkg, or the repository checkout on the target machine.

## Maintainer Packaging

Create a local release package with:

```powershell
.\scripts\package-release.ps1
```

The script builds the release executable, stages the required runtime files, runs the host and archive smoke tests from the staged package, creates a versioned zip under `out\release`, and writes a matching SHA256 file.

To package a specific version:

```powershell
.\scripts\package-release.ps1 -Version v0.1.0
```

## GitHub Actions

The repository has two release-related workflows:

- `.github/workflows/ci.yml` packages a Windows artifact on pushes and pull requests that touch build, source, script, asset, or workflow files.
- `.github/workflows/release.yml` publishes GitHub Release assets when a `v*` tag is pushed.

Both workflows build on `windows-latest`, set up MSVC, bootstrap a repo-local vcpkg, ensure Ninja is available, and run the same packaging script used locally.

## Design Rules

- Install per-user; do not require admin rights.
- Keep app behavior settings in the VoxInsert Settings dialog, not the installer.
- Preserve user config, logs, archives, and credentials on upgrade.
- Make destructive cleanup an explicit uninstall choice.
- Keep the zip flow simple unless a classic installer becomes clearly worth the extra maintenance.
