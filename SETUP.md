# DSVP Dependency Setup — Windows (MinGW)

## What You Need

DSVP links against FFmpeg's **shared libraries** (DLLs + headers + import libs)
and SDL2. Your existing `ffmpeg.exe` on PATH is just the binary — you also need
the development files (C headers and linker libraries).

## Step 1: FFmpeg Shared Dev Build

Go to: https://www.gyan.dev/ffmpeg/builds/

Under **Release builds → Shared**:
Download: `ffmpeg-release-full-shared.7z`

Extract it. You'll get a folder like `ffmpeg-7.1.1-full_build-shared/`.

Now copy its contents into your project:

```powershell
# From your DSVP project directory:
mkdir deps\ffmpeg

# Copy the three key directories:
# (adjust the extracted folder name to match your download)
Copy-Item -Recurse "C:\path\to\ffmpeg-7.1.1-full_build-shared\include" "deps\ffmpeg\include"
Copy-Item -Recurse "C:\path\to\ffmpeg-7.1.1-full_build-shared\lib"     "deps\ffmpeg\lib"
Copy-Item -Recurse "C:\path\to\ffmpeg-7.1.1-full_build-shared\bin"     "deps\ffmpeg\bin"
```

Verify you have:
```
deps/ffmpeg/include/libavcodec/avcodec.h     ← headers
deps/ffmpeg/lib/avcodec.lib                   ← import libraries
deps/ffmpeg/bin/avcodec-61.dll                ← runtime DLLs
```

NOTE: The gyan.dev shared builds include `.lib` files (MSVC-style import libraries).
MinGW's GCC can link against these directly. If your build has `.dll.a` files instead,
those work too — the Makefile handles both.

## Step 2: SDL2 MinGW Dev Package

Go to: https://github.com/libsdl-org/SDL/releases/tag/release-2.30.12
(or latest 2.x release — do NOT use SDL3)

Download: `SDL2-devel-2.30.12-mingw.zip`

Extract it. Inside you'll find platform-specific folders.

```powershell
mkdir deps\SDL2

# Copy the 64-bit MinGW files:
Copy-Item -Recurse "C:\path\to\SDL2-2.30.12\x86_64-w64-mingw32\include" "deps\SDL2\include"
Copy-Item -Recurse "C:\path\to\SDL2-2.30.12\x86_64-w64-mingw32\lib"     "deps\SDL2\lib"
Copy-Item -Recurse "C:\path\to\SDL2-2.30.12\x86_64-w64-mingw32\bin"     "deps\SDL2\bin"
```

Verify you have:
```
deps/SDL2/include/SDL2/SDL.h               ← headers
deps/SDL2/lib/libSDL2.a                    ← static import lib
deps/SDL2/lib/libSDL2main.a                ← SDL2main (provides WinMain)
deps/SDL2/bin/SDL2.dll                     ← runtime DLL
```

## Step 3: Build

```powershell
cd C:\Users\seth\Pictures\CLAUDE\DSVP
mingw32-make
```

If `mingw32-make` isn't found, try just `make`. If neither works, you may need to
add your MinGW bin directory to PATH, or install make via Scoop:
```powershell
scoop install make
```

## Step 4: Copy DLLs for Running

The built `build/dsvp.exe` needs the DLLs at runtime. Either:

**Option A** — Copy DLLs next to the exe:
```powershell
copy deps\ffmpeg\bin\*.dll build\
copy deps\SDL2\bin\SDL2.dll build\
```

**Option B** — Add deps to PATH temporarily:
```powershell
$env:PATH = "$PWD\deps\ffmpeg\bin;$PWD\deps\SDL2\bin;$env:PATH"
```

## Step 5: Run

```powershell
.\build\dsvp.exe                    # idle window, press O to open file
.\build\dsvp.exe path\to\movie.mkv  # open directly
```

## Troubleshooting

**"cannot find -lavformat"**: FFmpeg lib directory is wrong. Check that
`deps/ffmpeg/lib/` contains `.lib` or `.dll.a` files.

**"SDL.h: No such file"**: SDL2 include path is wrong. The header should be at
`deps/SDL2/include/SDL2/SDL.h`.

**"Entry point not found" or missing DLL errors at runtime**: Copy all DLLs from
`deps/ffmpeg/bin/` and `deps/SDL2/bin/` into `build/`.

**Linker errors about `WinMain`**: Make sure you're linking `-lmingw32 -lSDL2main`
*before* `-lSDL2`. The Makefile handles this.
