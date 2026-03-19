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

This installs SDL3, SDL3_ttf, FFmpeg (8.0+), and pkg-config under `/c/msys64/mingw64/`.

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

## Linux (Debian/Ubuntu)

### Step 1: Install system packages

```bash
sudo apt install gcc make pkg-config \
    libsdl3-dev libsdl3-ttf-dev \
    zlib1g-dev fonts-dejavu-core fonts-noto-cjk zenity
```

`fonts-noto-cjk` provides CJK subtitle fallback. `zenity` provides the file-open dialog.

### Step 2: FFmpeg 8.0+

DSVP requires FFmpeg 8.0 or newer. Check your system version:

```bash
ffmpeg -version | head -1
```

**If your system FFmpeg is 8.0+**, install the dev packages and skip to Step 3:

```bash
sudo apt install libavformat-dev libavcodec-dev libswscale-dev \
    libswresample-dev libavutil-dev
```

**If your system FFmpeg is older (e.g. Debian ships 7.x)**, build FFmpeg 8.1 from source into a local prefix. This does not replace your system FFmpeg — it installs alongside it in your home directory.

```bash
# Install build dependencies
sudo apt install build-essential nasm yasm \
    libx264-dev libx265-dev libvpx-dev libopus-dev

# Download and extract
cd ~/Documents
wget https://ffmpeg.org/releases/ffmpeg-8.1.tar.xz
tar xf ffmpeg-8.1.tar.xz
cd ffmpeg-8.1

# Configure for decode-only (no CLI tools, no encoders — just the libraries DSVP links against)
./configure --prefix=$HOME/ffmpeg-8.1-local \
    --enable-shared --disable-static \
    --enable-gpl \
    --disable-programs --disable-doc \
    --disable-encoders --disable-muxers

# Build and install to ~/ffmpeg-8.1-local/
make -j$(nproc)
make install
```

Then set `PKG_CONFIG_PATH` so the DSVP Makefile finds the local FFmpeg. Add this to your `~/.bashrc` for persistence:

```bash
export PKG_CONFIG_PATH=$HOME/ffmpeg-8.1-local/lib/pkgconfig:$PKG_CONFIG_PATH
```

Verify it took effect:

```bash
source ~/.bashrc
pkg-config --modversion libavcodec
# Should print 62.28.100 (FFmpeg 8.1)
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

If you built FFmpeg from source, the binary will link against the 8.1 `.so` files. At runtime you'll need `LD_LIBRARY_PATH` to find them (or use `package.sh` which bundles everything):

```bash
# Run directly (with local FFmpeg)
LD_LIBRARY_PATH=$HOME/ffmpeg-8.1-local/lib ./build/dsvp /path/to/movie.mkv

# Or package for distribution (bundles all libs automatically)
LD_LIBRARY_PATH=$HOME/ffmpeg-8.1-local/lib ./package.sh
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

This adds `-g -DDSVP_DEBUG`, which enables GPU validation layers, console output, verbose FFmpeg logging, and writes `dsvp.log` to the working directory.
