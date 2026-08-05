# DSVP on SteamOS (Steam Deck)

**The Steam Deck build of DSVP lives in its own repository:
[DSVP-deck](https://github.com/ASIXicle/DSVP-deck).** It is a dedicated
fork built and tuned on SteamOS — VAAPI hardware decode with zero-copy
Vulkan interop, gamescope/Game Mode integration, HDMI audio passthrough
work, and Deck-specific fixes that this repository does not carry.

This document previously described those deck-fork features as if they
were in this repo's binary; they are not. **This repo is the software-
decode-only reference build for Windows and Linux desktops.** Building
it on SteamOS gives you software decode without any of the Deck
integration — which works, but is not the Deck experience.

## What to do instead

1. Go to [DSVP-deck](https://github.com/ASIXicle/DSVP-deck).
2. Follow its `SteamOS.md` — install is a portable tarball into
   `/home/deck/`, no root, no developer mode, survives SteamOS updates.

## What this repo's build DOES offer on any Linux desktop

- Software decode only — bit-exact, no driver quirks
- The full quality pipeline: Lanczos-2 luma (anti-ringing, downscale
  dilation), Catmull-Rom chroma (siting-corrected), BT.2390 HDR→SDR
  tone mapping with dynamic scene peak, HLG, Dolby Vision P5/P8
  polynomial reshaping, temporal blue-noise dithering
- Content-aware bwdif deinterlacing
- See [README.md](README.md) for controls and
  [SETUP.md](SETUP.md) for build instructions
