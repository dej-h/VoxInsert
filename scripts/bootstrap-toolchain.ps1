param(
    [string]$VcpkgRoot = "C:\dev\vcpkg"
)

$ErrorActionPreference = "Stop"

function Get-WingetPath {
    $path = Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\winget.exe"
    if (-not (Test-Path $path)) {
        throw "winget.exe was not found at $path"
    }

    return $path
}

function Resolve-ToolPath {
    param(
        [string]$CommandName,
        [string[]]$FallbackPaths = @()
    )

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    foreach ($path in $FallbackPaths) {
        if (Test-Path $path) {
            return $path
        }
    }

    return $null
}

function Install-WingetPackageIfMissing {
    param(
        [string]$WingetPath,
        [string]$PackageId,
        [string]$DisplayName,
        [scriptblock]$IsInstalled,
        [string]$Override = ""
    )

    if (& $IsInstalled) {
        Write-Host "$DisplayName already available."
        return
    }

    $arguments = @(
        "install",
        "--id", $PackageId,
        "--exact",
        "--source", "winget",
        "--accept-package-agreements",
        "--accept-source-agreements",
        "--disable-interactivity"
    )

    if ($Override) {
        $arguments += @("--override", $Override)
    }

    & $WingetPath @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "winget install failed for $PackageId"
    }
}

function Resolve-VsDevCmd {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat")
    )

    return $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}

$winget = Get-WingetPath

Install-WingetPackageIfMissing -WingetPath $winget -PackageId "Git.Git" -DisplayName "Git" -IsInstalled {
    [bool](Resolve-ToolPath -CommandName "git" -FallbackPaths @(
        "C:\Program Files\Git\cmd\git.exe"
    ))
}

Install-WingetPackageIfMissing -WingetPath $winget -PackageId "Kitware.CMake" -DisplayName "CMake" -IsInstalled {
    [bool](Resolve-ToolPath -CommandName "cmake" -FallbackPaths @(
        "C:\Program Files\CMake\bin\cmake.exe"
    ))
}

Install-WingetPackageIfMissing -WingetPath $winget -PackageId "Ninja-build.Ninja" -DisplayName "Ninja" -IsInstalled {
    [bool](Resolve-ToolPath -CommandName "ninja" -FallbackPaths @(
        (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links\ninja.exe"),
        (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe")
    ))
}

Install-WingetPackageIfMissing -WingetPath $winget -PackageId "Microsoft.PowerShell" -DisplayName "PowerShell 7" -IsInstalled {
    [bool](Resolve-ToolPath -CommandName "pwsh")
}

Install-WingetPackageIfMissing -WingetPath $winget -PackageId "Microsoft.VisualStudio.2022.BuildTools" -DisplayName "Visual Studio Build Tools 2022" -IsInstalled {
    [bool](Resolve-VsDevCmd)
} -Override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --norestart"

$git = Resolve-ToolPath -CommandName "git" -FallbackPaths @(
    "C:\Program Files\Git\cmd\git.exe"
)

if (-not $git) {
    throw "git.exe is still unavailable after bootstrap."
}

if (-not (Test-Path "C:\dev")) {
    New-Item -ItemType Directory -Path "C:\dev" | Out-Null
}

if (-not (Test-Path $VcpkgRoot)) {
    & $git clone --depth 1 --filter=blob:none https://github.com/microsoft/vcpkg.git $VcpkgRoot
}

$bootstrapPath = Join-Path $VcpkgRoot "bootstrap-vcpkg.bat"
if (-not (Test-Path $bootstrapPath)) {
    throw "bootstrap-vcpkg.bat was not found under $VcpkgRoot"
}

& $bootstrapPath
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg bootstrap failed."
}

[Environment]::SetEnvironmentVariable("VCPKG_ROOT", $VcpkgRoot, "User")
$env:VCPKG_ROOT = $VcpkgRoot

$vsDevCmd = Resolve-VsDevCmd
if (-not $vsDevCmd) {
    throw "Visual Studio developer command script was not found after installing Build Tools."
}

Write-Host "Toolchain bootstrap completed."
Write-Host "VCPKG_ROOT=$VcpkgRoot"
Write-Host "Developer command script: $vsDevCmd"
Write-Host "Add your own CMake project files and source modules when you are ready to start building the app."