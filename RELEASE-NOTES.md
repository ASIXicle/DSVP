# DSVP 0.3.9-beta

**Pause, seek and step**
- A seek while paused now shows the frame it lands on and stays paused.
  It used to leave the old picture up until you pressed Space.
- `,` and `.` step one frame back and forward. Stepping pauses playback,
  and holding the key keeps stepping. Forward is instant. Back lands on
  the exact previous frame, which means decoding from the keyframe
  before it, so it is slower on heavy 4K files. Playback resumes in sync
  wherever you stepped to.
- Several quick presses of an arrow key move by that many steps. A
  press made while the previous seek was still landing used to ask for
  the same place again, and a click on the seek bar in that moment
  landed off by the pending seek's distance.
- Pressing Space in the instant a seek starts no longer plays a short
  burst of audio from the old position.

**Folders**
- At the end of a file the next one in the folder plays, and after the
  last file the folder starts again from the first. `DSVP_NO_AUTOPLAY=1`
  stops at the end instead.
- An audio or subtitle track you pick with `A` or `S` carries to the
  next file in the folder, matched by language and title. `R` on the
  idle screen resumes with the same tracks, and the choice is remembered
  across restarts.
  Subtitles are still off until you choose one, and turning them Off
  keeps them off. `DSVP_NO_KEEP_TRACKS=1` uses each file's own
  defaults.

**Help and readability**
- `K` shows every key binding, during playback and on the idle screen.
  The top bar lists it.
- The on-screen menus and bars follow the system display scale, so a
  high-resolution desktop running at a scale above 100% gets menus to
  match. Displays at 100% look the same as before.

**HDR**
- HDR10 files whose brightness metadata lives only in the video stream
  (common in encodes) are now tone-mapped against their real mastering
  peak instead of a 1000-nit guess, so bright highlights roll off
  instead of clipping.
- The black lift that keeps dark detail visible on LCD panels is now
  decided by what the display itself reports as its black level, so an
  OLED-class panel gets no lift. Windows only reports it while the
  desktop is in HDR, so the player learns it the first time the display
  switches to HDR.

**Instruments**
- When two seeks overlap, the log and the seek-recovery check now judge
  each frame against the seek it came from. Paused, the picture from
  the earlier seek no longer flashes up before the right one.
- The log records window moves and resizes, focus changes, minimize and
  restore, display changes and the display's HDR state with the
  playback clock, so a stall can be matched to what the desktop was
  doing. On Windows the HDR state comes from the OS itself.
- The Playback Summary's peak A/V figure is the whole run's, from
  settled playback only; it used to reset at every seek.
- Log labels say what they mean: the subtitle canvas a bitmap track
  will be drawn on, the swapchain format by name, and the colour matrix
  line on a Dolby Vision profile 5 file notes that the profile's own
  transform decides it.

**Known limitations**: stereo audio output (compressed passthrough is
detected but not yet sent to the receiver); ASS/SSA typesetting needs
libass present when the player is built — without it ASS tracks fall
back to plain text at the bottom; Linux HDR display control is KDE
Plasma on Wayland only; Windows paths longer than 260 characters. HDR
output is beta on both platforms.
