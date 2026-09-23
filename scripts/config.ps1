# Shared paths and pinned dependency versions for all build scripts.
# Dot-source this file:  . "$PSScriptRoot\config.ps1"

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# ---------------------------------------------------------------- versions ---
$MuPdfVersion = "1.28.4"
$WebView2Version = "1.0.4191.47"
$JsonVersion = "3.11.3"

# ------------------------------------------------------------------- paths ---
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ThirdParty = Join-Path $RepoRoot "third_party"
$Downloads = Join-Path $ThirdParty "downloads"
$BuildDir = Join-Path $RepoRoot "build"
$DistDir = Join-Path $RepoRoot "dist"

$MuPdfRoot = Join-Path $ThirdParty "mupdf"
$MuPdfSrc = Join-Path $MuPdfRoot "mupdf-$MuPdfVersion-source"
$MuPdfSolution = Join-Path $MuPdfSrc "platform\win32\mupdf.sln"
$MuPdfLib = Join-Path $MuPdfSrc "platform\win32\x64\Release\libmupdf.lib"

$WebView2Root = Join-Path $ThirdParty "webview2"
$WebView2Sdk = Join-Path $WebView2Root "build\native"

$JsonHeader = Join-Path $ThirdParty "nlohmann\json.hpp"

# --------------------------------------------------------------- utilities ---
function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Ok([string]$Message) {
    Write-Host "    $Message" -ForegroundColor DarkGray
}

function Get-VsWhere {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    throw "vswhere.exe not found. Install Visual Studio with the 'Desktop development with C++' workload."
}

function Get-MSBuild {
    $vswhere = Get-VsWhere
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
        -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
    if (-not $msbuild) {
        throw "MSBuild.exe not found. Install the 'Desktop development with C++' workload."
    }
    return $msbuild
}

function Get-DefaultPlatformToolset {
    # MuPDF pins its solution to an older toolset (v142 / VS2019). Retargeting it
    # to whatever this Visual Studio actually ships is far cheaper than
    # installing the legacy build tools.
    #
    # Careful: the toolset name is not the same as the MSBuild targets folder.
    # VS 2026 keeps its targets under ...\VC\v180\ but the toolset it provides is
    # named v145, so scan the real PlatformToolsets directories.
    $vswhere = Get-VsWhere
    $vsRoot = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { return $null }

    $roots = Get-ChildItem (Join-Path $vsRoot "MSBuild\Microsoft\VC") -Directory -ErrorAction SilentlyContinue
    $toolsets = foreach ($root in $roots) {
        $platformToolsets = Join-Path $root.FullName "Platforms\x64\PlatformToolsets"
        if (Test-Path $platformToolsets) {
            Get-ChildItem $platformToolsets -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '^v(\d+)$' }
        }
    }
    $best = $toolsets | Sort-Object { [int]$_.Name.Substring(1) } -Descending | Select-Object -First 1
    if (-not $best) { return $null }
    return $best.Name
}

function Get-VsDevShell {
    $vswhere = Get-VsWhere
    $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $install) {
        throw "Visual Studio C++ tools not found."
    }
    $shell = Join-Path $install "Common7\Tools\Launch-VsDevShell.ps1"
    if (-not (Test-Path $shell)) {
        throw "Launch-VsDevShell.ps1 not found at $shell"
    }
    return $shell
}

function Invoke-Download([string]$Url, [string]$Destination) {
    if (Test-Path $Destination) {
        Write-Ok "cached   $(Split-Path -Leaf $Destination)"
        return
    }
    Write-Ok "download $Url"
    $partial = "$Destination.partial"
    Invoke-WebRequest -Uri $Url -OutFile $partial -UseBasicParsing -TimeoutSec 1800
    Move-Item -Force $partial $Destination
}
