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
.\scripts\package-release.ps1 -Version v0.1.2
```

## GitHub Actions

The repository has two release-related workflows:

- `.github/workflows/ci.yml` packages a Windows artifact on pushes and pull requests that touch build, source, script, asset, or workflow files.
- `.github/workflows/release.yml` publishes GitHub Release assets when a `v*` tag is pushed.

Both workflows build on `windows-latest`, set up MSVC, bootstrap a repo-local vcpkg, ensure Ninja is available, and run the same packaging script used locally.

## Current Release Declaration

The current release trigger is explicit:

- update the repo version fields to the intended release version
- merge that change to `main`
- push a tag like `v0.1.2`

Important distinction:

- the pushed tag triggers the GitHub Release workflow
- commit signing is separate and is not what causes the release to publish

The current explicit version anchors are:

- `CMakeLists.txt` project version
- `vcpkg.json` `version-string`
- `src/VoxInsert.manifest` assembly version

If those files say `0.1.2` and the maintainer later pushes `v0.1.2`, the package name, release workflow, and app metadata stay aligned.

## Future Automation Options

There are three reasonable ways to automate releases from here.

### Option 1: Keep Manual Tags

This is the current model.

Pros:

- simplest workflow
- easiest to reason about
- release timing stays fully explicit
- works cleanly with locally created signed tags

Cons:

- maintainer must remember to create and push the tag every release
- release intent is not captured directly in the PR itself

### Option 2: Version In PR, Auto-Tag On Merge

In this model, the PR includes the version bump. After that PR merges to `main`, GitHub Actions reads the new version, creates the matching tag if it does not already exist, and the existing tagged-release workflow publishes the release.

Pros:

- closest fit to "this PR should ship as version X.Y.Z"
- release intent is visible in code review
- removes the manual tagging step
- keeps the version bump under normal review instead of hiding it in automation

Cons:

- concurrent version-bump PRs can conflict
- merging the version-bump PR effectively means "ship this now"
- auto-created tags usually come from GitHub Actions rather than a maintainer's local signing key

This is the recommended future automation path for VoxInsert if the goal is less manual release work without adding much workflow complexity.

### Option 3: Label-Driven Or Bot-Driven Releases

In this model, a PR gets a label such as `release-patch`, `release-minor`, or an exact version label. After merge, automation decides the next version, updates files, creates the tag, and publishes the release.

Pros:

- least manual release bookkeeping
- avoids hand-editing version files in many PRs

Cons:

- most workflow complexity
- the final release commit is bot-generated rather than the reviewed merge commit itself
- signed release tags are harder to preserve cleanly
- debugging release automation becomes a project of its own

For this repo, that is probably more machinery than the project needs right now.

## Recommended Maintainer Rule

For now:

- keep the explicit version bump in the PR when a release is intended
- keep the manual tag push after merge if signed tags are important

If manual tagging becomes too annoying later, move to Option 2 rather than jumping straight to a bot-managed semantic-release style workflow.

## Design Rules

- Install per-user; do not require admin rights.
- Keep app behavior settings in the VoxInsert Settings dialog, not the installer.
- Preserve user config, logs, archives, and credentials on upgrade.
- Make destructive cleanup an explicit uninstall choice.
- Keep the zip flow simple unless a classic installer becomes clearly worth the extra maintenance.
