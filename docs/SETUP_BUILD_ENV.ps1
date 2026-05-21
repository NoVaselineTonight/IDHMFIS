#Requires -RunAsAdministrator
# IDHMFIS Build Environment Setup
# Run once as Administrator in PowerShell:
#   Set-ExecutionPolicy -Scope Process Bypass
#   .\SETUP_BUILD_ENV.ps1

$ErrorActionPreference = "Stop"

Write-Host "=== IDHMFIS Build Environment Setup ===" -ForegroundColor Cyan

# ── 1. Visual Studio Build Tools 2022 ────────────────────────────────────────
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstances = & $vswhere -all -format json 2>$null | ConvertFrom-Json
if ($vsInstances -and $vsInstances.Count -gt 0) {
    Write-Host "[SKIP] Visual Studio / Build Tools already installed." -ForegroundColor Green
} else {
    Write-Host "[INSTALL] Visual Studio Build Tools 2022 (C++ Desktop workload)..." -ForegroundColor Yellow
    winget install --id Microsoft.VisualStudio.2022.BuildTools `
        --override "--quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.Windows11SDK.22621" `
        --accept-package-agreements --accept-source-agreements
    Write-Host "[OK] Build Tools installed." -ForegroundColor Green
}

# ── 2. CMake ─────────────────────────────────────────────────────────────────
if (Get-Command cmake -ErrorAction SilentlyContinue) {
    Write-Host "[SKIP] CMake already in PATH: $(cmake --version | Select-Object -First 1)" -ForegroundColor Green
} else {
    Write-Host "[INSTALL] CMake..." -ForegroundColor Yellow
    winget install --id Kitware.CMake --accept-package-agreements --accept-source-agreements
    Write-Host "[OK] CMake installed." -ForegroundColor Green
}

# ── 3. Ninja ─────────────────────────────────────────────────────────────────
if (Get-Command ninja -ErrorAction SilentlyContinue) {
    Write-Host "[SKIP] Ninja already in PATH." -ForegroundColor Green
} else {
    Write-Host "[INSTALL] Ninja..." -ForegroundColor Yellow
    winget install --id Ninja-build.Ninja --accept-package-agreements --accept-source-agreements
    Write-Host "[OK] Ninja installed." -ForegroundColor Green
}

# ── 4. Git (needed for CMake FetchContent) ────────────────────────────────────
if (Get-Command git -ErrorAction SilentlyContinue) {
    Write-Host "[SKIP] Git already in PATH." -ForegroundColor Green
} else {
    Write-Host "[INSTALL] Git..." -ForegroundColor Yellow
    winget install --id Git.Git --accept-package-agreements --accept-source-agreements
    Write-Host "[OK] Git installed." -ForegroundColor Green
}

Write-Host ""
Write-Host "=== Setup Complete ===" -ForegroundColor Cyan
Write-Host "IMPORTANT: Open a new 'Developer Command Prompt for VS 2022' and run:" -ForegroundColor Yellow
Write-Host "  .\build.ps1" -ForegroundColor White
Write-Host "Or run build.bat from any command prompt." -ForegroundColor White
