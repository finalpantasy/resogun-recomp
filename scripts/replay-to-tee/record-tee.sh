#!/bin/bash
# Test-only: one tee-route attempt that records the pad the guest receives (RESOGUN_PAD_RECORD) into $OUT.
# Needs scripts/tee-route (TEE=<its dir>) and the golf-heavy lock. Check the last frame shows the tee view (px 60,50 = D41018),
# then keep $OUT as hole10.pad and the last $TEMP/claude/dump/display_*.raw as the reference frame.
# Usage: record-tee.sh OUT.pad [timeout]
TEE=${TEE:-/a/golf-wt/tee-route/scripts/tee-route}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "/a/Everybodys Golf PS4"
SAVE='A:\golf-wt\pad-replay-state\save'
S="$(cygpath -u "$SAVE")/savedata/1/CUSA04687"
cp work/saves/after-tutorial/DATA*.BIN "$S/SAVEDATA/" && cp work/saves/after-tutorial/DATA*.BIN "$S/SAVEDATA.sce_backup/data/"
export GOLF_NP=offline GHOST_MEM_GB=32 RESOGUN_IME_TEXT=Golfer RESOGUN_FRAME_STATS=1
export EG_EXE="$(cygpath -w "$ROOT/out/everybodys-golf.exe")" RESOGUN_PAD_RECORD="$(cygpath -w "$1")"
rm -rf "$TEMP/claude/dump/display_"*; : > "$TEMP/claude/seq.log"
powershell -NoProfile -File "$TEE/boot-seq.ps1" -Timeout ${2:-570} -Warm 95 -Pulse 4000 -Gap 1500 -Seq "$(cat "$TEE/tee-route.seq")" -Kill -Save "$SAVE" 2>&1 | tail -2
tr '\n' ' ' < "$TEMP/claude/seq.log"; echo
ls "$TEMP/claude/dump/display_"*.raw | tail -1
