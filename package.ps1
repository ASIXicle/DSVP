# DSVP Portable Packaging Script (Windows)
# Creates a clean DSVP-portable/ folder with exe + all DLLs.
#
# Run from PowerShell in the DSVP repo root.
#
# Usage:
#   .\package.ps1
#   .\package.ps1 -SkipBuild    # skip compilation, just package

param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
# Version derives from src/dsvp.h — single source of truth, never hardcode here
$version = (Select-String -Path "src/dsvp.h" -Pattern 'DSVP_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value
$outDir  = "DSVP-portable"

Write-Host "=== DSVP Packager v$version ===" -ForegroundColor Cyan

# ── Ensure MSYS2 MinGW64 tools are on PATH ───────────────────────
$msysRoot = "C:\msys64\mingw64"
if (Test-Path "$msysRoot\bin") {
    $env:PATH = "$msysRoot\bin;C:\msys64\usr\bin;$env:PATH"
    $env:PKG_CONFIG_PATH = "$msysRoot\lib\pkgconfig;$env:PKG_CONFIG_PATH"
} else {
    Write-Host "WARNING: MSYS2 MinGW64 not found at $msysRoot." -ForegroundColor Yellow
}

# Verify pkg-config can find SDL3. No stderr redirect: under Windows
# PowerShell 5.1 with ErrorActionPreference=Stop, stderr from a
# redirected native command throws NativeCommandError before the
# friendly message below could ever run. --exists is silent anyway.
& pkg-config --exists sdl3
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: pkg-config cannot find sdl3. Install MSYS2 deps first (see SETUP.md)." -ForegroundColor Red
    exit 1
}

# Verify shadercross is present
$scDir = "deps\SDL3_shadercross-3.0.0-windows-mingw-x64"
if (-not (Test-Path "$scDir\include\SDL3_shadercross\SDL_shadercross.h")) {
    Write-Host "ERROR: SDL3_shadercross not found at $scDir\" -ForegroundColor Red
    exit 1
}

# ── Build ──────────────────────────────────────────────────────────

if (-not $SkipBuild) {
    Write-Host "`n[1/5] Building..." -ForegroundColor Yellow
    cmd /c "mingw32-make clean >nul 2>&1"   # cmd wrapper: stderr from a
    # redirected native command is a terminating error under PS 5.1
    mingw32-make
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Build failed." -ForegroundColor Red
        exit 1
    }
    Write-Host "      Build OK" -ForegroundColor Green
} else {
    Write-Host "`n[1/5] Skipping build" -ForegroundColor DarkGray
}

# ── Verify exe exists ──────────────────────────────────────────────

if (-not (Test-Path "build\dsvp.exe")) {
    Write-Host "ERROR: build\dsvp.exe not found. Run without -SkipBuild." -ForegroundColor Red
    exit 1
}

# ── Create output directory ────────────────────────────────────────

Write-Host "[2/5] Creating $outDir\" -ForegroundColor Yellow
if (Test-Path $outDir) {
    Remove-Item -Recurse -Force $outDir
}
New-Item -ItemType Directory -Path $outDir | Out-Null

# ── Copy exe and build/ DLLs ──────────────────────────────────────

Write-Host "[3/5] Copying exe and build DLLs..." -ForegroundColor Yellow
Copy-Item "build\dsvp.exe" "$outDir\"
Copy-Item "LICENSE" "$outDir\"   # GPL-3: binary distribution requires the license text

# Makefile already copies SDL3.dll, SDL3_ttf.dll, SDL3_shadercross.dll,
# dxcompiler.dll, dxil.dll to build/
$buildDlls = Get-ChildItem "build\*.dll" -ErrorAction SilentlyContinue
foreach ($f in $buildDlls) {
    Copy-Item $f.FullName "$outDir\"
}
Write-Host "      Copied $($buildDlls.Count) DLLs from build/" -ForegroundColor Green

# ── Resolve transitive DLL dependencies ────────────────────────────

Write-Host "[4/5] Resolving DLL dependencies..." -ForegroundColor Yellow

# Build list of directories to search for DLLs (MSYS2 + vcpkg)
$searchDirs = @()
# Bundled shadercross FIRST: it ships DLLs that exist nowhere else on
# the system (libspirv-cross-c-shared.dll and the MinGW runtime the
# bundle was built against), so a portable package assembled without
# this directory is missing dependencies no other search dir can supply.
if (Test-Path "deps\SDL3_shadercross-3.0.0-windows-mingw-x64\bin") {
    $searchDirs += (Resolve-Path "deps\SDL3_shadercross-3.0.0-windows-mingw-x64\bin").Path
}
if (Test-Path "C:\msys64\mingw64\bin")              { $searchDirs += "C:\msys64\mingw64\bin" }
# vcpkg deliberately NOT searched: its DLLs are MSVC-ABI builds —
# bundling one next to MinGW binaries is a different-CRT trap.

# Fallback: try pkg-config
if ($searchDirs.Count -eq 0) {
    $prefix = & pkg-config --variable=prefix sdl3 2>$null
    if ($prefix -and (Test-Path (Join-Path $prefix "bin"))) {
        $searchDirs += Join-Path $prefix "bin"
    }
}

if ($searchDirs.Count -eq 0) {
    Write-Host "WARNING: No MinGW/vcpkg bin dirs found. Skipping dependency resolution." -ForegroundColor Yellow
} else {
    Write-Host "      Search dirs: $($searchDirs -join ', ')" -ForegroundColor DarkGray

    # Iteratively resolve: check each DLL/exe in outDir, find missing deps
    $systemDirs = @("C:\Windows\system32", "C:\Windows\SysWOW64", "C:\Windows")
    $resolved = @{}
    $changed = $true

    while ($changed) {
        $changed = $false
        $files = Get-ChildItem "$outDir\*.dll", "$outDir\*.exe" -ErrorAction SilentlyContinue

        foreach ($f in $files) {
            if ($resolved[$f.Name]) { continue }
            $resolved[$f.Name] = $true

            # Use objdump to find DLL imports
            $deps = & objdump -p $f.FullName 2>$null | Select-String "DLL Name:" |
                ForEach-Object { ($_ -replace '.*DLL Name:\s*', '').Trim() }

            foreach ($dep in $deps) {
                $destPath = Join-Path $outDir $dep
                if (Test-Path $destPath) { continue }

                # Skip system DLLs
                $isSystem = $false
                foreach ($sysDir in $systemDirs) {
                    if (Test-Path (Join-Path $sysDir $dep)) {
                        $isSystem = $true
                        break
                    }
                }
                if ($isSystem) { continue }

                # Search all known directories
                foreach ($searchDir in $searchDirs) {
                    $srcPath = Join-Path $searchDir $dep
                    if (Test-Path $srcPath) {
                        Copy-Item $srcPath "$outDir\"
                        Write-Host "      + $dep" -ForegroundColor DarkGray
                        $changed = $true
                        break
                    }
                }
            }
        }
    }
}

$totalDlls = (Get-ChildItem "$outDir\*.dll" -ErrorAction SilentlyContinue).Count
Write-Host "      Total DLLs: $totalDlls" -ForegroundColor Green

# ── Summary ────────────────────────────────────────────────────────

Write-Host "`n[5/5] Package complete!" -ForegroundColor Green

$files = Get-ChildItem $outDir
$totalSize = ($files | Measure-Object -Property Length -Sum).Sum / 1MB

Write-Host "`n  Location:  $outDir\" -ForegroundColor White
Write-Host "  Files:     $($files.Count)" -ForegroundColor White
Write-Host "  Size:      $([math]::Round($totalSize, 1)) MB" -ForegroundColor White
Write-Host "`n  Run with:  .\$outDir\dsvp.exe" -ForegroundColor Cyan
Write-Host ""
