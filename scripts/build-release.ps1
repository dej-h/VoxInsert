$ErrorActionPreference = "Stop"

function Resolve-ToolPath {
    param(
        [string]$CommandName,
        [string[]]$FallbackPaths
    )

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    foreach ($fallback in $FallbackPaths) {
        if (Test-Path $fallback) {
            return $fallback
        }
    }

    throw "$CommandName was not found. Run scripts/bootstrap-toolchain.ps1 first."
}

function Enter-VsDevShell {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        return
    }

    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat")
    )

    $devCmd = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $devCmd) {
        throw "VsDevCmd.bat was not found. Install Visual Studio Build Tools 2022 with the C++ workload."
    }

    $commandLine = '"' + $devCmd + '" -arch=amd64 -host_arch=amd64 >nul && set'

    cmd.exe /c $commandLine |
        ForEach-Object {
            if ($_ -match '^(.*?)=(.*)$') {
                Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2]
            }
        }

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "MSVC environment initialization failed. cl.exe is still unavailable."
    }
}

$cmake = Resolve-ToolPath -CommandName "cmake" -FallbackPaths @(
    "C:\Program Files\CMake\bin\cmake.exe"
)

$ninja = Resolve-ToolPath -CommandName "ninja" -FallbackPaths @(
    (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Links\ninja.exe"),
    (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe")
)

Enter-VsDevShell

$env:Path = "{0};{1}" -f (Split-Path $cmake -Parent), $env:Path
$env:Path = "{0};{1}" -f (Split-Path $ninja -Parent), $env:Path
$env:CMAKE_MAKE_PROGRAM = $ninja

if (Test-Path "C:\dev\vcpkg") {
    $env:VCPKG_ROOT = "C:\dev\vcpkg"
}
elseif (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT is not set and C:\dev\vcpkg does not exist. Run scripts/bootstrap-toolchain.ps1 first."
}

& $cmake --preset windows-msvc-release -D CMAKE_MAKE_PROGRAM="$ninja"
& $cmake --build --preset release