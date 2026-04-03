#!/bin/bash
# ═══════════════════════════════════════════════════════════════════
# DSVP — Debian Package Builder
# ═══════════════════════════════════════════════════════════════════
#
# Builds DSVP, creates portable package, then wraps it into a .deb.
# Single command:  ./installer/package-deb.sh
#
# Options:
#   --skip-build    Skip compilation, use existing DSVP-portable/
#
# Output: dsvp_0.2.0-beta_amd64.deb in repo root
#
# Install:  sudo dpkg -i dsvp_0.2.0-beta_amd64.deb
# Remove:   sudo dpkg -r dsvp

set -e

VERSION="0.2.0-beta"
ARCH="amd64"
PKG_NAME="dsvp"
PKG_DIR="${PKG_NAME}_${VERSION}_${ARCH}"
PORTABLE_DIR="DSVP-portable"
SKIP_BUILD=0

if [ "$1" = "--skip-build" ]; then
    SKIP_BUILD=1
fi

echo "=== DSVP Debian Package Builder v${VERSION} ==="

# ── Ensure we're in repo root ─────────────────────────────────

if [ ! -f "src/dsvp.h" ]; then
    if [ -f "../src/dsvp.h" ]; then
        cd ..
    else
        echo "ERROR: Run this script from the DSVP repo root."
        exit 1
    fi
fi

# ── Step 1: Build and package ─────────────────────────────────

if [ "$SKIP_BUILD" -eq 0 ]; then
    echo ""
    echo "[1/3] Building portable package..."
    ./package.sh
else
    echo ""
    echo "[1/3] Skipping build (using existing ${PORTABLE_DIR}/)"
fi

# ── Verify portable build exists ──────────────────────────────

if [ ! -f "${PORTABLE_DIR}/dsvp" ]; then
    echo "ERROR: ${PORTABLE_DIR}/dsvp not found. Build failed or was skipped."
    exit 1
fi

if [ ! -d "${PORTABLE_DIR}/lib" ]; then
    echo "ERROR: ${PORTABLE_DIR}/lib/ not found. Build may have failed."
    exit 1
fi

# ── Clean and create package tree ─────────────────────────────

echo "[2/3] Assembling .deb package tree..."
rm -rf "$PKG_DIR"
mkdir -p "${PKG_DIR}/DEBIAN"
mkdir -p "${PKG_DIR}/usr/lib/${PKG_NAME}"
mkdir -p "${PKG_DIR}/usr/bin"
mkdir -p "${PKG_DIR}/usr/share/applications"

# ── Copy binary and libraries ─────────────────────────────────

echo "      Copying binary and libraries..."
cp "${PORTABLE_DIR}/dsvp" "${PKG_DIR}/usr/lib/${PKG_NAME}/dsvp"
chmod 755 "${PKG_DIR}/usr/lib/${PKG_NAME}/dsvp"

# Copy all bundled shared libraries
cp -a "${PORTABLE_DIR}/lib/"* "${PKG_DIR}/usr/lib/${PKG_NAME}/"

LIB_COUNT=$(find "${PKG_DIR}/usr/lib/${PKG_NAME}" -name "*.so*" | wc -l)
echo "      Copied binary + ${LIB_COUNT} libraries"

# ── Create launcher script ────────────────────────────────────

echo "      Creating launcher..."
cat > "${PKG_DIR}/usr/bin/${PKG_NAME}" << 'LAUNCHER'
#!/bin/bash
# DSVP launcher — sets library path for bundled shared libs
export LD_LIBRARY_PATH="/usr/lib/dsvp:$LD_LIBRARY_PATH"
exec /usr/lib/dsvp/dsvp "$@"
LAUNCHER
chmod 755 "${PKG_DIR}/usr/bin/${PKG_NAME}"

# ── Create .desktop file ──────────────────────────────────────

echo "      Creating desktop entry..."
cat > "${PKG_DIR}/usr/share/applications/${PKG_NAME}.desktop" << DESKTOP
[Desktop Entry]
Type=Application
Name=DSVP
GenericName=Video Player
Comment=Dead Simple Video Player — reference-quality playback
Exec=dsvp %f
Terminal=false
Categories=AudioVideo;Video;Player;
MimeType=video/x-matroska;video/mp4;video/x-msvideo;video/quicktime;video/webm;video/x-ms-wmv;video/x-flv;video/mpeg;video/mp2t;video/ogg;video/3gpp;
Keywords=video;player;media;mkv;mp4;hevc;hdr;dolby;
DESKTOP

# ── Create DEBIAN/control ─────────────────────────────────────

# Calculate installed size in KB
INSTALLED_SIZE=$(du -sk "${PKG_DIR}" | cut -f1)

cat > "${PKG_DIR}/DEBIAN/control" << CONTROL
Package: ${PKG_NAME}
Version: ${VERSION}
Section: video
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${INSTALLED_SIZE}
Depends: libc6 (>= 2.17), zlib1g, fonts-dejavu-core
Recommends: fonts-noto-cjk
Maintainer: Holden <holden@dsvp>
Homepage: https://github.com/ASIXicle/DSVP
Description: Dead Simple Video Player — reference-quality playback
 DSVP is a video player focused on reference-quality image fidelity.
 Features Lanczos-2 luma scaling, Catmull-Rom chroma upsampling,
 temporal blue noise dithering, HDR-to-SDR tone mapping (BT.2390),
 Dolby Vision Profile 5/8, 10-bit passthrough, and software decode
 for bit-exact output. Bundles FFmpeg 8.1, SDL3, and all dependencies.
CONTROL

# ── Create DEBIAN/postinst (update desktop database) ──────────

cat > "${PKG_DIR}/DEBIAN/postinst" << 'POSTINST'
#!/bin/bash
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications 2>/dev/null || true
fi
POSTINST
chmod 755 "${PKG_DIR}/DEBIAN/postinst"

# ── Create DEBIAN/postrm (cleanup on removal) ────────────────

cat > "${PKG_DIR}/DEBIAN/postrm" << 'POSTRM'
#!/bin/bash
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications 2>/dev/null || true
fi
POSTRM
chmod 755 "${PKG_DIR}/DEBIAN/postrm"

# ── Build .deb ────────────────────────────────────────────────

echo "[3/3] Building .deb..."
DEB_FILE="${PKG_NAME}_${VERSION}_${ARCH}.deb"
dpkg-deb --build "$PKG_DIR" "$DEB_FILE"

# ── Cleanup and summary ──────────────────────────────────────

rm -rf "$PKG_DIR"

DEB_SIZE=$(du -sh "$DEB_FILE" | cut -f1)
echo ""
echo "  Package:  ${DEB_FILE}"
echo "  Size:     ${DEB_SIZE}"
echo ""
echo "  Install:  sudo dpkg -i ${DEB_FILE}"
echo "  Remove:   sudo dpkg -r ${PKG_NAME}"
echo ""
