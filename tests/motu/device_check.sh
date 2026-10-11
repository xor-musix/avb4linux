#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Device check: short live loopback test of one (or more) MOTU devices in a
# topology, judged by a streaming glitch monitor instead of a recording.
#
#   ./tests/motu/device_check.sh <topo> [<host>...]     judge <host>s (default: all)
#   SECS=600 ./tests/motu/device_check.sh n31-swap 24ai
#
# 1. `motu_ds.sh check <topo>` must pass (apply the topology first).
# 2. The audio_check sine stimulus plays on all of our streams (as in
#    test_loopback.sh), and `arecord ... -t raw -` is piped straight into
#    glitch_monitor.py: no file, events print live, the verdict comes SECS
#    after the start instead of after a multi-GB analysis pass.
# 3. The topology maps every stream to its device, so each event names the
#    device and its output stream, and the devices not judged are controls.
#    An event is classified `copy-L` when the glitched frames equal, on all 8
#    channels, the frames L earlier (the 24Ai fault: copy-64).
#
# The monitor checks for glitches and silence, not amplitude, crosstalk or
# bit-exactness: that is test_loopback.sh's job.  Needs the stack up with the
# topology's N (avb_up.sh --num-streams), aplay/arecord, python3 + numpy.
#
# Env:
#   SECS=120          judged capture length (+ SKIP settle)
#   SKIP=3            settle window at the start, not judged
#   DEV=hw:igbavb  RATE=48000
#   CH=<8*N>          default: 8 x the running stack's stream count
#   CHECK=1           0 = skip the motu_ds.sh check (state known good)
#   LOGDIR=/dev/shm   where the arecord/gen logs go
#   MAP=              stream->device map instead of the topology's pairs, for
#                     streams the topology cannot describe (receive-only
#                     fan-out copies): "0-7=24ai:0" (see glitch_monitor.py)
#   SRC=              streams carrying another stream's tones: "4-7=0-3"
#                     (fanout.sh sets MAP and SRC)
# Exit: 0 PASS, 1 FAIL (events on a judged device), 2 INCONCLUSIVE / setup error
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TOOL=$ROOT/tests/audio/audio_check
DEV=${DEV:-hw:igbavb}
RATE=${RATE:-48000}
SECS=${SECS:-120}
SKIP=${SKIP:-3}
CHECK=${CHECK:-1}
LOGDIR=${LOGDIR:-/dev/shm}
PROCDIR=/proc/snd_igb_avb

say()  { printf '\033[1m== %s\033[0m\n' "$*"; }
die()  { printf '!! %s\n' "$*" >&2; exit 2; }

[ $# -ge 1 ] || die "usage: $0 <topo> [<host>...]"
TOPO=$1; shift
case $TOPO in */*|*.topo) TF=$TOPO ;; *) TF=$HERE/topologies/$TOPO.topo ;; esac
[ -r "$TF" ] || die "no topology '$TOPO' ($TF)"
for t in aplay arecord python3; do command -v $t >/dev/null || die "missing $t"; done
python3 -c 'import numpy' 2>/dev/null || die "python3 numpy missing"

N=$(grep -c '^stream_id=' "$PROCDIR/streams" 2>/dev/null) || die "stack not up ($PROCDIR/streams)"
CH=${CH:-$((N * 8))}

# <our-streams> <host> <device-streams>  ->  "lo-hi=host:dev0,..."  (same format as motu_ds.sh)
MAP=${MAP:-$(sed 's/#.*//' "$TF" | awk 'NF==3 { split($3, d, "-"); printf "%s%s=%s:%s", sep, $1, $2, d[1]; sep="," }')}
SRC=${SRC:-}
[ -n "$MAP" ] || die "$TF has no stream pairs"
HOSTS=$(tr ',' '\n' <<< "$MAP" | sed 's/.*=//; s/:.*//' | sort -u | tr '\n' ' ')
for h in "$@"; do
  case " $HOSTS " in *" $h "*) ;; *) die "$h is not in $TOPO (hosts: $HOSTS)" ;; esac
done
JUDGE=$(IFS=,; echo "$*")
TOPMAX=$(tr ',' '\n' <<< "$MAP" | awk -F'[=-]' '{ v = (NF == 3 ? $2 : $1) + 0; if (v > m) m = v } END { print m + 1 }')
[ "$TOPMAX" -le "$N" ] || die "the map needs N >= $TOPMAX, the stack runs N=$N"

say "device check: $TOPO, judge ${JUDGE:-all} ($HOSTS), ${SECS}s + ${SKIP}s settle, ${CH}ch @ $RATE"
if [ "$CHECK" = 1 ]; then
  "$HERE/motu_ds.sh" check "$TOPO" || die "topology $TOPO is not in place: ./tests/motu/motu_ds.sh apply $TOPO"
fi

[ -x "$TOOL" ] && [ "$TOOL" -nt "$TOOL.c" ] || make -C "$ROOT/tests/audio" >/dev/null || die "audio_check build failed"

GENLOG=$LOGDIR/device-check.gen.log
RECLOG=$LOGDIR/device-check.arecord.log
APLAY_PID=""
cleanup() { [ -n "$APLAY_PID" ] && kill "$APLAY_PID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT
trap 'exit 2' INT TERM

"$TOOL" gen - --raw --ch "$CH" --rate "$RATE" --secs $((SECS + SKIP + 10)) 2>"$GENLOG" |
  aplay -q -D "$DEV" -t raw -c "$CH" -r "$RATE" -f S32_LE >/dev/null 2>&1 &
APLAY_PID=$!
sleep 1
kill -0 "$APLAY_PID" 2>/dev/null || { APLAY_PID=""; die "aplay exited at once on $DEV (busy? CH=$CH for N=$N?): $(tail -2 "$GENLOG" | tr '\n' ' ')"; }
# "ch k -> 667+90*k Hz" from the generator: the monitor needs the same grid
read -r F0 DF < <(sed -nE 's/.*ch k -> ([0-9.]+)\+([0-9.]+)\*k Hz.*/\1 \2/p' "$GENLOG" | head -1)
[ -n "${F0:-}" ] || die "cannot read the tone grid from $GENLOG"

say "stimulus ${F0}+${DF}*k Hz playing; monitoring (events print live)"
TOT=$((SECS + ${SKIP%.*}))
# arecord starts 1 s late: the monitor must be draining the pipe (numpy imported,
# reader thread up) before the 44 ms ALSA capture buffer can fill
{ sleep 1; exec arecord -q -D "$DEV" -c "$CH" -r "$RATE" -f S32_LE -t raw -d "$TOT" - 2>"$RECLOG"; } |
  python3 -I "$HERE/glitch_monitor.py" --ch "$CH" --rate "$RATE" --f0 "$F0" --df "$DF" \
    --secs "$TOT" --skip "$SKIP" --map "$MAP" --judge "$JUDGE" ${SRC:+--src "$SRC"}
RC=${PIPESTATUS[1]}
OVR=$(grep -c 'overrun!!!' "$RECLOG" 2>/dev/null || true)
if [ "${OVR:-0}" -gt 0 ]; then
  echo "~~ arecord overran $OVR time(s) ($RECLOG): host-side gaps"
  RC=2
fi
exit "$RC"
