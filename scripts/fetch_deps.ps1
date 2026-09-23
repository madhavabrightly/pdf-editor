#requires -version 5.1
<#
.SYNOPSIS
    Downloads every third-party dependency Rpfg needs into third_party/.

.DESCRIPTION
    Fetches:
      * MuPDF source tarball  -> third_party/mupdf/mupdf-<ver>-source
      * Microsoft.Web.WebView2 NuGet package (it is just a zip) -> third_party/webview2
      * nlohmann/json single header -> third_party/nlohmann/json.hpp

    Idempotent: existing downloads are reused. Pass -Force to redo everything.
#>
[CmdletBinding()]
param(
    [switch]$Force
)

. "$PSScriptRoot\config.ps1"

Write-Step "Fetching dependencies into $ThirdParty"
New-Item -ItemType Directory -Force -Path $Downloads | Out-Null

# ------------------------------------------------------------------- MuPDF ---
Write-Step "MuPDF $MuPdfVersion"
$tarball = Join-Path $Downloads "mupdf-$MuPdfVersion-source.tar.gz"
if ($Force) { Remove-Item -Force -ErrorAction SilentlyContinue $tarball }
Invoke-Download "https://mupdf.com/downloads/archive/mupdf-$MuPdfVersion-source.tar.gz" $tarball

if ($Force -or -not (Test-Path (Join-Path $MuPdfSrc "include\mupdf\fitz.h"))) {
    Write-Ok "extracting source"
    New-Item -ItemType Directory -Force -Path $MuPdfRoot | Out-Null
    tar -xzf $tarball -C $MuPdfRoot
    if ($LASTEXITCODE -ne 0) { throw "tar failed to extract MuPDF ($LASTEXITCODE)" }
}
if (-not (Test-Path $MuPdfSolution)) {
    throw "MuPDF extracted but $MuPdfSolution is missing."
}
Write-Ok "source    $MuPdfSrc"

# ---------------------------------------------------------------- WebView2 ---
Write-Step "Microsoft.Web.WebView2 $WebView2Version"
$nupkg = Join-Path $Downloads "microsoft.web.webview2.$WebView2Version.nupkg"
$nupkgUrl = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$WebView2Version/microsoft.web.webview2.$WebView2Version.nupkg"
if ($Force) { Remove-Item -Force -ErrorAction SilentlyContinue $nupkg }
Invoke-Download $nupkgUrl $nupkg

if ($Force -or -not (Test-Path (Join-Path $WebView2Sdk "include\WebView2.h"))) {
    Write-Ok "extracting package"
    if (Test-Path $WebView2Root) { Remove-Item -Recurse -Force $WebView2Root }
    New-Item -ItemType Directory -Force -Path $WebView2Root | Out-Null
    # A .nupkg is a plain zip, but Expand-Archive only accepts a .zip extension,
    # so go through the .NET zip API instead of renaming the file.
    $staged = Join-Path $Downloads "webview2-extract"
    if (Test-Path $staged) { Remove-Item -Recurse -Force $staged }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory($nupkg, $staged)
    Move-Item (Join-Path $staged "build") (Join-Path $WebView2Root "build")
    Remove-Item -Recurse -Force $staged
}
if (-not (Test-Path (Join-Path $WebView2Sdk "include\WebView2.h"))) {
    throw "WebView2 SDK extraction failed."
}
Write-Ok "sdk       $WebView2Sdk"

# ------------------------------------------------------------ nlohmann/json ---
Write-Step "nlohmann/json $JsonVersion"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $JsonHeader) | Out-Null
if ($Force) { Remove-Item -Force -ErrorAction SilentlyContinue $JsonHeader }
Invoke-Download "https://raw.githubusercontent.com/nlohmann/json/v$JsonVersion/single_include/nlohmann/json.hpp" $JsonHeader
Write-Ok "header    $JsonHeader"

Write-Step "Verifying"
$missing = @()
if (-not (Test-Path (Join-Path $MuPdfSrc "include\mupdf\fitz.h"))) { $missing += "mupdf headers" }
if (-not (Test-Path $MuPdfSolution)) { $missing += "mupdf solution" }
if (-not (Test-Path (Join-Path $WebView2Sdk "include\WebView2.h"))) { $missing += "webview2 header" }
if (-not (Test-Path $JsonHeader)) { $missing += "nlohmann/json" }
if ($missing.Count -gt 0) { throw "Missing after fetch: $($missing -join ', ')" }

Write-Host ""
Write-Host "All dependencies present." -ForegroundColor Green
