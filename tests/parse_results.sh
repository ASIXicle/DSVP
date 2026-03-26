#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# DSVP Test Results Parser
# Reads DSVP log files and produces a markdown baseline document.
#
# Usage:
#   bash tests/parse_results.sh tests/logs/*_20260326_*.log
#   bash tests/parse_results.sh tests/logs/              # all logs in dir
#   bash tests/parse_results.sh tests/logs/ -o baseline.md
#
# Run from repo root. Fully portable (no grep -oP / PCRE).
# ─────────────────────────────────────────────────────────────────────
set -u

OUTFILE=""
LOGFILES=()

# ── Parse args ───────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) OUTFILE="$2"; shift 2 ;;
        *)
            if [[ -d "$1" ]]; then
                for f in "$1"/*.log; do
                    [[ -f "$f" ]] && LOGFILES+=("$f")
                done
            elif [[ -f "$1" ]]; then
                LOGFILES+=("$1")
            fi
            shift ;;
    esac
done

if [[ ${#LOGFILES[@]} -eq 0 ]]; then
    echo "Usage: $0 <logfiles or directory> [-o output.md]"
    exit 1
fi

IFS=$'\n' LOGFILES=($(sort <<< "${LOGFILES[*]}")); unset IFS

# ── Portable extraction helpers ──────────────────────────────────────

extract_num() {
    grep "$1" "$2" 2>/dev/null | tail -1 | sed 's/.*'"$1"' *//' | sed 's/[^0-9].*//' || true
}

extract_pct() {
    grep "Frames dropped:" "$1" 2>/dev/null | tail -1 | sed 's/.*(\([0-9.]*%\)).*/\1/' || true
}

# Uses | delimiter to handle A/V safely
extract_ms() {
    grep "$1" "$2" 2>/dev/null | tail -1 | sed 's|.*'"$1"' *||' | sed 's| *$||' || true
}

# ── Generate output ──────────────────────────────────────────────────
{
    echo "# DSVP Synthetic Test Baseline"
    echo ""
    echo "**Date:** $(date '+%B %d, %Y')"
    echo "**Branch:** main"

    dsvp_ver=$(grep "DSVP.*started" "${LOGFILES[0]}" 2>/dev/null | head -1 | sed 's/.*DSVP //' | sed 's/ .*//' || echo "unknown")
    ffmpeg_ver=$(grep "FFmpeg" "${LOGFILES[0]}" 2>/dev/null | head -1 | sed 's/.*FFmpeg //' | sed 's/ .*//' || echo "unknown")
    gpu_driver=$(grep "GPU device created" "${LOGFILES[0]}" 2>/dev/null | head -1 | sed 's/.*driver: //' | sed 's/).*//' || echo "unknown")
    echo "**DSVP:** $dsvp_ver  **FFmpeg:** $ffmpeg_ver  **GPU:** $gpu_driver"
    echo "**Clips tested:** ${#LOGFILES[@]}"
    echo ""

    # === Pipeline Verification Table ===
    echo "## Pipeline Verification"
    echo ""
    echo "| Test | Codec | Resolution | Pix Fmt | Swscale | Uniforms |"
    echo "|------|-------|------------|---------|---------|----------|"

    for logfile in "${LOGFILES[@]}"; do
        name=$(basename "$logfile" .log | sed 's/_[0-9]\{8\}_[0-9]\{6\}$//')
        codec=$(grep "Video codec:" "$logfile" 2>/dev/null | tail -1 | sed 's/.*Video codec: //' | sed 's/ .*//' || echo "—")
        resolution=$(grep "Video:" "$logfile" 2>/dev/null | tail -1 | sed 's/.* \([0-9]*x[0-9]*\).*/\1/' || echo "—")
        pix_fmt=$(grep "Video:" "$logfile" 2>/dev/null | tail -1 | sed 's/.*pix_fmt=//' | sed 's/[, ].*//' || echo "—")
        swscale=$(grep "swscale:" "$logfile" 2>/dev/null | tail -1 | sed 's/.*swscale: //' || echo "—")
        uniforms=$(grep "GPU: uniforms set" "$logfile" 2>/dev/null | tail -1 | sed 's/.*GPU: uniforms set //' | sed 's/[()]//g' || echo "—")
        printf "| %s | %s | %s | %s | %s | %s |\n" "$name" "$codec" "$resolution" "$pix_fmt" "$swscale" "$uniforms"
    done

    echo ""

    # === A/V Sync Table ===
    echo "## A/V Sync & Performance"
    echo ""
    echo "| Test | Decoded | Displayed | Dropped | Drop % | Peak Drift | Bias | Multi | Snaps | Errors |"
    echo "|------|---------|-----------|---------|--------|------------|------|-------|-------|--------|"

    for logfile in "${LOGFILES[@]}"; do
        name=$(basename "$logfile" .log | sed 's/_[0-9]\{8\}_[0-9]\{6\}$//')
        decoded=$(extract_num "Frames decoded:" "$logfile")
        displayed=$(extract_num "Frames displayed:" "$logfile")
        dropped_n=$(extract_num "Frames dropped:" "$logfile")
        dropped_pct=$(extract_pct "$logfile")
        peak=$(extract_ms "Peak A.V drift:" "$logfile")
        bias=$(extract_ms "A.V bias:" "$logfile")
        multi=$(grep "Multi-decode" "$logfile" 2>/dev/null | tail -1 | sed 's/.*: *//' || true)
        snaps=$(grep "snap-forwards:" "$logfile" 2>/dev/null | tail -1 | sed 's/.*: *//' || true)
        errors=$(grep -ciE "error|fatal|segfault|assert" "$logfile" 2>/dev/null) || errors=0
        printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n" \
            "$name" "${decoded:-—}" "${displayed:-—}" "${dropped_n:-0}" "${dropped_pct:-0.00%}" \
            "${peak:-—}" "${bias:-—}" "${multi:-0}" "${snaps:-0}" \
            "$([ "${errors:-0}" = "0" ] && echo "—" || echo "$errors")"
    done

    echo ""

    # === Periodic DIAG detail ===
    has_diag=0
    for logfile in "${LOGFILES[@]}"; do
        if grep -q "DIAG: \[" "$logfile" 2>/dev/null; then
            has_diag=1
            break
        fi
    done

    if [[ $has_diag -eq 1 ]]; then
        echo "## Drift Tracking (Periodic DIAG)"
        echo ""
        echo "| Test | Time | A/V | Peak | Bias |"
        echo "|------|------|-----|------|------|"
        for logfile in "${LOGFILES[@]}"; do
            name=$(basename "$logfile" .log | sed 's/_[0-9]\{8\}_[0-9]\{6\}$//')
            grep "DIAG: \[" "$logfile" 2>/dev/null | while read -r line; do
                time=$(echo "$line" | sed 's/.*\[\([0-9]*s\)\].*/\1/')
                av=$(echo "$line" | sed 's|.*A/V=\([-0-9.]*ms\).*|\1|')
                pk=$(echo "$line" | sed 's/.*peak=\([-0-9.]*ms\).*/\1/')
                bi=$(echo "$line" | sed 's/.*bias=\([-0-9.]*ms\).*/\1/')
                printf "| %s | %s | %s | %s | %s |\n" "$name" "$time" "$av" "$pk" "$bi"
            done
        done
        echo ""
    fi

    # === Frame drop detail ===
    has_drops=0
    for logfile in "${LOGFILES[@]}"; do
        if grep -q "frame dropped at" "$logfile" 2>/dev/null; then
            has_drops=1
            break
        fi
    done

    if [[ $has_drops -eq 1 ]]; then
        echo "## Frame Drop Detail"
        echo ""
        echo "| Test | Time | A/V Drift |"
        echo "|------|------|-----------|"
        for logfile in "${LOGFILES[@]}"; do
            name=$(basename "$logfile" .log | sed 's/_[0-9]\{8\}_[0-9]\{6\}$//')
            grep "frame dropped at" "$logfile" 2>/dev/null | while read -r line; do
                time=$(echo "$line" | sed 's/.*at \([0-9.]*s\).*/\1/')
                drift=$(echo "$line" | sed 's/.*drift: \([-0-9.]*ms\).*/\1/')
                printf "| %s | %s | %s |\n" "$name" "$time" "$drift"
            done
        done
        echo ""
    fi

    echo "---"
    echo "*Generated by \`tests/parse_results.sh\` on $(date)*"

} > "${OUTFILE:-/dev/stdout}"

if [[ -n "$OUTFILE" ]]; then
    echo "Baseline written to: $OUTFILE"
    echo "Lines: $(wc -l < "$OUTFILE")"
fi
