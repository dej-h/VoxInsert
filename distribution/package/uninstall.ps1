param(
    [switch]$FullCleanup,
    [switch]$RemoveUserData,
    [switch]$RemoveCredentials,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

function Read-JsonFile {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }

    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Remove-PathIfExists {
    param([string]$Path)

    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
        return $true
    }

    return $false
}

function Stop-RunningApp {
    param(
        [string[]]$CandidateDirectories = @()
    )

    $processes = @(Get-Process VoxInsert -ErrorAction SilentlyContinue)
    foreach ($candidateDirectory in $CandidateDirectories) {
        if ([string]::IsNullOrWhiteSpace($candidateDirectory) -or -not (Test-Path -LiteralPath $candidateDirectory)) {
            continue
        }

        $normalizedCandidate = (Resolve-Path -LiteralPath $candidateDirectory).Path
        $processes += Get-Process -ErrorAction SilentlyContinue | Where-Object {
            try {
                $processPath = $_.Path
                return -not [string]::IsNullOrWhiteSpace($processPath) -and
                    $processPath.StartsWith($normalizedCandidate, [System.StringComparison]::OrdinalIgnoreCase)
            }
            catch {
                return $false
            }
        }
    }

    if ($processes) {
        $processes | Sort-Object Id -Unique | Stop-Process -Force
    }
}

function Remove-StartupRegistration {
    $runKeyPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
    Remove-ItemProperty -Path $runKeyPath -Name "VoxInsert" -ErrorAction SilentlyContinue
}

function Ask-YesNo {
    param(
        [string]$Caption,
        [string]$Message,
        [bool]$DefaultNo = $true
    )

    if ($Force) {
        return -not $DefaultNo
    }

    $choices = @(
        [System.Management.Automation.Host.ChoiceDescription]::new("&Yes", "Yes"),
        [System.Management.Automation.Host.ChoiceDescription]::new("&No", "No")
    )

    $defaultIndex = if ($DefaultNo) { 1 } else { 0 }
    return $Host.UI.PromptForChoice($Caption, $Message, $choices, $defaultIndex) -eq 0
}

function Resolve-InstallLocation {
    param(
        [pscustomobject]$InstallMetadata,
        [string]$CurrentScriptDirectory,
        [string]$DefaultInstallDirectory
    )

    if ($InstallMetadata -and $InstallMetadata.installDir) {
        return [string]$InstallMetadata.installDir
    }

    if ((Test-Path -LiteralPath (Join-Path $CurrentScriptDirectory "VoxInsert.exe")) -and
        ($CurrentScriptDirectory -eq $DefaultInstallDirectory)) {
        return $CurrentScriptDirectory
    }

    if (Test-Path -LiteralPath $DefaultInstallDirectory) {
        return $DefaultInstallDirectory
    }

    $packageMetadataPath = Join-Path $CurrentScriptDirectory "package-metadata.json"
    if (Test-Path -LiteralPath $packageMetadataPath) {
        throw @"
No installed VoxInsert copy was found.

You are running uninstall.ps1 from the extracted release package folder:
$CurrentScriptDirectory

That folder is only the downloaded package. Deleting it does not uninstall VoxInsert.

If VoxInsert is installed, use one of these instead:
- & "$DefaultInstallDirectory\uninstall.ps1"
- The "Uninstall VoxInsert" Start Menu shortcut

If you only want to remove the extracted package files, just delete this folder.
"@
    }

    throw "No installed VoxInsert copy was found. Install VoxInsert first, then run uninstall.ps1 from the installed folder or the Start Menu uninstall shortcut."
}

function Add-CredentialInterop {
    if (-not ("VoxInsert.CredentialNative" -as [type])) {
        Add-Type -Namespace VoxInsert -Name CredentialNative -MemberDefinition @"
using System;
using System.Runtime.InteropServices;

public static class CredentialNative {
    [DllImport("Advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool CredDeleteW(string target, int type, int flags);
}
"@
    }
}

function Get-CredentialTargets {
    param([pscustomobject]$Config)

    $targets = New-Object System.Collections.Generic.HashSet[string] ([System.StringComparer]::OrdinalIgnoreCase)
    [void]$targets.Add("VoiceAgentTyper/OpenAI")
    [void]$targets.Add("VoiceAgentTyper/Mistral")

    if ($Config -and $Config.transcription) {
        if ($Config.transcription.openai -and $Config.transcription.openai.credential_target) {
            [void]$targets.Add([string]$Config.transcription.openai.credential_target)
        }
        if ($Config.transcription.mistral -and $Config.transcription.mistral.credential_target) {
            [void]$targets.Add([string]$Config.transcription.mistral.credential_target)
        }
    }

    return @($targets)
}

function Remove-Credentials {
    param([string[]]$Targets)

    Add-CredentialInterop

    $removedTargets = @()
    foreach ($target in $Targets) {
        if ([string]::IsNullOrWhiteSpace($target)) {
            continue
        }

        $deleted = [VoxInsert.CredentialNative]::CredDeleteW($target, 1, 0)
        if ($deleted) {
            $removedTargets += $target
            continue
        }

        $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        if ($errorCode -ne 1168) {
            throw "CredDeleteW failed for '$target' with Win32 error $errorCode."
        }
    }

    return $removedTargets
}

function Resolve-ArchivePath {
    param([pscustomobject]$Config)

    $defaultArchivePath = Join-Path $env:LOCALAPPDATA "VoxInsert\Archive"
    if (-not $Config -or -not $Config.archive) {
        return $defaultArchivePath
    }

    if ([string]::IsNullOrWhiteSpace([string]$Config.archive.folder)) {
        return $defaultArchivePath
    }

    return [string]$Config.archive.folder
}

function Should-RemoveArchivePath {
    param(
        [string]$ArchivePath,
        [string]$DefaultArchivePath
    )

    if ([string]::IsNullOrWhiteSpace($ArchivePath)) {
        return $false
    }

    if (-not (Test-Path -LiteralPath $ArchivePath)) {
        return $false
    }

    if ($ArchivePath -eq $DefaultArchivePath) {
        return $true
    }

    if ($Force) {
        return $true
    }

    return Ask-YesNo -Caption "Remove archive folder" -Message "VoxInsert is configured to archive files in:`n$ArchivePath`n`nRemove this folder and all of its contents?" -DefaultNo $true
}

$defaultInstallDir = Join-Path $env:LOCALAPPDATA "Programs\VoxInsert"
$installMetadataDirectory = Join-Path $env:LOCALAPPDATA "VoxInsert"
$installMetadataPath = Join-Path $installMetadataDirectory "install.json"
$currentScriptDirectory = (Resolve-Path $PSScriptRoot).Path
$installMetadata = Read-JsonFile -Path $installMetadataPath
$installDir = Resolve-InstallLocation -InstallMetadata $installMetadata -CurrentScriptDirectory $currentScriptDirectory -DefaultInstallDirectory $defaultInstallDir
$configPath = Join-Path $env:APPDATA "VoxInsert\config.json"
$config = Read-JsonFile -Path $configPath
$defaultArchivePath = Join-Path $env:LOCALAPPDATA "VoxInsert\Archive"
$archivePath = Resolve-ArchivePath -Config $config
$removedItems = New-Object System.Collections.Generic.List[string]
$preservedItems = New-Object System.Collections.Generic.List[string]

if ($FullCleanup) {
    $RemoveUserData = $true
    $RemoveCredentials = $true
}

if (-not $Force -and -not $FullCleanup -and -not $RemoveUserData) {
    $RemoveUserData = Ask-YesNo -Caption "Remove user data" -Message "Also remove VoxInsert config, logs, and archive data?" -DefaultNo $true
}

if (-not $Force -and -not $FullCleanup -and -not $RemoveCredentials) {
    $RemoveCredentials = Ask-YesNo -Caption "Remove stored credentials" -Message "Also remove VoxInsert API keys from Windows Credential Manager?" -DefaultNo $true
}

Stop-RunningApp -CandidateDirectories @($installDir, $currentScriptDirectory)
Remove-StartupRegistration

$startMenuDirectory = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\VoxInsert"
$startMenuShortcutPath = Join-Path $startMenuDirectory "VoxInsert.lnk"
$uninstallShortcutPath = Join-Path $startMenuDirectory "Uninstall VoxInsert.lnk"
$desktopShortcutPath = Join-Path ([Environment]::GetFolderPath("Desktop")) "VoxInsert.lnk"

if (Remove-PathIfExists -Path $startMenuShortcutPath) {
    [void]$removedItems.Add($startMenuShortcutPath)
}
if (Remove-PathIfExists -Path $uninstallShortcutPath) {
    [void]$removedItems.Add($uninstallShortcutPath)
}
if (Test-Path -LiteralPath $startMenuDirectory) {
    $remainingStartMenuItems = Get-ChildItem -LiteralPath $startMenuDirectory -Force -ErrorAction SilentlyContinue
    if (-not $remainingStartMenuItems) {
        Remove-Item -LiteralPath $startMenuDirectory -Force
        [void]$removedItems.Add($startMenuDirectory)
    }
}
if (Remove-PathIfExists -Path $desktopShortcutPath) {
    [void]$removedItems.Add($desktopShortcutPath)
}

if ($RemoveCredentials) {
    $credentialTargets = Get-CredentialTargets -Config $config
    $removedTargets = Remove-Credentials -Targets $credentialTargets
    foreach ($removedTarget in $removedTargets) {
        [void]$removedItems.Add("Credential Manager: $removedTarget")
    }
}
else {
    [void]$preservedItems.Add("Credential Manager entries")
}

if ($RemoveUserData) {
    if (Remove-PathIfExists -Path (Join-Path $env:APPDATA "VoxInsert")) {
        [void]$removedItems.Add((Join-Path $env:APPDATA "VoxInsert"))
    }

    $logsDirectory = Join-Path $env:LOCALAPPDATA "VoxInsert\logs"
    if (Remove-PathIfExists -Path $logsDirectory) {
        [void]$removedItems.Add($logsDirectory)
    }

    if (Should-RemoveArchivePath -ArchivePath $archivePath -DefaultArchivePath $defaultArchivePath) {
        if (Remove-PathIfExists -Path $archivePath) {
            [void]$removedItems.Add($archivePath)
        }
    }
    elseif (Test-Path -LiteralPath $archivePath) {
        [void]$preservedItems.Add($archivePath)
    }
}
else {
    if (Test-Path -LiteralPath (Join-Path $env:APPDATA "VoxInsert")) {
        [void]$preservedItems.Add((Join-Path $env:APPDATA "VoxInsert"))
    }
    $logsDirectory = Join-Path $env:LOCALAPPDATA "VoxInsert\logs"
    if (Test-Path -LiteralPath $logsDirectory) {
        [void]$preservedItems.Add($logsDirectory)
    }
    if (Test-Path -LiteralPath $archivePath) {
        [void]$preservedItems.Add($archivePath)
    }
}

if (Test-Path -LiteralPath $installMetadataPath) {
    Remove-Item -LiteralPath $installMetadataPath -Force
}

$installMetadataChildren = Get-ChildItem -LiteralPath $installMetadataDirectory -Force -ErrorAction SilentlyContinue
if (-not $installMetadataChildren) {
    Remove-Item -LiteralPath $installMetadataDirectory -Force -ErrorAction SilentlyContinue
}

$quotedInstallDir = '"' + $installDir + '"'
$cleanupCommand = "timeout /t 2 /nobreak >nul & if exist $quotedInstallDir rmdir /s /q $quotedInstallDir"
Start-Process -FilePath "cmd.exe" -ArgumentList "/c", $cleanupCommand -WindowStyle Hidden
[void]$removedItems.Add($installDir)

Write-Host "VoxInsert uninstall started."
Write-Host ""
Write-Host "Removed or scheduled for removal:"
foreach ($removedItem in $removedItems) {
    Write-Host "- $removedItem"
}

if ($preservedItems.Count -gt 0) {
    Write-Host ""
    Write-Host "Preserved:"
    foreach ($preservedItem in $preservedItems) {
        Write-Host "- $preservedItem"
    }
}
