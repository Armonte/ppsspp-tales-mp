#!/usr/bin/env bash
# One "Claude-Plays-Tales" step.
#
# Args:
#   $1  hold buttons (comma list, e.g. "circle" or "down,circle" or "" for no input)
#   $2  duration seconds (e.g. 3)
#   $3  output tag (e.g. "step-01")
#
# Resumes from states/latest.ppst, holds the buttons for the duration,
# saves a new state at states/latest.ppst, captures screenshot to
# tests/talesmp-output/$tag.png.
#
# Use:  tests/talesmp-step.sh circle 3 s01
#       tests/talesmp-step.sh down,circle 2 s02
#       tests/talesmp-step.sh "" 1 s03    # no input, just advance

set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${REPO}/build/PPSSPPHeadless"
ISO="${REPO}/../tales-jp/Tales of Phantasia - Narikiri Dungeon X (Japan).iso"
STATE="${REPO}/tests/talesmp-output/states/latest.ppst"
TMP_STATE="${REPO}/tests/talesmp-output/states/latest.next.ppst"
TMP_BMP="${REPO}/tests/talesmp-output/.step.bmp"

HOLD="${1:-}"
DUR="${2:-2}"
TAG="${3:-step}"

EXTRA=()
if [[ -n "$HOLD" ]]; then
    EXTRA+=(--hold="$HOLD")
fi
if [[ -f "$STATE" ]]; then
    EXTRA+=(--state="$STATE")
fi

"$BIN" "$ISO" \
    --graphics=software \
    --timeout="$DUR" \
    "${EXTRA[@]}" \
    --save-state-on-exit="$TMP_STATE" \
    --dump-screenshot="$TMP_BMP" \
    2>&1 | grep -iE "TalesMp:|wrote save" | tail -3

# Promote tmp state to latest.
if [[ -f "$TMP_STATE" ]]; then
    mv "$TMP_STATE" "$STATE"
fi

# Convert and save tagged screenshot.
if [[ -f "$TMP_BMP" ]]; then
    python3 -c "from PIL import Image; Image.open('$TMP_BMP').save('${REPO}/tests/talesmp-output/${TAG}.png')"
    echo "->  tests/talesmp-output/${TAG}.png"
fi
