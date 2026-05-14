#!/usr/bin/env bash
# Quick smoke test for the Tales of Phantasia MP patcher.
#
# Runs PPSSPPHeadless on the JP ISO with the software renderer for ~10s,
# captures every TalesMp log line, and dumps a screenshot of the last frame
# (when --dump-screenshot is wired up). Output goes to tests/talesmp-output/
# so it can be committed to the repo for review.
#
# Usage:  tests/talesmp-smoketest.sh [/path/to/Tales-NDX.iso]
#         (defaults to /mnt/c/dev/u4ick/psp/tales-jp/Tales*.iso)

set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
ISO="${1:-/mnt/c/dev/u4ick/psp/tales-jp/Tales of Phantasia - Narikiri Dungeon X (Japan).iso}"
BIN="${REPO}/build/PPSSPPHeadless"
OUTDIR="${REPO}/tests/talesmp-output"

if [[ ! -f "$BIN" ]]; then
    echo "PPSSPPHeadless not found at $BIN — build it first:"
    echo "  cd build && ninja PPSSPPHeadless"
    exit 1
fi
if [[ ! -f "$ISO" ]]; then
    echo "Tales ISO not found at: $ISO"
    exit 1
fi

mkdir -p "$OUTDIR"
LOG="$OUTDIR/run-$(date +%Y%m%d-%H%M%S).log"

echo "==> ISO:   $ISO"
echo "==> Bin:   $BIN"
echo "==> Log:   $LOG"
echo

# Run for 10 seconds via headless's own --timeout so PSP_Shutdown fires
# cleanly (lets us log the hook counter on the way out).
"$BIN" "$ISO" \
    --graphics=software \
    --timeout=10 \
    -l \
    > "$LOG" 2>&1 || true

# Surface the interesting lines.
echo "==> TalesMp log lines:"
grep -E "TalesMp:" "$LOG" || echo "    (no TalesMp lines — patcher not invoked?)"
echo
echo "==> DISC_ID detection:"
grep -E "DISC_ID|disc[Ii][dD]|GAME_ID" "$LOG" | head -5 || echo "    (none)"
echo
echo "==> Module loading:"
grep -E "Module entry|Loading module|exitGame" "$LOG" | head -10 || echo "    (none)"
echo
echo "==> Errors / warnings:"
grep -iE "^[A-Z] (ERROR|WARN|FATAL)|error:" "$LOG" | head -10 || echo "    (none)"

# Latest run pointer for easy `git diff` of new/changed log content.
cp "$LOG" "$OUTDIR/run-latest.log"

echo
echo "==> Log saved: $LOG"
echo "==> Symlinked latest at: $OUTDIR/run-latest.log"
