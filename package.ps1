# DSVP Portable Packaging Script (Windows)
# Creates a clean DSVP-portable/ folder ready for distribution.
#
# Usage:
#   .\package.ps1
#   .\package.ps1 -SkipBuild    # skip compilation, just package

param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$version = "0.1.2"
$outDir  = "DSVP-portable"

Write-Host "=== DSVP Packager v$version ===" -ForegroundColor Cyan

# ── Build ──────────────────────────────────────────────────────────

if (-not $SkipBuild) {
    Write-Host "`n[1/4] Building..." -ForegroundColor Yellow
    mingw32-make clean 2>$null
    mingw32-make
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Build failed." -ForegroundColor Red
        exit 1
    }
    Write-Host "      Build OK" -ForegroundColor Green
} else {
    Write-Host "`n[1/4] Skipping build" -ForegroundColor DarkGray
}

# ── Verify exe exists ──────────────────────────────────────────────

if (-not (Test-Path "build\dsvp.exe")) {
    Write-Host "ERROR: build\dsvp.exe not found. Run without -SkipBuild." -ForegroundColor Red
    exit 1
}

# ── Create output directory ────────────────────────────────────────

Write-Host "[2/4] Creating $outDir\" -ForegroundColor Yellow
if (Test-Path $outDir) {
    Remove-Item -Recurse -Force $outDir
}
New-Item -ItemType Directory -Path $outDir | Out-Null

# ── Copy exe ───────────────────────────────────────────────────────

Write-Host "[3/4] Copying files..." -ForegroundColor Yellow
Copy-Item "build\dsvp.exe" "$outDir\"

# ── Copy FFmpeg DLLs ───────────────────────────────────────────────

$ffmpegBin = "deps\ffmpeg\bin"
if (-not (Test-Path $ffmpegBin)) {
    Write-Host "ERROR: $ffmpegBin not found." -ForegroundColor Red
    exit 1
}

# Only copy the DLLs we actually link against (plus their dependencies)
$requiredDlls = @(
    "avcodec-*.dll",
    "avformat-*.dll",
    "avutil-*.dll",
    "swscale-*.dll",
    "swresample-*.dll",
    "postproc-*.dll"
)

$copied = 0
foreach ($pattern in $requiredDlls) {
    $files = Get-ChildItem -Path $ffmpegBin -Filter $pattern -ErrorAction SilentlyContinue
    foreach ($f in $files) {
        Copy-Item $f.FullName "$outDir\"
        $copied++
    }
}

# Also copy any other DLLs that FFmpeg depends on (OpenSSL, etc.)
# The gyan.dev full build bundles these in bin/
$supportDlls = Get-ChildItem -Path $ffmpegBin -Filter "*.dll" -ErrorAction SilentlyContinue
foreach ($f in $supportDlls) {
    $dest = Join-Path $outDir $f.Name
    if (-not (Test-Path $dest)) {
        Copy-Item $f.FullName "$outDir\"
        $copied++
    }
}

Write-Host "      Copied $copied DLLs from FFmpeg" -ForegroundColor Green

# ── Copy SDL2 DLL ──────────────────────────────────────────────────

$sdlDll = "deps\SDL2\bin\SDL2.dll"
if (Test-Path $sdlDll) {
    Copy-Item $sdlDll "$outDir\"
    Write-Host "      Copied SDL2.dll" -ForegroundColor Green
} else {
    Write-Host "WARNING: SDL2.dll not found at $sdlDll" -ForegroundColor Yellow
}

# ── Copy SDL2_ttf DLLs ────────────────────────────────────────────

$ttfBin = "deps\SDL2_ttf\bin"
if (Test-Path $ttfBin) {
    $ttfDlls = Get-ChildItem -Path $ttfBin -Filter "*.dll" -ErrorAction SilentlyContinue
    $ttfCount = 0
    foreach ($f in $ttfDlls) {
        Copy-Item $f.FullName "$outDir\"
        $ttfCount++
    }
    Write-Host "      Copied $ttfCount DLLs from SDL2_ttf" -ForegroundColor Green
} else {
    Write-Host "WARNING: SDL2_ttf not found at $ttfBin" -ForegroundColor Yellow
}

# ── Summary ────────────────────────────────────────────────────────

Write-Host "`n[4/4] Package complete!" -ForegroundColor Green

$files = Get-ChildItem $outDir
$totalSize = ($files | Measure-Object -Property Length -Sum).Sum / 1MB

Write-Host "`n  Location:  $outDir\" -ForegroundColor White
Write-Host "  Files:     $($files.Count)" -ForegroundColor White
Write-Host "  Size:      $([math]::Round($totalSize, 1)) MB" -ForegroundColor White
Write-Host "`n  Run with:  .\$outDir\dsvp.exe" -ForegroundColor Cyan
Write-Host ""
