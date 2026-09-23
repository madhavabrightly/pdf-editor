#requires -version 5.1
<#
.SYNOPSIS
    Builds the MuPDF static libraries Rpfg links against.

.DESCRIPTION
    MuPDF ships a Visual Studio solution for Windows (platform/win32/mupdf.sln).
    We build only the libmupdf target (plus whatever static libs it depends on),
    NOT the full solution - the viewer/GLUT/Java/OCR targets are irrelevant here
    and pull in dependencies we do not want.

    Output lands in third_party/mupdf/mupdf-<ver>-source/platform/win32/x64/Release/.
#>
[CmdletBinding()]
param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64",
    [switch]$Force
)

. "$PSScriptRoot\config.ps1"

if (-not (Test-Path $MuPdfSolution)) {
    throw "MuPDF source not found at $MuPdfSrc. Run scripts\fetch_deps.ps1 first."
}

Write-Step "Building MuPDF $MuPdfVersion ($Configuration|$Platform)"

if (-not $Force -and (Test-Path $MuPdfLib)) {
    Write-Ok "already built: $MuPdfLib"
    Write-Ok "pass -Force to rebuild"
    exit 0
}

$msbuild = Get-MSBuild
Write-Ok "msbuild  $msbuild"

$msbuildArgs = @(
    $MuPdfSolution,
    "/t:libmupdf",
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/m",
    "/nologo",
    "/v:minimal",
    "/p:WarningLevel=1",
    "/p:TreatWarningsAsErrors=false"
)

# The shipped solution asks for v142; retarget every project to the toolset we
# actually have installed.
$toolset = Get-DefaultPlatformToolset
if ($toolset) {
    Write-Ok "toolset  $toolset (solution pins v142)"
    $msbuildArgs += "/p:PlatformToolset=$toolset"
} else {
    Write-Ok "toolset  not overridden - relying on the solution default"
}

Write-Ok "msbuild  /t:libmupdf /p:Configuration=$Configuration /p:Platform=$Platform /m"
Write-Host ""

& $msbuild @msbuildArgs
if ($LASTEXITCODE -ne 0) {
    throw "MuPDF build failed with exit code $LASTEXITCODE"
}

Write-Step "MuPDF artifacts"
$libDir = Split-Path -Parent $MuPdfLib
$libs = Get-ChildItem $libDir -Filter *.lib -ErrorAction SilentlyContinue
if (-not $libs) {
    throw "Build reported success but no .lib files found in $libDir"
}
$libs | ForEach-Object { Write-Ok ("{0,-28} {1,10:N0} bytes" -f $_.Name, $_.Length) }

Write-Host ""
Write-Host "MuPDF built." -ForegroundColor Green
