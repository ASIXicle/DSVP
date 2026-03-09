#!/bin/bash
# DSVP Portable Packaging Script (Linux/macOS)
# Creates a clean DSVP-portable/ folder ready for distribution.
#
# Usage:
#   ./package.sh
#   ./package.sh --skip-build

set -e

VERSION="0.1.0"
OUTDIR="DSVP-portable"
SKIP_BUILD=0

if [ "$1" = "--skip-build" ]; then
    SKIP_BUILD=1
fi

echo "=== DSVP Packager v${VERSION} ==="

# ── Build ──────────────────────────────────────────────────────────

if [ "$SKIP_BUILD" -eq 0 ]; then
    echo -e "\n[1/4] Building..."
    make clean 2>/dev/null || true
    make
    echo "      Build OK"
else
    echo -e "\n[1/4] Skipping build"
fi

# ── Verify binary ─────────────────────────────────────────────────

if [ ! -f "build/dsvp" ]; then
    echo "ERROR: build/dsvp not found."
    exit 1
fi

# ── Create output directory ────────────────────────────────────────

echo "[2/4] Creating ${OUTDIR}/"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

# ── Copy binary ───────────────────────────────────────────────────

echo "[3/4] Copying files..."
cp build/dsvp "$OUTDIR/"

# ── Copy shared libraries ─────────────────────────────────────────

# On Linux, bundle the FFmpeg/SDL2 .so files if not using system libs
if [ "$(uname)" = "Linux" ]; then
    # Use ldd to find all linked shared libraries
    echo "      Bundling shared libraries..."
    ldd build/dsvp | grep -E "libav|libsw|libSDL2|libpostproc|libfreetype|libharfbuzz" | \
        awk '{print $3}' | while read -r lib; do
        if [ -f "$lib" ]; then
            cp "$lib" "$OUTDIR/"
            echo "      $(basename "$lib")"
        fi
    done

    # Create a launcher script that sets LD_LIBRARY_PATH
    cat > "$OUTDIR/dsvp.sh" << 'EOF'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$DIR:$LD_LIBRARY_PATH"
exec "$DIR/dsvp" "$@"
EOF
    chmod +x "$OUTDIR/dsvp.sh"
    echo "      Created launcher: dsvp.sh"
fi

if [ "$(uname)" = "Darwin" ]; then
    # On macOS, use otool to find dylibs
    echo "      Bundling dylibs..."
    otool -L build/dsvp | grep -E "libav|libsw|libSDL2|libpostproc|libfreetype|libharfbuzz" | \
        awk '{print $1}' | while read -r lib; do
        if [ -f "$lib" ]; then
            cp "$lib" "$OUTDIR/"
            echo "      $(basename "$lib")"
        fi
    done
fi

# ── Summary ────────────────────────────────────────────────────────

echo -e "\n[4/4] Package complete!"
FILE_COUNT=$(ls -1 "$OUTDIR" | wc -l)
TOTAL_SIZE=$(du -sh "$OUTDIR" | cut -f1)
echo ""
echo "  Location:  ${OUTDIR}/"
echo "  Files:     ${FILE_COUNT}"
echo "  Size:      ${TOTAL_SIZE}"
echo ""
if [ "$(uname)" = "Linux" ]; then
    echo "  Run with:  ./${OUTDIR}/dsvp.sh"
else
    echo "  Run with:  ./${OUTDIR}/dsvp"
fi
echo ""
