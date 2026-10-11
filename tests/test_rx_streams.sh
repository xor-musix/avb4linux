#!/bin/bash
# RX multi-stream gate: named counter checks on the i210 AVB listener path.
#
# Starts a background `arecord` on the AVB capture device (the RX datapath only
# runs, and only counts, while capture is open), samples
# /proc/snd_igb_avb/{streams,rx_engine} once capture is flowing and again
# DURATION seconds later, reads the kernel log for the window, and judges:
#
#   pkts     every EXPECT_LIVE stream gains rx_pkts at PKT_RATE/s +-5%;
#            every other stream gains 0
#   drops    rx_drops delta = 0 on every stream
#   clamp    no new "align clamp" kernel lines
#   fps      capture_frames_total delta / elapsed = RATE +-0.5%  (the shared
#            capture pointer must track the sample rate, whatever N)
#   xrun     0 "overrun!!!" from the background arecord, which is SIGSTOPped
#            for STALL_MS mid-window: a /dev/null reader never stalls on its
#            own, so without the stall a too-small capture buffer could
#            never show.  Usable headroom is ~1/3 of the buffer (arecord
#            wakes per period; RX delivers in bursts): measured 3..5 ms for
#            today's 32ch 512-frame buffer, ~32 ms for the 8ch
#            2048-frame one.  10 ms sits between with ~2x margin either side.
#   engine   rx_seq_errors, rx_clock_slips, rx_budget_hits, rx_ring_near_lap
#            deltas = 0, and no new "descriptor backlog" kernel lines
#   filters  every stream with filter>=0 has non-zero src_mac and dst_mac
#            (an enabled filter that matches nothing is a FAIL)
#
# It ends with ONE summary line, the one pasted into each step's Results table:
#   == RX: pkts=ok drops=FAIL clamp=FAIL fps=ok xrun=FAIL engine=ok filters=FAIL
#
# Run it as the normal user (audio group; journal readable, e.g. wheel/adm) with
# the AVB stack up and the MOTU loopback connected:
#
#   ./tests/test_rx_streams.sh                              # N=1
#   CH=32 EXPECT_LIVE=0,1,2,3 ./tests/test_rx_streams.sh    # N=4, all four streams
#
# Knobs:
#   CH=8                 capture channels (8 per listener stream)
#   RATE=48000           sample rate
#   DURATION=10          seconds between the two /proc samples
#   EXPECT_LIVE=0        comma list of stream indices that must receive packets
#   DEV=hw:igbavb        ALSA capture device
#   CHECKS=all           subset to run, e.g. CHECKS=pkts,drops
#   PKT_RATE=8000        expected packets/s per live stream (class A, 125 us)
#   STALL_MS=10          consumer stall injected mid-window (0 = none)
#   PERIOD=              capture period in frames (default: arecord's choice);
#                        must negotiate EXACTLY, as a DAW sets it (else INCONCLUSIVE)
#   PERIODS=2            periods per buffer when PERIOD is set
#   PROCDIR=/proc/snd_igb_avb
#
# Exit codes:
#   0 PASS
#   1 FAIL
#   2 INCONCLUSIVE / setup not ready (not a code verdict)

# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>

set -u

CH=${CH:-8}
RATE=${RATE:-48000}
DURATION=${DURATION:-10}
EXPECT_LIVE=${EXPECT_LIVE:-0}
DEV=${DEV:-hw:igbavb}
CHECKS=${CHECKS:-all}
PKT_RATE=${PKT_RATE:-8000}
STALL_MS=${STALL_MS:-10}
PERIOD=${PERIOD:-}
PERIODS=${PERIODS:-2}
PROCDIR=${PROCDIR:-/proc/snd_igb_avb}

ALL_CHECKS="pkts drops clamp fps xrun engine filters"

usage() {
  awk '
    NR == 1 { next }
    /^#/ { sub(/^# ?/, ""); print; next }
    { exit }
  ' "$0"
}

say(){  printf '\033[1m== %s\033[0m\n' "$*"; }
ok(){   printf '\033[32m%s\033[0m\n' "$*"; }
warn(){ printf '\033[33m!! %s\033[0m\n' "$*" >&2; }
inconc(){ printf '\033[33m~~ INCONCLUSIVE: %s\033[0m\n' "$*" >&2; exit 2; }

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  "") ;;
  *) echo "unknown arg: $1" >&2; usage; exit 2 ;;
esac

for v in CH RATE DURATION PKT_RATE STALL_MS PERIODS ${PERIOD:+PERIOD}; do
  case "${!v}" in ''|*[!0-9]*) inconc "$v must be a positive integer (got '${!v}')" ;; esac
done
[ "$DURATION" -ge 2 ] || inconc "DURATION must be >= 2"
case "$EXPECT_LIVE" in ''|*[!0-9,]*) inconc "EXPECT_LIVE must be a comma list of stream indices (got '$EXPECT_LIVE')" ;; esac

[ "$CHECKS" = all ] && CHECKS=$ALL_CHECKS
CHECKS=${CHECKS//,/ }
for c in $CHECKS; do
  case " $ALL_CHECKS " in *" $c "*) ;; *) inconc "unknown check '$c' (have: $ALL_CHECKS)" ;; esac
done
want(){ case " $CHECKS " in *" $1 "*) return 0 ;; esac; return 1; }

for t in arecord awk date grep journalctl mktemp sed sleep; do
  command -v "$t" >/dev/null || inconc "missing required tool: $t"
done

STREAMS=$PROCDIR/streams
RXE=$PROCDIR/rx_engine
[ -r "$STREAMS" ] && [ -r "$RXE" ] || inconc "$PROCDIR is missing. Is snd_igb_avb loaded?  sudo ./avb_up.sh <iface> ..."

BENCH=/sys/module/snd_igb_avb/parameters/avb_rx_bench
if [ -e "$BENCH" ] && [ "$(cat "$BENCH" 2>/dev/null)" = "1" ]; then
  inconc "snd_igb_avb is loaded with avb_rx_bench=1 (broad RX filter) -- reload without it"
fi

# The kernel log is evidence for clamp/engine; unreadable is a setup problem.
if want clamp || want engine; then
  [ -n "$(journalctl -k -n 1 -q --no-pager 2>/dev/null)" ] ||
    inconc "cannot read the kernel journal (journalctl -k). Run as a user in the wheel/adm/systemd-journal group, or set CHECKS without clamp,engine."
fi

TMP=$(mktemp -d)
AREC_PID=""
cleanup(){ [ -n "$AREC_PID" ] && kill "$AREC_PID" 2>/dev/null; wait 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

# snap <tag>: copy both proc files + a ns timestamp, atomically enough for deltas
snap() {
  cat "$STREAMS" > "$TMP/$1.streams" 2>/dev/null || return 1
  cat "$RXE" > "$TMP/$1.rxe" 2>/dev/null || return 1
  date +%s%N > "$TMP/$1.t"
}

# rx_rows <tag>: "idx sid src dst filter pkts drops align", one row per stream
rx_rows() {
  awk '
    /^  rx: expected_sid=/ {
      sid=src=dst=filt=pk=dr=al="?";
      for (i = 2; i <= NF; i++) {
        split($i, kv, "=");
        if (kv[1] == "expected_sid") sid = kv[2];
        else if (kv[1] == "src_mac") src = kv[2];
        else if (kv[1] == "dst_mac") dst = kv[2];
        else if (kv[1] == "filter") filt = kv[2];
        else if (kv[1] == "rx_pkts") pk = kv[2];
        else if (kv[1] == "rx_drops") dr = kv[2];
        else if (kv[1] == "align") al = kv[2];
      }
      print n++, sid, src, dst, filt, pk, dr, al;
    }
  ' "$TMP/$1.streams"
}

rxe_field() {  # rxe_field <tag> <key>
  awk -F= -v k="$2" '$1 == k { print $2; found=1 } END { if (!found) exit 1 }' "$TMP/$1.rxe"
}

say "preflight"
echo "  device=$DEV  ch=$CH  rate=$RATE  duration=${DURATION}s  expect-live=$EXPECT_LIVE  pkt-rate=${PKT_RATE}/s  stall=${STALL_MS}ms"
echo "  checks: $CHECKS"

snap pre || inconc "could not read $PROCDIR"
NSTREAMS=$(rx_rows pre | wc -l)
[ "$NSTREAMS" -gt 0 ] || inconc "no '  rx: expected_sid=' rows in $STREAMS (module too old for the per-stream RX exports?)"
if want fps; then
  rxe_field pre capture_frames_total >/dev/null ||
    inconc "$RXE has no capture_frames_total (module predates the per-stream RX exports)"
fi
for s in ${EXPECT_LIVE//,/ }; do
  [ "$s" -lt "$NSTREAMS" ] || inconc "EXPECT_LIVE stream $s does not exist (num_streams=$NSTREAMS)"
done
echo "  num_streams=$NSTREAMS"

# ---- capture in the background; sample inside its lifetime ----------------
since="@$(date +%s)"
say "capture ${DURATION}s on $DEV (background arecord) and sample /proc"
PERIOD_ARGS=()
[ -n "$PERIOD" ] && PERIOD_ARGS=(--period-size="$PERIOD" --buffer-size=$((PERIOD * PERIODS)))
# -t raw: a WAV is split at 2 GiB (45 s at 248 ch), and arecord then fails to
# create /dev/null-01 and exits -- capture stops mid-window.
arecord -D "$DEV" -c "$CH" -r "$RATE" -f S32_LE -t raw "${PERIOD_ARGS[@]}" -d $((DURATION + 2)) /dev/null \
  > "$TMP/arecord.log" 2>&1 &
AREC_PID=$!
sleep 1
if ! kill -0 "$AREC_PID" 2>/dev/null; then
  AREC_PID=""
  sed 's/^/  /' "$TMP/arecord.log" >&2
  inconc "arecord could not open $DEV ($CH ch @ $RATE) -- busy, wrong CH for num_streams=$NSTREAMS (8 per stream), or no permission (audio group)"
fi
snap start || inconc "could not read $PROCDIR at start"
# the buffer the capture actually negotiated (usable headroom is ~1/3 of it)
HWP=$(awk '/^(period|buffer)_size:/ { printf "%s=%s ", $1, $2 }' \
        /proc/asound/igbavb/pcm0c/sub0/hw_params 2>/dev/null)
BUF=$(echo "$HWP" | sed -nE 's/.*buffer_size:=([0-9]+).*/\1/p')
[ -n "$BUF" ] && echo "  negotiated ${HWP% }(buffer = $(awk -v b="$BUF" -v r="$RATE" 'BEGIN { printf "%.1f", b * 1000 / r }') ms)"
if [ -n "$PERIOD" ]; then
  GOT=$(echo "$HWP" | sed -nE 's/.*period_size:=([0-9]+).*/\1/p')
  [ "$GOT" = "$PERIOD" ] && [ "$BUF" = $((PERIOD * PERIODS)) ] ||
    inconc "asked for period $PERIOD x $PERIODS, ALSA negotiated period ${GOT:-?} buffer ${BUF:-?} -- a DAW setting this size exactly would fail to open"
fi
half=$((DURATION / 2))
sleep "$half"
if [ "$STALL_MS" -gt 0 ]; then
  kill -STOP "$AREC_PID" 2>/dev/null
  sleep "$(awk -v m="$STALL_MS" 'BEGIN { printf "%.3f", m / 1000 }')"
  kill -CONT "$AREC_PID" 2>/dev/null
fi
sleep $((DURATION - half))
snap end || inconc "could not read $PROCDIR at end"
wait "$AREC_PID"; AREC_RC=$?; AREC_PID=""

T0=$(cat "$TMP/start.t"); T1=$(cat "$TMP/end.t")
ELAPSED=$(awk -v a="$T0" -v b="$T1" 'BEGIN { printf "%.3f", (b - a) / 1e9 }')

journalctl -k --since "$since" --no-pager -q 2>/dev/null > "$TMP/klog" || true

# ---- per-stream table ----------------------------------------------------
rx_rows start > "$TMP/rows.start"
rx_rows end   > "$TMP/rows.end"
[ "$(wc -l < "$TMP/rows.start")" = "$(wc -l < "$TMP/rows.end")" ] ||
  inconc "stream count changed between samples (module reloaded?)"
paste -d' ' "$TMP/rows.start" "$TMP/rows.end" > "$TMP/rows"
# rows: $1-$8 start, $9-$16 end -- each: idx sid src dst filter pkts drops align
awk -v el="$ELAPSED" '
  BEGIN { printf "  %-3s %-16s %-17s %-17s %-6s %-10s %-9s %-10s %s\n",
                 "idx", "expected_sid", "src_mac", "dst_mac", "filter", "pkts/s", "d_pkts", "d_drops", "align" }
  { printf "  %-3s %-16s %-17s %-17s %-6s %-10.0f %-9d %-10d %s\n",
           $1, $10, $11, $12, $13, ($14 - $6) / el, $14 - $6, $15 - $7, $16 }
' "$TMP/rows"
echo "  elapsed=${ELAPSED}s  arecord rc=$AREC_RC"

# ---- judge ---------------------------------------------------------------
declare -A RES
FAIL=0
verdict() {  # verdict <check> <ok|reason...>
  local c=$1 why; shift; why="$*"; why=${why% }
  if [ "$why" = ok ]; then RES[$c]=ok; printf '  %-8s \033[32mok\033[0m\n' "$c"
  else RES[$c]=FAIL; FAIL=1; printf '  %-8s \033[31mFAIL\033[0m %s\n' "$c" "$why"; fi
}

say "checks"

live_any=0
if want pkts; then
  why=""
  while read -r idx _ _ _ _ pk0 _ _ _ _ _ _ _ pk1 _ _; do
    d=$((pk1 - pk0))
    case ",$EXPECT_LIVE," in
      *",$idx,"*)
        [ "$d" -gt 0 ] && live_any=1
        awk -v d="$d" -v el="$ELAPSED" -v r="$PKT_RATE" \
          'BEGIN { x = d / el; exit !(x < r * 0.95 || x > r * 1.05) }' &&
          why+="s$idx=$(awk -v d="$d" -v el="$ELAPSED" 'BEGIN { printf "%.0f", d / el }')/s(want ${PKT_RATE}) "
        ;;
      *) [ "$d" -ne 0 ] && why+="s$idx=+$d(want 0) " ;;
    esac
  done < "$TMP/rows"
  [ "$live_any" = 1 ] || inconc "no EXPECT_LIVE stream ($EXPECT_LIVE) received any packet -- loopback/ACMP not connected or gPTP not locked (setup, not code)"
  [ -z "$why" ] && verdict pkts ok || verdict pkts "$why"
fi

if want drops; then
  why=$(awk '{ d = $15 - $7; if (d != 0) printf "s%s=+%d ", $1, d }' "$TMP/rows")
  [ -z "$why" ] && verdict drops ok || verdict drops "$why"
fi

if want clamp; then
  n=$(grep -c 'align clamp' "$TMP/klog")
  [ "$n" -eq 0 ] && verdict clamp ok ||
    verdict clamp "$n new 'align clamp' lines (ratelimited), e.g.: $(grep -m1 'align clamp' "$TMP/klog" | sed 's/.*avb rx: //')"
fi

if want fps; then
  f0=$(rxe_field start capture_frames_total); f1=$(rxe_field end capture_frames_total)
  fps=$(awk -v a="$f0" -v b="$f1" -v el="$ELAPSED" 'BEGIN { printf "%.0f", (b - a) / el }')
  awk -v x="$fps" -v r="$RATE" 'BEGIN { exit !(x >= r * 0.995 && x <= r * 1.005) }' &&
    verdict fps ok || verdict fps "${fps} frames/s (want $RATE +-0.5%)"
fi

if want xrun; then
  n=$(grep -c 'overrun' "$TMP/arecord.log")
  [ "$n" -eq 0 ] && verdict xrun ok ||
    verdict xrun "$n overrun(s) from arecord (${STALL_MS}ms stall vs ${BUF:-?}-frame buffer)"
fi

if want engine; then
  why=""
  for k in rx_seq_errors rx_clock_slips rx_budget_hits rx_ring_near_lap; do
    a=$(rxe_field start "$k") && b=$(rxe_field end "$k") || inconc "$RXE has no $k"
    [ $((b - a)) -ne 0 ] && why+="$k=+$((b - a)) "
  done
  n=$(grep -c 'descriptor backlog' "$TMP/klog")
  [ "$n" -ne 0 ] && why+="'descriptor backlog' x$n "
  [ -z "$why" ] && verdict engine ok || verdict engine "$why"
fi

if want filters; then
  Z=00:00:00:00:00:00
  why=$(awk -v z="$Z" '$13 >= 0 && ($11 == z || $12 == z) {
          printf "s%s filter=%s src=%s dst=%s ", $1, $13, ($11 == z ? "0" : "set"), ($12 == z ? "0" : "set") }' "$TMP/rows")
  [ -z "$why" ] && verdict filters ok || verdict filters "$why"
fi

summary="== RX:"
for c in $ALL_CHECKS; do want "$c" && summary+=" $c=${RES[$c]}"; done
echo
printf '\033[1m%s\033[0m\n' "$summary"

if [ "$FAIL" = 0 ]; then
  ok "  PASS -- RX counters clean for EXPECT_LIVE=$EXPECT_LIVE at N=$NSTREAMS"
  exit 0
fi
warn "FAIL -- see the check rows above"
exit 1
