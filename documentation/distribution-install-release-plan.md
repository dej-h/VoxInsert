# VoxInsert Distribution, Install, And Release Automation Plan

This document is the end-to-end plan for making VoxInsert easy to install, update, uninstall, and ship from GitHub.

It is intentionally written as a project plan, not as implementation code. The goal is to define the final workflow before adding scripts, packaging logic, or GitHub Actions.

## 1. Goal

The target user experience is:

```text
1. User opens the GitHub Releases page.
2. User downloads one zip file.
3. User extracts it.
4. User runs one install command or double-clicks one installer helper.
5. VoxInsert is copied into a stable per-user install folder.
6. Shortcuts are created.
7. VoxInsert launches.
8. User opens Settings and enters provider credentials.
9. The app works without the user needing CMake, Visual Studio, vcpkg, or the source repo.
```

The maintainer experience is:

```text
1. Build release.
2. Package release.
3. Upload the zip to GitHub Releases.
4. Let GitHub Actions repeat the same process automatically on main and on version tags.
```

## 2. Why This Is Worth Adding

VoxInsert is now useful enough that asking every user to clone the repo, install native toolchains, and build from source creates unnecessary friction.

An install and release workflow is now justified because it solves three real problems:

- setup on a second machine without rebuilding from source
- a cleaner experience for other users testing the app
- a repeatable release process that does not depend on manual file copying

## 3. Recommended Distribution Strategy

The recommended rollout is staged.

### Phase 1: Portable Release Zip With Install And Uninstall Scripts

This is the first shipping target.

Users download a zip from GitHub and run:

```powershell
.\install.ps1
```

Or, for non-technical users, double-click:

```text
Install VoxInsert.bat
```

This phase gives the best effort-to-value ratio:

- no admin requirement
- no external installer technology yet
- easy to inspect and debug
- easy to distribute through GitHub Releases
- keeps the app local-first and per-user

### Phase 2: GitHub Actions Packaging And Release Automation

Every push to `main` should build, test, package, and upload a downloadable artifact.

Every version tag such as `v0.1.0` should build, test, package, and publish a GitHub Release with the zip attached.

### Phase 3: Optional Polished Installer Later

After the zip-based flow works, the project can optionally add NSIS or WiX for a classic Windows installer `.exe`.

That phase is optional. It should not block the simpler zip workflow.

## 4. Preferred User-Facing Install Model

VoxInsert should install per-user, not machine-wide.

Default install location:

```text
%LOCALAPPDATA%\Programs\VoxInsert
```

Reasons:

- no admin rights required
- matches the current app behavior of using per-user AppData and Credential Manager
- simpler uninstall and update path
- fewer Windows permission edge cases

## 5. What The Installer Should And Should Not Configure

The installer should only configure OS integration and install behavior.

The installer should not duplicate the full Settings dialog.

### Installer Preferences That Make Sense

The install workflow may offer these preferences:

- install location, defaulting to `%LOCALAPPDATA%\Programs\VoxInsert`
- create Start Menu shortcut, default `Yes`
- create Desktop shortcut, default `No`
- launch VoxInsert after install, default `Yes`
- open Settings after first launch, default `Yes`
- preserve existing user data when reinstalling, default `Yes`

### Settings That Should Stay Inside VoxInsert

These should remain in the app Settings dialog rather than the installer:

- transcription provider selection
- OpenAI and Mistral API keys
- provider credential targets
- model names
- prompts and context-bias values
- hotkeys
- archive behavior
- status pill placement and UI behavior
- send-enter behavior

Reason: those are app configuration settings, not installation settings. Putting them in both places would create two competing control surfaces.

## 6. End User Workflows

### 6.1 New Install On Another Machine

Desired flow:

```text
1. Open GitHub Releases.
2. Download VoxInsert-vX.Y.Z-win-x64.zip.
3. Extract the zip.
4. Run Install VoxInsert.bat or install.ps1.
5. Installer copies files to %LOCALAPPDATA%\Programs\VoxInsert.
6. Installer creates shortcuts.
7. Installer optionally launches VoxInsert.
8. VoxInsert opens its tray icon.
9. User opens Settings and enters API keys.
10. User starts using the app.
```

Expected install command:

```powershell
.\install.ps1
```

Automation and test command:

```powershell
.\install.ps1 -NonInteractive
```

### 6.2 Reinstall Or Upgrade

Desired flow:

```text
1. User downloads a newer zip.
2. User runs install.ps1 again.
3. Installer detects an existing install.
4. Installer stops a running VoxInsert process if needed.
5. Installer replaces binaries and packaged assets.
6. Installer preserves AppData config and stored credentials by default.
7. Installer optionally relaunches the updated app.
```

Upgrade should not wipe:

- `%APPDATA%\VoxInsert\config.json`
- `%LOCALAPPDATA%\VoxInsert\logs`
- optional archive folders
- provider secrets stored in Windows Credential Manager

### 6.3 Uninstall

Desired flow:

```text
1. User runs uninstall.ps1 from the package or installed folder.
2. Uninstaller stops VoxInsert if it is running.
3. Uninstaller removes shortcuts.
4. Uninstaller removes the installed program files.
5. Uninstaller removes any installer-created startup integration.
6. Uninstaller asks whether to keep or remove user data.
7. Uninstaller asks whether to keep or remove stored provider credentials.
8. Uninstall completes cleanly.
```

Safe default behavior:

- remove program files and shortcuts
- keep user config and archives unless the user explicitly requests full cleanup
- ask before deleting provider credentials

## 7. Package Contents

The release zip should contain only what a normal user needs.

Target structure:

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

Notes:

- The exact DLL set depends on the final runtime output and linkage.
- The package should not include the full source tree.
- The package should not require vcpkg on the target machine.

## 8. Proposed Maintainer Scripts

### 8.1 `scripts/package-release.ps1`

This is the maintainer packaging command.

Purpose:

- build the release executable
- collect the files needed for distribution
- run validation checks
- emit a versioned zip for GitHub distribution

Expected maintainer command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package-release.ps1
```

Planned responsibilities:

```text
1. Ensure the release build exists or build it.
2. Create a clean staging folder.
3. Copy VoxInsert.exe into staging.
4. Copy all required runtime DLLs into staging.
5. Copy config.example.json, README.md, and LICENSE.
6. Copy install.ps1, uninstall.ps1, and Install VoxInsert.bat.
7. Run --smoke-test against the staged executable.
8. Run --archive-smoke-test against the staged executable.
9. Create VoxInsert-vX.Y.Z-win-x64.zip.
10. Optionally create a SHA256 checksum file.
11. Print the output path.
```

Suggested output path:

```text
out\release\VoxInsert-vX.Y.Z-win-x64.zip
```

### 8.2 `install.ps1`

This is the end-user installation script.

Purpose:

- install the already-built packaged app onto a Windows user profile
- configure shortcuts and optional launch behavior
- support upgrades without wiping data

Planned responsibilities:

```text
1. Verify the package contains the expected files.
2. Detect an existing install under %LOCALAPPDATA%\Programs\VoxInsert.
3. Stop a running VoxInsert process if one exists.
4. Create or refresh the install directory.
5. Copy packaged files into the install directory.
6. Create a Start Menu shortcut.
7. Optionally create a Desktop shortcut.
8. Optionally launch VoxInsert after install.
9. Optionally open Settings after first launch.
10. Print the installed path and next steps.
```

### 8.3 `uninstall.ps1`

This is the end-user uninstall script.

Purpose:

- remove installed binaries and shortcuts
- optionally remove data and credentials
- keep uninstall behavior explicit and safe

Planned responsibilities:

```text
1. Stop a running VoxInsert process.
2. Remove installer-created shortcuts.
3. Remove the install directory.
4. Remove installer-created startup integration, if any.
5. Offer a standard uninstall path that keeps AppData and archive files.
6. Offer a full cleanup path that also removes AppData, archives, and credentials.
7. Print a clear summary of what was removed and what was preserved.
```

### 8.4 `Install VoxInsert.bat`

This is a thin wrapper for users who do not want to type PowerShell commands.

Purpose:

- launch `install.ps1` with the right execution-policy bypass
- provide a double-click entry point from Explorer

Expected behavior:

```text
1. Start PowerShell.
2. Run install.ps1 from the extracted package directory.
3. Keep the window open long enough to show success or failure.
```

## 9. Data Ownership And Removal Rules

The install and uninstall design must be explicit about what lives where.

### Installed Program Files

```text
%LOCALAPPDATA%\Programs\VoxInsert
```

### Runtime Config

```text
%APPDATA%\VoxInsert\config.json
```

### Logs

```text
%LOCALAPPDATA%\VoxInsert\logs
```

### Optional Archive

Default:

```text
%LOCALAPPDATA%\VoxInsert\Archive
```

But user settings may point this somewhere else.

### Provider Credentials

Stored in Windows Credential Manager under the configured provider targets.

Expected uninstall policy:

- standard uninstall keeps config, logs, archive files, and credentials
- full uninstall removes config, logs, archive files, and credentials after explicit confirmation

## 10. Startup Behavior

The app already owns the runtime setting for Windows startup.

Recommended rule:

- do not introduce a second startup toggle in the installer unless there is a strong reason
- keep startup registration controlled by the app Settings UI and config system

This avoids drift between install-time choices and in-app settings.

If a startup option is eventually exposed by the installer, it must reuse the same underlying per-user startup mechanism and must stay synchronized with the app setting.

## 11. Release Packaging Rules

The release package should be deterministic and easy to audit.

Rules:

- package only release builds
- always run smoke tests before producing the final zip
- never package from a dirty or partially copied staging folder
- use a versioned top-level folder name inside the zip
- include human-readable README and LICENSE files
- prefer a checksum file for downloaded artifacts

Future improvement:

- switch from ad-hoc file copying to CMake install rules if runtime dependency collection becomes harder to maintain

## 12. GitHub Actions Automation Plan

GitHub Actions should handle two related but different workflows.

### 12.1 Continuous Integration On Push To `main`

Purpose:

- prove that the app still builds and packages on a clean Windows machine
- attach a downloadable zip artifact to each successful main build

Suggested workflow file:

```text
.github/workflows/ci.yml
```

Suggested trigger:

```yaml
on:
  push:
    branches: [main]
  pull_request:
    branches: [main]
```

Planned job steps:

```text
1. Checkout the repo.
2. Set up an MSVC developer environment on the GitHub Windows runner.
3. Set up or restore vcpkg.
4. Cache vcpkg downloads and build artifacts where practical.
5. Run the release build.
6. Run --smoke-test.
7. Run --archive-smoke-test.
8. Run scripts/package-release.ps1.
9. Upload the resulting zip as a workflow artifact.
```

Important repo-specific note:

- current local build scripts look for Build Tools or Community paths
- GitHub-hosted Windows runners often expose Visual Studio Enterprise
- the CI plan should either use an action such as `ilammy/msvc-dev-cmd@v1` or expand script detection so CI does not depend on one specific Visual Studio edition path

### 12.2 Tagged Release Workflow

Purpose:

- create the actual public download that users see under GitHub Releases

Suggested workflow file:

```text
.github/workflows/release.yml
```

Suggested trigger:

```yaml
on:
  push:
    tags:
      - "v*"
```

Planned job steps:

```text
1. Checkout the repo.
2. Set up MSVC and vcpkg.
3. Build release.
4. Run both smoke tests.
5. Run scripts/package-release.ps1 using the tag as the version.
6. Create a GitHub Release.
7. Attach the zip artifact.
8. Attach an optional SHA256 checksum file.
```

Result:

```text
Users go to GitHub Releases and download one zip file.
```

### 12.3 Manual Workflow Dispatch

Optional improvement:

- add `workflow_dispatch` to allow a manual packaging run without pushing a commit or tag

This is useful for testing release automation before the first public version tag.

## 13. Recommended Implementation Order

The work should be done in this order.

### Step 1: Lock The Data And Install Rules

Define and document:

- install directory
- shortcut behavior
- uninstall cleanup rules
- what is preserved on upgrade

### Step 2: Add `install.ps1` And `uninstall.ps1`

Get the local packaging story working by hand before adding CI.

Success condition:

```text
An extracted local package can install, upgrade, and uninstall on the same machine.
```

### Step 3: Add `scripts/package-release.ps1`

Get one command that assembles a clean release zip from the local repo.

Success condition:

```text
The generated zip can be extracted and installed without the source tree.
```

### Step 4: Validate A Fresh-Machine Simulation

Test the staged zip on:

- the current machine after uninstall
- a second Windows machine
- or a clean VM

Success condition:

```text
Download zip -> extract -> run installer -> launch app -> open Settings -> add key -> works.
```

### Step 5: Add `.github/workflows/ci.yml`

Automate build, smoke tests, packaging, and artifact upload on `main`.

Success condition:

```text
Every successful main build has a downloadable packaged artifact.
```

### Step 6: Add `.github/workflows/release.yml`

Automate tagged public releases.

Success condition:

```text
Pushing vX.Y.Z creates a GitHub Release with the zip attached.
```

### Step 7: Optional NSIS Or WiX Installer

Only consider this after the zip flow is proven stable.

Success condition:

```text
The project gains a classic Windows installer without changing install locations, uninstall rules, or user data rules.
```

## 14. Acceptance Criteria

This plan is complete when the following are true.

### User Experience

- a user can install VoxInsert from a GitHub-downloaded zip without building from source
- the install path is per-user and does not require admin rights
- update and reinstall preserve config and credentials by default
- uninstall removes program files and shortcuts cleanly
- full cleanup is available as an explicit choice

### Maintainer Experience

- one command produces a clean release zip
- the zip is self-contained enough for end users
- smoke tests run before packaging succeeds
- GitHub Actions produces downloadable artifacts on `main`
- version tags produce public GitHub Releases

### Scope Discipline

- installer preferences stay limited to OS integration and install behavior
- app behavior settings remain in the existing Settings dialog
- no installer work blocks the current source-build workflow

## 15. Summary Recommendation

The recommended path for VoxInsert is:

```text
1. Ship a per-user release zip.
2. Include install.ps1, uninstall.ps1, and a double-click batch wrapper.
3. Keep provider and runtime settings inside the app.
4. Add package-release.ps1 for maintainers.
5. Add GitHub Actions to package every push to main.
6. Publish real GitHub Releases on version tags.
7. Only consider NSIS or WiX after the zip workflow is stable.
```

That path gives the simplest working experience for users while keeping the project aligned with its current Windows-native, per-user, local-first design.