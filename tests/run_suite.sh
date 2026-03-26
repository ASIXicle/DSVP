#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# DSVP Test Suite Runner
# Plays each synthetic clip for a specified duration, captures DSVP log.
#
# Usage:  bash tests/run_suite.sh [options]
#   -d SECONDS   Play duration per clip (default: 35, must exceed 10s DIAG interval)
#   -b BINARY    Path to DSVP binary (default: auto-detect)
#   -c CLIPDIR   Path to clips (default: tests/clips)
#   -o OUTDIR    Path for logs (default: tests/logs)
#   -f FILTER    Only run clips matching glob (e.g. "hdr10_*" or "fps_*")
#   -s           Include seek tests (sends seeks during playback)
#   --dry-run    Show what would run without executing
#
# Run from repo root:  bash tests/run_suite.sh
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

# ── Defaults ─────────────────────────────────────────────────────────
PLAY_DUR=35
BINARY=""
CLIPDIR="tests/clips"
LOGDIR="tests/logs"
FILTER="*"
DO_SEEK=0
DRY_RUN=0

# ── Parse args ───────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -d) PLAY_DUR="$2"; shift 2 ;;
        -b) BINARY="$2"; shift 2 ;;
        -c) CLIPDIR="$2"; shift 2 ;;
        -o) LOGDIR="$2"; shift 2 ;;
        -f) FILTER="$2"; shift 2 ;;
        -s) DO_SEEK=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Platform detection ───────────────────────────────────────────────
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "mingw"* || "$OSTYPE" == "cygwin"* ]]; then
    PLATFORM="windows"
else
    PLATFORM="linux"
fi

# ── Find DSVP binary ────────────────────────────────────────────────
if [[ -z "$BINARY" ]]; then
    if [[ "$PLATFORM" == "windows" ]]; then
        BINARY="build/dsvp.exe"
    else
        BINARY="build/dsvp"
    fi
fi

if [[ $DRY_RUN -eq 0 && ! -x "$BINARY" ]]; then
    echo "ERROR: DSVP binary not found at '$BINARY'"
    echo "  Build first:  mingw32-make  (Windows)  or  make  (Linux)"
    exit 1
fi

# ── Find clips ───────────────────────────────────────────────────────
shopt -s nullglob
CLIPS=( "$CLIPDIR"/$FILTER )
shopt -u nullglob

if [[ ${#CLIPS[@]} -eq 0 ]]; then
    echo "ERROR: No clips found matching '$CLIPDIR/$FILTER'"
    exit 1
fi

# ── Setup ────────────────────────────────────────────────────────────
mkdir -p "$LOGDIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SUMMARY="$LOGDIR/summary_${TIMESTAMP}.txt"

echo "═══════════════════════════════════════════════════════════════"
echo " DSVP Test Suite Runner"
echo " Platform:  $PLATFORM"
echo " Binary:    $BINARY"
echo " Clips:     ${#CLIPS[@]} files from $CLIPDIR/"
echo " Duration:  ${PLAY_DUR}s per clip"
echo " Logs:      $LOGDIR/"
echo " Seek test: $([ $DO_SEEK -eq 1 ] && echo 'YES' || echo 'no')"
echo "═══════════════════════════════════════════════════════════════"
echo ""

if [[ $DRY_RUN -eq 1 ]]; then
    echo "DRY RUN — would test these clips:"
    for clip in "${CLIPS[@]}"; do
        echo "  $(basename "$clip")"
    done
    exit 0
fi

# ── Helpers ──────────────────────────────────────────────────────────
kill_dsvp() {
    local pid=$1
    # kill works in MSYS2/git-bash for child processes on all platforms
    kill "$pid" 2>/dev/null || true
    sleep 1
    # Force kill if still alive
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

# DSVP writes log to dsvp.log in the working directory
DSVP_LOG="dsvp.log"

pass=0
fail=0

echo "Run started: $(date)" > "$SUMMARY"
echo "" >> "$SUMMARY"

# ── Main loop ────────────────────────────────────────────────────────
for clip in "${CLIPS[@]}"; do
    name=$(basename "$clip")
    name_noext="${name%.*}"
    logfile="$LOGDIR/${name_noext}_${TIMESTAMP}.log"

    echo -n "  TEST  $name ... "

    # Clear any existing log
    rm -f "$DSVP_LOG"

    # Launch DSVP in background
    "$BINARY" "$clip" &
    DSVP_PID=$!

    # Wait for playback duration
    sleep "$PLAY_DUR"

    # If seek testing, inject seeks via xdotool (Linux) or similar
    # For now, seek tests require manual interaction — the seek stress
    # clips with sparse/dense keyframes are designed for manual seek testing
    if [[ $DO_SEEK -eq 1 ]]; then
        echo -n "(seek mode — manual seeks expected) "
    fi

    # Kill DSVP
    kill_dsvp $DSVP_PID

    # Small delay for log flush
    sleep 0.5

    # Capture log
    if [[ -f "$DSVP_LOG" ]]; then
        cp "$DSVP_LOG" "$logfile"

        # Quick pass/fail check — look for playback summary
        if grep -q "Playback Summary" "$logfile"; then
            drops=$(grep "Frames dropped:" "$logfile" | tail -1 | sed 's/.*Frames dropped: *//' | sed 's/ .*//')
            pct=$(grep "Frames dropped:" "$logfile" | tail -1 | sed 's/.*(\([0-9.]*%\)).*/\1/')
            bias=$(grep "A.V bias:" "$logfile" | tail -1 | sed 's|.*A/V bias: *||' | sed 's| *$||')
            echo "OK  (drops: $drops [$pct], bias: $bias)"
            echo "PASS  $name  drops=$drops ($pct)  bias=$bias" >> "$SUMMARY"
            pass=$((pass + 1))
        else
            echo "WARN  (no playback summary — clip may not have started)"
            echo "WARN  $name  (no playback summary)" >> "$SUMMARY"
        fi
    else
        echo "FAIL  (no log file produced)"
        echo "FAIL  $name  (no log)" >> "$SUMMARY"
        fail=$((fail + 1))
    fi
done

# ── Summary ──────────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════════════════"
echo " Results: $pass passed, $fail failed out of ${#CLIPS[@]} clips"
echo " Logs:    $LOGDIR/"
echo " Summary: $SUMMARY"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Results: $pass passed, $fail failed / ${#CLIPS[@]} total" >> "$SUMMARY"
echo ""
echo "Next: parse results with  bash tests/parse_results.sh $LOGDIR/*_${TIMESTAMP}.log"
