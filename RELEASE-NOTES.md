# DSVP 0.3.0-beta — Release Notes

This branch carries release-facing files only; development history lives elsewhere.

This round was a full-codebase review followed by a fix-and-feature pass across
every subsystem. Everything below is in the pending build.

---

## Video fidelity

- **Chroma siting corrected — for real this time.** The siting offset table held
  filter-phase values while the shader applies texture-coordinate offsets (the
  negative of each other), so the "correction" displaced chroma a full luma pixel
  the wrong way on essentially every 4:2:0 file. The table now carries the
  verified convention (cross-checked against H.273 §8.7, mpv, libplacebo, zimg,
  and swscale). Files decoded through the swscale fallback no longer receive a
  second, redundant siting offset on top of swscale's own resampling.
- **Dolby Vision MMR chroma reshaping.** Profile 5 RPUs overwhelmingly reshape
  the chroma components with MMR — a cross-channel polynomial over the full
  (I, Ct, Cp) triple — which the polynomial-only pipeline could not represent;
  those components silently fell back to identity (correct luminance, wrong
  color). MMR is now evaluated per pixel, per frame, orders 1–3, alongside the
  existing piecewise-polynomial path.
- **Dolby Vision colour accuracy corrected.** The LMS→RGB matrix used for the
  DV decode chain was the inverse of the crosstalk-carrying BT.2100 cone
  matrix, but Dolby Vision emits crosstalk-free HPE LMS — every Profile 5
  pixel carried a small systematic cross-channel error. Corrected to the
  reference constants.
- **High-bit-depth sources keep their precision.** 12-bit HEVC and 10-bit
  4:2:2 / 4:4:4 masters were converted to 8-bit before tone mapping, which
  quantised the PQ signal where it is steepest and banded shadows beyond what
  dithering can repair. They now convert to 10-bit and take the same
  full-precision GPU path as native 10-bit content.
- **Chroma siting on the conversion path is now explicit.** Both sides of the
  scaler are pinned and the shader reconstructs at the scaler's actual output
  siting, instead of assuming a re-siting that same-geometry conversions never
  perform.
- **HLG support.** ARIB STD-B67 content was detected but rendered through the
  SDR path (washed out, desaturated). HLG now goes through the inverse OETF and
  the BT.2100 OOTF at nominal 1000 nits into the shared BT.2390 tone-mapping
  path.
- **Reference output transfer.** Tone-mapped output previously encoded with the
  piecewise sRGB curve, whose linear toe no display actually decodes — a slight
  shadow lift on calibrated screens. Output now encodes pure power gamma 2.2 by
  default; `DSVP_OUTPUT_GAMMA=srgb|2.2|2.4` selects (2.4 equals BT.1886 on a
  zero-black display such as an OLED).
- **Scaling kernels dilate on downscale.** The fixed 4-tap Lanczos-2/Catmull-Rom
  kernels undersampled whenever the window was smaller than the video, causing
  aliasing and moiré. Both kernels now stretch to the output pixel's true source
  footprint (derived from screen-space derivatives, so letterboxing is exact),
  capped at 4×. At 1:1 and upscale the output is bit-identical to before.
- **SDR BT.2020 gamut conversion.** SDR content tagged BT.2020 decoded with the
  correct matrix but displayed unconverted on the 709 swapchain (visibly
  desaturated). Primaries are now converted to 709 in linear light.
- **Full-range chroma neutral fixed.** Half-an-LSB constant color cast removed
  on JPEG-range and swscale-path content (H.273 puts neutral at 2^(n−1); the
  shader math placed it half a code low).
- **Odd-dimension 4:2:0** no longer drops the last chroma column/row.

## New: content-aware deinterlacing

- Interlaced content (DVD remuxes, broadcast captures) previously played with
  comb artifacts — there was no deinterlacer at all. DSVP now builds a bwdif
  filter graph the first time a frame arrives flagged interlaced. Progressive
  files never touch the filter layer; progressive frames inside mixed streams
  pass through untouched; frame rate and A/V sync are unchanged
  (send-frame mode). `DSVP_DEINT=0` disables, `DSVP_DEINT=1` forces.
- Build dependency added: libavfilter.

## Security / hardening

- **"No networking" is now enforced, not just claimed:** file opening runs under
  an FFmpeg protocol whitelist (`file` only), so even a URL argument cannot
  touch the network through the bundled libraries.
- **Bitmap-subtitle allocation overflow guarded.** A crafted or corrupt PGS/
  VobSub rect could wrap the size arithmetic, allocate a small buffer, and write
  far past it — a heap overflow reachable from an untrusted media file. Both
  conversion sites now reject overflowing dimensions.
- 10-bit histogram scan clamps its bin index against nonconforming planes.

## Playback correctness

- **Files no longer end early.** The decoder is properly drained at end of
  file; frame-threading was withholding the final ~0.3–0.5 s of every file
  (fade-outs, closing cards were never shown).
- **Seeking is smoother.** The A/V bias model survives seeks (its reset caused a
  burst of dropped frames after every seek); a race that could flash a stale
  pre-seek frame — and on backward seeks scramble sync badly — is closed; a
  failed seek now leaves playback completely untouched instead of rewriting
  clocks toward a position it never reached.
- **No more permanent freezes.** Audio-device failure at open, audio-track-switch
  failures, mid-file read errors (dying USB drive), and persistently failing
  decoders all previously froze the picture forever with no escape; every one of
  these now either recovers, degrades to video-only, or ends playback cleanly.
- **Audio robustness.** Mid-stream format changes (broadcast 5.1↔2.0 switches,
  sample-rate changes) rebuild the resampler instead of crashing or
  pitch-shifting; unplugging the audio device mid-playback reopens or falls back
  to video-only; when a file's audio track ends before its video, playback
  continues at full rate instead of degrading to a slideshow.
- **PTS-less frames** no longer jump the clock to the start of the file.
- **Audio recovery is reliable.** If the output device fails or is removed
  mid-playback, DSVP drops to video-only cleanly and stays there; switching
  audio tracks afterwards reopens the device instead of leaving playback
  wedged, and a file that hits a read error can still be closed or navigated
  away from rather than freezing on its last frame.

## Subtitles

- Switching subtitle tracks takes effect instantly. Subtitle queues are kept as
  rolling ~35-second windows per track: pressing S has the current moment's
  packets on hand (previously either subtitles lagged the switch, or on long
  multi-track files inactive tracks accumulated unbounded memory — both are
  gone).
- PGS captions that directly replace one another no longer lose the incoming
  caption (the previous one also no longer overstays on screen).
- Multi-event subtitles display all simultaneous lines, not just the last.
- Subtitle size scales correctly at 4K (the old 54 px ceiling was a 1080p-era
  limit), the bottom margin scales with the display, and the CJK/extended-script
  fallback fonts now resize with the primary font instead of staying at their
  load-time size.
- Truncated multibyte text no longer renders replacement characters at the cut.
- PGS subtitles in MKV: a caption that directly replaces another now appears.
  (MKV muxers strip the segment that signals "display this", so the previous
  caption used to stay on screen and the incoming one was never shown.)
- Subtitle rendering is far cheaper: each caption is rasterised once instead of
  on every frame, and bitmap subtitles composite with integer arithmetic.

## Interface

- **Key auto-repeat is filtered.** Holding Q closed the file and then quit the
  app; holding Space thrashed pause state; holding B/N fired repeated file
  switches. Volume keys still repeat.
- **Fullscreen is reliable.** One code path owns all transitions, the toggle
  reads the window's actual state, and compositor-side changes (WM shortcuts)
  re-synchronize instead of inverting the toggle. Leaving fullscreen restores a
  window matched to the video's aspect.
- **UI scales by display, not by fullscreen state.** Windowed on 4K/HiDPI no
  longer renders half-size UI; the pause banner, OSD, and debug/info panels
  scale with everything else; seek-bar clicks land exactly where the bar is
  drawn.
- Double-click fullscreen no longer also triggers a seek when it lands on the
  seek bar.
- Folder navigation (B/N) works when a file is opened by bare relative name,
  no longer lists folders whose names merely end in a media extension, and no
  longer skips dot-prefixed files on Windows.
- **Folder order is now natural**: `E2` plays before `E10`, so episodes advance
  in story order in folders with unpadded numbering.
- Overlays cost far less to draw: only the rows that actually changed are sent
  to the GPU each frame, instead of the whole screen.

## Known issues

- Windows paths longer than 260 characters are not supported by the file
  dialog or folder navigation.
- Audio is downmixed to stereo; multichannel output is not yet implemented.
- HDR content is tone-mapped to SDR; HDR passthrough to an HDR display is not
  yet implemented.

## Packaging / diagnostics

- Bundled libraries updated: FFmpeg 8.1.2, SDL 3.4.12 (SDL GPU Vulkan
  stability fixes, PipeWire under-load fix), SDL3_ttf via local build.

- Version is single-sourced from the code — installer, portable packages, and
  deb metadata can no longer drift from the binary they ship.
- Builds are stamped with their git commit and print it at startup, so a log
  always identifies the exact tree that produced it.
- Incremental builds with real header dependency tracking.
- `dsvp.log` is written next to the executable instead of the working directory
  (opening a video by file association no longer drops a log file into that
  video's folder; system-installed launches no longer lose the log entirely),
  and concurrent threads can no longer interleave each other's log lines.
- Windows installer targets 64-bit Program Files with the 64-bit registry view.
- README feature claims audited against the code and corrected.
