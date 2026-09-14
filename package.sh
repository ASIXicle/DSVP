#!/bin/bash
# DSVP Portable Packaging Script (Linux)
# Creates a self-contained DSVP-portable/ folder with binary + shared libs.
#
# Usage:
#   ./package.sh
#   ./package.sh --skip-build

set -e

# Version derives from src/dsvp.h — single source of truth, never hardcode here
VERSION=$(sed -n 's/.*DSVP_VERSION *"\(.*\)"/\1/p' src/dsvp.h)
if [ -z "$VERSION" ]; then
    echo "ERROR: could not extract DSVP_VERSION from src/dsvp.h"
    exit 1
fi
OUTDIR="DSVP-portable"
SKIP_BUILD=0
ALLOW_DIRTY=0

for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=1 ;;
        --allow-dirty) ALLOW_DIRTY=1 ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

echo "=== DSVP Packager v${VERSION} ==="

# ── Build ──────────────────────────────────────────────────────────

if [ "$SKIP_BUILD" -eq 0 ]; then
    echo -e "\n[1/5] Building..."
    make clean 2>/dev/null || true
    make
    echo "      Build OK"
else
    echo -e "\n[1/5] Skipping build"
fi

# ── Verify binary ─────────────────────────────────────────────────

if [ ! -f "build/dsvp" ]; then
    echo "ERROR: build/dsvp not found."
    exit 1
fi

# ── Provenance gate (review M9) ────────────────────────────────────
# The Makefile writes build/dsvp.stamp ("<sha>[+dirty] <mode>") at link.
# A package must carry a real, clean, release stamp that matches HEAD,
# or say --allow-dirty and mean it.
STAMP=$(cat build/dsvp.stamp 2>/dev/null || echo "missing")
STAMP_SHA=${STAMP%% *}
STAMP_MODE=${STAMP##* }
echo "      Binary stamp: ${STAMP}"
stamp_bad=""
case "$STAMP_SHA" in
    unknown|missing) stamp_bad="stamp is '${STAMP_SHA}' (no git tree or pre-stamp binary)" ;;
    *+dirty)         stamp_bad="stamp is +dirty (uncommitted changes)" ;;
esac
if [ -z "$stamp_bad" ] && [ "$STAMP_MODE" != "release" ]; then
    stamp_bad="binary is a ${STAMP_MODE} build"
fi
if [ -z "$stamp_bad" ] && [ -e .git ]; then
    # Stale means the BUILD INPUTS changed since the binary was linked —
    # not merely that HEAD moved (a docs or packaging commit must not
    # force a rebuild; field 2026-09-06). Inputs: sources, the Makefile,
    # the resource file, the bundled shadercross/deps trees.
    HEAD_SHA=$(git rev-parse --short HEAD 2>/dev/null || echo none)
    if [ "$STAMP_SHA" != "$HEAD_SHA" ]; then
        if ! git cat-file -e "${STAMP_SHA}^{commit}" 2>/dev/null; then
            stamp_bad="stamp ${STAMP_SHA} is not a commit in this repository"
        elif ! git diff --quiet "$STAMP_SHA" HEAD -- src Makefile dsvp.rc shadercross deps 2>/dev/null; then
            stamp_bad="build inputs changed between stamp ${STAMP_SHA} and HEAD ${HEAD_SHA} (stale binary): $(git diff --stat "$STAMP_SHA" HEAD -- src Makefile dsvp.rc shadercross deps | tail -1)"
        else
            echo "      Stamp ${STAMP_SHA} predates HEAD ${HEAD_SHA}, but no build input changed between them — binary is current"
        fi
    fi
fi
if [ -n "$stamp_bad" ]; then
    if [ "$ALLOW_DIRTY" -eq 1 ]; then
        echo "      WARNING: ${stamp_bad} — packaging anyway (--allow-dirty)"
    else
        echo "ERROR: refusing to package: ${stamp_bad}. Rebuild from a clean commit, or pass --allow-dirty for a test build."
        exit 1
    fi
fi

# Refuse to package with unresolved libraries. The bundling walk below
# has no path field for a "not found" line and would SILENTLY skip it —
# a clean shell / sudo / CI run (no interactive LD_LIBRARY_PATH) used
# to produce a tarball missing every FFmpeg/SDL lib, exit code 0.
if ldd build/dsvp | grep -q "not found"; then
    echo "ERROR: unresolved shared libraries — export LD_LIBRARY_PATH for"
    echo "       your ffmpeg/sdl prefixes (see SETUP.md) and retry:"
    ldd build/dsvp | grep "not found"
    exit 1
fi

# ── Create output directory ────────────────────────────────────────

echo "[2/5] Creating ${OUTDIR}/"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR/lib"

# ── Copy binary ───────────────────────────────────────────────────

echo "[3/5] Copying binary..."
cp build/dsvp "$OUTDIR/"
cp LICENSE "$OUTDIR/"   # GPL-3: binary distribution requires the license text

# ── Bundle shared libraries ───────────────────────────────────────

echo "[4/5] Bundling shared libraries..."

if [ "$(uname)" = "Linux" ]; then
    # System libs stay on the host; everything else is bundled. The
    # display/driver stack (X11, xcb, DRM, GL, Vulkan, Wayland, xkb) is
    # host-owned by policy — the AppImage exclude-list reasoning: these
    # must match the running server and drivers, and every desktop has
    # them (the .deb declares them; field 2026-09-06).
    SYSTEM_LIBS="linux-vdso|ld-linux|libc\.so|libm\.so|libpthread|libdl|librt\.so|libgcc_s|libstdc\+\+"
    SYSTEM_LIBS="${SYSTEM_LIBS}|libX11|libxcb|libXau|libXdmcp|libXext|libXfixes|libXrandr|libXcursor|libXi\.so|libXinerama|libXss|libdrm\.so|libgbm|libGL|libEGL|libvulkan|libwayland|libxkbcommon|libdecor"
    # fontconfig is host-owned too (libass's font provider): its cache
    # format and /etc/fonts configuration belong to the host's copy, and
    # every desktop ships libfontconfig1 (the .deb declares it). libass
    # itself and its text stack (fribidi, harfbuzz, freetype, unibreak)
    # ride the walk like any other library.
    # ...and so is EXPAT, which only enters the closure through that
    # host-owned fontconfig. Field 2026-09-14, first package.sh run on
    # dellbian since libass landed: the walk bundled libexpat (it is in
    # the flattened closure) but the host's libfontconfig is what loads
    # it, and that copy has no $ORIGIN RUNPATH — so ldd resolved
    # /lib/x86_64-linux-gnu/libexpat.so.1 and the post-check failed on a
    # bundle that would in fact have run fine. Host-owning a library
    # means host-owning its dependencies: bundling half of fontconfig's
    # closure produces a copy that can never be the one that loads. Any
    # machine with libfontconfig1 has libexpat1 (fontconfig Depends on
    # it), so this adds no burden to the tarball or the .deb.
    SYSTEM_LIBS="${SYSTEM_LIBS}|libfontconfig|libexpat"

    # The libdirs the binary was LINKED against (pkg-config). Any SDL or
    # FFmpeg library that ldd resolves OUTSIDE these directories is the
    # wrong copy (review C1: the shadercross artifact's libSDL3.so.0 —
    # same soname, version 0.5.0 — used to win when LD_LIBRARY_PATH was
    # unset) and the run refuses it.
    # libdir of a package: pkg-config if present, else the .pc file found
    # on PKG_CONFIG_PATH read directly (same answer, no tool dependency).
    pc_libdir() {
        if command -v pkg-config >/dev/null 2>&1; then
            pkg-config --exists "$1" 2>/dev/null && pkg-config --variable=libdir "$1" 2>/dev/null
            return
        fi
        local IFS=: d pc prefix libdir
        for d in ${PKG_CONFIG_PATH:-}; do
            pc="$d/$1.pc"; [ -f "$pc" ] || continue
            prefix=$(sed -n 's/^prefix=//p' "$pc" | head -1)
            libdir=$(sed -n 's/^libdir=//p' "$pc" | head -1)
            printf '%s\n' "${libdir//\$\{prefix\}/$prefix}"
            return
        done
    }
    LINKED_DIRS=""
    for pkg in sdl3 SDL3_ttf sdl3-ttf libavcodec libavformat libavfilter libavutil libswscale libswresample; do
        d=$(pc_libdir "$pkg" || true)
        [ -n "$d" ] && [ -d "$d" ] && LINKED_DIRS="${LINKED_DIRS} $(readlink -f "$d")"
    done
    LINKED_DIRS=$(printf '%s\n' $LINKED_DIRS | sort -u | tr '\n' ' ')
    if [ -z "$LINKED_DIRS" ]; then
        echo "      WARNING: no .pc for sdl3/SDL3_ttf/libav* on PKG_CONFIG_PATH — cannot verify library provenance"
    fi

    # ldd walk. The path is captured by sed, not awk '{print $3}', so a
    # prefix with a space in it does not silently drop the library; a
    # resolved path that is not a readable file is FATAL, not skipped.
    bundle_fail=0
    while IFS= read -r line; do
        case "$line" in
            *"=>"*) ;;
            *) continue ;;
        esac
        name=$(printf '%s' "$line" | sed -n 's/^[[:space:]]*\([^[:space:]]*\) =>.*/\1/p')
        path=$(printf '%s' "$line" | sed -n 's/.* => \(.*\) (0x[0-9a-f]*)$/\1/p')
        if printf '%s' "$name" | grep -qE "$SYSTEM_LIBS"; then continue; fi
        if [ -z "$path" ] || [ ! -f "$path" ]; then
            echo "      ERROR: ${name} resolved to '${path}' — not a readable file"
            bundle_fail=1; continue
        fi
        rpath=$(readlink -f "$path")
        rdir=$(dirname "$rpath")
        case "$name" in
            libSDL3.so*|libSDL3_ttf.so*|libav*|libsw*|libpostproc*)   # not libSDL3_shadercross: that one is staged in build/lib
                if [ -n "$LINKED_DIRS" ]; then
                    ok=0
                    for d in $LINKED_DIRS; do [ "$rdir" = "$d" ] && ok=1; done
                    if [ "$ok" -eq 0 ]; then
                        echo "      ERROR: ${name} resolved to ${rpath} — not in a pkg-config libdir (${LINKED_DIRS# }); fix LD_LIBRARY_PATH/rpath"
                        bundle_fail=1; continue
                    fi
                fi ;;
        esac
        cp "$rpath" "$OUTDIR/lib/$(basename "$rpath")"
        printf '      %-32s <= %s\n' "$name" "$rpath"
    done < <(ldd build/dsvp)
    if [ "$bundle_fail" -ne 0 ]; then
        echo "ERROR: library provenance check failed — nothing packaged."
        exit 1
    fi

    # Shadercross runtime libs the walk may not have seen (dlopen'd or
    # only reachable via RUNPATH), from build/lib (staged by the Makefile
    # without the artifact's libSDL3) or the artifact itself.
    for src in build/lib "shadercross/SDL3_shadercross-3.0.0-linux-x64/lib"; do
        [ -d "$src" ] || continue
        for so in "$src"/*.so.*; do
            [ -f "$so" ] && [ ! -L "$so" ] || continue
            base=$(basename "$so")
            case "$base" in libSDL3.so.*) continue ;; esac   # CI artifact's own SDL — never
            if [ ! -f "$OUTDIR/lib/$base" ]; then
                cp "$so" "$OUTDIR/lib/"
                printf '      %-32s <= %s (runtime)\n' "$base" "$so"
            fi
        done
    done

    # Create soname symlinks (dynamic linker needs these) — named by the
    # library's REAL SONAME from its ELF header, not by a regex on the
    # filename: libbz2's SONAME is libbz2.so.1.0 (two components) and
    # the old rule produced libbz2.so.1, so the loader fell back to the
    # host copy (field 2026-09-06, dellbian).
    for so in "$OUTDIR/lib"/*.so*; do
        [ -f "$so" ] && [ ! -L "$so" ] || continue
        base=$(basename "$so")
        soname=$(readelf -d "$so" 2>/dev/null | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p' | head -1)
        [ -n "$soname" ] || soname=$(echo "$base" | sed 's/\(\.so\.[0-9]*\)\..*/\1/')
        if [ "$soname" != "$base" ] && [ ! -e "$OUTDIR/lib/$soname" ]; then
            ln -s "$base" "$OUTDIR/lib/$soname"
        fi
    done

    LIB_COUNT=$(find "$OUTDIR/lib" -maxdepth 1 -type f | wc -l)
    echo "      Bundled $LIB_COUNT libraries"

    # Every bundled library gets RUNPATH $ORIGIN (field 2026-09-06): the
    # binary's $ORIGIN/lib covers only ITS direct dependencies; a bundled
    # library's own dependencies (harfbuzz under SDL_ttf, dav1d under
    # FFmpeg) were resolving to the host's copies and the bundled files
    # were dead weight. The launcher's old LD_LIBRARY_PATH did this job;
    # the rpath does it without leaking into child processes.
    if command -v patchelf >/dev/null 2>&1; then
        find "$OUTDIR/lib" -maxdepth 1 -type f -name '*.so*' -print0 \
            | xargs -0 -n1 patchelf --set-rpath '$ORIGIN' 2>/dev/null || true
        echo "      RUNPATH \$ORIGIN set on ${LIB_COUNT} bundled libraries"
    fi

    # ── RUNPATH of the SHIPPED binary (field 2026-09-06, dellbian): the
    # link line inherits an rpath from the SDL prefix's own sdl3.pc
    # (/home/<dev>/sdl3-local/lib), so every packaged binary carried the
    # developer's home path — a private path in a public artifact, and
    # a search of a directory no user has. The shipped copy gets exactly
    # $ORIGIN/lib:$ORIGIN and nothing else; the dev binary in build/
    # keeps its convenience rpath. patchelf is a hard requirement for a
    # release package for that reason.
    if command -v patchelf >/dev/null 2>&1; then
        patchelf --set-rpath '$ORIGIN/lib:$ORIGIN' "$OUTDIR/dsvp"
        echo "      RUNPATH: $(readelf -d "$OUTDIR/dsvp" | sed -n 's/.*\(RUNPATH\|RPATH\)[^[]*\[\(.*\)\]/\2/p')  (patchelf)"
    else
        if [ "$ALLOW_DIRTY" -eq 1 ]; then
            echo "      WARNING: patchelf not installed — the shipped binary keeps the build-time rpath (may contain a private path)"
        else
            echo "ERROR: patchelf is required to package (it rewrites the shipped RUNPATH to \$ORIGIN/lib:\$ORIGIN): sudo apt install patchelf"
            exit 1
        fi
    fi
    if readelf -d "$OUTDIR/dsvp" | grep -E 'RUNPATH|RPATH' | grep -q '/home/'; then
        echo "ERROR: the shipped binary's RUNPATH still names a home directory — refusing to package."
        exit 1
    fi

    # ── Post-bundle check: the ASSEMBLED tree, with the environment
    # stripped. The binary's RUNPATH is $ORIGIN/lib, so with
    # LD_LIBRARY_PATH unset every non-system library must resolve into
    # the bundle — this is the run a user's desktop launch performs.
    post_fail=0; post_lib=0; post_sys=0
    bundle_abs=$(readlink -f "$OUTDIR/lib")
    while IFS= read -r line; do
        case "$line" in *"=>"*) ;; *) continue ;; esac
        name=$(printf '%s' "$line" | sed -n 's/^[[:space:]]*\([^[:space:]]*\) =>.*/\1/p')
        path=$(printf '%s' "$line" | sed -n 's/.* => \(.*\) (0x[0-9a-f]*)$/\1/p')
        if printf '%s' "$name" | grep -qE "$SYSTEM_LIBS"; then post_sys=$((post_sys+1)); continue; fi
        if [ -z "$path" ] || [ "$(dirname "$(readlink -f "$path")")" != "$bundle_abs" ]; then
            echo "      ERROR: post-check: ${name} => '${path}' (expected inside ${OUTDIR}/lib)"
            post_fail=1
        else
            post_lib=$((post_lib+1))
        fi
    done < <(env -u LD_LIBRARY_PATH ldd "$OUTDIR/dsvp")
    if env -u LD_LIBRARY_PATH ldd "$OUTDIR/dsvp" | grep -q "not found"; then
        env -u LD_LIBRARY_PATH ldd "$OUTDIR/dsvp" | grep "not found" | sed 's/^/      ERROR: post-check: /'
        post_fail=1
    fi
    if [ "$post_fail" -ne 0 ]; then
        echo "ERROR: the assembled bundle does not resolve on its own (see above)."
        exit 1
    fi
    echo "      Post-check: ${post_lib} libs resolve from ${OUTDIR}/lib, ${post_sys} from the system (LD_LIBRARY_PATH unset)"

    # The stamp travels with the bundle so the .deb builder can gate on it too.
    cp build/dsvp.stamp "$OUTDIR/dsvp.stamp" 2>/dev/null || true

    # Launcher: no LD_LIBRARY_PATH (review M11 — a trailing ':' made the
    # loader search the CURRENT DIRECTORY for every non-bundled library,
    # and the file dialog child inherited the bundle's freetype/harfbuzz).
    # The binary finds lib/ through its own RUNPATH; the launcher only
    # keeps the working directory independent of where it was started.
    cat > "$OUTDIR/dsvp.sh" << 'LAUNCHER'
#!/bin/bash
# DSVP launcher — the binary locates lib/ through its RUNPATH ($ORIGIN/lib);
# this wrapper only makes a double-click or a symlinked call work from anywhere.
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
exec "$DIR/dsvp" "$@"
LAUNCHER
    chmod +x "$OUTDIR/dsvp.sh"
    echo "      Created launcher: dsvp.sh"

    # Create README
    cat > "$OUTDIR/README.txt" << 'README'
DSVP — Dead Simple Video Player

Run:
  ./dsvp.sh                        Open DSVP (press O to open a file)
  ./dsvp.sh /path/to/movie.mkv    Open a file directly

./dsvp itself also runs from anywhere: it finds lib/ through its own RUNPATH.

Controls:
  O          Open file
  Q          Quit / close file
  Space      Pause / resume
  F          Toggle fullscreen
  S          Cycle subtitle tracks
  A          Cycle audio tracks
  Left/Right Seek ±5 seconds
  Up/Down    Volume
  B/N        Previous / next file in folder
  D          Debug overlay
  I          Media info overlay
  H          Cycle HDR debug views (normal/comparison/PQ bypass/grayscale)
  T          Cycle SDR target nits (203/300/400)
  G          Cycle midtone gain (1.0/1.1/1.2/1.3/1.35/1.4)

More info: https://github.com/ASIXicle/DSVP
README
    echo "      Created README.txt"
fi

# ── Summary ────────────────────────────────────────────────────────

echo -e "\n[5/5] Package complete!"
FILE_COUNT=$(find "$OUTDIR" -type f | wc -l)
TOTAL_SIZE=$(du -sh "$OUTDIR" | cut -f1)
echo ""
echo "  Location:  ${OUTDIR}/"
echo "  Files:     ${FILE_COUNT}"
echo "  Size:      ${TOTAL_SIZE}"
echo ""
echo "  Run with:  ./${OUTDIR}/dsvp.sh"
echo ""
