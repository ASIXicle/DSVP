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
#   -m PCT       Max dropped-frame percent for PASS (default 5; env DSVP_SUITE_MAX_DROP_PCT)
#   --dry-run    Show what would run without executing
#
# Exit status: 0 only if every clip PASSED. A clip with no Playback
# Summary, a drop rate over the threshold, or ERROR/assert lines in its
# log is a FAIL (review M20/M22: the old runner awarded PASS on the mere
# presence of "Playback Summary" and always exited 0).
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
MAX_DROP_PCT="${DSVP_SUITE_MAX_DROP_PCT:-5}"

# ── Parse args ───────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        -d) PLAY_DUR="$2"; shift 2 ;;
        -b) BINARY="$2"; shift 2 ;;
        -c) CLIPDIR="$2"; shift 2 ;;
        -o) LOGDIR="$2"; shift 2 ;;
        -f) FILTER="$2"; shift 2 ;;
        -s) DO_SEEK=1; shift ;;
        -m) MAX_DROP_PCT="$2"; shift 2 ;;
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
# shellcheck disable=SC2206  # $FILTER is a glob by design (-f "hdr10_*")
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

# DSVP writes dsvp.log NEXT TO THE EXECUTABLE (log.c resolves the exe
# dir; field 2026-09-07: the runner looked in the CWD and scored every
# clip 'no log file produced' while the player logged a full summary
# to build/dsvp.log). The CWD copy is the player's fallback for an
# unwritable exe dir, so it is checked second.
DSVP_LOG="$(dirname "$BINARY")/dsvp.log"
DSVP_LOG_CWD="dsvp.log"
echo " Log:       $DSVP_LOG (fallback $DSVP_LOG_CWD)"

# The suite must not overwrite the developer's resume record with a
# synthetic clip (every kill_dsvp is a SIGTERM = a resume save).
export DSVP_NO_RESUME=1

pass=0
fail=0
nosum=0

echo "Run started: $(date)" > "$SUMMARY"
echo "" >> "$SUMMARY"

# ── Main loop ────────────────────────────────────────────────────────
for clip in "${CLIPS[@]}"; do
    name=$(basename "$clip")
    name_noext="${name%.*}"
    logfile="$LOGDIR/${name_noext}_${TIMESTAMP}.log"

    echo -n "  TEST  $name ... "

    # Clear any existing log (both places the player can put it)
    rm -f "$DSVP_LOG" "$DSVP_LOG_CWD"

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

    # Capture log (exe-dir first, then the player's CWD fallback)
    got_log=""
    if [[ -f "$DSVP_LOG" ]]; then got_log="$DSVP_LOG"
    elif [[ -f "$DSVP_LOG_CWD" ]]; then got_log="$DSVP_LOG_CWD"; fi
    if [[ -n "$got_log" ]]; then
        cp "$got_log" "$logfile"

        # Pass criterion: a summary exists, drops are under the threshold,
        # and the log carries no ERROR/assert line.
        if grep -q "Playback Summary" "$logfile"; then
            drops=$(grep "Frames dropped:" "$logfile" | tail -1 | sed 's/.*Frames dropped: *//' | sed 's/ .*//')
            pct=$(grep "Frames dropped:" "$logfile" | tail -1 | sed 's/.*(\([0-9.]*\)%).*/\1/')
            bias=$(grep "A.V bias:" "$logfile" | tail -1 | sed 's|.*A/V bias: *||' | sed 's| *$||')
            errs=$(grep -ciE "ERROR|FATAL|segfault|assert" "$logfile" || true)
            over=$(awk -v p="${pct:-0}" -v m="$MAX_DROP_PCT" 'BEGIN { print (p+0 > m+0) ? 1 : 0 }')
            if [[ "$over" -eq 0 && "${errs:-0}" -eq 0 ]]; then
                echo "OK  (drops: $drops [${pct}%], bias: $bias)"
                echo "PASS  $name  drops=$drops (${pct}%)  bias=$bias" >> "$SUMMARY"
                pass=$((pass + 1))
            else
                echo "FAIL  (drops: $drops [${pct}% > ${MAX_DROP_PCT}%] errors: ${errs:-0})"
                echo "FAIL  $name  drops=$drops (${pct}%, max ${MAX_DROP_PCT}%)  errors=${errs:-0}  bias=$bias" >> "$SUMMARY"
                fail=$((fail + 1))
            fi
        else
            echo "FAIL  (no playback summary — clip did not start or did not finish)"
            echo "FAIL  $name  (no playback summary)" >> "$SUMMARY"
            fail=$((fail + 1))
            nosum=$((nosum + 1))
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
echo " Results: $pass passed, $fail failed ($nosum no-summary) out of ${#CLIPS[@]} clips (max drop ${MAX_DROP_PCT}%)"
echo " Logs:    $LOGDIR/"
echo " Summary: $SUMMARY"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Results: $pass passed, $fail failed ($nosum no-summary) / ${#CLIPS[@]} total" >> "$SUMMARY"
echo ""
echo "Next: parse results with  bash tests/parse_results.sh $LOGDIR/*_${TIMESTAMP}.log"
exit $(( fail > 0 ? 1 : 0 ))
