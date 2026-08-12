# DSVP 0.3.2-beta

A stability and correctness release: the entire codebase went through a
structured multi-perspective review, and every finding was fixed and
field-tested on Windows 11 and Debian 13 before this release.

**Reliability**
- Seeking is now bulletproof under abuse: rapid consecutive seeks are never
  silently dropped, a race that could resync the clocks to the pre-seek
  position (long silence after backward seeks) is closed, and
  pause/fullscreen/dialog actions during seek recovery no longer cause
  audio to run ahead and snap.
- Fixed on-screen display corruption (garbled seek bar / overlay flashes)
  after window resizes and fullscreen toggles.
- Graphics-format changes mid-stream (seen in some broadcast recordings)
  are now handled safely instead of risking a crash.
- PGS (Blu-ray) subtitles no longer render twice or disappear early on
  well-formed streams.

**Color accuracy**
- RGB sources (screen recordings, RGB-coded video) previously encoded and
  decoded with mismatched color matrices — fixed.
- BT.2020/HDR content with stripped metadata now reconstructs chroma at the
  spec-correct top-left siting (fixes a quarter-texel vertical chroma shift
  on most HDR files), detected via HDR/Dolby Vision signals even when tags
  are missing.
- Multi-piece Dolby Vision MMR curves now apply the correct reshaping piece
  per pixel.

**Under the hood**
- FFmpeg 9.0 and SDL 3.4.14 on both platforms.
- Packaging integrity: the license text ships in all bundles, the Windows
  uninstaller only removes DSVP's own files, the Debian package version is
  upgrade-safe, and the packager refuses to produce incomplete bundles.
- New crisp application icon at all sizes.

**Known limitations** (unchanged): stereo audio output; no HDR passthrough
on x64 (tone-mapped output only); Windows paths longer than 260 characters.
