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
#   --allow-dirty   Package an unknown/+dirty/debug stamp (test builds only)
#
# Needs: dpkg-deb, objdump (binutils), patchelf (via package.sh); lintian optional.
#
# Output: dsvp_<version>_amd64.deb in repo root
#
# Install:  sudo dpkg -i dsvp_<version>_amd64.deb
# Remove:   sudo dpkg -r dsvp

set -e

# Version derives from src/dsvp.h — single source of truth, never hardcode here
VERSION=$(sed -n 's/.*DSVP_VERSION *"\(.*\)"/\1/p' src/dsvp.h)
if [ -z "$VERSION" ]; then
    echo "ERROR: could not extract DSVP_VERSION from src/dsvp.h"
    exit 1
fi
# Debian version: '-' would be parsed as a Debian revision, making
# "0.3.0-beta" sort NEWER than the final "0.3.0" and blocking the
# upgrade path. '~' sorts BEFORE — the Debian pre-release convention.
DEB_VERSION=$(echo "$VERSION" | sed 's/-/~/g')
ARCH="amd64"
PKG_NAME="dsvp"
PKG_DIR="${PKG_NAME}_${DEB_VERSION}_${ARCH}"
PORTABLE_DIR="DSVP-portable"
SKIP_BUILD=0
ALLOW_DIRTY=0

for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=1 ;;
        --allow-dirty) ALLOW_DIRTY=1 ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

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
    if [ "$ALLOW_DIRTY" -eq 1 ]; then ./package.sh --allow-dirty; else ./package.sh; fi
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

# Provenance gate on the portable's stamp (review M9): a .deb is a
# public artifact — never from an unknown, dirty or debug binary.
STAMP=$(cat "${PORTABLE_DIR}/dsvp.stamp" 2>/dev/null || echo "missing")
echo "      Binary stamp: ${STAMP}"
case "$STAMP" in
    *release) ;;
    *) if [ "$ALLOW_DIRTY" -eq 1 ]; then echo "      WARNING: stamp '${STAMP}' is not a clean release build — packaging anyway (--allow-dirty)";
       else echo "ERROR: refusing to build a .deb from stamp '${STAMP}' (pass --allow-dirty for a test build)"; exit 1; fi ;;
esac
case "$STAMP" in
    unknown*|missing|*+dirty*) if [ "$ALLOW_DIRTY" -ne 1 ]; then echo "ERROR: refusing to build a .deb from stamp '${STAMP}'"; exit 1; fi ;;
esac

# ── Clean and create package tree ─────────────────────────────

echo "[2/3] Assembling .deb package tree..."
rm -rf "$PKG_DIR"
mkdir -p "${PKG_DIR}/DEBIAN"
mkdir -p "${PKG_DIR}/usr/lib/${PKG_NAME}/lib"
mkdir -p "${PKG_DIR}/usr/bin"
mkdir -p "${PKG_DIR}/usr/share/applications"
mkdir -p "${PKG_DIR}/usr/share/metainfo"
mkdir -p "${PKG_DIR}/usr/share/icons/hicolor/128x128/apps"
mkdir -p "${PKG_DIR}/usr/share/doc/${PKG_NAME}"

# ── Copy binary and libraries ─────────────────────────────────

echo "      Copying binary and libraries..."
cp "${PORTABLE_DIR}/dsvp" "${PKG_DIR}/usr/lib/${PKG_NAME}/dsvp"
chmod 755 "${PKG_DIR}/usr/lib/${PKG_NAME}/dsvp"

# Bundled shared libraries go under lib/ — the binary's RUNPATH is
# $ORIGIN/lib:$ORIGIN, so /usr/lib/dsvp/lib resolves with no environment.
cp -a "${PORTABLE_DIR}/lib/"* "${PKG_DIR}/usr/lib/${PKG_NAME}/lib/"

LIB_COUNT=$(find "${PKG_DIR}/usr/lib/${PKG_NAME}/lib" -name "*.so*" -type f | wc -l)
echo "      Copied binary + ${LIB_COUNT} libraries"

# Debian hygiene (lintian E-class, 2026-09-06): shared libraries are
# not executable, and the package ships stripped objects — the portable
# bundle keeps its symbols, the .deb does not need them.
find "${PKG_DIR}/usr/lib/${PKG_NAME}/lib" -type f -name '*.so*' -exec chmod 644 {} \;
if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "${PKG_DIR}/usr/lib/${PKG_NAME}/dsvp" 2>/dev/null || true
    find "${PKG_DIR}/usr/lib/${PKG_NAME}/lib" -type f -name '*.so*' -exec strip --strip-unneeded {} \; 2>/dev/null || true
    echo "      Stripped binary + libraries"
fi

# ── Create launcher script ────────────────────────────────────

echo "      Creating launcher..."
cat > "${PKG_DIR}/usr/bin/${PKG_NAME}" << 'LAUNCHER'
#!/bin/bash
# DSVP launcher. No LD_LIBRARY_PATH (review M11): the binary's RUNPATH
# finds /usr/lib/dsvp/lib, and an exported path with an empty element
# made the loader search the current directory for system libraries
# and leaked the bundle's libs into the file-dialog child.
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
Icon=dsvp
Terminal=false
Categories=AudioVideo;Video;Player;
MimeType=video/x-matroska;video/mp4;video/x-msvideo;video/quicktime;video/webm;video/x-ms-wmv;video/x-flv;video/mpeg;video/mp2t;video/ogg;video/3gpp;
Keywords=video;player;media;mkv;mp4;hevc;hdr;dolby;
DESKTOP

# ── Install application icon ─────────────────────────────────

echo "      Installing icon..."
if [ -f "src/dsvp.png" ]; then
    cp "src/dsvp.png" "${PKG_DIR}/usr/share/icons/hicolor/128x128/apps/dsvp.png"
elif [ -f "src/dsvp.ico" ] && command -v convert >/dev/null 2>&1; then
    # Extract largest frame from .ico and resize to 128x128
    convert "src/dsvp.ico[0]" -resize 128x128 \
        "${PKG_DIR}/usr/share/icons/hicolor/128x128/apps/dsvp.png"
    echo "      Converted dsvp.ico → 128x128 PNG"
else
    echo "      WARNING: No icon installed (provide src/dsvp.png or install imagemagick)"
fi

# ── Create copyright file (shown by Discover/dpkg) ──────────

cat > "${PKG_DIR}/usr/share/doc/${PKG_NAME}/copyright" << 'COPYRIGHT'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: DSVP
Upstream-Contact: https://github.com/ASIXicle/DSVP
Source: https://github.com/ASIXicle/DSVP

Files: *
Copyright: 2025-2026 Holden
License: GPL-3.0+

License: GPL-3.0+
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 .
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 .
 On Debian systems, the full text of the GNU General Public
 License version 3 can be found in /usr/share/common-licenses/GPL-3.
COPYRIGHT

# ── Debian changelog (lintian: no-changelog) ─────────────────
# One entry per build; the release notes live in GitHub Releases.
printf '%s (%s) unstable; urgency=medium\n\n  * Build of DSVP %s (%s).\n\n -- Holden <asixicle@users.noreply.github.com>  %s\n' \
    "$PKG_NAME" "$DEB_VERSION" "$VERSION" "${STAMP%% *}" "$(date -R)" \
    | gzip -9n > "${PKG_DIR}/usr/share/doc/${PKG_NAME}/changelog.gz"

# ── Create AppStream metainfo (Discover/GNOME Software) ─────
# This is the file that software centers actually read for the
# human-readable name, license, author, URL, and description.

echo "      Creating AppStream metainfo..."
cat > "${PKG_DIR}/usr/share/metainfo/${PKG_NAME}.metainfo.xml" << METAINFO
<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>dsvp.desktop</id>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>GPL-3.0-or-later</project_license>

  <name>Dead Simple Video Player</name>
  <summary>Reference-quality video playback with HDR and Dolby Vision support</summary>

  <developer id="com.github.asixicle">
    <name>ASIXicle</name>
  </developer>

  <description>
    <p>
      DSVP is a video player focused on reference-quality image fidelity.
      It uses Lanczos-2 luma scaling with anti-ringing, Catmull-Rom chroma
      upsampling with sub-texel siting correction, and temporal blue noise
      dithering — all in a single GPU shader pass.
    </p>
    <p>
      Features include HDR-to-SDR tone mapping (BT.2390 with dynamic peak
      detection), Dolby Vision Profile 5 and 8 support, 10-bit passthrough
      without truncation, and software decode for bit-exact output. Plays
      everything FFmpeg supports: H.264, HEVC, AV1, VP9, MKV, MP4, and
      hundreds more formats.
    </p>
  </description>

  <url type="homepage">https://github.com/ASIXicle/DSVP</url>
  <url type="bugtracker">https://github.com/ASIXicle/DSVP/issues</url>

  <launchable type="desktop-id">dsvp.desktop</launchable>

  <provides>
    <binary>dsvp</binary>
    <mediatype>video/x-matroska</mediatype>
    <mediatype>video/mp4</mediatype>
    <mediatype>video/x-msvideo</mediatype>
    <mediatype>video/quicktime</mediatype>
    <mediatype>video/webm</mediatype>
    <mediatype>video/mpeg</mediatype>
    <mediatype>video/mp2t</mediatype>
    <mediatype>video/ogg</mediatype>
  </provides>

  <content_rating type="oars-1.1" />

  <releases>
    <!-- Current release carries no prose: pairing hardcoded notes
         with ${VERSION} + build date mis-attributed old notes to every
         new version. Notes live in GitHub Releases. -->
    <release version="${VERSION}" date="$(date +%Y-%m-%d)"/>
    <release version="0.2.8-beta" date="2026-05-25">
      <description>
        <p>Subtitle: extended font fallback chain covers Arabic, Hebrew,
        Indic scripts (Devanagari, Bengali, Tamil, Telugu, Kannada,
        Malayalam, Gujarati, Gurmukhi, Oriya, Sinhala), SE Asian (Thai,
        Lao, Khmer, Myanmar), Georgian, Armenian, Ethiopic, and Tibetan.
        Previously these rendered as missing-glyph boxes on most platforms.</p>
      </description>
    </release>
    <release version="0.2.7-beta" date="2026-05-25">
      <description>
        <p>Subtitle: skip stale packets before decode when cycling streams,
        eliminating cascade drops triggered by catch-up bitmap subtitle work
        on the main thread.</p>
      </description>
    </release>
    <release version="0.2.6-beta" date="2026-04-03">
      <description>
        <p>Windows and Debian installers. Seek stall fix, stream discard,
        audio defer, MPEG-PS startup drop fix, EOF snap-forward fix.</p>
      </description>
    </release>
  </releases>
</component>
METAINFO

# ── Create DEBIAN/control ─────────────────────────────────────

# Calculate installed size in KB
INSTALLED_SIZE=$(du -sk "${PKG_DIR}" | cut -f1)

# Depends computed from the binaries, not hand-written (review M13):
# the glibc floor is the highest GLIBC_x.y symbol version the binary
# and the bundled libraries reference; libstdc++6/libgcc-s1 are needed
# by the C++ shadercross/dxc/spirv-cross libs (deliberately not
# bundled). SDL dlopen()s its video/audio backends, which never appear
# in ldd — Recommends, so a headless install stays installable.
GLIBC_FLOOR=$( (objdump -T "${PKG_DIR}/usr/lib/${PKG_NAME}/dsvp"; \
                find "${PKG_DIR}/usr/lib/${PKG_NAME}/lib" -type f -name '*.so*' -exec objdump -T {} \; ) 2>/dev/null \
              | sed -n 's/.*GLIBC_\([0-9][0-9.]*\).*/\1/p' | sort -t. -k1,1n -k2,2n -u | tail -1)
[ -n "$GLIBC_FLOOR" ] || GLIBC_FLOOR="2.36"
NEEDS_CXX=""
if find "${PKG_DIR}/usr/lib/${PKG_NAME}/lib" -type f -name '*.so*' -exec objdump -p {} \; 2>/dev/null | grep -q 'NEEDED.*libstdc++'; then
    NEEDS_CXX=", libstdc++6, libgcc-s1"
fi
# The host-owned display stack (package.sh leaves it out of the bundle
# by policy) is a hard dependency: it is DT_NEEDED by SDL/FFmpeg.
# libfontconfig1: host-owned by package.sh (libass's font provider reads
# the host's /etc/fonts and caches through it).
DEPENDS="libc6 (>= ${GLIBC_FLOOR})${NEEDS_CXX}, libx11-6, libx11-xcb1, libxcb1, libxcb-dri3-0, libxext6, libxfixes3, libxau6, libxdmcp6, libdrm2, libfontconfig1, fonts-dejavu-core"
echo "      Depends: ${DEPENDS}  [glibc floor from the binary + bundled libs]"

cat > "${PKG_DIR}/DEBIAN/control" << CONTROL
Package: ${PKG_NAME}
Version: ${DEB_VERSION}
Section: video
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${INSTALLED_SIZE}
Depends: ${DEPENDS}
Recommends: fonts-noto-cjk, libwayland-client0, libxkbcommon0, libvulkan1, libgl1, libgbm1, libpipewire-0.3-0 | libpulse0 | libasound2
Maintainer: Holden <asixicle@users.noreply.github.com>
Homepage: https://github.com/ASIXicle/DSVP
Description: Dead Simple Video Player — reference-quality playback
 DSVP is a video player focused on reference-quality image fidelity.
 Features Lanczos-2 luma scaling, Catmull-Rom chroma upsampling,
 temporal blue noise dithering, HDR-to-SDR tone mapping (BT.2390),
 Dolby Vision Profile 5/8, 10-bit passthrough, and software decode
 for bit-exact output. Bundles FFmpeg, SDL3, and all dependencies.
CONTROL

# ── Create DEBIAN/postinst (update desktop database) ──────────

cat > "${PKG_DIR}/DEBIAN/postinst" << 'POSTINST'
#!/bin/bash
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications 2>/dev/null || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q /usr/share/icons/hicolor 2>/dev/null || true
fi
POSTINST
chmod 755 "${PKG_DIR}/DEBIAN/postinst"

# ── Create DEBIAN/postrm (cleanup on removal) ────────────────

cat > "${PKG_DIR}/DEBIAN/postrm" << 'POSTRM'
#!/bin/bash
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications 2>/dev/null || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q /usr/share/icons/hicolor 2>/dev/null || true
fi
POSTRM
chmod 755 "${PKG_DIR}/DEBIAN/postrm"

# ── Build .deb ────────────────────────────────────────────────

echo "[3/3] Building .deb..."
DEB_FILE="${PKG_NAME}_${DEB_VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$PKG_DIR" "$DEB_FILE"

# ── Lint (informational; lintian is optional) ────────────────
if command -v lintian >/dev/null 2>&1; then
    echo "      lintian:"
    lintian --tag-display-limit 0 "$DEB_FILE" 2>&1 | sed 's/^/        /' || true
fi

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
