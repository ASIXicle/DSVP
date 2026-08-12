# DSVP Dependency Setup

Detailed setup instructions for building DSVP from source. See the [README](README.md) for a quick-start overview.

## Windows (MSYS2 MinGW64 + git-bash)

DSVP on Windows uses MSYS2 for SDL3 and FFmpeg packages, with GCC from Scoop (or MSYS2). You build and run from git-bash.

### Step 1: Install MSYS2

Download and install from [msys2.org](https://www.msys2.org/). Default path: `C:\msys64`.

### Step 2: Install SDL3 and FFmpeg

Open the **MSYS2 MinGW 64-bit** shell (not MSYS2 MSYS) and run:

```bash
pacman -Syu
pacman -S mingw-w64-x86_64-sdl3 mingw-w64-x86_64-sdl3-ttf mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-pkg-config
```

This installs SDL3, SDL3_ttf, FFmpeg (8.1+), and pkg-config under `/c/msys64/mingw64/`.

### Step 3: Install GCC (if you don't have it)

Either via Scoop:
```powershell
scoop install gcc make
```

Or via MSYS2:
```bash
pacman -S mingw-w64-x86_64-gcc make
```

### Step 4: Configure git-bash

Add these to your `~/.bashrc` so git-bash can find MSYS2 packages:

```bash
export PKG_CONFIG_PATH="/c/msys64/mingw64/lib/pkgconfig:$PKG_CONFIG_PATH"
export PATH="/c/msys64/mingw64/bin:$PATH"
```

Restart git-bash or run `source ~/.bashrc`.

### Step 5: SDL3_shadercross

Already bundled in the repo at `deps/SDL3_shadercross-3.0.0-windows-mingw-x64/`. The Makefile finds it automatically. No action needed.

If you need a fresh copy, download from [SDL_shadercross GitHub Actions CI](https://github.com/libsdl-org/SDL_shadercross/actions/workflows/main.yml) → latest successful run → Artifacts → `SDL3_shadercross-3.0.0-windows-mingw-x64`.

### Step 6: Build

```bash
cd ~/Pictures/CLAUDE/GITHUB/DSVP   # or wherever your clone lives
mingw32-make
```

Output: `build/dsvp.exe` plus auto-copied DLLs (SDL3.dll, SDL3_ttf.dll, SDL3_shadercross.dll, dxcompiler.dll, dxil.dll).

### Step 7: Run

```bash
./build/dsvp.exe                    # idle window, press O to open file
./build/dsvp.exe path/to/movie.mkv  # open directly
```

### Troubleshooting (Windows)

**"Package sdl3 was not found"** — pkg-config can't see MSYS2 packages. Check that `PKG_CONFIG_PATH` includes `/c/msys64/mingw64/lib/pkgconfig`.

**"cannot find -lSDL3_shadercross"** — the `deps/` directory is missing or misnamed. Verify `deps/SDL3_shadercross-3.0.0-windows-mingw-x64/lib/` exists.

**Missing DLL at runtime** — the Makefile copies SDL3/shadercross DLLs to `build/`, but FFmpeg DLLs are found via PATH. Make sure `/c/msys64/mingw64/bin` is on your PATH.

**Linker errors about WinMain** — `SDL_MAIN_HANDLED` must be defined before `#include <SDL3/SDL.h>` in dsvp.h. This is already in the source.

---

## Updating dependencies (Linux)

Point releases carry decoder and driver fixes worth having in release
builds. The portable tarball and .deb bundle whatever the binary links
against (package.sh walks the link dependencies), so upgrading a local
prefix and rebuilding is all it takes — no packaging changes needed.

### FFmpeg: major version bump (e.g. 8.1.x → 9.0) — NEW prefix

Major bumps change sonames, so they get their own prefix: the old one
stays on disk as an instant rollback (re-aim two `~/.bashrc` lines and
rebuild). Bugfix releases within a branch instead over-install the
same prefix with the same configure.

```bash
cd ~/Documents
wget https://ffmpeg.org/releases/ffmpeg-9.0.tar.xz
tar xf ffmpeg-9.0.tar.xz
cd ffmpeg-9.0
./configure --prefix=$HOME/ffmpeg-9.0-local \
    --enable-shared --disable-static \
    --enable-gpl \
    --disable-programs --disable-doc \
    --disable-encoders --disable-muxers \
    --enable-muxer=spdif \
    --enable-libdav1d
make -j$(nproc)
make install
```

Every flag is load-bearing; three have bitten before:

- `--enable-gpl`: bwdif deinterlacing is a GPL filter — without this
  it silently vanishes from libavfilter and interlaced content
  breaks. (Field case 2026-08-10: a 9.0 build made from the deck
  repo's configure — which doesn't deinterlace — shipped without it.)
- `--enable-muxer=spdif`: bitstream.c frames passthrough audio with
  the spdif muxer; a plain `--disable-muxers` eats it silently and
  passthrough degrades to PCM with only a log line to show for it.
  (The original 8.1 prefix had exactly this hole on Linux; Windows
  never saw it because MSYS2 ships full FFmpeg.)
- NO `--enable-vaapi`: the x64 tree is software-decode only — the
  flag adds nothing and fails configure when libva-dev is absent.
  (VAAPI is a DSVP-deck thing; the repos have diverged.)

Do NOT add `--disable-filters` — libavfilter must build.

Re-aim `~/.bashrc` (comment the old lines — that comment IS the
rollback):

```bash
# export PKG_CONFIG_PATH="$HOME/ffmpeg-8.1-local/lib/pkgconfig:$PKG_CONFIG_PATH"
# export LD_LIBRARY_PATH="$HOME/ffmpeg-8.1-local/lib:$LD_LIBRARY_PATH"
export PKG_CONFIG_PATH="$HOME/ffmpeg-9.0-local/lib/pkgconfig:$HOME/sdl3-local/lib/pkgconfig:$PKG_CONFIG_PATH"
export LD_LIBRARY_PATH="$HOME/ffmpeg-9.0-local/lib:$HOME/sdl3-local/lib:$LD_LIBRARY_PATH"
```

Fresh shell (`exec bash`), then relink and confirm the startup line:

```bash
pkg-config --modversion libavcodec    # 63.x = FFmpeg 9.0
cd ~/Documents/DSVP/DSVP && make clean && make
```

### SDL3: distro package → upstream (worth it for release builds)

Debian's libsdl3 lags upstream, and recent upstream releases carry SDL
GPU Vulkan crash fixes and a PipeWire under-load fix that matter for
DSVP specifically. Check what you have:

```bash
pkg-config --modversion sdl3
```

If it's below the current upstream release, build SDL3 + SDL3_ttf into
a local prefix (does not touch the system packages):

Install the backend dev packages FIRST — a source SDL only enables
backends whose headers it finds at configure time. Miss libwayland-dev
and it silently builds X11-only, which changes presentation behavior:

```bash
sudo apt install -y cmake build-essential \
  libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev \
  libegl1-mesa-dev libgl1-mesa-dev libvulkan-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
  libxfixes-dev libxtst-dev \
  libpipewire-0.3-dev libasound2-dev libpulse-dev libudev-dev \
  libdbus-1-dev libfreetype-dev libharfbuzz-dev
```

Then build (check cmake's summary for Wayland/Vulkan/PipeWire before
letting it compile):

```bash
cd ~/Documents
git clone --depth 1 --branch release-3.4.14 https://github.com/libsdl-org/SDL
cmake -S SDL -B SDL/build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$HOME/sdl3-local
cmake --build SDL/build -j$(nproc) && cmake --install SDL/build

# SDL3_ttf against the local SDL3 (pick the latest release tag)
git clone --depth 1 --branch release-3.2.2 https://github.com/libsdl-org/SDL_ttf
cmake -S SDL_ttf -B SDL_ttf/build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$HOME/sdl3-local \
      -DCMAKE_PREFIX_PATH=$HOME/sdl3-local
cmake --build SDL_ttf/build -j$(nproc) && cmake --install SDL_ttf/build
```

Note: newer SDL3_ttf installs its pkg-config file as `sdl3-ttf.pc`
(lowercase) where older/distro packages used `SDL3_ttf.pc`. The
Makefile probes both names — nothing to fix, but manual
`pkg-config --modversion` checks need the name that exists:
`pkg-config --modversion sdl3 sdl3-ttf`. Field result 2026-08-10:
SDL 3.2.10 → 3.4.14 cut fullscreen Wayland present starvation on an
Intel UHD 620 laptop by ~10x (multi_ticks 86-147 → 7 per 40s run) —
this upgrade is worth it for desktop Linux users specifically.

The `~/.bashrc` wiring above (FFmpeg section) already includes the
sdl3-local entries. Rebuild (`make clean && make`), then verify which
libraries the binary actually linked before packaging:

```bash
ldd build/dsvp | grep -E 'SDL3|avcodec'
# every line should resolve into ~/sdl3-local and ~/ffmpeg-9.0-local
```

`./package.sh` and `./installer/package-deb.sh` bundle exactly what ldd
shows, so the release artifacts inherit the upgraded libraries
automatically.

On Windows, `pacman -Syu` in MSYS2 tracks SDL3/FFmpeg point releases —
run it before a release build.

---

## Linux (Debian/Ubuntu)

### Step 1: Install system packages

```bash
sudo apt install gcc make pkg-config \
    libsdl3-dev libsdl3-ttf-dev \
    zlib1g-dev fonts-dejavu-core fonts-noto-cjk zenity
```

`fonts-noto-cjk` provides CJK subtitle fallback. `zenity` provides the file-open dialog.

### Step 2: FFmpeg 8.1+

DSVP requires FFmpeg 8.1 or newer. Check your system version:

```bash
ffmpeg -version | head -1
```

**If your system FFmpeg is 8.1+**, install the dev packages and skip to Step 3:

```bash
sudo apt install libavformat-dev libavcodec-dev libavfilter-dev libswscale-dev \
    libswresample-dev libavutil-dev
```

**If your system FFmpeg is older (e.g. Debian ships 7.x)**, build FFmpeg 9.0 from source into a local prefix. This does not replace your system FFmpeg — it installs alongside it in your home directory.

```bash
# Install build dependencies
sudo apt install build-essential nasm yasm \
    libx264-dev libx265-dev libvpx-dev libopus-dev libdav1d-dev

# Download and extract
cd ~/Documents
wget https://ffmpeg.org/releases/ffmpeg-9.0.tar.xz
tar xf ffmpeg-9.0.tar.xz
cd ffmpeg-9.0

# Configure for decode-only (no CLI tools, no encoders — just the
# libraries DSVP links against). Every flag is load-bearing — see the
# annotated version in "Updating dependencies (Linux)" above; in
# particular --enable-muxer=spdif (bitstream passthrough silently
# degrades without it) and --enable-gpl (bwdif deinterlacing).
./configure --prefix=$HOME/ffmpeg-9.0-local \
    --enable-shared --disable-static \
    --enable-gpl \
    --disable-programs --disable-doc \
    --disable-encoders --disable-muxers \
    --enable-muxer=spdif \
    --enable-libdav1d

# Build and install to ~/ffmpeg-9.0-local/
make -j$(nproc)
make install
```

Then set `PKG_CONFIG_PATH` so the DSVP Makefile finds the local FFmpeg, and `LD_LIBRARY_PATH` so the binary can find the `.so` files at runtime. Add both to your `~/.bashrc` for persistence:

```bash
export PKG_CONFIG_PATH=$HOME/ffmpeg-9.0-local/lib/pkgconfig:$PKG_CONFIG_PATH
export LD_LIBRARY_PATH=$HOME/ffmpeg-9.0-local/lib:$LD_LIBRARY_PATH
```

Verify it took effect:

```bash
source ~/.bashrc
pkg-config --modversion libavcodec
# Should print 63.x (FFmpeg 9.0)
```

### Step 3: SDL3_shadercross

Already bundled in the repo at `shadercross/SDL3_shadercross-3.0.0-linux-x64/`. The Makefile finds it automatically. No action needed.

If you need a fresh copy, download from [SDL_shadercross GitHub Actions CI](https://github.com/libsdl-org/SDL_shadercross/actions/workflows/main.yml) → latest successful run → Artifacts → `SDL3_shadercross-3.0.0-linux-x64`.

**Note:** Some Git/OS combinations don't preserve symlinks on clone. If you see linker errors about missing `.so` files, recreate the symlinks:

```bash
cd shadercross/SDL3_shadercross-3.0.0-linux-x64/lib
ln -sf libSDL3_shadercross.so.0.0.0 libSDL3_shadercross.so.0
ln -sf libSDL3_shadercross.so.0.0.0 libSDL3_shadercross.so
ln -sf libspirv-cross-c-shared.so.0.64.0 libspirv-cross-c-shared.so.0
ln -sf libspirv-cross-c-shared.so.0.64.0 libspirv-cross-c-shared.so
ln -sf libvkd3d.so.1.19.0 libvkd3d.so.1
ln -sf libvkd3d.so.1.19.0 libvkd3d.so
ln -sf libvkd3d-shader.so.1.17.0 libvkd3d-shader.so.1
ln -sf libvkd3d-shader.so.1.17.0 libvkd3d-shader.so
```

### Step 4: Build

```bash
cd ~/Documents/DSVP/DSVP   # or wherever your clone lives
make
```

Output: `build/dsvp`

If you built FFmpeg from source, the binary links against your local prefix's `.so` files. At runtime you'll need `LD_LIBRARY_PATH` to find them (or use `package.sh`, which bundles everything — and refuses to package if any library is unresolved):

```bash
# Run directly (with local FFmpeg)
LD_LIBRARY_PATH=$HOME/ffmpeg-9.0-local/lib ./build/dsvp /path/to/movie.mkv

# Or package for distribution (bundles all libs automatically)
LD_LIBRARY_PATH=$HOME/ffmpeg-9.0-local/lib ./package.sh
```

### Step 5: Run

```bash
./build/dsvp                        # idle window, press O to open file
./build/dsvp /path/to/movie.mkv     # open directly
```

### Troubleshooting (Linux)

**"cannot find -lSDL3_shadercross"** — the `shadercross/` directory is missing or symlinks weren't created. Run `ls -la shadercross/SDL3_shadercross-3.0.0-linux-x64/lib/` and verify `.so` symlinks exist.

**"error while loading shared libraries: libSDL3_shadercross.so.0"** — the runtime linker can't find shadercross. The Makefile sets `-Wl,-rpath` relative to the binary, but if you move the binary out of `build/`, the rpath won't resolve. Run from the repo root or use `LD_LIBRARY_PATH`.

**"error while loading shared libraries: libavformat.so.62"** — you built against FFmpeg 8.1 from source but the runtime linker can't find the `.so` files. Either run with `LD_LIBRARY_PATH=$HOME/ffmpeg-8.1-local/lib` or use the portable tarball from `package.sh` which bundles all libraries.

**Vulkan validation errors** — install `vulkan-tools` and run `vulkaninfo` to verify your GPU supports Vulkan. DSVP forces Vulkan via `SDL_SetHint`.

---

## Debug Build

Both platforms support a debug target:

```bash
make debug          # Linux
mingw32-make debug  # Windows
```

This adds `-g -DDSVP_DEBUG`, which enables GPU validation layers, console output, verbose FFmpeg logging, and writes `dsvp.log` next to the executable (working-directory fallback if unwritable).
