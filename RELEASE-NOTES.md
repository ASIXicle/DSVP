# DSVP 0.3.7-beta

**HDR output — live, beta, your mileage may vary**
- HDR10, HLG and Dolby Vision content is tone-mapped for SDR displays
  by default, with the tone map now running in the PQ domain the
  standard specifies. The previous curve rendered HDR dark and flat;
  this one was qualified against a reference harness to within a few
  code values. Black level is lifted a hair for IPS panels (0.01 nits)
  and Dolby Vision titles use their authored per-scene peak.
- On an HDR display, press `Z` for passthrough: DSVP switches the
  display into HDR mode itself (Windows, and KDE Plasma on Wayland),
  presents the untouched PQ signal, and puts every display setting it
  changed back exactly as it found it on close or exit. If the player
  dies mid-film, the next launch finishes the restore. Subtitles and
  menus keep a fixed graphics white instead of riding the video's
  brightness. `DSVP_HDR_PASS=1` makes passthrough the default;
  `DSVP_NO_SYS_HDR=1` keeps DSVP's hands off the display entirely.
- Files whose container carries no colour tags are no longer treated
  as SDR: the colour information the encoder wrote into the video
  stream is used when the container is silent. Some remuxes and older
  muxers produce such files, and they played washed out before.

**Resume Playback added**
- `R` on the idle screen resumes the last file where you left off. One
  three-line record next to the executable, or in your per-user data
  folder when that is not writable; `DSVP_NO_RESUME=1` and nothing is
  ever written.
- Chapters: `PgUp` / `PgDn` skip between MKV and MP4 chapters, the name
  shows on screen, the media info panel lists them, and the seek bar
  carries a tick per chapter.

**Subtitles**
- Overlapping text cues now stack on screen with their own timings.
  Before, when two cues overlapped, the second was dropped or shown
  late.
- Blu-ray (PGS) captions on MKV remuxes no longer vanish or stick:
  display sets are shown only when due and replaced by the next one
  on time, and cycling onto a subtitle track mid-film no longer
  flickers through past captions.
- Long bitmap captions are no longer cut at 30 seconds; the ASS hard
  space renders as a space; cues without timestamps show immediately.

**Seeking**
- Seek recovery no longer anchors on a stale frame: frames left in the
  decoder from before the seek are drained, a backward seek that lands
  far ahead of its target is refused and retried, and chapter and
  resume seeks land at or before the spot instead of a keyframe past it.

**Reliability**
- Linux: the file-open dialog no longer blocks the player (the old one
  could get the window killed by the compositor while it waited);
  Ctrl-C, logout and hangups shut down cleanly with the display revert
  running; every external call is bounded.
- Windows: non-ASCII user names and paths work for the log and the
  resume record; the crash-restore stamp lives under your local app
  data.
- The log's first line says where the log went.

**Packaging and provenance**
- Every bundle carries the build it was made from, and the packagers
  refuse to ship a stale, modified or debug build on either platform.
- Linux: the portable bundle resolves its libraries from its own
  folder with no environment variables, the shipped binary no longer
  carries the build machine's home directory, and the `.deb` declares
  the dependencies its binaries actually need.
- Windows: the DLL resolver searches the linked prefixes first and
  fails loudly on anything unresolved; the installer version comes
  from the source.

**Known limitations**: stereo audio output (compressed passthrough is
detected but not yet sent to the receiver); ASS/SSA typesetting —
positioned signs and styled effects — renders as plain text at the
bottom, a proper renderer is the next cycle's headline; Linux HDR
display control is KDE Plasma on Wayland only; Windows paths longer
than 260 characters. HDR output is beta on both platforms.
