#requires -version 5.1
<#
.SYNOPSIS
    One-shot build for Rpfg: dependencies -> MuPDF -> the application.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Run
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Clean
#>
[CmdletBinding()]
param(
    [switch]$Run,
    [switch]$Clean,
    [switch]$SkipDeps,
    [switch]$RebuildMuPdf,
    [string]$Target = ""
)

. "$PSScriptRoot\config.ps1"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Step "Cleaning $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

# 1. third-party ---------------------------------------------------------------
if (-not $SkipDeps) {
    if (-not (Test-Path (Join-Path $WebView2Sdk "include\WebView2.h")) -or
        -not (Test-Path (Join-Path $MuPdfSrc "include\mupdf\fitz.h"))) {
        & "$PSScriptRoot\fetch_deps.ps1"
    }
}

if (-not (Test-Path (Join-Path $WebView2Sdk "include\WebView2.h"))) {
    throw "WebView2 SDK missing. Run scripts\fetch_deps.ps1."
}
if (-not (Test-Path $JsonHeader)) {
    throw "nlohmann/json missing. Run scripts\fetch_deps.ps1."
}

# 2. MuPDF --------------------------------------------------------------------
if ($RebuildMuPdf -or -not (Test-Path $MuPdfLib)) {
    & "$PSScriptRoot\build_mupdf.ps1" -Force:$RebuildMuPdf
}

# 3. Application --------------------------------------------------------------
Write-Step "Configuring CMake"
$devShell = Get-VsDevShell
# Launch-VsDevShell needs to run in this scope so the environment sticks.
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "MSVC environment was not initialised - cl.exe is not on PATH."
}

$mupdfLibDir = Split-Path -Parent $MuPdfLib

$configureArgs = @(
    "-S", $RepoRoot,
    "-B", $BuildDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DMUPDF_SRC=$MuPdfSrc",
    "-DMUPDF_LIB_DIR=$mupdfLibDir",
    "-DWEBVIEW2_SDK=$WebView2Sdk"
)

& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

Write-Step "Building Rpfg"
if ($Target) {
    & cmake --build $BuildDir --target $Target
} else {
    & cmake --build $BuildDir
}
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$exe = Join-Path $BuildDir "Rpfg.exe"
if (-not $Target -and -not (Test-Path $exe)) { throw "Expected $exe to exist." }

Write-Host ""
Write-Host "Built $exe" -ForegroundColor Green

if ($Run) {
    Write-Step "Launching"
    & $exe
}
