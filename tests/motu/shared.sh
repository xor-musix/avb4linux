#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Shared-listener test: several of OUR inputs listen to ONE
# talker stream.  On top of a topology's loopback pairs (our input k <- device
# uid j), each copy input connects to the device uid its source input already
# receives -- no device routing, the talker sends one stream and the kernel
# writes every packet into each listener's slice:
#   SHARE="4-7=0-3"   our inputs 4-7 <- the talker streams of our inputs 0-3
#
#   ./tests/motu/shared.sh <topo> apply    motu_ds.sh apply <topo> + connect the copies
#   ./tests/motu/shared.sh <topo> check    read-only: pairs, copies, kernel ids shared
#   ./tests/motu/shared.sh <topo> exact    short capture: each copy's slice == its source's, bit for bit
#   SECS=120 ./tests/motu/shared.sh <topo> run     check + exact, then device_check.sh (copies = twins)
#   ./tests/motu/shared.sh <topo> leave <k>   disconnect our input k; the inputs sharing its stream must keep receiving
#   ./tests/motu/shared.sh <topo> join <k>    reconnect it (to the stream the topology / SHARE gives it)
#   ./tests/motu/shared.sh <topo> undo     disconnect the copies (motu_ds.sh restore undoes everything)
#
# Needs the stack at N = topology pairs + copies, its stream 0 on the
# topology's stream-0 device:
#   sudo AVB_DEVICE_SOURCE_MAC=<device MAC> ./avb_up.sh <iface> 48000 --num-streams <N>
# Every write goes through motu_ds.sh (snapshot first).
#
# Env: SHARE="4-7=0-3"  SECS=120 (run)  EXACT_SECS=3  SETTLE=3 (leave/join)
#      DEV=hw:igbavb  RATE=48000  LOGDIR=/dev/shm
# Exit: 0 ok, 1 FAIL, 2 setup error
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DS=$HERE/motu_ds.sh
TOOL=$ROOT/tests/audio/audio_check
SHARE=${SHARE:-4-7=0-3}
EXACT_SECS=${EXACT_SECS:-3}
SETTLE=${SETTLE:-3}
DEV=${DEV:-hw:igbavb}
RATE=${RATE:-48000}
LOGDIR=${LOGDIR:-/dev/shm}
PROCDIR=${PROCDIR:-/proc/snd_igb_avb}
NULL_EID=0000000000000000

say() { printf '\033[1m== %s\033[0m\n' "$*"; }
ok()  { printf '\033[32mok\033[0m   %s\n' "$*"; }
bad() { printf '\033[31mFAIL\033[0m %s\n' "$*"; }
die() { printf '!! %s\n' "$*" >&2; exit 2; }

[ $# -ge 2 ] || die "usage: $0 <topo> apply|check|exact|run|leave <k>|join <k>|undo"
TOPO=$1 CMD=$2
case $TOPO in */*|*.topo) TF=$TOPO ;; *) TF=$HERE/topologies/$TOPO.topo ;; esac
[ -r "$TF" ] || die "no topology '$TOPO' ($TF)"

span() { local lo=${1%-*} hi=${1#*-}; seq "$lo" "$hi"; }

# the topology's pairs: PH[k] PJ[k] = host, device uid of our input k
declare -A PH=() PJ=()
while read -r ours host devs; do
  ks=($(span "$ours")) js=($(span "$devs"))
  [ ${#ks[@]} = ${#js[@]} ] || die "$TF: $ours and $devs differ in length"
  for i in "${!ks[@]}"; do PH[${ks[i]}]=$host PJ[${ks[i]}]=${js[i]}; done
done < <(sed 's/#.*//' "$TF" | awk 'NF == 3')
NPAIRS=${#PH[@]}
[ "$NPAIRS" -gt 0 ] || die "$TF has no stream pairs"

# the copies: SRCOF[c] = our input whose talker stream copy c shares
declare -A SRCOF=()
for item in ${SHARE//,/ }; do
  d=($(span "${item%=*}")) s=($(span "${item#*=}"))
  [ ${#d[@]} = ${#s[@]} ] || die "SHARE $item: ranges differ in length"
  for i in "${!d[@]}"; do
    [ -n "${PH[${s[i]}]:-}" ] || die "SHARE $item: our input ${s[i]} is not in $TOPO"
    [ -z "${PH[${d[i]}]:-}" ] || die "SHARE $item: our input ${d[i]} is a $TOPO pair, not a free copy"
    SRCOF[${d[i]}]=${s[i]}
  done
done
COPIES=$(printf '%s\n' "${!SRCOF[@]}" | sort -n | tr '\n' ' ')
NCOPY=${#SRCOF[@]}
NWANT=$((NPAIRS + NCOPY))

# host and uid our input k listens to, pair or copy
host_of() { local k=$1; [ -n "${SRCOF[$k]:-}" ] && k=${SRCOF[$k]}; echo "${PH[$k]}"; }
uid_of()  { local k=$1; [ -n "${SRCOF[$k]:-}" ] && k=${SRCOF[$k]}; echo "${PJ[$k]}"; }
# inputs listening to the same talker stream as k (pairs and copies), k excluded
siblings() {
  local k=$1 o
  for o in "${!PH[@]}" "${!SRCOF[@]}"; do
    [ "$o" != "$k" ] && [ "$(host_of "$o")" = "$(host_of "$k")" ] && [ "$(uid_of "$o")" = "$(uid_of "$k")" ] && echo "$o"
  done | sort -n | tr '\n' ' '
}

stack_n() { grep -c '^stream_id=' "$PROCDIR/streams" 2>/dev/null; }
need_stack() {
  local n; n=$(stack_n) || die "stack not up ($PROCDIR/streams)"
  [ "$n" = "$NWANT" ] || die "stack N=$n, $TOPO + $NCOPY copies needs N=$NWANT; the avb_up.sh line: IFACE=<iface> EXTRA_STREAMS=$NCOPY $DS --dry-run apply $TOPO"
}
# kernel per-stream fields from /proc streams: "<k> <expected_sid> <rx_pkts> <seq_errors>"
rx_table() {
  awk '/^  rx: expected_sid=/ { k++; sid[k] = $2; pk[k] = $8 }
       /^  rx: seq_errors=/   { se[k] = $2 }
       END { for (i = 1; i <= k; i++) { sub(/.*=/, "", sid[i]); sub(/.*=/, "", pk[i]); sub(/.*=/, "", se[i]);
                                        print i - 1, sid[i], pk[i], se[i] } }' "$PROCDIR/streams"
}

our_eid_on() { # <host>: our entity id as <host> sees it
  curl -s -m 5 "http://$1/datastore" |
    jq -r 'to_entries[] | select(.key | test("^avb/[0-9a-f]+/model_name$")) | select(.value == "Linux AVB") | .key | split("/")[1]' | head -1
}
dev_eid() { curl -s -m 5 "http://$1/datastore" | jq -r .uid; }

# Kernel side of the setup, cheap: every copy holds its source's non-zero
# stream id (except `skip`, an input that is meant to be left).  Our inputs
# reset on a stack restart, so after one this fails until `apply` runs again.
in_place() { # [skip]
  local tbl c s sc ss bad=""
  tbl=$(rx_table)
  for c in $COPIES; do
    s=${SRCOF[$c]}
    [ "$c" = "${1:-}" ] || [ "$s" = "${1:-}" ] && continue
    sc=$(awk -v k="$c" '$1 == k { print $2 }' <<< "$tbl")
    ss=$(awk -v k="$s" '$1 == k { print $2 }' <<< "$tbl")
    if [ -z "$sc" ] || [ "$sc" = 0000000000000000 ] || [ "$sc" != "$ss" ]; then
      bad+=" $c($sc)/$s($ss)"
    fi
  done
  [ -z "$bad" ] || die "not in place, copy/source stream ids differ:$bad -- stack restarted? our inputs reset then: $0 $TOPO apply"
}

connect() { # <k> <talker-host|0> <uid>
  local h; h=$(host_of "$1")
  "$DS" connect "$h" us "$1" "$2" "$3" | tail -1
}

check() {
  local rc=0 c s h us e got want sc ss
  if EXTRA_STREAMS=$NCOPY "$DS" check "$TOPO" >/dev/null 2>&1; then
    ok "$TOPO pairs ($NPAIRS) + stack N=$NWANT"
  else
    EXTRA_STREAMS=$NCOPY "$DS" check "$TOPO" 2>&1 | grep -i fail; rc=1
  fi
  declare -A seen=()
  for c in $COPIES; do
    s=${SRCOF[$c]} h=$(host_of "$c")
    if [ -z "${seen[$h]:-}" ]; then us=$(our_eid_on "$h") e=$(dev_eid "$h"); seen[$h]="$us $e"; fi
    read -r us e <<< "${seen[$h]}"
    [ -n "$us" ] || { bad "our entity is not in $h's datastore"; rc=1; continue; }
    got=$("$DS" get "$h" "avb/$us/cfg/0/input_streams/$c/talker" 2>/dev/null | tr -d '"')
    want="$e:$(uid_of "$c")"
    [ "$got" = "$want" ] || { bad "our input $c <- '$got' (want $want = $h uid $(uid_of "$c"), shared with input $s)"; rc=1; continue; }
    # the kernel must have the SAME stream id for copy and source (= one talker stream, fanned out)
    read -r _ sc _ _ < <(rx_table | awk -v k="$c" '$1 == k')
    read -r _ ss _ _ < <(rx_table | awk -v k="$s" '$1 == k')
    if [ -z "$sc" ] || [ "$sc" = 0000000000000000 ] || [ "$sc" != "$ss" ]; then
      bad "kernel stream $c expected_sid=$sc, stream $s $ss (want the same, non-zero)"; rc=1; continue
    fi
    ok "our input $c <- $h uid $(uid_of "$c") = input $s's stream $sc"
  done
  return $rc
}

# Bit-exact: with the stimulus playing, every copy's 8ch slice must equal its
# source's in every frame (the kernel writes one packet into both at the same
# position), and not be silent.
exact() {
  local raw=$LOGDIR/shared-exact.raw ch=$((NWANT * 8)) aplay_pid f0 df rc
  [ -x "$TOOL" ] && [ "$TOOL" -nt "$TOOL.c" ] || make -C "$ROOT/tests/audio" >/dev/null || die "audio_check build failed"
  "$TOOL" gen - --raw --ch "$ch" --rate "$RATE" --secs $((EXACT_SECS + 5)) 2>"$LOGDIR/shared-exact.gen.log" |
    aplay -q -D "$DEV" -t raw -c "$ch" -r "$RATE" -f S32_LE >/dev/null 2>&1 &
  aplay_pid=$!
  sleep 1
  arecord -q -D "$DEV" -c "$ch" -r "$RATE" -f S32_LE -t raw -d $((EXACT_SECS + 1)) "$raw" 2>"$LOGDIR/shared-exact.arecord.log"
  kill "$aplay_pid" 2>/dev/null; wait 2>/dev/null
  python3 -I - "$raw" "$ch" "$RATE" "$(for c in $COPIES; do printf '%s=%s ' "$c" "${SRCOF[$c]}"; done)" <<'EOF'
import sys
import numpy as np
raw, ch, rate, pairs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4].split()
x = np.fromfile(raw, dtype="<i4")
x = x[: len(x) // ch * ch].reshape(-1, ch)[rate:]          # drop the first second (capture start)
if len(x) < rate:
    sys.exit("~~ short capture: %d frames" % len(x))
rc = 0
for p in pairs:
    c, s = map(int, p.split("="))
    a, b = x[:, c * 8:(c + 1) * 8], x[:, s * 8:(s + 1) * 8]
    diff = np.flatnonzero(np.any(a != b, axis=1))
    silent = not np.any(b)
    if silent:
        print("FAIL stream %d (source of %d) is silent" % (s, c)); rc = 1
    elif len(diff):
        print("FAIL stream %d != stream %d in %d of %d frames (first at frame %d)" % (c, s, len(diff), len(a), diff[0])); rc = 1
    else:
        print("ok   stream %d == stream %d, %d frames bit-exact" % (c, s, len(a)))
sys.exit(rc)
EOF
  rc=$?
  rm -f "$raw"
  return $rc
}

# rx_pkts and seq_errors of the given streams, SETTLE seconds apart.  The
# kernel classifies RX packets only while a capture is open (no capture = the
# ring is not drained, every stream +0), so one runs meanwhile.
rx_deltas() { # <streams...>
  local before after k rec
  arecord -q -D "$DEV" -c $((NWANT * 8)) -r "$RATE" -f S32_LE -t raw -d $((SETTLE + 3)) /dev/null \
    2>"$LOGDIR/shared-rx.arecord.log" &
  rec=$!
  sleep 1
  if ! kill -0 "$rec" 2>/dev/null; then
    # EIO = ALSA's read timeout (max(100 ms, 1.1 x buffer)): the capture pointer
    # did not move, i.e. the reference stream 0 receives nothing
    grep -q 'Input/output error' "$LOGDIR/shared-rx.arecord.log" && { echo STALL; return; }
    die "arecord exited at once on $DEV (busy?): $(tail -1 "$LOGDIR/shared-rx.arecord.log")"
  fi
  before=$(rx_table); sleep "$SETTLE"; after=$(rx_table)
  kill "$rec" 2>/dev/null; wait "$rec" 2>/dev/null
  for k in "$@"; do
    join <(awk -v k="$k" '$1 == k' <<< "$before") <(awk -v k="$k" '$1 == k' <<< "$after") |
      awk '{ printf "%s %d %d\n", $1, $6 - $3, $7 - $4 }'
  done
}

case $CMD in
apply)
  need_stack
  say "shared $TOPO: $NPAIRS pairs + copies $(for c in $COPIES; do printf 'in %s=in %s ' "$c" "${SRCOF[$c]}"; done)"
  EXTRA_STREAMS=$NCOPY "$DS" apply "$TOPO" || die "apply of $TOPO failed"
  for c in $COPIES; do connect "$c" "$(host_of "$c")" "$(uid_of "$c")"; done
  "$0" "$TOPO" check
  ;;
check)
  need_stack
  check && ok "shared $TOPO in place: $NCOPY copies" || exit 1
  ;;
exact)
  need_stack
  in_place
  say "bit-exact: ${EXACT_SECS}s at $((NWANT * 8))ch"
  exact || exit 1
  ;;
run)
  need_stack
  check || die "not in place: $0 $TOPO apply"
  exact || exit 1
  MAP=$(for k in $(printf '%s\n' "${!PH[@]}" "${!SRCOF[@]}" | sort -n); do printf '%s=%s:%s,' "$k" "$(host_of "$k")" "$(uid_of "$k")"; done)
  SRC=$(for c in $COPIES; do printf '%s=%s,' "$c" "${SRCOF[$c]}"; done)
  CHECK=0 MAP=${MAP%,} SRC=${SRC%,} EXTRA_STREAMS=$NCOPY exec "$HERE/device_check.sh" "$TF"
  ;;
leave|join)
  [ $# = 3 ] || die "usage: $0 <topo> $CMD <k>"
  k=$3
  [ -n "$(host_of "$k")" ] || die "our input $k is neither a $TOPO pair nor a copy"
  need_stack
  sib=$(siblings "$k"); sib=${sib% }
  [ -n "$sib" ] || echo "~~ input $k shares its talker stream with no other input"
  # The group (k + siblings) as it is now: every bound member holds the same
  # stream id, and for a leave k itself is bound.  Members left earlier (id 0)
  # are allowed -- churn leaves several in turn -- and are expected idle.
  tbl=$(rx_table) gsid="" left=" "
  for s in $k $sib; do
    sd=$(awk -v k="$s" '$1 == k { print $2 }' <<< "$tbl")
    if [ -z "$sd" ] || [ "$sd" = 0000000000000000 ]; then
      left+="$s "
    elif [ -z "$gsid" ]; then
      gsid=$sd
    elif [ "$sd" != "$gsid" ]; then
      die "not in place: input $s holds $sd, its group $gsid -- stack restarted? our inputs reset then: $0 $TOPO apply"
    fi
  done
  if [ "$CMD" = leave ]; then
    case $left in *" $k "*) die "input $k is not connected (stream id 0): nothing to leave" ;; esac
  fi
  if [ "$CMD" = leave ]; then
    connect "$k" 0 0
  else
    connect "$k" "$(host_of "$k")" "$(uid_of "$k")"
  fi
  say "after $CMD of input $k: rx over ${SETTLE}s (stream pkts seq_errors)"
  rc=0
  while read -r s d e; do
    if [ "$s" = STALL ]; then
      bad "capture stalled (arecord EIO): stream 0, the capture reference, receives nothing"; rc=1; continue
    fi
    want=live
    if [ "$s" = "$k" ]; then
      [ "$CMD" = leave ] && want=idle
    else
      case $left in *" $s "*) want="idle (left earlier)" ;; esac
    fi
    if [ "$want" = live ] && { [ "$d" -le 0 ] || [ "$e" -ne 0 ]; }; then
      bad "stream $s: +$d pkts, +$e seq errors (want receiving, 0 errors)"; rc=1
    elif [ "$want" != live ] && [ "$d" -gt 0 ]; then
      bad "stream $s: +$d pkts while not connected (want 0)"; rc=1
    else
      ok "stream $s: +$d pkts, +$e seq errors ($want)"
    fi
  done < <(rx_deltas "$k" $sib)
  echo "   MSRP: journalctl -t avb-user --since -1min | grep -E 'listener\\[($k|${sib// /|})\\]'"
  exit $rc
  ;;
undo)
  for c in $COPIES; do connect "$c" 0 0; done
  ;;
*) die "unknown command $CMD" ;;
esac
