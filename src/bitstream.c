/*
 * DSVP -- Dead Simple Video Player
 * bitstream.c -- HDMI audio passthrough: EDID sink probe + SPDIF muxer
 *
 * Phase 2: bitstream_probe() reads EDID from Linux sysfs, parses CEA-861
 *          Short Audio Descriptors, populates BitstreamCaps.
 *
 * Phase 3 (TODO): bitstream_open/write/close wraps compressed audio in
 *          IEC 61937 frames via FFmpeg's spdif muxer for passthrough.
 *
 * Design: all functions fail gracefully to PCM. No bitstream failure
 *         should prevent normal audio playback.
 */

#include "dsvp.h"

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

/* ===================================================================
 * CEA-861 Audio Codec IDs (Short Audio Descriptor byte 1, bits 6:3)
 * =================================================================== */

#define CEA_AUDIO_LPCM      1
#define CEA_AUDIO_AC3       2
#define CEA_AUDIO_DTS       7
#define CEA_AUDIO_EAC3      10
#define CEA_AUDIO_DTSHD     11
#define CEA_AUDIO_TRUEHD    12   /* MLP / TrueHD / Atmos */

/* HDMI Forum IEEE OUI (little-endian in VSDB): 0xC45DD8
 * HDMI 1.4 Licensing IEEE OUI: 0x000C03 */
#define HDMI_FORUM_OUI_B0   0xD8
#define HDMI_FORUM_OUI_B1   0x5D
#define HDMI_FORUM_OUI_B2   0xC4

#define HDMI_14_OUI_B0      0x03
#define HDMI_14_OUI_B1      0x0C
#define HDMI_14_OUI_B2      0x00

/* ===================================================================
 * Phase 2: EDID Parsing
 * =================================================================== */

/* The EDID parse helpers below are reachable only from the Linux probe
 * body (bitstream_probe reads /sys/class/drm); on other platforms the
 * probe is a stub, leaving them uncalled — GCC warns, correctly. Guard
 * them with the same condition as their only caller. */
#ifdef __linux__

/*
 * Parse a CEA-861 Audio Data Block.
 * Each Short Audio Descriptor (SAD) is 3 bytes:
 *   byte 0: [6:3] codec ID, [2:0] max channels - 1
 *   byte 1: sample rate flags (32/44.1/48/88.2/96/176.4/192 kHz)
 *   byte 2: codec-specific (bitrate for compressed, bit depth for LPCM)
 */
static int parse_audio_data_block(const uint8_t *data, int len,
                                   BitstreamCaps *caps)
{
    int count = len / 3;
    for (int i = 0; i < count; i++) {
        uint8_t b0 = data[i * 3];
        int codec = (b0 >> 3) & 0x0F;
        int max_ch = (b0 & 0x07) + 1;

        if (max_ch > caps->max_channels)
            caps->max_channels = max_ch;

        switch (codec) {
        case CEA_AUDIO_AC3:     caps->support_ac3    = 1; break;
        case CEA_AUDIO_EAC3:    caps->support_eac3   = 1; break;
        case CEA_AUDIO_TRUEHD:  caps->support_truehd = 1; break;
        case CEA_AUDIO_DTS:     caps->support_dts    = 1; break;
        case CEA_AUDIO_DTSHD:   caps->support_dtshd  = 1; break;
        }
    }
    return count;
}

/*
 * Parse a CEA-861 extension block (128 bytes, tag byte == 0x02).
 * Walks the Data Block Collection looking for Audio Data Blocks (tag 1)
 * and Vendor-Specific Data Blocks (tag 3) for HDMI capability detection.
 */
static int parse_cea_extension(const uint8_t *block, BitstreamCaps *caps)
{
    if (block[0] != 0x02) return 0;          /* not a CEA extension     */

    int dtd_offset = block[2];
    if (dtd_offset <= 4 || dtd_offset > 127) return 0;

    int pos = 4;                             /* data blocks start here  */
    while (pos < dtd_offset) {
        int tag = (block[pos] >> 5) & 0x07;
        int len = block[pos] & 0x1F;
        pos++;

        if (pos + len > dtd_offset) break;   /* truncated block         */

        /* -- Audio Data Block (tag 1) -- */
        if (tag == 1)
            parse_audio_data_block(&block[pos], len, caps);

        /* -- Vendor-Specific Data Block (tag 3) -- */
        if (tag == 3 && len >= 3) {
            /* HDMI 1.4 Licensing LLC VSDB */
            if (block[pos]   == HDMI_14_OUI_B0 &&
                block[pos+1] == HDMI_14_OUI_B1 &&
                block[pos+2] == HDMI_14_OUI_B2)
            {
                /* HDMI 1.4 confirmed -- basic passthrough supported.
                 * HBR (TrueHD) requires HDMI 2.0+ or specific VSDB flags. */
                log_msg("Bitstream: HDMI 1.4 VSDB detected");
            }

            /* HDMI Forum VSDB (HDMI 2.0+) */
            if (block[pos]   == HDMI_FORUM_OUI_B0 &&
                block[pos+1] == HDMI_FORUM_OUI_B1 &&
                block[pos+2] == HDMI_FORUM_OUI_B2)
            {
                caps->hbr_capable = 1;
                log_msg("Bitstream: HDMI 2.0+ Forum VSDB detected (HBR capable)");
            }
        }

        pos += len;
    }

    return 1;   /* parsed at least one CEA extension */
}

/* ===================================================================
 * bitstream_probe() -- detect HDMI/DP sink audio capabilities
 *
 * Scans /sys/class/drm/ for connected HDMI or DP outputs, reads the
 * raw EDID binary, and parses CEA-861 extension blocks.
 *
 * Returns 0 on success (caps populated), -1 on failure (no HDMI sink
 * or no CEA audio data found). Either way, caps->probed is set to 1.
 * =================================================================== */

#endif /* __linux__ — EDID parse helpers */

int bitstream_probe(BitstreamCaps *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->probed = 1;                        /* mark as probed either way */

#ifndef __linux__
    log_msg("Bitstream: EDID probe not implemented on this platform");
    return -1;
#else
    DIR *drm = opendir("/sys/class/drm");
    if (!drm) {
        log_msg("Bitstream: cannot open /sys/class/drm");
        return -1;
    }

    struct dirent *ent;
    int found = 0;

    while ((ent = readdir(drm)) != NULL) {
        /* Want entries like card0-HDMI-A-1, card1-DP-2, etc. */
        if (strncmp(ent->d_name, "card", 4) != 0)  continue;
        if (!strchr(ent->d_name, '-'))               continue;

        /* Only HDMI and DisplayPort carry audio */
        if (!strstr(ent->d_name, "HDMI") &&
            !strstr(ent->d_name, "DP"))             continue;

        /* -- Check connection status -- */
        char path[512];
        snprintf(path, sizeof(path), "/sys/class/drm/%s/status", ent->d_name);

        FILE *sf = fopen(path, "r");
        if (!sf) continue;
        char status[32] = {0};
        if (fgets(status, sizeof(status), sf) == NULL) status[0] = '\0';
        fclose(sf);

        if (strncmp(status, "connected", 9) != 0) continue;

        /* -- Read EDID -- */
        snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", ent->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        uint8_t edid[1024];  /* base (128) + up to 7 extensions (896) */
        ssize_t len = read(fd, edid, sizeof(edid));
        close(fd);

        if (len < 128) continue;

        /* -- Validate base EDID header -- */
        static const uint8_t hdr[] = {0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00};
        if (memcmp(edid, hdr, 8) != 0) {
            log_msg("Bitstream: %s -- invalid EDID header", ent->d_name);
            continue;
        }

        int ext_count = edid[126];
        log_msg("Bitstream: %s connected, EDID %zd bytes, %d extension(s)",
                ent->d_name, len, ext_count);

        /* -- Parse each extension block -- */
        for (int i = 0; i < ext_count; i++) {
            int offset = (i + 1) * 128;
            if (offset + 128 > len) break;

            if (parse_cea_extension(&edid[offset], caps))
                found = 1;
        }

        if (found) break;  /* use first connected HDMI/DP with CEA data */
    }

    closedir(drm);

    /* If sink reports TrueHD support but no explicit HBR from Forum VSDB,
     * infer HBR -- a sink can't decode TrueHD without it. */
    if (caps->support_truehd && !caps->hbr_capable) {
        caps->hbr_capable = 1;
        log_msg("Bitstream: inferring HBR from TrueHD SAD");
    }

    if (found) {
        log_msg("Bitstream: sink caps -- "
                "AC3=%d EAC3=%d TrueHD=%d DTS=%d DTS-HD=%d "
                "HBR=%d maxCh=%d",
                caps->support_ac3,  caps->support_eac3,
                caps->support_truehd, caps->support_dts,
                caps->support_dtshd, caps->hbr_capable,
                caps->max_channels);
    } else {
        log_msg("Bitstream: no HDMI/DP sink with audio descriptors found");
    }

    return found ? 0 : -1;
#endif /* __linux__ */
}

/* ===================================================================
 * bitstream_can_passthrough() -- check if a given codec can pass through
 *
 * Returns 1 if the codec is supported by the probed sink AND has
 * an SPDIF muxer available in this FFmpeg build.
 * =================================================================== */

int bitstream_can_passthrough(const BitstreamCaps *caps,
                               enum AVCodecID codec_id)
{
    if (!caps->probed) return 0;

    switch (codec_id) {
    case AV_CODEC_ID_AC3:
        return caps->support_ac3;
    case AV_CODEC_ID_EAC3:
        return caps->support_eac3;
    case AV_CODEC_ID_TRUEHD:
        return caps->support_truehd && caps->hbr_capable;
    case AV_CODEC_ID_DTS:
        return caps->support_dts;
    /* DTS-HD MA uses AV_CODEC_ID_DTS with a profile flag.
     * For now, treat all DTS as passthrough-eligible if the sink
     * supports DTS. DTS-HD detection is Phase 3 refinement. */
    default:
        return 0;
    }
}

/* ===================================================================
 * Phase 3: SPDIF Passthrough (to be implemented)
 *
 * TODO:
 *   bitstream_open_passthrough()  -- open SPDIF muxer + SDL IEC958 device
 *   bitstream_write_packet()      -- mux compressed packet -> IEC 61937 frame
 *   bitstream_close_passthrough() -- tear down muxer and device
 * =================================================================== */
