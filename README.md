# DSVP — Dead Simple Video Player

WIP, this 1.0
WHY? Because I can. And education. And I want to offer a mpv-style player without configs or intimidation-factor.

TODO- subtitles are next up

Claude wrote most of this stuff:

A minimalist, reference-quality video player. Software decode only. No networking. No nonsense.

Built on FFmpeg's libraries and SDL2 — the same stack that powers `ffplay`, but with a clean interface and precise control over the decode pipeline.

REQUIRES Visual C++ Redistributable runtime (vcruntime140.dll). It's probably already on your PC but you can get it here:
https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170

![Windows](https://img.shields.io/badge/Windows-supported-blue)
![Linux](https://img.shields.io/badge/Linux-supported-blue)
![macOS](https://img.shields.io/badge/macOS-supported-blue)

## Features

- **Reference-quality playback** — Lanczos scaling, error-diffusion dithering, faithful color/gamma/framerate adherence
- **Software decode only** — no hardware acceleration, no driver quirks, bit-exact output
- **Supports everything FFmpeg supports** — H.264, HEVC, AV1, VP9, MKV, MP4, and hundreds more
- **Multi-threaded decoding** — uses all available CPU cores
- **Minimal interface** — overlays appear on mouse activity, auto-hide after 3 seconds
- **Portable** — single folder, no installer, no PATH changes
- **Secure** — no networking capabilities whatsoever

## Controls

| Key | Action |
|---|---|
| `O` | Open file |
| `Q` | Quit / close current file |
| `Space` | Pause / resume |
| `F` / double-click | Toggle fullscreen |
| `←` / `→` | Seek ±5 seconds |
| `↑` / `↓` | Volume up / down |
| `D` | Toggle debug overlay |
| `I` | Toggle media info overlay |

## Building from Source

### Requirements

- **GCC** (MinGW on Windows, gcc/clang on Linux/macOS)
- **FFmpeg 6.0+** shared development libraries
- **SDL2 2.28+** development libraries
- **GNU Make**

### Windows (MinGW)

**1. Install build tools** (via [Scoop](https://scoop.sh)):
```powershell
scoop install gcc make
```

**2. Download dependencies:**

- FFmpeg shared build: [gyan.dev/ffmpeg/builds](https://www.gyan.dev/ffmpeg/builds/) → `ffmpeg-release-full-shared.7z`
- SDL2 MinGW dev: [github.com/libsdl-org/SDL/releases](https://github.com/libsdl-org/SDL/releases) → `SDL2-devel-x.xx.x-mingw.zip`

**3. Place in `deps/`:**
```
deps/
  ffmpeg/
    bin/      ← DLLs
    include/  ← headers
    lib/      ← import libraries
  SDL2/
    bin/      ← SDL2.dll
    include/  ← SDL2/ headers
    lib/      ← libSDL2.a, libSDL2main.a
```

**4. Build:**
```powershell
mingw32-make
```

**5. Package for distribution:**
```powershell
.\package.ps1
```

This creates a `DSVP-portable/` folder with the exe and all required DLLs.

### Linux

```bash
# Debian/Ubuntu
sudo apt install gcc make libavformat-dev libavcodec-dev libswscale-dev \
    libswresample-dev libavutil-dev libsdl2-dev

make
./package.sh
```

### macOS

```bash
brew install ffmpeg sdl2 pkg-config
make
./package.sh
```

## Architecture

```
┌───────────────────────────────────────────────────┐
│  Main Thread          │  Demux Thread             │
│                       │                           │
│  SDL Events ──────┐   │  av_read_frame() ───────┐ │
│  Video Decode  ◄──┤   │    ├─▶ Video PacketQueue │ │
│  sws_scale     ◄──┤   │    └─▶ Audio PacketQueue │ │
│  SDL Render    ◄──┘   │                           │
│                       │  SDL Audio Thread          │
│  Overlays             │  audio_callback() ◄───────│
│  (bitmap font)        │  Audio Decode + Resample  │
└───────────────────────────────────────────────────┘
```

A/V sync strategy: **audio is the master clock**. Video frame timing adjusts to match audio playback position.

## Project Structure

```
DSVP/
  src/
    dsvp.h      ← shared types, constants, declarations
    main.c      ← SDL init, event loop, overlays, file dialog
    player.c    ← demux, video decode, display, seeking, info
    audio.c     ← audio decode, resample, SDL audio callback
    log.c       ← crash-safe file logger
  Makefile      ← cross-platform build
  package.ps1   ← Windows packaging script
  package.sh    ← Linux/macOS packaging script
```

## Debug Build

```powershell
mingw32-make debug
```

This enables console output, verbose FFmpeg logging, and debug symbols. A `dsvp.log` file is written to the working directory with timestamped events.

## License

MIT — see [LICENSE](LICENSE).
