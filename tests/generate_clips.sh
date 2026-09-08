#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# DSVP Synthetic Test Clip Generator
# Generates all clips needed for baseline testing.
# Run from repo root:  bash tests/generate_clips.sh
# Requires: ffmpeg with libx264, libx265, libvpx-vp9, libaom-av1/libsvtav1, libopus, libfdk_aac or aac
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

OUTDIR="${DSVP_CLIP_DIR:-tests/clips}"
DUR="${DSVP_CLIP_DUR:-30}"          # seconds — enough for 2-3 DIAG intervals
SEEK_DUR="${DSVP_CLIP_SEEK_DUR:-60}"  # seconds — for seek stress tests
AUDIO_HZ=48000
AUDIO_CH=2      # stereo (real-world content is stereo)

# Bitrates for reasonable file sizes (synthetic content compresses well)
V_BR_720="2M"
V_BR_1080="4M"
V_BR_4K="12M"

# ── Helpers ──────────────────────────────────────────────────────────

mkdir -p "$OUTDIR"

gen_count=0
skip_count=0

# Sine tone: 440Hz left, 1kHz right (easy to verify stereo mapping)
AUDIO_FILTER="sine=frequency=440:sample_rate=${AUDIO_HZ}:duration=${DUR}[l];sine=frequency=1000:sample_rate=${AUDIO_HZ}:duration=${DUR}[r];[l][r]amerge=inputs=2,aformat=channel_layouts=stereo"

gen() {
    local name="$1"; shift
    local outfile="$OUTDIR/$name"
    # DSVP_CLIP_ONLY='glob [glob ...]' generates only matching clips,
    # e.g. DSVP_CLIP_ONLY='hlg_* hdr10_pq_*' (space-separated globs).
    if [[ -n "${DSVP_CLIP_ONLY:-}" ]]; then
        local pat match=0
        for pat in $DSVP_CLIP_ONLY; do
            # shellcheck disable=SC2053  # unquoted RHS = glob match, by design
            [[ "$name" == $pat ]] && match=1
        done
        [[ $match -eq 1 ]] || return 0
    fi
    if [[ -f "$outfile" ]]; then
        # Validate on skip (review M23/S6-14): a stale or partial clip
        # from an interrupted run must not be reused forever.
        if ffprobe -v error -show_entries stream=codec_type -of csv=p=0 "$outfile" 2>/dev/null | grep -q video; then
            echo "  SKIP  $name (exists, probes OK)"
            skip_count=$((skip_count + 1))
            return
        fi
        echo "  REGEN $name (exists but does not probe — regenerating)"
        rm -f "$outfile"
    fi
    echo "  GEN   $name"
    ffmpeg -hide_banner -loglevel warning "$@" -y "$outfile"
    gen_count=$((gen_count + 1))
}

echo "═══════════════════════════════════════════════════════════════"
echo " DSVP Synthetic Test Clip Generator"
echo " Output: $OUTDIR/    Duration: ${DUR}s (${SEEK_DUR}s seek tests)"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# ─────────────────────────────────────────────────────────────────────
# 1. FORMAT MATRIX — codec × pix_fmt × resolution
#    Tests: pipeline selection (bypass vs swscale), texture format,
#           range handling, colorspace detection
# ─────────────────────────────────────────────────────────────────────
echo "── Category 1: Format Matrix ──"

# --- 720p ---
gen "fmt_h264_720p_420.mkv" \
    -f lavfi -i "testsrc2=size=1280x720:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_720 -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_h264_720p_422.mkv" \
    -f lavfi -i "testsrc2=size=1280x720:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv422p -b:v $V_BR_720 -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_h264_720p_444.mkv" \
    -f lavfi -i "testsrc2=size=1280x720:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv444p -b:v $V_BR_720 -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_h264_720p_fullrange.mkv" \
    -f lavfi -i "testsrc2=size=1280x720:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuvj420p -b:v $V_BR_720 -g 48 \
    -color_range pc -c:a aac -b:a 128k -ac $AUDIO_CH

# --- 1080p ---
gen "fmt_h264_1080p_420.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_1080 -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_h264_1080p_422.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv422p -b:v $V_BR_1080 -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_h264_1080p_444.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv444p -b:v $V_BR_1080 -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_h264_1080p_fullrange.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuvj420p -b:v $V_BR_1080 -g 48 \
    -color_range pc -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_h264_1080p_10bit.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p10le -profile:v high10 -b:v $V_BR_1080 -g 48 \
    -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_hevc_1080p_420.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx265 -pix_fmt yuv420p -b:v $V_BR_1080 -x265-params keyint=48 \
    -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_hevc_1080p_10bit.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx265 -pix_fmt yuv420p10le -b:v $V_BR_1080 -x265-params keyint=48 \
    -c:a aac -b:a 128k -ac $AUDIO_CH

# --- 4K ---
gen "fmt_h264_4k_420.mkv" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_4K -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_hevc_4k_10bit.mkv" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx265 -pix_fmt yuv420p10le -b:v $V_BR_4K -x265-params keyint=48 \
    -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_h264_4k_444.mkv" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv444p -b:v $V_BR_4K -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_h264_4k_fullrange.mkv" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuvj420p -b:v $V_BR_4K -g 48 \
    -color_range pc -c:a aac -b:a 128k -ac $AUDIO_CH

gen "fmt_av1_4k_10bit.mkv" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libsvtav1 -pix_fmt yuv420p10le -b:v $V_BR_4K -g 48 -preset 8 \
    -c:a aac -b:a 128k -ac $AUDIO_CH

# ─────────────────────────────────────────────────────────────────────
# 2. FRAMERATE MATRIX
#    Tests: frame pacing, 1:1 VSync detection, pulldown cadence,
#           micro-correction, A/V drift accumulation
# ─────────────────────────────────────────────────────────────────────
echo "── Category 2: Framerate Matrix ──"

for fps_tag in "23.976:24000/1001" "24:24" "25:25" "29.97:30000/1001" "30:30" "50:50" "59.94:60000/1001" "60:60"; do
    tag="${fps_tag%%:*}"
    rate="${fps_tag##*:}"
    safe_tag="${tag//./_}"  # 23.976 → 23_976

    gen "fps_1080p_${safe_tag}.mkv" \
        -f lavfi -i "testsrc2=size=1920x1080:rate=${rate}:duration=$DUR" \
        -f lavfi -i "$AUDIO_FILTER" \
        -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_1080 -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH
done

# 4K HFR stress tests
for fps_tag in "23.976:24000/1001" "30:30" "60:60"; do
    tag="${fps_tag%%:*}"
    rate="${fps_tag##*:}"
    safe_tag="${tag//./_}"

    gen "fps_4k_${safe_tag}.mkv" \
        -f lavfi -i "testsrc2=size=3840x2160:rate=${rate}:duration=$DUR" \
        -f lavfi -i "$AUDIO_FILTER" \
        -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_4K -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH
done

gen "fps_4k_60_hevc10.mkv" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=60:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx265 -pix_fmt yuv420p10le -b:v $V_BR_4K -x265-params keyint=120 \
    -c:a aac -b:a 128k -ac $AUDIO_CH

# ─────────────────────────────────────────────────────────────────────
# 3. AUDIO VARIETY
#    Tests: audio codec handling, thread scheduling contention,
#           downmix behavior, sync under different decode loads
#    Base: h264 1080p yuv420p 24fps — isolates audio variable
# ─────────────────────────────────────────────────────────────────────
echo "── Category 3: Audio Variety ──"

# AAC stereo (baseline — already covered by format matrix, alias for clarity)
gen "audio_aac_stereo.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_1080 -g 48 -c:a aac -b:a 128k -ac 2

# FLAC lossless stereo (known scheduling contention with 4K decode)
gen "audio_flac_stereo.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_1080 -g 48 -c:a flac -ac 2

# FLAC + 4K stress (the known problematic combination)
gen "audio_flac_4k.mkv" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_4K -g 48 -c:a flac -ac 2

# EAC3 (Dolby Digital Plus) stereo
gen "audio_eac3_stereo.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_1080 -g 48 -c:a eac3 -b:a 192k -ac 2

# AC3 5.1 surround (tests downmix to stereo output)
AUDIO_51="sine=f=440:r=${AUDIO_HZ}:d=${DUR}[fl];sine=f=554:r=${AUDIO_HZ}:d=${DUR}[fr];sine=f=660:r=${AUDIO_HZ}:d=${DUR}[fc];sine=f=220:r=${AUDIO_HZ}:d=${DUR}[lfe];sine=f=880:r=${AUDIO_HZ}:d=${DUR}[bl];sine=f=1047:r=${AUDIO_HZ}:d=${DUR}[br];[fl][fr][fc][lfe][bl][br]amerge=inputs=6,aformat=channel_layouts=5.1"
gen "audio_ac3_51.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_51" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_1080 -g 48 -c:a ac3 -b:a 384k

# Opus in WebM (VP9+Opus is the standard WebM combo)
gen "audio_opus_vp9.webm" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libvpx-vp9 -pix_fmt yuv420p -b:v $V_BR_1080 -g 48 -c:a libopus -b:a 128k -ac 2

# ─────────────────────────────────────────────────────────────────────
# 4. LEGACY CODECS
#    Tests: BT.601 detection, MPEG-PS container, interlaced metadata
# ─────────────────────────────────────────────────────────────────────
echo "── Category 4: Legacy Codecs ──"

# Note: MPEG-PS + MPEG-2 uses mp2 audio (period-correct)
gen "legacy_mpeg2_480i.mpg" \
    -f lavfi -i "testsrc2=size=720x480:rate=30000/1001:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v mpeg2video -pix_fmt yuv420p -b:v 4M -g 15 \
    -colorspace bt470bg -color_primaries bt470bg -color_trc gamma28 \
    -c:a mp2 -b:a 192k -ac $AUDIO_CH -f mpeg

gen "legacy_mpeg4_576p.avi" \
    -f lavfi -i "testsrc2=size=720x576:rate=25:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v mpeg4 -pix_fmt yuv420p -b:v 2M -g 50 \
    -c:a mp3 -b:a 128k -ac $AUDIO_CH

# ─────────────────────────────────────────────────────────────────────
# 5. CONTAINER VARIETY
#    Tests: container demuxer behavior, timestamp handling
# ─────────────────────────────────────────────────────────────────────
echo "── Category 5: Container Variety ──"

gen "container_h264_1080p.mp4" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_1080 -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

gen "container_vp9_720p.webm" \
    -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libvpx-vp9 -pix_fmt yuv420p -b:v $V_BR_720 -g 60 -c:a libopus -b:a 128k -ac $AUDIO_CH

gen "container_vp9_4k.webm" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=30:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libvpx-vp9 -pix_fmt yuv420p -b:v $V_BR_4K -g 60 -c:a libopus -b:a 128k -ac $AUDIO_CH

# ─────────────────────────────────────────────────────────────────────
# 6. HDR10 SYNTHETIC
#    Tests: BT.2020+PQ detection, HDR→SDR tone mapping pipeline,
#           dynamic peak detection, gamut mapping
#    Metadata: BT.2020 primaries, PQ transfer, 10-bit HEVC
#    master-display: DCI-P3 D65 mastering, 1000 nit peak, 0.001 min
# ─────────────────────────────────────────────────────────────────────
echo "── Category 6: HDR10 ──"

HDR_META="-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc"
# The ffmpeg-level flags above do NOT reach the x265 VUI on every
# ffmpeg build (Debian 7.1.5: the clip came out transfer=unknown,
# primaries=unknown, and x265 refused hdr-opt). The x265 params are
# the form that always lands; the flags stay for the container.
HDR_X265="colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc"
MASTER_DISPLAY="G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,50)"
MAX_CLL="1000,400"

gen "hdr10_hevc_1080p.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx265 -pix_fmt yuv420p10le -b:v $V_BR_1080 \
    -x265-params "keyint=48:${HDR_X265}:master-display=${MASTER_DISPLAY}:max-cll=${MAX_CLL}:hdr-opt=1" \
    $HDR_META -c:a aac -b:a 128k -ac $AUDIO_CH

gen "hdr10_hevc_4k.mkv" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx265 -pix_fmt yuv420p10le -b:v $V_BR_4K \
    -x265-params "keyint=48:${HDR_X265}:master-display=${MASTER_DISPLAY}:max-cll=${MAX_CLL}:hdr-opt=1" \
    $HDR_META -c:a aac -b:a 128k -ac $AUDIO_CH

# HLG (ARIB STD-B67): the second HDR transfer the shader carries
# (hlg_oetf_inv + BT.2100 OOTF at 1000 nits); the suite had no clip
# for it (review M23). testsrc2 values are SDR-range; the clip drives
# the code PATH, not a known luminance — see the staircase below.
gen "hlg_hevc_1080p.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx265 -pix_fmt yuv420p10le -b:v $V_BR_1080 \
    -x265-params "keyint=48:colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc" \
    -color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc \
    -c:a aac -b:a 128k -ac $AUDIO_CH

# PQ STAIRCASE PROBE (review M23/S6-13): patches of KNOWN luminance so a
# screenshot is a measurement. 13 vertical bars at 1, 5, 10, 25, 50,
# 100, 203, 300, 400, 600, 1000, 2000, 4000 nits, encoded as genuine PQ
# code values (10-bit full-range code = round(1023*PQ(nits/10000))),
# one clip per master-display peak. Read each bar's output code off a
# capture and compare with tools/eetf-audit.c's spec column.
pq_code10() {   # nits -> 10-bit PQ code (full range), via awk
    awk -v n="$1" 'BEGIN { m1=0.1593017578125; m2=78.84375; c1=0.8359375; c2=18.8515625; c3=18.6875;
        Np=(n/10000.0)^m1; v=((c1+c2*Np)/(1+c3*Np))^m2; printf "%d", int(v*1023+0.5) }'
}
STAIR_NITS=(1 5 10 25 50 100 203 300 400 600 1000 2000 4000)
stair_filter() {
    # geq works in the plane's native depth, so the luma expression IS the
    # 10-bit code. 13 disjoint bars of 1920/13 px; chroma pinned at 512
    # (neutral). (lutyuv silently fell back to an 8-bit format and
    # saturated every bar to 1023 — read back before trusting a probe.)
    local expr="" i x0 x1 n code
    for i in "${!STAIR_NITS[@]}"; do
        n=${STAIR_NITS[$i]}; code=$(pq_code10 "$n")
        x0=$(( i * 1920 / 13 )); x1=$(( (i + 1) * 1920 / 13 - 1 ))
        expr+="${expr:++}${code}*between(X,${x0},${x1})"
    done
    printf 'nullsrc=s=1920x1080:r=24:d=%s,format=yuv444p10le,setrange=pc,geq=lum=%s:cb=512:cr=512,format=yuv420p10le[v]' "$DUR" "'${expr}'"
}
for peak in 600 1000 4000 10000; do
    md="G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L($((peak*10000)),50)"
    gen "hdr10_pq_staircase_${peak}.mkv" \
        -f lavfi -i "$AUDIO_FILTER" \
        -filter_complex "$(stair_filter)" -map "[v]" -map 0:a \
        -c:v libx265 -pix_fmt yuv420p10le -b:v $V_BR_1080 \
        -x265-params "keyint=48:${HDR_X265}:range=full:master-display=${md}:max-cll=${peak},400:hdr-opt=1" \
        -color_range pc $HDR_META -c:a aac -b:a 128k -ac $AUDIO_CH
done

# Dolby Vision cannot be synthesised by stock ffmpeg encoders. A P5
# fixture is external: set DSVP_DV_FIXTURE=/path/to/p5.mkv and it is
# linked into the suite; otherwise the absence is announced, not silent.
if [[ -n "${DSVP_DV_FIXTURE:-}" && -f "${DSVP_DV_FIXTURE}" ]]; then
    ln -sf "$(readlink -f "$DSVP_DV_FIXTURE")" "$OUTDIR/dovi_p5_fixture.mkv"
    echo "  LINK  dovi_p5_fixture.mkv -> $DSVP_DV_FIXTURE"
else
    echo "  NONE  Dolby Vision P5: no fixture (set DSVP_DV_FIXTURE=<p5.mkv>) — the DV reshape/MMR path is NOT exercised by this suite"
fi

# BT.2020 gamut WITHOUT PQ (SDR BT.2020 — tests gamut path alone)
gen "hdr10_bt2020_sdr.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx265 -pix_fmt yuv420p10le -b:v $V_BR_1080 \
    -x265-params "keyint=48:colorprim=bt2020:transfer=bt709:colormatrix=bt2020nc" \
    -color_primaries bt2020 -color_trc bt709 -colorspace bt2020nc \
    -c:a aac -b:a 128k -ac $AUDIO_CH

# ─────────────────────────────────────────────────────────────────────
# 7. SEEK STRESS
#    Tests: seek-recovery behavior, keyframe distance impact,
#           A/V sync recovery after distant seeks
#    60s clips with sparse keyframes (GOP=240 = 10s at 24fps)
# ─────────────────────────────────────────────────────────────────────
echo "── Category 7: Seek Stress ──"

AUDIO_FILTER_SEEK="sine=frequency=440:sample_rate=${AUDIO_HZ}:duration=${SEEK_DUR}[l];sine=frequency=1000:sample_rate=${AUDIO_HZ}:duration=${SEEK_DUR}[r];[l][r]amerge=inputs=2,aformat=channel_layouts=stereo"

gen "seek_h264_4k_sparse.mkv" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=24:duration=$SEEK_DUR" \
    -f lavfi -i "$AUDIO_FILTER_SEEK" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_4K -g 240 -c:a aac -b:a 128k -ac $AUDIO_CH

gen "seek_hevc_4k_sparse.mkv" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=24:duration=$SEEK_DUR" \
    -f lavfi -i "$AUDIO_FILTER_SEEK" \
    -c:v libx265 -pix_fmt yuv420p10le -b:v $V_BR_4K -x265-params keyint=240 \
    -c:a aac -b:a 128k -ac $AUDIO_CH

# Dense keyframes for comparison (GOP=12 = 0.5s at 24fps)
gen "seek_h264_4k_dense.mkv" \
    -f lavfi -i "testsrc2=size=3840x2160:rate=24:duration=$SEEK_DUR" \
    -f lavfi -i "$AUDIO_FILTER_SEEK" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_4K -g 12 -c:a aac -b:a 128k -ac $AUDIO_CH

# ─────────────────────────────────────────────────────────────────────
# 8. EDGE CASES
#    Tests: unusual resolutions, very small content, alignment
# ─────────────────────────────────────────────────────────────────────
echo "── Category 8: Edge Cases ──"

gen "edge_320x240.mkv" \
    -f lavfi -i "testsrc2=size=320x240:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v 500k -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

# Non-mod-16 resolution (stress-tests texture alignment)
gen "edge_1366x768.mkv" \
    -f lavfi -i "testsrc2=size=1366x768:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_1080 -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

# Ultra-wide aspect ratio (21:9)
gen "edge_2560x1080.mkv" \
    -f lavfi -i "testsrc2=size=2560x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_1080 -g 48 -c:a aac -b:a 128k -ac $AUDIO_CH

# 480p with BT.601 (tests colorspace switch mid-playlist if interleaved with BT.709)
gen "edge_bt601_1080p.mkv" \
    -f lavfi -i "testsrc2=size=1920x1080:rate=24:duration=$DUR" \
    -f lavfi -i "$AUDIO_FILTER" \
    -c:v libx264 -pix_fmt yuv420p -b:v $V_BR_1080 -g 48 \
    -colorspace bt470bg -color_primaries bt470bg -color_trc gamma28 \
    -c:a aac -b:a 128k -ac $AUDIO_CH

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo " Done: $gen_count generated, $skip_count skipped (already exist)"
echo " Total clips: $(ls -1 "$OUTDIR" 2>/dev/null | wc -l)"
echo " Total size:  $(du -sh "$OUTDIR" 2>/dev/null | cut -f1)"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Verify with:  ls -lhS $OUTDIR/"
echo "Delete all:   rm -rf $OUTDIR/"
