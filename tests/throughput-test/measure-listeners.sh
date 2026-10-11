#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#
# measure-listeners.sh -- how many AVB listener streams can this i210 sink?
#
# Loads an i210 listener ($I210) with synthetic AVTP talker streams from a second
# NIC ($BLAST, direct-cabled), and finds the maximum stream count it can receive
# with ZERO drops. Each stream = one 8-ch @ 48 kHz listener = 8000 pps, so the
# answer is "this system can host N such AVB listeners on one i210".
#
# Two modes:
#   --sweep  (default) ramp the stream count until drops appear or the link's
#            bandwidth runs out; report the ceiling + a per-step table.
#   --fixed  run one STREAMS x PPS configuration and print PASS/FAIL (spot check).
#
# Requires the i210 to be driven by snd_igb_avb loaded in measurement mode:
#       sudo modprobe snd_igb_avb avb_rx_bench=1        (or via avb_up.sh)
# In that mode the driver captures ALL AVTP on RX-q1 (any MAC, no MSRP needed)
# and counts packets at /sys/kernel/debug/snd_igb_avb/rx_bench. This script does
# NOT load the module.
#
# Usage:  sudo ./measure-listeners.sh                 # sweep with defaults
#         sudo ./measure-listeners.sh --prep --sanity # clean the link, gate, sweep
#         sudo ./measure-listeners.sh --fixed         # single STREAMS x PPS run
#         sudo STEPS="1 4 8 16 32 48 64" ./measure-listeners.sh
#         sudo BLAST=eno1 I210=enp5s0 PPS=8000 DWELL=8 ./measure-listeners.sh
#         sudo IRQ_CPU=2 NIC_CPU=4 APP_CPU=6 ./measure-listeners.sh
#
# --prep makes the two NICs a clean raw-L2 test link: flushes stale IPs, disables
# 802.3x flow control on the blaster NIC (so the i210 can't PAUSE-throttle it and
# hide drops), and registers the blaster's VLAN id on the i210 (rx-vlan-filter
# drops tagged frames whose VID isn't registered). Internet on those ports goes
# away until you replug -- that's expected for a point-to-point link.
#
# --sanity blasts a low rate for 5s first and aborts unless RX-q1 traffic is seen,
# so the real sweep only fires once the cable + filter are confirmed working.
#
# NOTE: the custom AVB datapath (snd_avb_receive) bypasses igb's NAPI accounting,
# so ethtool's rx_queue_1_packets never moves. Received counts come from the
# rx_bench debugfs node; drop counters (rx_queue_1_drops / mpc / rnbc) ARE valid
# (the igb watchdog maintains them from hardware).
#
# See README.md.

set -u

# ---- config (override via env) -------------------------------------------
I210=${I210:-enp5s0}        # i210 under test (RX-q1 sink)
BLAST=${BLAST:-eno1}        # second NIC, direct-cabled to $I210 (load source)
PPS=${PPS:-8000}            # per-stream pps (8000 = one 8-ch @ 48 kHz listener)
DWELL=${DWELL:-8}           # seconds per sweep step
SECS=${SECS:-60}            # seconds for --fixed
STREAMS=${STREAMS:-16}      # stream count for --fixed
STEPS=${STEPS:-"1 2 4 8 12 16 24 32 48 64"}   # sweep ramp (stream counts)
IRQ_CPU=${IRQ_CPU:-2}       # core dedicated to the i210 RX-q1 IRQ
NIC_CPU=${NIC_CPU:-4}       # core for the blaster NIC's TX IRQ
APP_CPU=${APP_CPU:-6}       # core for the blaster process
DSTMAC=91:e0:f0:00:fe:00    # multicast dst the blaster uses (matches avtp-blaster.c)
BVID=${BVID:-2}             # blaster VLAN id (low 12 bits of tag 0x6002); the i210's
                            # rx-vlan-filter drops tagged frames whose VID isn't registered

BLASTER_MAX=64              # must match MAX_STREAMS in avtp-blaster.c
WIRE_BYTES=266              # ~242B AVTP frame + 24B preamble/SFD/FCS/IFG, for Mbps est.

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BLASTER="$HERE/avtp-blaster"
PARAM=/sys/module/snd_igb_avb/parameters/avb_rx_bench
DBG=/sys/kernel/debug/snd_igb_avb/rx_bench

MODE=sweep
SANITY=0
PREP=0
for a in "$@"; do
  case "$a" in
    --sweep)  MODE=sweep ;;
    --fixed)  MODE=fixed ;;
    --sanity) SANITY=1 ;;
    --prep)   PREP=1 ;;
    *) echo "unknown arg: $a (use --sweep|--fixed and/or --prep --sanity)" >&2; exit 2 ;;
  esac
done

say(){  printf '\033[1m== %s\033[0m\n' "$*"; }
warn(){ printf '\033[33m!! %s\033[0m\n' "$*" >&2; }
die(){  printf '\033[31mXX %s\033[0m\n' "$*" >&2; exit 1; }
ok(){   printf '\033[32m%s\033[0m\n' "$*"; }

# ---- restore irqbalance + remove our VLAN subif on any exit ---------------
had_irqbalance=0
ADDED_VLAN=""
restore(){
  [ "$had_irqbalance" = 1 ] && systemctl start irqbalance 2>/dev/null && echo "(irqbalance restarted)"
  [ -n "$ADDED_VLAN" ] && ip link del "$ADDED_VLAN" 2>/dev/null && echo "(removed VLAN subif $ADDED_VLAN)"
}
trap restore EXIT

# ---- preflight -----------------------------------------------------------
say "preflight"
[ "$(id -u)" -eq 0 ] || die "run as root (IRQ affinity + raw socket + debugfs need it)"
command -v ethtool >/dev/null || die "ethtool not found"
command -v taskset >/dev/null || die "taskset not found (util-linux)"
command -v gcc     >/dev/null || die "gcc not found (to build the blaster)"

ip link show "$I210"  >/dev/null 2>&1 || die "i210 iface '$I210' not found"
ip link show "$BLAST" >/dev/null 2>&1 || die "blast iface '$BLAST' not found"
[ "$I210" != "$BLAST" ] || die "I210 and BLAST must differ (need two NICs)"

NCPU=$(nproc)
for c in "$IRQ_CPU" "$NIC_CPU" "$APP_CPU"; do
  [ "$c" -ge 0 ] 2>/dev/null && [ "$c" -lt "$NCPU" ] || die "CPU '$c' out of range (0..$((NCPU-1)))"
done

drv=$(ethtool -i "$I210" 2>/dev/null | awk '/^driver:/{print $2}')
[ "$drv" = "igb_avb" ] || die "i210 ($I210) driver is '$drv', expected 'igb_avb'.
   Load the AVB module first in measurement mode:
     sudo modprobe snd_igb_avb avb_rx_bench=1      (or via avb_up.sh with that param)"

# measurement mode must be ON (the broad flex filter + RX counter are gated on it)
if [ "$(cat "$PARAM" 2>/dev/null)" != "1" ]; then
  die "snd_igb_avb is loaded WITHOUT avb_rx_bench=1 ($PARAM != 1).
   In normal mode the flex filter only steers the specific-MAC stream to RX-q1, so the
   blaster's frames are ignored (RX-q1 = 0 packets). Reload with measurement mode:
     sudo rmmod snd_igb_avb; sudo modprobe snd_igb_avb avb_rx_bench=1 ...
   (check: dmesg shows 'AVB RX benchmark mode')."
fi

# debugfs node = the received-packet counter (datapath bypasses ethtool's rx counts)
if [ ! -e "$DBG" ]; then
  mountpoint -q /sys/kernel/debug 2>/dev/null || mount -t debugfs none /sys/kernel/debug 2>/dev/null
fi
[ -e "$DBG" ] || die "counter node $DBG missing even though avb_rx_bench=1.
   Is debugfs mounted? try: sudo mount -t debugfs none /sys/kernel/debug"

sp=$(ethtool "$BLAST" 2>/dev/null | awk -F': ' '/Speed:/{print $2}')
[ "$sp" = "1000Mb/s" ] || warn "$BLAST link speed is '$sp' (want 1000Mb/s); high stream counts (~245 Mbps at 16) won't fit -> TX-side drops, not an i210 limit"
ethtool "$I210" 2>/dev/null | grep -q "Link detected: yes" \
  || warn "$I210 link not detected -- is the direct cable $BLAST <-> $I210 connected?"

# ---- build blaster -------------------------------------------------------
say "build blaster"
make -C "$HERE" >/dev/null || die "blaster build failed"
[ -x "$BLASTER" ] || die "blaster binary missing after build"

# ---- pin IRQs ------------------------------------------------------------
say "pin: i210 RX-q1 -> CPU$IRQ_CPU | $BLAST TX -> CPU$NIC_CPU | blaster -> CPU$APP_CPU"
if systemctl is-active --quiet irqbalance 2>/dev/null; then
  systemctl stop irqbalance && had_irqbalance=1 && echo "(stopped irqbalance for the run)"
fi

pin_irqs(){ # $1=regex match in /proc/interrupts, $2=cpu
  local match=$1 cpu=$2 n=0 irq
  for irq in $(grep -E "$match" /proc/interrupts | awk -F: '{print $1}' | tr -d ' '); do
    if echo "$cpu" > "/proc/irq/$irq/smp_affinity_list" 2>/dev/null; then n=$((n+1)); fi
  done
  echo "  '$match' -> CPU$cpu : pinned $n irq(s)"
}

RX1_RE="${I210}-TxRx-1$"
if grep -qE "$RX1_RE" /proc/interrupts; then
  pin_irqs "$RX1_RE" "$IRQ_CPU"
else
  warn "no '${I210}-TxRx-1' vector found; pinning ALL ${I210} vectors instead"
  RX1_RE="$I210"
  pin_irqs "$I210" "$IRQ_CPU"
fi
pin_irqs "$BLAST" "$NIC_CPU"
RX1_IRQ=$(grep -E "$RX1_RE" /proc/interrupts | awk -F: '{print $1}' | tr -d ' ' | head -1)

# ---- snapshot helpers ----------------------------------------------------
estat(){ local v; v=$(ethtool -S "$I210" 2>/dev/null | awk -v k="$1:" '$1==k{print $2}'); echo "${v:-0}"; }
irq_total(){ # sum the per-cpu (numeric-only) columns for irq number $1
  [ -n "${1:-}" ] || { echo 0; return; }
  grep -E "^[[:space:]]*$1:" /proc/interrupts \
    | awk '{s=0; for(i=2;i<=NF;i++) if($i ~ /^[0-9]+$/) s+=$i; print s}'
}
cpu_jiffies(){ # for cpu $1 print: "idle total irqsoft"
  awk -v c="cpu$1" '$1==c{idle=$5+$6; tot=0; for(i=2;i<=NF;i++)tot+=$i; print idle, tot, ($7+$8)}' /proc/stat
}
rx_total(){ awk -F= '/^total=/{print $2+0}' "$DBG" 2>/dev/null; }   # kernel received counter
rx_reset(){ echo 1 > "$DBG" 2>/dev/null; }                          # zero the counters
carrier(){       cat "/sys/class/net/$1/carrier" 2>/dev/null || echo 0; }
link_changes(){  cat "/sys/class/net/$1/carrier_changes" 2>/dev/null || echo 0; }
nstat(){ local v; v=$(ethtool -S "$1" 2>/dev/null | awk -v k="$2:" '$1==k{print $2}'); echo "${v:-0}"; }

# do_run <streams> <pps> <secs> -- blast and fill D_* / RX_PPS / IRQ_RATE / BUSY /
# BLAST_PPS / FLAP_* / MBPS / RUN_TARGET.
do_run(){
  local nstreams=$1 pps=$2 secs=$3
  RUN_STREAMS=$nstreams
  RUN_TARGET=$((nstreams * pps))
  local bd bm bn bf bi ci ct cs fi fb bx
  rx_reset
  bd=$(estat rx_queue_1_drops)
  bm=$(estat rx_missed_errors); bn=$(estat rx_no_buffer_count); bf=$(estat rx_fifo_errors)
  bi=$(irq_total "$RX1_IRQ"); read -r ci ct cs <<<"$(cpu_jiffies "$IRQ_CPU")"
  fi=$(link_changes "$I210"); fb=$(link_changes "$BLAST")
  bx=$(nstat "$BLAST" rx_flow_control_xoff)

  say "blasting $nstreams streams x $pps pps = $RUN_TARGET pps for ${secs}s"
  local out; out=$(mktemp)
  taskset -c "$APP_CPU" "$BLASTER" -i "$BLAST" --streams "$nstreams" --pps "$pps" --secs "$secs" | tee "$out"
  BLAST_PPS=$(awk '{for(i=1;i<=NF;i++) if($i=="(avg") print $(i+1)}' "$out" | tail -1)
  BLAST_PPS=${BLAST_PPS:-0}
  rm -f "$out"

  local ad am an af ai ci2 ct2 cs2
  D_TOTAL=$(rx_total); D_TOTAL=${D_TOTAL:-0}
  ad=$(estat rx_queue_1_drops)
  am=$(estat rx_missed_errors); an=$(estat rx_no_buffer_count); af=$(estat rx_fifo_errors)
  ai=$(irq_total "$RX1_IRQ"); read -r ci2 ct2 cs2 <<<"$(cpu_jiffies "$IRQ_CPU")"

  D_DROPS=$((ad-bd)); D_MISS=$((am-bm)); D_NOBUF=$((an-bn)); D_FIFO=$((af-bf))
  D_IRQ=$((ai-bi))
  FLAP_I210=$(( $(link_changes "$I210") - fi )); FLAP_BLAST=$(( $(link_changes "$BLAST") - fb ))
  PAUSE_XOFF=$(( $(nstat "$BLAST" rx_flow_control_xoff) - bx ))
  RX_PPS=$(( secs > 0 ? D_TOTAL/secs : 0 ))
  IRQ_RATE=$(( secs > 0 ? D_IRQ/secs : 0 ))
  MBPS=$(( BLAST_PPS * WIRE_BYTES * 8 / 1000000 ))
  local dt=$((ct2-ct)) didle=$((ci2-ci)) dis=$((cs2-cs))
  BUSY="n/a"; IRQPCT="n/a"; IDLE="n/a"
  if [ "$dt" -gt 0 ]; then BUSY=$(((dt-didle)*100/dt)); IRQPCT=$((dis*100/dt)); IDLE=$((didle*100/dt)); fi
  TOTAL_DROPS=$((D_DROPS + D_MISS + D_NOBUF + D_FIFO))
}

delivered_ok(){ [ "${BLAST_PPS:-0}" -ge $((RUN_TARGET*9/10)) ]; }

# ---- link check ----------------------------------------------------------
say "link check"
for nic in "$I210" "$BLAST"; do
  printf "  %-8s carrier=%s  (carrier_changes so far: %s)\n" "$nic" "$(carrier "$nic")" "$(link_changes "$nic")"
  [ "$(carrier "$nic")" = 1 ] || warn "$nic has NO carrier -- check the cable $BLAST <-> $I210"
done
ip_i=$(ip -4 -o addr show "$I210"  2>/dev/null | awk '{print $4}' | head -1)
ip_b=$(ip -4 -o addr show "$BLAST" 2>/dev/null | awk '{print $4}' | head -1)
printf "  IPs: %s=%s  %s=%s\n" "$I210" "${ip_i:-none}" "$BLAST" "${ip_b:-none}"
fc_b=$(ethtool -a "$BLAST" 2>/dev/null | awk '/RX negotiated:/{r=$3} /TX negotiated:/{t=$3} END{print r"/"t}')
fc_i=$(ethtool -a "$I210"  2>/dev/null | awk '/RX negotiated:/{r=$3} /TX negotiated:/{t=$3} END{print r"/"t}')
printf "  flow-control (rx/tx negotiated): %s=%s  %s=%s\n" "$BLAST" "${fc_b:-?}" "$I210" "${fc_i:-?}"
vlan_i=$(ethtool -k "$I210" 2>/dev/null | awk -F': ' '/rx-vlan-offload/{print $2}')
printf "  %s rx-vlan-offload (tag stripping): %s\n" "$I210" "${vlan_i:-?}"
case "$vlan_i" in
  on*) warn "$I210 strips VLAN tags -> the broad flex filter (matches the 802.1Q tag at"
       warn "byte 12) will MISS the blaster's tagged frames. Disable BEFORE loading the module:"
       warn "  sudo ethtool -K $I210 rxvlan off   (then reload igb_avb so the filter reinstalls)" ;;
esac

if [ "$PREP" = 1 ]; then
  say "prep: flushing IPs (both) + disabling flow control (blaster NIC only) + registering VLAN $BVID"
  for nic in "$I210" "$BLAST"; do
    ip addr flush dev "$nic" 2>/dev/null && echo "  flushed IPs on $nic"
  done
  # Disable PAUSE only on the blaster NIC. NEVER 'ethtool -A/-s/-G/-K' the i210:
  # it triggers a reset that WIPES the AVB flex filter (installed once at load).
  if ethtool -A "$BLAST" autoneg off rx off tx off 2>/dev/null; then
    echo "  flow control off on $BLAST"
  else
    warn "could not disable flow control on $BLAST (driver may not support -A)"
  fi
  warn "$BLAST/$I210 IPs flushed -- no internet on those ports until you replug them."
  # Register the blaster's VID so rx-vlan-filter passes the tagged frames to the
  # flex filter. Adding a VLAN subif programs the VFTA without resetting the NIC.
  if ip link show "$I210.$BVID" >/dev/null 2>&1; then
    echo "  VLAN $BVID already registered on $I210"
  elif ip link add link "$I210" name "$I210.$BVID" type vlan id "$BVID" 2>/dev/null \
       && ip link set "$I210.$BVID" up 2>/dev/null; then
    ADDED_VLAN="$I210.$BVID"
    echo "  registered VLAN $BVID on $I210 (rx-vlan-filter now passes the blaster's frames)"
  else
    warn "could not add VLAN $BVID on $I210 -- tagged frames may stay filtered out"
  fi
else
  { [ -n "$ip_i" ] || [ -n "$ip_b" ]; } && \
    warn "NICs still have IPs (stale DHCP). For a clean test run with --prep."
  case "${fc_b}${fc_i}" in *on*) warn "flow control is ON somewhere -- the i210 can PAUSE-throttle the blaster; run with --prep." ;; esac
fi

# ---- optional sanity pre-check -------------------------------------------
if [ "$SANITY" = 1 ]; then
  say "sanity pre-check (16 x 1000 pps for 5s) + on-wire frame capture"
  cap_tx=$(mktemp); cap_rx=$(mktemp); have_td=0
  if command -v tcpdump >/dev/null; then
    have_td=1
    timeout 9 tcpdump -i "$BLAST" -c 2 -e -xx -nn "ether dst $DSTMAC" >"$cap_tx" 2>/dev/null &
    timeout 9 tcpdump -i "$I210"  -c 2 -e -xx -nn "ether dst $DSTMAC" >"$cap_rx" 2>/dev/null &
    sleep 0.3
  else
    warn "tcpdump not installed -- skipping on-wire byte capture"
  fi
  do_run 16 1000 5
  if [ "$have_td" = 1 ]; then
    wait 2>/dev/null
    say "on-wire capture (dst $DSTMAC) -- EXPECT bytes 0x0c-0x11 = 81 00 60 02 22 f0"
    echo "  --- $BLAST TX: ---"
    [ -s "$cap_tx" ] && sed 's/^/    /' "$cap_tx" || echo "    (no TX frames captured -- blaster not sending?)"
    echo "  --- $I210 RX: ---"
    if [ -s "$cap_rx" ]; then sed 's/^/    /' "$cap_rx"
    else echo "    (nothing on $I210 RX -- frames arrive at the NIC (see mpc) but are dropped"
         echo "     at the FIFO before the host: filter not steering them to a serviced queue.)"; fi
  fi
  rm -f "$cap_tx" "$cap_rx"
  printf "  blaster=~%s pps (target %d)  RX-q1 IRQ delta=%d  recv=%d  drops=%d\n" \
         "$BLAST_PPS" "$RUN_TARGET" "$D_IRQ" "$D_TOTAL" "$TOTAL_DROPS"
  if ! delivered_ok; then
    die "sanity FAILED: blaster delivered only ~$BLAST_PPS pps of $RUN_TARGET (flaps
   $I210=$FLAP_I210 $BLAST=$FLAP_BLAST, PAUSE rx'd $PAUSE_XOFF). The load source couldn't
   transmit -- run 'sudo $0 --prep' to flush stale IPs + disable PAUSE, then re-run."
  fi
  [ "$D_IRQ" -gt 0 ] || die "sanity FAILED: blaster sent ~$BLAST_PPS pps but RX-q1 saw 0 IRQs
   (mpc=$D_MISS fifo=$D_FIFO -- frames reach the NIC but aren't steered to RX-q1). Fixes:
     1) register the blaster's VID: re-run with '--prep' (adds VLAN $BVID on $I210), OR
        manually: sudo ip link add link $I210 name $I210.$BVID type vlan id $BVID; ip link set $I210.$BVID up
     2) if STILL 0 IRQs and 'rx-vlan-offload' was ON above, the i210 strips the tag before
        the filter: sudo ethtool -K $I210 rxvlan off  then reload igb_avb (avb_up.sh)."
  ok "  sanity OK -- blaster ~$BLAST_PPS pps, RX-q1 receiving. Proceeding."
fi

# ==========================================================================
# FIXED mode -- single configuration PASS/FAIL
# ==========================================================================
if [ "$MODE" = fixed ]; then
  do_run "$STREAMS" "$PPS" "$SECS"
  say "results (deltas over ${SECS}s)"
  printf "  blaster sent  : ~%s pps (target %d)\n" "$BLAST_PPS" "$RUN_TARGET"
  printf "  link flaps    : %s=%d  %s=%d\n" "$I210" "$FLAP_I210" "$BLAST" "$FLAP_BLAST"
  printf "  PAUSE rx'd    : %d on %s\n" "$PAUSE_XOFF" "$BLAST"
  printf "  RX-q1 recv    : %d  (~%d pps, target %d, ~%d Mbps)\n" "$D_TOTAL" "$RX_PPS" "$RUN_TARGET" "$MBPS"
  printf "  drops         : q1=%d mpc=%d nobuf=%d fifo=%d  (total %d)\n" \
         "$D_DROPS" "$D_MISS" "$D_NOBUF" "$D_FIFO" "$TOTAL_DROPS"
  printf "  RX-q1 IRQs    : %d (%d /s) on CPU%s\n" "$D_IRQ" "$IRQ_RATE" "$IRQ_CPU"
  printf "  CPU%s busy     : %s%% (irq+softirq %s%%, idle %s%%)\n" "$IRQ_CPU" "$BUSY" "$IRQPCT" "$IDLE"
  say "verdict"
  if ! delivered_ok; then
    die "INVALID: blaster delivered only ~$BLAST_PPS pps of $RUN_TARGET (link/PAUSE/TX limit,
   not an i210 result). PAUSE rx'd=$PAUSE_XOFF, flaps $I210=$FLAP_I210/$BLAST=$FLAP_BLAST.
   Run 'sudo $0 --prep' and confirm the BROAD module is loaded, then re-run."
  elif [ "$D_IRQ" -le 0 ]; then
    die "INVALID: ~$BLAST_PPS pps delivered but RX-q1 saw 0 IRQs (mpc=$D_MISS fifo=$D_FIFO).
   Frames reach the i210 but aren't steered to RX-q1 -- run with --prep (registers VID $BVID)
   and/or 'sudo ethtool -K $I210 rxvlan off' then reload igb_avb."
  elif [ "$TOTAL_DROPS" -eq 0 ]; then
    ok "  PASS -- $STREAMS listeners sustained at ~$RX_PPS pps (~$MBPS Mbps) with ZERO drops."
  else
    printf '\033[31m  FAIL -- %d drops at %d listeners (~%d pps).\033[0m\n' "$TOTAL_DROPS" "$STREAMS" "$RX_PPS"
  fi
  exit 0
fi

# ==========================================================================
# SWEEP mode (default) -- ramp stream count, find the zero-drop ceiling
# ==========================================================================
say "sweep: max listeners with zero drops (pps/stream=$PPS, ${DWELL}s/step)"
echo "  steps: $STEPS"
printf '  %-9s %-12s %-12s %-7s %-8s %-7s %s\n' "streams" "sent(pps)" "recv(pps)" "Mbps" "drops" "CPU%" "result"

best=0
wall=""
wall_reason=""
for n in $STEPS; do
  if [ "$n" -gt "$BLASTER_MAX" ]; then
    warn "skip $n streams (> blaster MAX_STREAMS=$BLASTER_MAX; raise it in avtp-blaster.c)"
    continue
  fi
  do_run "$n" "$PPS" "$DWELL"
  res="ok"
  if ! delivered_ok; then res="TX-LIMIT"; fi
  if [ "$D_IRQ" -le 0 ]; then res="NO-RX"; fi
  if [ "$TOTAL_DROPS" -gt 0 ]; then res="DROPS"; fi
  printf '  %-9s %-12s %-12s %-7s %-8s %-7s %s\n' \
         "$n" "${BLAST_PPS:-0}" "$RX_PPS" "$MBPS" "$TOTAL_DROPS" "$BUSY" "$res"

  case "$res" in
    ok)
      best=$n ;;
    DROPS)
      wall=$n; wall_reason="i210 dropped $TOTAL_DROPS pkts (q1=$D_DROPS mpc=$D_MISS nobuf=$D_NOBUF fifo=$D_FIFO) at $n listeners -- a real RX limit."
      break ;;
    TX-LIMIT)
      wall=$n; wall_reason="blaster under-delivered (~$BLAST_PPS of $RUN_TARGET pps, ~$MBPS Mbps) -- bandwidth/TX wall (gigabit link or sender), NOT an i210 drop limit. Use a faster/second sender to push higher."
      [ "${PAUSE_XOFF:-0}" -gt 0 ] && wall_reason="$wall_reason PAUSE rx'd=$PAUSE_XOFF: run --prep."
      break ;;
    NO-RX)
      wall=$n; wall_reason="RX-q1 saw 0 IRQs at $n (mpc=$D_MISS fifo=$D_FIFO) -- frames not steered to RX-q1; run --prep (VID $BVID) and/or 'ethtool -K $I210 rxvlan off' + reload."
      break ;;
  esac
done

say "verdict"
if [ "$best" -eq 0 ]; then
  die "no step passed. ${wall_reason:-check link / --prep / module mode}"
fi
ch=$((best * 8))
mbps_best=$(( best * PPS * WIRE_BYTES * 8 / 1000000 ))
ok "  Max AVB listeners sustained with ZERO drops on $I210 = $best  (= $best x 8ch = $ch channels, ~$mbps_best Mbps on-wire)"
if [ -n "$wall" ]; then
  echo "  Wall at $wall listeners: $wall_reason"
else
  echo "  No wall hit within the tested range (max step $best). Raise STEPS to push further:"
  echo "    sudo STEPS=\"$STEPS 96 128\" $0   (and raise MAX_STREAMS in avtp-blaster.c if needed)"
fi
echo "  Tip: a TX-LIMIT/gigabit wall is the link, not the i210; re-run from a faster or"
echo "  second sender to find the true RX ceiling. A DROPS wall is the i210's real limit."
