# DSVP Portable Packaging Script (Windows)
# Creates a clean DSVP-portable/ folder with exe + all DLLs.
#
# Run from PowerShell in the DSVP repo root.
#
# Usage:
#   .\package.ps1
#   .\package.ps1 -SkipBuild    # skip compilation, just package

param(
    [switch]$SkipBuild,
    [switch]$AllowDirty   # package an unknown/+dirty/debug stamp (test bundles only)
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
    # No `cmd /c` wrapper (field 2026-09-07): with C:\msys64\usr\bin at
    # the front of PATH, PowerShell resolves the bare name `cmd` to the
    # extension-less MSYS wrapper C:\msys64\usr\bin\cmd and ShellExecutes
    # it as a document — the "what program do you want to use to open
    # this file" dialog on every packager run. Stdout only is silenced;
    # stderr is left alone (a redirected native stderr is a terminating
    # error under PS 5.1), and `rm -rf` of a missing build/ is silent
    # anyway (receipted twice, rc 0).
    mingw32-make clean | Out-Null
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

# ── Provenance gate (review M9, Windows half; field 2026-09-07: a bare
# makensis after a failed package step shipped the previous day's
# bundle as a new version). The Makefile writes build\dsvp.stamp
# ("<sha>[+dirty] <mode>") at link; the bundle must carry a real,
# clean, release stamp whose build inputs match HEAD — same rules as
# package.sh. The stamp is copied into the bundle so dsvp.nsi can
# refuse a stampless or non-release bundle on its own.
$stamp = if (Test-Path "build\dsvp.stamp") { (Get-Content "build\dsvp.stamp" -First 1).Trim() } else { "missing" }
$stampSha  = ($stamp -split ' ')[0]
$stampMode = ($stamp -split ' ')[-1]
Write-Host "      Binary stamp: $stamp" -ForegroundColor White
$stampBad = ""
if ($stampSha -eq "unknown" -or $stampSha -eq "missing") {
    $stampBad = "stamp is '$stampSha' (no git tree or pre-stamp binary)"
} elseif ($stampSha -like "*+dirty") {
    $stampBad = "stamp is +dirty (uncommitted changes)"
} elseif ($stampMode -ne "release") {
    $stampBad = "binary is a $stampMode build"
} elseif (Test-Path ".git") {
    $headSha = (& git rev-parse --short HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $headSha -and $stampSha -ne $headSha) {
        & git cat-file -e "$stampSha^{commit}" 2>$null
        if ($LASTEXITCODE -ne 0) {
            $stampBad = "stamp $stampSha is not a commit in this repository"
        } else {
            & git diff --quiet $stampSha HEAD -- src Makefile dsvp.rc shadercross deps 2>$null
            if ($LASTEXITCODE -ne 0) {
                $changed = (& git diff --stat $stampSha HEAD -- src Makefile dsvp.rc shadercross deps | Select-Object -Last 1)
                $stampBad = "build inputs changed between stamp $stampSha and HEAD $headSha (stale binary): $changed"
            } else {
                Write-Host "      Stamp $stampSha predates HEAD $headSha, but no build input changed between them - binary is current" -ForegroundColor DarkGray
            }
        }
    }
}
if ($stampBad) {
    if ($AllowDirty) {
        Write-Host "      WARNING: $stampBad - packaging anyway (-AllowDirty)" -ForegroundColor Yellow
    } else {
        Write-Host "ERROR: refusing to package: $stampBad (pass -AllowDirty for a test bundle)" -ForegroundColor Red
        exit 1
    }
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
# The bundle carries its provenance: dsvp.nsi reads this and refuses a
# bundle that has none or is not a release build.
Set-Content -Path "$outDir\dsvp.stamp" -Value $stamp -NoNewline

# Makefile already copies SDL3.dll, SDL3_ttf.dll, SDL3_shadercross.dll,
# dxcompiler.dll, dxil.dll to build/
$buildDlls = Get-ChildItem "build\*.dll" -ErrorAction SilentlyContinue
foreach ($f in $buildDlls) {
    Copy-Item $f.FullName "$outDir\"
}
Write-Host "      Copied $($buildDlls.Count) DLLs from build/" -ForegroundColor Green

# ── Resolve transitive DLL dependencies ────────────────────────────

Write-Host "[4/5] Resolving DLL dependencies..." -ForegroundColor Yellow

# Build the DLL search list (review M12 / d-S5-c / S5-10):
#   1. the bin/ of every prefix the exe was LINKED against (pkg-config)
#   2. the toolchain's own bin (the GCC that linked the exe — the
#      libstdc++/libgcc_s/libwinpthread trio must come from here)
#   3. C:\msys64\mingw64\bin as a catch-all
#   4. the shadercross CI bin LAST — only libspirv-cross-c-shared.dll
#      is unique to it; its own SDL3.dll and MinGW runtime must never
#      win over the toolchain's.
# Every copied DLL prints the directory it came from, and an import
# found nowhere is a hard failure, not a silent skip.
$searchDirs = @()
# --exists first (silent on a miss), then --variable only for a package
# that exists (no stderr, so no PS 5.1 NativeCommandError). A cmd /c
# wrapper returned nothing when launched from PowerShell (field
# 2026-09-06), which silently emptied this list.
function Get-PkgPrefix([string]$pkg) {
    & pkg-config --exists $pkg
    if ($LASTEXITCODE -ne 0) { return $null }
    return (& pkg-config --variable=prefix $pkg)
}
foreach ($pkg in @("sdl3", "SDL3_ttf", "libavcodec", "libavformat", "libavfilter",
                   "libavutil", "libswscale", "libswresample")) {
    $prefix = Get-PkgPrefix $pkg
    if ($prefix) {
        $bin = Join-Path ($prefix -replace '/', '\') "bin"
        if ((Test-Path $bin) -and ($searchDirs -notcontains $bin)) { $searchDirs += $bin }
    }
}
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if ($gcc) {
    $tcBin = Split-Path $gcc.Source
    if ($searchDirs -notcontains $tcBin) { $searchDirs += $tcBin }
}
if ((Test-Path "C:\msys64\mingw64\bin") -and ($searchDirs -notcontains "C:\msys64\mingw64\bin")) {
    $searchDirs += "C:\msys64\mingw64\bin"
}
if (Test-Path "$scDir\bin") {
    $searchDirs += (Resolve-Path "$scDir\bin").Path
}
# vcpkg deliberately NOT searched: its DLLs are MSVC-ABI builds —
# bundling one next to MinGW binaries is a different-CRT trap.

if ($searchDirs.Count -eq 0) {
    Write-Host "ERROR: no DLL search directories (pkg-config prefixes, gcc, msys64, shadercross all absent)." -ForegroundColor Red
    exit 1
}
Write-Host "      Search dirs (in order):" -ForegroundColor DarkGray
foreach ($d in $searchDirs) { Write-Host "        $d" -ForegroundColor DarkGray }

# A dependency is "system" by NAME on a fixed allowlist, not by name
# collision with a file in system32 (review m-S5-c). A name that is
# found in system32 but is not on the list is reported, not silently
# trusted.
$systemAllow = @(
    "kernel32.dll","user32.dll","gdi32.dll","advapi32.dll","shell32.dll","ole32.dll",
    "oleaut32.dll","comdlg32.dll","comctl32.dll","ws2_32.dll","winmm.dll","imm32.dll",
    "version.dll","setupapi.dll","cfgmgr32.dll","dwmapi.dll","shlwapi.dll","uxtheme.dll",
    "msvcrt.dll","ntdll.dll","bcrypt.dll","crypt32.dll","secur32.dll","dxgi.dll","d3d11.dll",
    "d3d12.dll","dxcore.dll","opengl32.dll","hid.dll","dinput8.dll","avrt.dll","ksuser.dll",
    "propsys.dll","wintrust.dll","rpcrt4.dll","sechost.dll","ucrtbase.dll","psapi.dll",
    "iphlpapi.dll","dbghelp.dll","shcore.dll","windowscodecs.dll","dsound.dll","gdiplus.dll",
    "netapi32.dll","userenv.dll","wldap32.dll","normaliz.dll","mpr.dll","wtsapi32.dll",
    "mfplat.dll","mf.dll","mfreadwrite.dll","mfuuid.dll","kernelbase.dll","powrprof.dll",
    "winhttp.dll","wininet.dll","oleacc.dll","msimg32.dll","d3dcompiler_47.dll",
    # field 2026-09-06 (first packager run on WIN11 flagged these as WARN):
    "dwrite.dll","usp10.dll","ncrypt.dll","bcryptprimitives.dll","wsock32.dll","dnsapi.dll"
)
$systemDirs = @("C:\Windows\system32", "C:\Windows")
$resolved = @{}
$unresolved = @()
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
            $depL = $dep.ToLower()

            # System DLLs: allowlist by name, or api-ms-win-*/ext-ms-* forwarders
            if (($systemAllow -contains $depL) -or ($depL -like "api-ms-win-*") -or ($depL -like "ext-ms-*")) { continue }

            # Search the known directories in order
            $found = $false
            foreach ($searchDir in $searchDirs) {
                $srcPath = Join-Path $searchDir $dep
                if (Test-Path $srcPath) {
                    Copy-Item $srcPath "$outDir\"
                    Write-Host "      + $dep  ($searchDir)" -ForegroundColor DarkGray
                    $changed = $true
                    $found = $true
                    break
                }
            }
            if ($found) { continue }

            # Not on the allowlist, not in any search dir: is it a system
            # DLL we did not list? Say so — never trust by collision silently.
            $inSys = $false
            foreach ($sysDir in $systemDirs) {
                if (Test-Path (Join-Path $sysDir $dep)) { $inSys = $true; break }
            }
            if ($inSys) {
                Write-Host "      WARN: $dep (needed by $($f.Name)) treated as system — found in Windows dir, not on the allowlist" -ForegroundColor Yellow
                continue
            }
            $unresolved += "$dep (needed by $($f.Name))"
        }
    }
}

if ($unresolved.Count -gt 0) {
    Write-Host "ERROR: unresolved imports — not in any search dir and not a known system DLL:" -ForegroundColor Red
    foreach ($u in ($unresolved | Sort-Object -Unique)) { Write-Host "        $u" -ForegroundColor Red }
    exit 1
}

# The SDL3.dll that ships must be the one the exe was linked against
# (review S5-6): compare against the sdl3 pkg-config prefix's copy.
$sdlPrefix = Get-PkgPrefix "sdl3"
if ($sdlPrefix) {
    $refSdl = Join-Path (Join-Path ($sdlPrefix -replace '/', '\') "bin") "SDL3.dll"
    $outSdl = Join-Path $outDir "SDL3.dll"
    if ((Test-Path $refSdl) -and (Test-Path $outSdl)) {
        # .NET hasher, not Get-FileHash: that cmdlet's module does not
        # auto-load in a PowerShell launched from the MSYS shell (field
        # 2026-09-06, "not recognized").
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $h1 = [BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes((Resolve-Path $refSdl).Path)))
        $h2 = [BitConverter]::ToString($sha.ComputeHash([System.IO.File]::ReadAllBytes((Resolve-Path $outSdl).Path)))
        if ($h1 -ne $h2) {
            Write-Host "ERROR: bundled SDL3.dll differs from $refSdl (the one the exe was linked against)." -ForegroundColor Red
            exit 1
        }
        Write-Host "      SDL3.dll matches $refSdl" -ForegroundColor Green
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
