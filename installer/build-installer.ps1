# DSVP — One-Shot Windows Installer Builder
#
# Builds DSVP, creates portable package, zips it (residue-checked), then compiles NSIS installer.
# Single command:  .\installer\build-installer.ps1
#
# Prerequisites:
#   - MSYS2 MinGW64 toolchain (gcc, mingw32-make, pkg-config)
#   - NSIS:  pacman -S mingw-w64-x86_64-nsis
#
# Output: DSVP-<version>-setup.exe and DSVP-<version>-windows-portable-x64.zip
#         in repo root (version from src/dsvp.h)

param(
    [switch]$SkipBuild,   # skip compilation, use existing DSVP-portable/
    [switch]$AllowDirty   # passed to package.ps1: package a non-release stamp (test only)
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
    Write-Host "`n[1/3] Building portable package..." -ForegroundColor Yellow
    if ($AllowDirty) { & .\package.ps1 -AllowDirty } else { & .\package.ps1 }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: package.ps1 failed." -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "`n[1/3] Skipping build (using existing DSVP-portable/)" -ForegroundColor DarkGray
    if (-not (Test-Path "DSVP-portable\dsvp.exe")) {
        Write-Host "ERROR: DSVP-portable\dsvp.exe not found. Run without -SkipBuild." -ForegroundColor Red
        exit 1
    }
    if (-not (Test-Path "DSVP-portable\dsvp.stamp")) {
        Write-Host "ERROR: DSVP-portable\dsvp.stamp not found - the bundle predates the provenance gate. Run without -SkipBuild." -ForegroundColor Red
        exit 1
    }
    Write-Host "      Bundle stamp: $((Get-Content 'DSVP-portable\dsvp.stamp' -First 1).Trim())" -ForegroundColor White
}

# ── Step 2: Portable zip (ZIP GUARD, 2026-10-06) ──────────────
# Nothing in the repo produced the portable zip until now: the bundle
# was zipped by hand, with no exclusions and no residue check, while
# the installer had both (dsvp.nsi: /x dsvp.log /x dsvp.resume /x
# dsvp.resume.tmp). A test launch from DSVP-portable\ writes
# dsvp.resume.tmp beside the exe on every start (767a951) and dsvp.log
# on every run, so a hand-zipped bundle that was ever tried carried
# them. Here the bundle is refused if it carries residue, zipped with
# .NET (the Archive module does not auto-load from an MSYS-launched
# PowerShell, same trap as Get-FileHash), and read back. The name is
# the one the release page carries: DSVP-<version>-windows-portable-x64.zip,
# with DSVP-portable\ as the top-level folder like the Linux tarball.

Write-Host "`n[2/3] Portable zip..." -ForegroundColor Yellow
$zip = "DSVP-${version}-windows-portable-x64.zip"
$residue = Get-ChildItem "DSVP-portable" -Recurse -File | Where-Object { $_.Name -match '(?i)dsvp\.log|dsvp\.resume' }
if ($residue) {
    foreach ($r in $residue) { Write-Host "      residue: $($r.FullName)" -ForegroundColor Red }
    Write-Host "ERROR: the bundle carries a log or resume record (a launch from DSVP-portable\ wrote it). Re-run package.ps1 for a fresh bundle." -ForegroundColor Red
    exit 1
}
if (Test-Path $zip) { Remove-Item -Force $zip }
Add-Type -AssemblyName System.IO.Compression.FileSystem
# Entries are written one by one with forward slashes: .NET Framework's
# CreateFromDirectory names them with backslashes (field 2026-10-06:
# the read-back could not find DSVP-portable/dsvp.exe), and a zip with
# backslash separators extracts to one flat folder of odd names on
# Linux/macOS.
$root = (Resolve-Path "DSVP-portable").Path
$za = [System.IO.Compression.ZipFile]::Open((Join-Path (Get-Location).Path $zip), [System.IO.Compression.ZipArchiveMode]::Create)
foreach ($f in (Get-ChildItem $root -Recurse -File)) {
    $rel = $f.FullName.Substring($root.Length).TrimStart('\', '/') -replace '\\', '/'
    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, "DSVP-portable/$rel", [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
}
$za.Dispose()
$za = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $zip).Path)
$entries = @($za.Entries | ForEach-Object { $_.FullName -replace '\\', '/' })
$za.Dispose()
$bad = @($entries | Where-Object { $_ -match '(?i)dsvp\.log|dsvp\.resume' })
if ($bad.Count -gt 0) {
    foreach ($b in $bad) { Write-Host "      in zip: $b" -ForegroundColor Red }
    Write-Host "ERROR: residue inside $zip after the pre-check passed - do not ship it." -ForegroundColor Red
    exit 1
}
if (-not ($entries -contains "DSVP-portable/dsvp.exe")) {
    Write-Host "ERROR: $zip does not contain DSVP-portable/dsvp.exe." -ForegroundColor Red
    exit 1
}
Write-Host "      $zip : $($entries.Count) entries, residue check clean, $([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB" -ForegroundColor Green

# ── Step 3: Compile NSIS installer ────────────────────────────

Write-Host "`n[3/3] Compiling installer..." -ForegroundColor Yellow

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
    Write-Host "  Portable:   $zip" -ForegroundColor Green
    Write-Host "  Size:       ${size} MB" -ForegroundColor White
} else {
    Write-Host "`nWARNING: Expected $exe not found — check NSIS output above." -ForegroundColor Yellow
}

Write-Host ""
