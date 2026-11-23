#!/bin/bash
# Test-only: replays A:/golf-wt/pad-replay-data/hole10.pad (recorded with scripts/tee-route) to the Green C.C. hole-10 tee,
# one boot per label, then compares the last frame with the recorded run's. Usage: replay-to-tee.sh LABEL [pad file]
# Take the lock first: until mkdir "$TEMP/claude/golf-heavy.lock" 2>/dev/null; do sleep 20; done ... rmdir it after.
D="$(cd "$(dirname "$0")" && pwd)"
PAD=${2:-A:/golf-wt/pad-replay-data/hole10.pad}
powershell -NoProfile -File "$D/replay-to-tee.ps1" -Pad "$(cygpath -w "$PAD")" -Label "$1" | tail -1
[ -d A:/golf-wt/pad-replay-data/hole10-ref ] && python -I "$D/compare-dump.py" "$TEMP/claude/pad-replay/$1/dump" A:/golf-wt/pad-replay-data/hole10-ref
