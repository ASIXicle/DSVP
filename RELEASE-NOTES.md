# DSVP 0.3.8-beta

**ASS/SSA typesetting**
- ASS and SSA subtitle tracks now render the way their author typeset
  them: real styles and fonts, positioning, rotation, `\move` and `\t`
  animation, karaoke timing, and vector drawings. Fonts attached to the
  file are used from the file itself, so a track built around a font you
  do not have still looks the way it was meant to. 0.3.7-beta rendered
  all of this as plain text at the bottom of the screen.
- Typesetting is drawn at the time of the frame you are actually
  looking at. A sign pinned to something moving in shot travels with the
  picture instead of sliding against it.
- Subtitles stay off until you ask for them. `S` cycles off → track 1 →
  track 2 → off, and nothing is ever switched on for you because a
  container flagged it.
- `DSVP_NO_LIBASS=1` renders ASS tracks as plain text in the house
  style, as before.

**Lip sync across the HDR display switch**
- Pressing `Z` to switch the display into HDR mode no longer pushes
  audio and video apart. The player used to mistake its own display
  switch for a change in the audio path's latency and then correct for a
  lag that was not there — sync stayed off until the next seek. That
  stall is now kept out of the measurement, and the frame clock is
  re-aligned once the player has caught back up.

**Resume**
- A file watched through to the end no longer comes back as something to
  resume.
- A portable copy now keeps its own history. It used to offer the last
  file an installed copy had watched, and clearing it deleted that
  installed copy's record — a portable stays out of the machine's way,
  which is the point of a portable.
- The Windows installer no longer bundles a stray resume record from the
  machine that built it, and uninstalling removes the one in the install
  folder.

**Instruments**
- The debug panel and the log no longer disagree about the tone-map
  numbers, and each figure says where it came from.
- A seek that fails says so, and says the position did not change.

**Known limitations**: stereo audio output (compressed passthrough is
detected but not yet sent to the receiver); ASS/SSA typesetting needs
libass present when the player is built — without it ASS tracks fall
back to plain text at the bottom; Linux HDR display control is KDE
Plasma on Wayland only; Windows paths longer than 260 characters. HDR
output is beta on both platforms.
