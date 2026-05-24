param(
    [string]$Version,
    [switch]$SkipBuild,
    [switch]$SkipSmokeTests,
    [switch]$SkipChecksum
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Get-ProjectVersion {
    param(
        [string]$RepoRoot,
        [string]$RequestedVersion
    )

    if (-not [string]::IsNullOrWhiteSpace($RequestedVersion)) {
        return $RequestedVersion.Trim().TrimStart('v')
    }

    $cmakePath = Join-Path $RepoRoot "CMakeLists.txt"
    if (-not (Test-Path -LiteralPath $cmakePath)) {
        throw "CMakeLists.txt was not found at $cmakePath"
    }

    $cmakeText = Get-Content -LiteralPath $cmakePath -Raw
    $match = [regex]::Match($cmakeText, 'VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $match.Success) {
        throw "Unable to determine the project version from CMakeLists.txt. Pass -Version explicitly."
    }

    return $match.Groups[1].Value
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
        throw "Required file was not found: $Path"
    }
}

function Copy-ReleaseArtifacts {
    param(
        [string]$BuildDirectory,
        [string]$StageDirectory
    )

    $artifactNames = @("VoxInsert.exe")
    $artifactNames += Get-ChildItem -LiteralPath $BuildDirectory -Filter "*.dll" | Sort-Object Name | ForEach-Object { $_.Name }

    foreach ($artifactName in $artifactNames) {
        $sourcePath = Join-Path $BuildDirectory $artifactName
        Require-File -Path $sourcePath
        Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $StageDirectory $artifactName) -Force
    }
}

function Write-PackageMetadata {
    param(
        [string]$Path,
        [string]$Version,
        [string]$PackageName
    )

    $metadata = [ordered]@{
        appName = "VoxInsert"
        executableName = "VoxInsert.exe"
        version = $Version
        packageName = $PackageName
        platform = "win-x64"
        generatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        defaultInstallDir = Join-Path $env:LOCALAPPDATA "Programs\VoxInsert"
    }

    $metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Path
}

function New-ZipArchive {
    param(
        [string]$SourceDirectory,
        [string]$DestinationPath
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem

    for ($attempt = 1; $attempt -le 5; ++$attempt) {
        try {
            [System.IO.Compression.ZipFile]::CreateFromDirectory($SourceDirectory, $DestinationPath)
            return
        }
        catch {
            if ($attempt -eq 5) {
                throw
            }

            Start-Sleep -Milliseconds 300
        }
    }
}

function Invoke-StagedExecutable {
    param(
        [string]$ExecutablePath,
        [string]$Arguments,
        [string]$WorkingDirectory
    )

    $process = Start-Process -FilePath $ExecutablePath -ArgumentList $Arguments -WorkingDirectory $WorkingDirectory -PassThru -Wait
    return $process.ExitCode
}

$repoRoot = Get-RepoRoot
$version = Get-ProjectVersion -RepoRoot $repoRoot -RequestedVersion $Version
$packageName = "VoxInsert-v{0}-win-x64" -f $version

$buildDirectory = Join-Path $repoRoot "out\build\windows-msvc-release"
$releaseDirectory = Join-Path $repoRoot "out\release"
$stageDirectory = Join-Path $releaseDirectory $packageName
$zipPath = Join-Path $releaseDirectory ($packageName + ".zip")
$checksumPath = $zipPath + ".sha256"
$packageTemplateDirectory = Join-Path $repoRoot "distribution\package"
$buildScriptPath = Join-Path $PSScriptRoot "build-release.ps1"

Ensure-Directory -Path $releaseDirectory

if (-not $SkipBuild) {
    & powershell -ExecutionPolicy Bypass -File $buildScriptPath
}

Require-File -Path (Join-Path $buildDirectory "VoxInsert.exe")
Require-File -Path (Join-Path $repoRoot "config.example.json")
Require-File -Path (Join-Path $repoRoot "README.md")
Require-File -Path (Join-Path $repoRoot "LICENSE")
Require-File -Path (Join-Path $packageTemplateDirectory "install.ps1")
Require-File -Path (Join-Path $packageTemplateDirectory "uninstall.ps1")
Require-File -Path (Join-Path $packageTemplateDirectory "Install VoxInsert.bat")

Remove-PathIfExists -Path $stageDirectory
Remove-PathIfExists -Path $zipPath
Remove-PathIfExists -Path $checksumPath
Ensure-Directory -Path $stageDirectory

Copy-ReleaseArtifacts -BuildDirectory $buildDirectory -StageDirectory $stageDirectory

Copy-Item -LiteralPath (Join-Path $repoRoot "config.example.json") -Destination (Join-Path $stageDirectory "config.example.json") -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "README.md") -Destination (Join-Path $stageDirectory "README.md") -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $stageDirectory "LICENSE") -Force
Copy-Item -LiteralPath (Join-Path $packageTemplateDirectory "install.ps1") -Destination (Join-Path $stageDirectory "install.ps1") -Force
Copy-Item -LiteralPath (Join-Path $packageTemplateDirectory "uninstall.ps1") -Destination (Join-Path $stageDirectory "uninstall.ps1") -Force
Copy-Item -LiteralPath (Join-Path $packageTemplateDirectory "Install VoxInsert.bat") -Destination (Join-Path $stageDirectory "Install VoxInsert.bat") -Force

Write-PackageMetadata -Path (Join-Path $stageDirectory "package-metadata.json") -Version $version -PackageName $packageName

if (-not $SkipSmokeTests) {
    Get-Process VoxInsert -ErrorAction SilentlyContinue | Stop-Process -Force
    Push-Location $stageDirectory
    try {
        $smokeExitCode = Invoke-StagedExecutable -ExecutablePath (Join-Path $stageDirectory "VoxInsert.exe") -Arguments "--smoke-test" -WorkingDirectory $stageDirectory
        if ($smokeExitCode -ne 0) {
            throw "Staged --smoke-test failed with exit code $smokeExitCode"
        }

        $archiveSmokeExitCode = Invoke-StagedExecutable -ExecutablePath (Join-Path $stageDirectory "VoxInsert.exe") -Arguments "--archive-smoke-test" -WorkingDirectory $stageDirectory
        if ($archiveSmokeExitCode -ne 0) {
            throw "Staged --archive-smoke-test failed with exit code $archiveSmokeExitCode"
        }
    }
    finally {
        Pop-Location
    }
}

Get-Process VoxInsert -ErrorAction SilentlyContinue | Stop-Process -Force
New-ZipArchive -SourceDirectory $stageDirectory -DestinationPath $zipPath

if (-not $SkipChecksum) {
    $hash = Get-FileHash -LiteralPath $zipPath -Algorithm SHA256
    "{0} *{1}" -f $hash.Hash.ToLowerInvariant(), (Split-Path -Leaf $zipPath) | Set-Content -LiteralPath $checksumPath
}

Write-Host "Created package: $zipPath"
if (-not $SkipChecksum) {
    Write-Host "Created checksum: $checksumPath"
}
