#!/bin/bash
# Runtime check: PHC-derived timer times and shadow period pacing.
#
# Run while the AVB stack is up and playback is active, e.g. after:
#
#   GPTP_SYNTONIZE=-S sudo ./avb_up.sh <iface> 48000
#
# MODE=shadow validates the default shadow path: tx_engine_enabled=0, timer active,
# ticks/PHC/shadow pointer moving, drift bounded, no shadow XRUN growth, and fresh
# "avb period shadow drift=..." logs. MODE=live keeps the PHC-vs-RX log
# comparison for tx_engine_enabled=1. MODE=auto selects from the first procfs
# sample.
#
# Knobs:
#   MODE=auto                auto | shadow | live
#   DURATION=10              seconds to sample /proc and logs
#   INTERVAL=1               /proc sample interval in seconds
#   PROCFS=/proc/snd_igb_avb/tx_engine
#   MIN_LOG_LINES=3          minimum fresh avb phc lines required
#   MAX_ABS_DELTA_NS=1000000 maximum absolute PHC-vs-RX delta, default 1 ms
#   MAX_SPREAD_NS=1000000    maximum delta range during the sample, default 1 ms
#   MAX_SHADOW_DRIFT_SAMPLES=1200 maximum absolute shadow-vs-legacy drift
#   TICKS_MIN_PER_S=6000     warning threshold for 125 us timer, expected ~8000/s
#   TICKS_MAX_PER_S=10000    warning threshold for 125 us timer, expected ~8000/s
#
# Exit codes:
#   0 PASS
#   1 FAIL
#   2 INCONCLUSIVE / setup not ready

# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>

set -u

MODE=${MODE:-auto}
DURATION=${DURATION:-10}
INTERVAL=${INTERVAL:-1}
PROCFS=${PROCFS:-/proc/snd_igb_avb/tx_engine}
MIN_LOG_LINES=${MIN_LOG_LINES:-3}
MAX_ABS_DELTA_NS=${MAX_ABS_DELTA_NS:-1000000}
MAX_SPREAD_NS=${MAX_SPREAD_NS:-1000000}
MAX_SHADOW_DRIFT_SAMPLES=${MAX_SHADOW_DRIFT_SAMPLES:-1200}
TICKS_MIN_PER_S=${TICKS_MIN_PER_S:-6000}
TICKS_MAX_PER_S=${TICKS_MAX_PER_S:-10000}

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
fail(){ printf '\033[31mXX %s\033[0m\n' "$*" >&2; FAIL=1; }
inconc(){ printf '\033[33m~~ INCONCLUSIVE: %s\033[0m\n' "$*" >&2; exit 2; }

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  "") ;;
  *) echo "unknown arg: $1" >&2; usage; exit 2 ;;
esac

case "$MODE" in
  auto|shadow|live) ;;
  *) inconc "MODE must be auto, shadow, or live" ;;
esac

for t in awk cat date grep journalctl mktemp sed sleep tail tr wc; do
  command -v "$t" >/dev/null || inconc "missing required tool: $t"
done

[ -r "$PROCFS" ] || inconc "$PROCFS is not readable. Is snd_igb_avb loaded?"

TMP=$(mktemp -d)
SAMPLES="$TMP/proc-samples.txt"
LOGS="$TMP/kernel-avb-phc.log"
SHADOW_LOGS="$TMP/kernel-avb-shadow.log"
WARN_LOGS="$TMP/kernel-warns.log"
DELTAS="$TMP/deltas.txt"
FAIL=0

cleanup(){ rm -rf "$TMP"; }
trap cleanup EXIT

read_proc_field() {
  local key=$1 file=$2
  awk -F= -v k="$key" '$1 == k { print $2; found=1 } END { if (!found) exit 1 }' "$file"
}

sample_proc() {
  local snap="$TMP/proc-one.txt"
  cat "$PROCFS" > "$snap" 2>/dev/null || return 1

  local enabled active ticks pres launch acc shadow drift xrun now_ns
  enabled=$(read_proc_field tx_engine_enabled "$snap") || return 1
  active=$(read_proc_field tx_timer_active "$snap") || return 1
  ticks=$(read_proc_field tx_engine_ticks "$snap") || return 1
  pres=$(read_proc_field tx_phc_pres "$snap") || return 1
  launch=$(read_proc_field tx_phc_launch "$snap") || return 1
  acc=$(read_proc_field tx_period_acc "$snap") || return 1
  shadow=$(read_proc_field tx_shadow_pptr "$snap") || return 1
  drift=$(read_proc_field tx_drift_samples "$snap") || return 1
  xrun=$(read_proc_field tx_xrun "$snap") || return 1
  now_ns=$(date +%s%N)

  printf '%s %s %s %s %s %s %s %s %s %s\n' \
    "$now_ns" "$enabled" "$active" "$ticks" "$pres" "$launch" "$acc" "$shadow" "$drift" "$xrun" >> "$SAMPLES"
}

say "preflight"
echo "  procfs=$PROCFS"
echo "  mode=$MODE"
echo "  duration=${DURATION}s interval=${INTERVAL}s"
echo "  max-abs-delta=${MAX_ABS_DELTA_NS}ns max-spread=${MAX_SPREAD_NS}ns"
echo "  max-shadow-drift=${MAX_SHADOW_DRIFT_SAMPLES} samples"

since=$(date '+%Y-%m-%d %H:%M:%S')

say "sample /proc tx engine"
end=$((SECONDS + DURATION))
sample_proc || inconc "could not parse $PROCFS"
while [ "$SECONDS" -lt "$end" ]; do
  sleep "$INTERVAL"
  sample_proc || inconc "could not parse $PROCFS during sampling"
done

awk '
  BEGIN { printf "  %-8s %-7s %-6s %-12s %-12s %-12s %-7s %-8s %-8s %s\n", "t+sec", "enabled", "active", "ticks", "phc_pres", "phc_launch", "acc", "shadow", "drift", "xrun" }
  NR == 1 { t0=$1 }
  {
    printf "  %-8.3f %-7s %-6s %-12s %-12s %-12s %-7s %-8s %-8s %s\n", (($1 - t0) / 1000000000), $2, $3, $4, $5, $6, $7, $8, $9, $10
  }
' "$SAMPLES"

proc_summary=$(awk '
  NR == 1 {
    first_t=$1; first_enabled=$2; first_active=$3; first_ticks=$4; first_pres=$5; first_launch=$6;
    first_acc=$7; first_shadow=$8; first_drift=$9; first_xrun=$10;
    enabled_all_1=($2 == 1); enabled_all_0=($2 == 0); active_ok=($3 == 1);
    pres_changed=0; launch_changed=0; shadow_changed=0;
    max_abs_drift=($9 < 0 ? -$9 : $9);
    last_t=$1; last_ticks=$4; last_pres=$5; last_launch=$6; last_acc=$7; last_shadow=$8; last_drift=$9; last_xrun=$10;
    next
  }
  {
    if ($2 != 1) enabled_all_1=0;
    if ($2 != 0) enabled_all_0=0;
    if ($3 != 1) active_ok=0;
    if ($5 != first_pres) pres_changed=1;
    if ($6 != first_launch) launch_changed=1;
    if ($8 != first_shadow) shadow_changed=1;
    abs_drift=($9 < 0 ? -$9 : $9);
    if (abs_drift > max_abs_drift) max_abs_drift=abs_drift;
    last_t=$1; last_ticks=$4; last_pres=$5; last_launch=$6; last_acc=$7; last_shadow=$8; last_drift=$9; last_xrun=$10;
  }
  END {
    if (NR < 2) { print "bad"; exit 1; }
    dt=(last_t - first_t) / 1000000000;
    tick_delta=last_ticks - first_ticks;
    xrun_delta=last_xrun - first_xrun;
    rate=(dt > 0) ? tick_delta / dt : 0;
    printf "first_enabled=%d enabled_all_1=%d enabled_all_0=%d active_ok=%d tick_delta=%d dt=%.3f tick_rate=%.1f pres_changed=%d launch_changed=%d shadow_changed=%d max_abs_drift=%d xrun_delta=%d first_pres=%s last_pres=%s first_launch=%s last_launch=%s first_shadow=%s last_shadow=%s first_drift=%s last_drift=%s\n",
           first_enabled, enabled_all_1, enabled_all_0, active_ok, tick_delta, dt, rate,
           pres_changed, launch_changed, shadow_changed, max_abs_drift, xrun_delta,
           first_pres, last_pres, first_launch, last_launch, first_shadow, last_shadow, first_drift, last_drift;
  }
' "$SAMPLES") || inconc "could not summarize $PROCFS samples"

echo "  $proc_summary"

first_enabled=$(echo "$proc_summary" | sed -nE 's/.*first_enabled=([0-9]+).*/\1/p')
enabled_all_1=$(echo "$proc_summary" | sed -nE 's/.*enabled_all_1=([0-9]+).*/\1/p')
enabled_all_0=$(echo "$proc_summary" | sed -nE 's/.*enabled_all_0=([0-9]+).*/\1/p')
active_ok=$(echo "$proc_summary" | sed -nE 's/.*active_ok=([0-9]+).*/\1/p')
tick_delta=$(echo "$proc_summary" | sed -nE 's/.*tick_delta=([0-9-]+).*/\1/p')
tick_rate=$(echo "$proc_summary" | sed -nE 's/.*tick_rate=([0-9.]+).*/\1/p')
pres_changed=$(echo "$proc_summary" | sed -nE 's/.*pres_changed=([0-9]+).*/\1/p')
launch_changed=$(echo "$proc_summary" | sed -nE 's/.*launch_changed=([0-9]+).*/\1/p')
shadow_changed=$(echo "$proc_summary" | sed -nE 's/.*shadow_changed=([0-9]+).*/\1/p')
max_abs_drift=$(echo "$proc_summary" | sed -nE 's/.*max_abs_drift=([0-9]+).*/\1/p')
xrun_delta=$(echo "$proc_summary" | sed -nE 's/.*xrun_delta=([0-9-]+).*/\1/p')

if [ "$MODE" = auto ]; then
  if [ "$first_enabled" = 1 ]; then
    MODE=live
  else
    MODE=shadow
  fi
  echo "  auto-selected mode=$MODE"
  if [ "$MODE" = live ]; then
    warn "auto selected live mode because tx_engine_enabled=1; this is not the shadow-path check"
  fi
fi

[ "$active_ok" = 1 ] || inconc "tx_timer_active was not 1 for the full sample window. Start playback, then rerun."
[ "$tick_delta" -gt 0 ] || fail "tx_engine_ticks did not increase"
[ "$pres_changed" = 1 ] || fail "tx_phc_pres did not change during sampling"
[ "$launch_changed" = 1 ] || fail "tx_phc_launch did not change during sampling"

awk -v r="$tick_rate" -v min="$TICKS_MIN_PER_S" -v max="$TICKS_MAX_PER_S" \
  'BEGIN { exit !(r < min || r > max) }' \
  && warn "timer rate ${tick_rate}/s is outside expected ${TICKS_MIN_PER_S}..${TICKS_MAX_PER_S}/s for a 125 us timer"

if [ "$MODE" = shadow ]; then
  [ "$enabled_all_0" = 1 ] || inconc "MODE=shadow requires tx_engine_enabled=0 for the full sample window"
  [ "$shadow_changed" = 1 ] || fail "tx_shadow_pptr did not change during sampling"
  [ "$max_abs_drift" -le "$MAX_SHADOW_DRIFT_SAMPLES" ] || fail "shadow drift max abs ${max_abs_drift} samples exceeds ${MAX_SHADOW_DRIFT_SAMPLES}"
  [ "$xrun_delta" -eq 0 ] || fail "tx_xrun increased by ${xrun_delta} during shadow sampling"

  say "collect fresh shadow drift logs"
  journalctl -k --since "$since" --no-pager 2>/dev/null | grep 'avb period shadow drift=' > "$SHADOW_LOGS" || true
  shadow_log_lines=$(wc -l < "$SHADOW_LOGS" | tr -d ' ')
  echo "  fresh shadow drift lines: $shadow_log_lines"
  tail -n 20 "$SHADOW_LOGS" | sed 's/^/  /'
  [ "$shadow_log_lines" -ge "$MIN_LOG_LINES" ] || inconc "not enough fresh shadow drift logs. Keep playback running, then rerun."

  journalctl -k --since "$since" --no-pager 2>/dev/null |
    grep -E 'scheduling while atomic|BUG:|WARNING:|deadlock|possible circular locking dependency' > "$WARN_LOGS" || true
  warn_lines=$(wc -l < "$WARN_LOGS" | tr -d ' ')
  if [ "$warn_lines" -gt 0 ]; then
    tail -n 20 "$WARN_LOGS" | sed 's/^/  /' >&2
    fail "fresh kernel WARN/BUG/locking lines appeared during shadow sampling"
  fi

  say "result"
  if [ "$FAIL" = 0 ]; then
    ok "  PASS -- shadow period timer is active, bounded, and not raising XRUNs."
    exit 0
  fi

  warn "FAIL -- shadow period evidence is outside bounds."
  exit 1
fi

[ "$enabled_all_1" = 1 ] || inconc "MODE=live requires tx_engine_enabled=1 for the full sample window"

say "collect fresh kernel comparison logs"
journalctl -k --since "$since" --no-pager 2>/dev/null | grep 'avb phc' > "$LOGS" || true

log_lines=$(wc -l < "$LOGS" | tr -d ' ')
invalid_pres_lines=$(grep -c 'delta-rx pres=invalid' "$LOGS" 2>/dev/null || true)
echo "  fresh avb phc lines: $log_lines (${invalid_pres_lines} invalid RX presentation timestamps)"
tail -n 20 "$LOGS" | sed 's/^/  /'

[ "$log_lines" -ge "$MIN_LOG_LINES" ] || inconc "not enough fresh 'avb phc' logs. Start playback/RX traffic, then rerun."

sed -nE 's/.*delta-rx pres=([-0-9]+)ns launch=([-0-9]+)ns.*/\1 \2/p' "$LOGS" > "$DELTAS"
delta_lines=$(wc -l < "$DELTAS" | tr -d ' ')
[ "$delta_lines" -ge "$MIN_LOG_LINES" ] || inconc "not enough fresh logs with valid RX presentation timestamps. Invalid count=${invalid_pres_lines}."
[ "$invalid_pres_lines" -eq 0 ] || warn "ignored ${invalid_pres_lines} invalid RX presentation timestamp lines; launch deltas are still scored on valid lines"

delta_summary=$(awk '
  function abs(x) { return x < 0 ? -x : x }
  NR == 1 {
    minp=maxp=$1; minl=maxl=$2; maxabsp=abs($1); maxabsl=abs($2); next
  }
  {
    if ($1 < minp) minp=$1; if ($1 > maxp) maxp=$1;
    if ($2 < minl) minl=$2; if ($2 > maxl) maxl=$2;
    if (abs($1) > maxabsp) maxabsp=abs($1);
    if (abs($2) > maxabsl) maxabsl=abs($2);
  }
  END {
    printf "n=%d pres_min=%d pres_max=%d pres_spread=%d pres_max_abs=%d launch_min=%d launch_max=%d launch_spread=%d launch_max_abs=%d\n",
           NR, minp, maxp, maxp - minp, maxabsp, minl, maxl, maxl - minl, maxabsl;
  }
' "$DELTAS")

echo "  $delta_summary"

pres_max_abs=$(echo "$delta_summary" | sed -nE 's/.*pres_max_abs=([0-9]+).*/\1/p')
launch_max_abs=$(echo "$delta_summary" | sed -nE 's/.*launch_max_abs=([0-9]+).*/\1/p')
pres_spread=$(echo "$delta_summary" | sed -nE 's/.*pres_spread=([0-9-]+).*/\1/p')
launch_spread=$(echo "$delta_summary" | sed -nE 's/.*launch_spread=([0-9-]+).*/\1/p')

[ "$pres_max_abs" -le "$MAX_ABS_DELTA_NS" ] || fail "presentation delta max abs ${pres_max_abs}ns exceeds ${MAX_ABS_DELTA_NS}ns"
[ "$launch_max_abs" -le "$MAX_ABS_DELTA_NS" ] || fail "launch delta max abs ${launch_max_abs}ns exceeds ${MAX_ABS_DELTA_NS}ns"
[ "$pres_spread" -le "$MAX_SPREAD_NS" ] || fail "presentation delta spread ${pres_spread}ns exceeds ${MAX_SPREAD_NS}ns"
[ "$launch_spread" -le "$MAX_SPREAD_NS" ] || fail "launch delta spread ${launch_spread}ns exceeds ${MAX_SPREAD_NS}ns"

say "result"
if [ "$FAIL" = 0 ]; then
  ok "  PASS -- PHC shadow timing evidence is within configured bounds."
  exit 0
fi

warn "FAIL -- PHC shadow timing evidence is outside bounds. Rerun with wider thresholds only if a known fixed offset is expected, e.g.:"
warn "  MAX_ABS_DELTA_NS=5000000 MAX_SPREAD_NS=1000000 $0"
exit 1
