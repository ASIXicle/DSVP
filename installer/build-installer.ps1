# DSVP — One-Shot Windows Installer Builder
#
# Builds DSVP, creates portable package, then compiles NSIS installer.
# Single command:  .\installer\build-installer.ps1
#
# Prerequisites:
#   - MSYS2 MinGW64 toolchain (gcc, mingw32-make, pkg-config)
#   - NSIS:  pacman -S mingw-w64-x86_64-nsis
#
# Output: DSVP-0.3.0-beta-setup.exe in repo root

param(
    [switch]$SkipBuild    # skip compilation, use existing DSVP-portable/
)

$ErrorActionPreference = "Stop"

# ── Ensure we're in repo root FIRST ───────────────────────────
# (The version extraction below throws under ErrorActionPreference=Stop
# when src/dsvp.h isn't at the CWD — running via double-click or from
# installer\ used to kill the script before any error could print.)

if (-not (Test-Path "src\dsvp.h")) {
    if (Test-Path "..\src\dsvp.h") {
        Set-Location ..                     # invoked from installer\
    } elseif ($PSScriptRoot -and (Test-Path (Join-Path $PSScriptRoot "..\src\dsvp.h"))) {
        Set-Location (Join-Path $PSScriptRoot "..")   # invoked by absolute path / double-click
    } else {
        Write-Host "ERROR: Run this script from the DSVP repo root." -ForegroundColor Red
        Read-Host  "Press Enter to close"   # keep the window alive for double-click runs
        exit 1
    }
}

# Version derives from src/dsvp.h — single source of truth, never hardcode here
$version = (Select-String -Path "src/dsvp.h" -Pattern 'DSVP_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value

Write-Host "`n=== DSVP Installer Builder v${version} ===" -ForegroundColor Cyan

# ── Step 1: Build and package ─────────────────────────────────

if (-not $SkipBuild) {
    Write-Host "`n[1/2] Building portable package..." -ForegroundColor Yellow
    & .\package.ps1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: package.ps1 failed." -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "`n[1/2] Skipping build (using existing DSVP-portable/)" -ForegroundColor DarkGray
    if (-not (Test-Path "DSVP-portable\dsvp.exe")) {
        Write-Host "ERROR: DSVP-portable\dsvp.exe not found. Run without -SkipBuild." -ForegroundColor Red
        exit 1
    }
}

# ── Step 2: Compile NSIS installer ────────────────────────────

Write-Host "`n[2/2] Compiling installer..." -ForegroundColor Yellow

# Find makensis — prefer MSYS2, fall back to PATH
$makensis = $null
$msys2_nsis = "C:\msys64\mingw64\bin\makensis.exe"
if (Test-Path $msys2_nsis) {
    $makensis = $msys2_nsis
} else {
    $makensis = Get-Command makensis -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
}

if (-not $makensis) {
    Write-Host "ERROR: makensis not found." -ForegroundColor Red
    Write-Host "       Install NSIS:  pacman -S mingw-w64-x86_64-nsis" -ForegroundColor Yellow
    exit 1
}

Write-Host "      Using: $makensis" -ForegroundColor DarkGray
# Pass the dsvp.h-derived version in; dsvp.nsi's !define is only a fallback
& $makensis "/DPRODUCT_VERSION=$version" installer\dsvp.nsi
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: NSIS compilation failed." -ForegroundColor Red
    exit 1
}

# ── Done ──────────────────────────────────────────────────────

$exe = "DSVP-${version}-setup.exe"
if (Test-Path $exe) {
    $size = [math]::Round((Get-Item $exe).Length / 1MB, 1)
    Write-Host "`n  Installer:  $exe" -ForegroundColor Green
    Write-Host "  Size:       ${size} MB" -ForegroundColor White
} else {
    Write-Host "`nWARNING: Expected $exe not found — check NSIS output above." -ForegroundColor Yellow
}

Write-Host ""
