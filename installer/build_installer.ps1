#Requires -Version 5.1
<#
.SYNOPSIS
    Builds the IDHMFIS Release binary and packages it as an MSI installer.

.DESCRIPTION
    1. Locates CMake and runs a Release build into .\build-release\
    2. Runs `wix build` on installer\IDHMFIS.wxs
    3. Outputs dist\IDHMFIS-Setup-x64.msi

.PARAMETER SkipBuild
    Skip the CMake build step (use an existing build in .\build-release\).

.PARAMETER VcpkgRoot
    Path to a vcpkg installation.  Defaults to .\vcpkg if present,
    otherwise vcpkg is bootstrapped in that location.

.PARAMETER WixPath
    Explicit path to the wix.exe executable.  If omitted the script
    searches PATH and standard dotnet tool locations.

.EXAMPLE
    .\installer\build_installer.ps1

.EXAMPLE
    .\installer\build_installer.ps1 -SkipBuild -VcpkgRoot C:\vcpkg
#>

[CmdletBinding()]
param(
    [switch] $SkipBuild,
    [string] $VcpkgRoot  = "",
    [string] $WixPath    = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────
function Write-Step { param([string]$Msg) Write-Host "`n==> $Msg" -ForegroundColor Cyan }
function Fail       { param([string]$Msg) Write-Error  "FAILED: $Msg"; exit 1 }

# ─────────────────────────────────────────────────────────────────────────────
# Paths
# ─────────────────────────────────────────────────────────────────────────────
$RepoRoot    = Split-Path -Parent $PSScriptRoot
$BuildDir    = Join-Path $RepoRoot "build-release"
$DistDir     = Join-Path $RepoRoot "dist"
$InstallerDir = Join-Path $RepoRoot "installer"
$WxsFile     = Join-Path $InstallerDir "IDHMFIS.wxs"
$OutputMsi   = Join-Path $DistDir "IDHMFIS-Setup-x64.msi"

# Determine the Release binary path (VS generator puts it in bin\Release\)
$BinDir = Join-Path $BuildDir "bin"
$ExePath = Join-Path $BinDir "Release\IDHMFIS.exe"
if (-not (Test-Path $ExePath)) {
    # Ninja / single-config generator
    $ExePath = Join-Path $BinDir "IDHMFIS.exe"
}

# ─────────────────────────────────────────────────────────────────────────────
# Locate CMake
# ─────────────────────────────────────────────────────────────────────────────
$CMakeExe = (Get-Command "cmake" -ErrorAction SilentlyContinue)?.Source
if (-not $CMakeExe) {
    # Try common VS install path
    $vsCmake = "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $vsCmake) { $CMakeExe = $vsCmake }
    else { Fail "cmake.exe not found in PATH or standard VS 2022 location." }
}
Write-Step "CMake: $CMakeExe"

# ─────────────────────────────────────────────────────────────────────────────
# Locate / bootstrap vcpkg
# ─────────────────────────────────────────────────────────────────────────────
if (-not $VcpkgRoot) {
    $VcpkgRoot = Join-Path $RepoRoot "vcpkg"
}
$VcpkgToolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"

if (-not (Test-Path $VcpkgToolchain)) {
    Write-Step "Bootstrapping vcpkg at $VcpkgRoot"
    if (-not (Test-Path $VcpkgRoot)) {
        git clone "https://github.com/microsoft/vcpkg.git" $VcpkgRoot
    }
    Push-Location $VcpkgRoot
    try { & ".\bootstrap-vcpkg.bat" -disableMetrics }
    finally { Pop-Location }
}
Write-Step "vcpkg toolchain: $VcpkgToolchain"

# ─────────────────────────────────────────────────────────────────────────────
# CMake configure + build
# ─────────────────────────────────────────────────────────────────────────────
if (-not $SkipBuild) {
    Write-Step "Configuring CMake (Release)"
    & $CMakeExe -B $BuildDir -S $RepoRoot `
        -G "Visual Studio 17 2022" -A x64 `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain `
        -DVCPKG_TARGET_TRIPLET=x64-windows `
        -DENABLE_CLANG_TIDY=OFF `
        -DENABLE_NDI=OFF `
        -DIDHMFIS_BUILD_TESTS=OFF `
        -DIDHMFIS_BUILD_TOOLS=OFF
    if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed (exit $LASTEXITCODE)" }

    Write-Step "Building Release"
    & $CMakeExe --build $BuildDir --config Release --parallel
    if ($LASTEXITCODE -ne 0) { Fail "CMake build failed (exit $LASTEXITCODE)" }
} else {
    Write-Step "Skipping build (-SkipBuild specified)"
}

# Verify the binary exists
if (-not (Test-Path $ExePath)) {
    Fail "Expected binary not found at: $ExePath"
}
$SourceDir = Split-Path -Parent $ExePath
Write-Step "Source directory for installer: $SourceDir"

# ─────────────────────────────────────────────────────────────────────────────
# Copy assets next to the binary (installer harvests from SourceDir)
# ─────────────────────────────────────────────────────────────────────────────
$AssetsSource = Join-Path $RepoRoot "assets"
if (Test-Path $AssetsSource) {
    Write-Step "Copying assets to $SourceDir\assets\"
    Copy-Item -Recurse -Force $AssetsSource (Join-Path $SourceDir "assets")
} else {
    Write-Warning "No assets\ directory found — installer will omit assets."
    # Create a stub icon so WiX does not fail on the Icon element
    $StubAssets = Join-Path $SourceDir "assets"
    if (-not (Test-Path $StubAssets)) { New-Item -ItemType Directory -Force $StubAssets | Out-Null }
    # WiX needs a real .ico; copy a system icon as placeholder if missing
    $StubIco = Join-Path $StubAssets "IDHMFIS.ico"
    if (-not (Test-Path $StubIco)) {
        Copy-Item "C:\Windows\System32\imageres.dll" $StubIco -ErrorAction SilentlyContinue
        if (-not (Test-Path $StubIco)) {
            # Create a zero-byte placeholder so the WiX file element resolves
            New-Item -ItemType File -Force $StubIco | Out-Null
            Write-Warning "Created placeholder IDHMFIS.ico — replace with real icon."
        }
    }
}

# ─────────────────────────────────────────────────────────────────────────────
# Locate wix.exe
# ─────────────────────────────────────────────────────────────────────────────
if (-not $WixPath) {
    $WixPath = (Get-Command "wix" -ErrorAction SilentlyContinue)?.Source
}

if (-not $WixPath) {
    # dotnet global tool default location
    $dotnetTools = Join-Path $env:USERPROFILE ".dotnet\tools\wix.exe"
    if (Test-Path $dotnetTools) { $WixPath = $dotnetTools }
}

if (-not $WixPath -or -not (Test-Path $WixPath)) {
    Write-Warning "wix.exe not found.  Installing WiX 4 as a dotnet global tool..."
    dotnet tool install --global wix
    if ($LASTEXITCODE -ne 0) { Fail "Failed to install WiX 4 dotnet tool." }
    $WixPath = (Get-Command "wix" -ErrorAction SilentlyContinue)?.Source
    if (-not $WixPath) { Fail "wix.exe still not found after install." }
}

Write-Step "WiX: $WixPath"

# ─────────────────────────────────────────────────────────────────────────────
# Ensure WiX UI extension is available
# ─────────────────────────────────────────────────────────────────────────────
Write-Step "Ensuring WiX UI extension is installed"
& $WixPath extension add WixToolset.UI.wixext --global 2>&1 | Out-Null

# ─────────────────────────────────────────────────────────────────────────────
# Create output directory
# ─────────────────────────────────────────────────────────────────────────────
if (-not (Test-Path $DistDir)) {
    New-Item -ItemType Directory -Force $DistDir | Out-Null
}

# ─────────────────────────────────────────────────────────────────────────────
# Run wix build
# ─────────────────────────────────────────────────────────────────────────────
Write-Step "Building MSI installer"
& $WixPath build $WxsFile `
    -d "SourceDir=$SourceDir" `
    -ext WixToolset.UI.wixext `
    -arch x64 `
    -out $OutputMsi

if ($LASTEXITCODE -ne 0) { Fail "wix build failed (exit $LASTEXITCODE)" }

# ─────────────────────────────────────────────────────────────────────────────
# Done
# ─────────────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "Installer built successfully:" -ForegroundColor Green
Write-Host "  $OutputMsi" -ForegroundColor Green
Write-Host ""
