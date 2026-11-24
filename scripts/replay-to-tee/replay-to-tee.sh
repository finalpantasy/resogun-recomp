#!/bin/bash
# Test-only: replays A:/golf-wt/pad-replay-data/hole10.pad (recorded with record-tee.sh from scripts/tee-route) to the Green C.C.
# hole-10 tee, one boot per label, with pixel resyncs and a Cross-until-tee tail (see replay-to-tee.ps1), then compares the
# static part of the last frame with the reference. Usage: replay-to-tee.sh LABEL [pad file]
# Take the lock first: until mkdir "$TEMP/claude/golf-heavy.lock" 2>/dev/null; do sleep 20; done ... rmdir it after.
D="$(cd "$(dirname "$0")" && pwd)"
PAD=${2:-A:/golf-wt/pad-replay-data/hole10.pad}
powershell -NoProfile -File "$D/replay-to-tee.ps1" -Pad "$(cygpath -w "$PAD")" -Label "$1" | tail -1
python -I "$D/compare-dump.py" "$TEMP/claude/pad-replay/$1/final.raw" A:/golf-wt/pad-replay-data/hole10-ref/display_046_1920x1080.raw
