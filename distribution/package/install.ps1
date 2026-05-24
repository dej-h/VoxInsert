param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA "Programs\VoxInsert"),
    [switch]$NonInteractive,
    [switch]$CreateDesktopShortcut,
    [switch]$NoStartMenuShortcut,
    [switch]$NoLaunch,
    [switch]$LaunchSettings
)

$ErrorActionPreference = "Stop"

function Read-JsonFile {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }

    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Ensure-Directory {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Remove-PathIfExists {
    param([string]$Path)

    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

function Require-File {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required package file was not found: $Path"
    }
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

function New-Shortcut {
    param(
        [string]$Path,
        [string]$TargetPath,
        [string]$Arguments,
        [string]$WorkingDirectory,
        [string]$Description
    )

    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($Path)
    $shortcut.TargetPath = $TargetPath
    if (-not [string]::IsNullOrWhiteSpace($Arguments)) {
        $shortcut.Arguments = $Arguments
    }
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $shortcut.WorkingDirectory = $WorkingDirectory
    }
    if (-not [string]::IsNullOrWhiteSpace($Description)) {
        $shortcut.Description = $Description
    }
    $shortcut.Save()
}

function Prompt-TextWithDefault {
    param(
        [string]$Message,
        [string]$DefaultValue
    )

    $prompt = $Message
    if (-not [string]::IsNullOrWhiteSpace($DefaultValue)) {
        $prompt = "{0} [{1}]" -f $Message, $DefaultValue
    }

    $response = Read-Host $prompt
    if ([string]::IsNullOrWhiteSpace($response)) {
        return $DefaultValue
    }

    return $response.Trim()
}

function Prompt-YesNo {
    param(
        [string]$Message,
        [bool]$DefaultValue
    )

    $choices = @(
        [System.Management.Automation.Host.ChoiceDescription]::new("&Yes", "Yes"),
        [System.Management.Automation.Host.ChoiceDescription]::new("&No", "No")
    )

    $defaultIndex = if ($DefaultValue) { 0 } else { 1 }
    return $Host.UI.PromptForChoice("VoxInsert Installer", $Message, $choices, $defaultIndex) -eq 0
}

$packageRoot = (Resolve-Path $PSScriptRoot).Path
$packageMetadataPath = Join-Path $packageRoot "package-metadata.json"
$packageMetadata = Read-JsonFile -Path $packageMetadataPath

$packageVersion = if ($packageMetadata -and $packageMetadata.version) { $packageMetadata.version } else { "unknown" }
$appName = if ($packageMetadata -and $packageMetadata.appName) { $packageMetadata.appName } else { "VoxInsert" }
$appExecutableName = if ($packageMetadata -and $packageMetadata.executableName) { $packageMetadata.executableName } else { "VoxInsert.exe" }

$packageFiles = @(
    (Join-Path $packageRoot $appExecutableName),
    (Join-Path $packageRoot "config.example.json"),
    (Join-Path $packageRoot "uninstall.ps1")
)

foreach ($packageFile in $packageFiles) {
    Require-File -Path $packageFile
}

$installMetadataDirectory = Join-Path $env:LOCALAPPDATA "VoxInsert"
$installMetadataPath = Join-Path $installMetadataDirectory "install.json"
$existingInstallMetadata = Read-JsonFile -Path $installMetadataPath
$previousInstallDir = if ($existingInstallMetadata -and $existingInstallMetadata.installDir) {
    [string]$existingInstallMetadata.installDir
}
else {
    $null
}

$existingConfigPath = Join-Path $env:APPDATA "VoxInsert\config.json"

$effectiveInstallDir = if ($PSBoundParameters.ContainsKey("InstallDir")) {
    $InstallDir
}
elseif (-not [string]::IsNullOrWhiteSpace($previousInstallDir)) {
    $previousInstallDir
}
else {
    $InstallDir
}

$createStartMenuShortcut = if ($PSBoundParameters.ContainsKey("NoStartMenuShortcut")) {
    -not $NoStartMenuShortcut
}
else {
    $true
}

$createDesktopShortcut = if ($PSBoundParameters.ContainsKey("CreateDesktopShortcut")) {
    [bool]$CreateDesktopShortcut
}
elseif ($existingInstallMetadata -and $null -ne $existingInstallMetadata.desktopShortcutCreated) {
    [bool]$existingInstallMetadata.desktopShortcutCreated
}
else {
    $false
}

$launchAfterInstall = if ($PSBoundParameters.ContainsKey("NoLaunch")) {
    -not $NoLaunch
}
else {
    $true
}

$openSettingsAfterLaunch = if ($PSBoundParameters.ContainsKey("LaunchSettings")) {
    [bool]$LaunchSettings
}
elseif (-not (Test-Path -LiteralPath $existingConfigPath)) {
    $true
}
else {
    $false
}

if (-not $NonInteractive) {
    Write-Host "VoxInsert installer"
    Write-Host "Press Enter to accept the default shown in brackets."
    Write-Host ""

    $effectiveInstallDir = Prompt-TextWithDefault -Message "Install location" -DefaultValue $effectiveInstallDir
    $createStartMenuShortcut = Prompt-YesNo -Message "Create a Start Menu shortcut?" -DefaultValue $createStartMenuShortcut
    $createDesktopShortcut = Prompt-YesNo -Message "Create a Desktop shortcut?" -DefaultValue $createDesktopShortcut
    $launchAfterInstall = Prompt-YesNo -Message "Launch VoxInsert after install?" -DefaultValue $launchAfterInstall

    if ($launchAfterInstall) {
        $openSettingsAfterLaunch = Prompt-YesNo -Message "Open Settings after launch?" -DefaultValue $openSettingsAfterLaunch
    }
    else {
        $openSettingsAfterLaunch = $false
    }

    Write-Host ""
}

$startMenuDirectory = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\VoxInsert"
$startMenuShortcutPath = Join-Path $startMenuDirectory "VoxInsert.lnk"
$uninstallShortcutPath = Join-Path $startMenuDirectory "Uninstall VoxInsert.lnk"
$desktopShortcutPath = Join-Path ([Environment]::GetFolderPath("Desktop")) "VoxInsert.lnk"

Stop-RunningApp -CandidateDirectories @($effectiveInstallDir, $previousInstallDir)

if (-not [string]::IsNullOrWhiteSpace($previousInstallDir) -and
    ($previousInstallDir -ne $effectiveInstallDir) -and
    (Test-Path -LiteralPath $previousInstallDir)) {
    Remove-PathIfExists -Path $previousInstallDir
}

Remove-PathIfExists -Path $effectiveInstallDir
Ensure-Directory -Path $effectiveInstallDir

Get-ChildItem -LiteralPath $packageRoot -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $effectiveInstallDir $_.Name) -Recurse -Force
}

$installedExecutablePath = Join-Path $effectiveInstallDir $appExecutableName
Require-File -Path $installedExecutablePath

if (-not $createStartMenuShortcut) {
    Remove-PathIfExists -Path $startMenuShortcutPath
    Remove-PathIfExists -Path $uninstallShortcutPath
    if (Test-Path -LiteralPath $startMenuDirectory) {
        $remainingStartMenuItems = Get-ChildItem -LiteralPath $startMenuDirectory -Force -ErrorAction SilentlyContinue
        if (-not $remainingStartMenuItems) {
            Remove-Item -LiteralPath $startMenuDirectory -Force
        }
    }
}
else {
    Ensure-Directory -Path $startMenuDirectory
    New-Shortcut -Path $startMenuShortcutPath -TargetPath $installedExecutablePath -Arguments "" -WorkingDirectory $effectiveInstallDir -Description "Launch VoxInsert"

    $hostPath = (Get-Process -Id $PID).Path
    $uninstallArguments = "-ExecutionPolicy Bypass -File `"{0}`"" -f (Join-Path $effectiveInstallDir "uninstall.ps1")
    New-Shortcut -Path $uninstallShortcutPath -TargetPath $hostPath -Arguments $uninstallArguments -WorkingDirectory $effectiveInstallDir -Description "Uninstall VoxInsert"
}

if ($createDesktopShortcut) {
    New-Shortcut -Path $desktopShortcutPath -TargetPath $installedExecutablePath -Arguments "" -WorkingDirectory $effectiveInstallDir -Description "Launch VoxInsert"
}
else {
    Remove-PathIfExists -Path $desktopShortcutPath
}

Ensure-Directory -Path $installMetadataDirectory
$installMetadata = [ordered]@{
    appName = $appName
    version = $packageVersion
    installDir = $effectiveInstallDir
    executablePath = $installedExecutablePath
    packageRoot = $packageRoot
    installedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
    startMenuShortcutCreated = $createStartMenuShortcut
    desktopShortcutCreated = $createDesktopShortcut
}
$installMetadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $installMetadataPath

if ($launchAfterInstall) {
    $launchArguments = @()
    if ($openSettingsAfterLaunch) {
        $launchArguments += "--settings"
    }

    if ($launchArguments.Count -gt 0) {
        Start-Process -FilePath $installedExecutablePath -WorkingDirectory $effectiveInstallDir -ArgumentList $launchArguments
    }
    else {
        Start-Process -FilePath $installedExecutablePath -WorkingDirectory $effectiveInstallDir
    }
}

Write-Host "Installed $appName $packageVersion to $effectiveInstallDir"
if ($createStartMenuShortcut) {
    Write-Host "Created Start Menu shortcuts in $startMenuDirectory"
}
if ($createDesktopShortcut) {
    Write-Host "Created Desktop shortcut at $desktopShortcutPath"
}
Write-Host "To uninstall later, run $effectiveInstallDir\uninstall.ps1 or use the Start Menu uninstall shortcut."
