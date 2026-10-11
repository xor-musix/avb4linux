#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2019 Ralf Beck
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Simple script to run snd_igb_avb

export AVBHOME=${AVBHOME:-$HOME/.avb}

# Parse command line arguments.  Flags or the legacy positional form
#   avb_up.sh <iface> [<samplerate> [<num_streams>]]
# both work; defaults are applied AFTER parsing -- presetting them here made
# the positional branches below unreachable ("Unknown argument: 48000").
INTERFACE=""
SAMPLERATE=""
NUM_STREAMS=""
TX_MIRROR=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --samplerate)
            SAMPLERATE="$2"
            shift 2
            ;;
        --num-streams)
            NUM_STREAMS="$2"
            shift 2
            ;;
        --tx-mirror)
            TX_MIRROR="1"
            shift
            ;;
        --*)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
        *)
            if [[ -z "$INTERFACE" ]]; then
                INTERFACE="$1"
            elif [[ -z "$SAMPLERATE" ]]; then
                SAMPLERATE="$1"
            elif [[ -z "$NUM_STREAMS" ]]; then
                NUM_STREAMS="$1"
            else
                echo "Unknown argument: $1" >&2
                exit 1
            fi
            shift
            ;;
    esac
done

if [[ -z "$INTERFACE" ]]; then
    echo "Usage: $0 <interface> [--samplerate <rate>] [--num-streams <count>] [--tx-mirror]" >&2
    exit 1
fi
SAMPLERATE=${SAMPLERATE:-48000}
NUM_STREAMS=${NUM_STREAMS:-1}

export INTERFACE
export SAMPLERATE
export NUM_STREAMS
export TX_MIRROR

# AVB_MAAP (optional): by default avb-user allocates the talker destination
# block via MAAP and writes it to the kernel; AVB_MAAP=0 uses the fixed base.
[ -n "$AVB_MAAP" ] && export AVB_MAAP

# AVB device MACs, written by avb_apply_mac.sh / avb_autoconfig.sh.  They are
# passed to the kernel module and exported for avb-user, which refuses to start
# without them (the device's MACs have no default).  The kernel uses them as the
# RX stream-0 identity until ACMP binds the streams.  A source MAC given on the
# command line wins over the conf (`sudo AVB_DEVICE_SOURCE_MAC=... ./avb_up.sh
# ...`, as tests/motu/motu_ds.sh prints it): it is the AECP rate target and the
# stream-0 fallback, so it follows the topology's stream-0 device.
MACCONF=$AVBHOME/avb-macs.conf
if [ -f "$MACCONF" ]; then
    SOURCE_MAC_OVERRIDE=$AVB_DEVICE_SOURCE_MAC
    . "$MACCONF"
    AVB_DEVICE_SOURCE_MAC=${SOURCE_MAC_OVERRIDE:-$AVB_DEVICE_SOURCE_MAC}
    export AVB_DEVICE_SOURCE_MAC AVB_DEVICE_TALKER_MAC_BASE
else
    echo "Note: $MACCONF not found; no remote-device MACs -> avb-user will not start (run utils/avb_autoconfig.sh)." >&2
fi

# Refuse to start with a stale/missing binary instead of
# silently running whatever happens to be there.
for bin in snd_igb_avb.ko avb-user daemon_cl mrpd maap_daemon shaper_daemon; do
    if [ ! -e "$AVBHOME/$bin" ]; then
        echo "Missing $AVBHOME/$bin" >&2
        echo "  -> build and stage them first: make && sudo make install" >&2
        exit 1
    fi
done

rmmod igb
rmmod snd_igb_avb
modprobe i2c_algo_bit
#modprobe dca
modprobe ptp

# TX ring sizing (tx_size, in HW descriptors).  The launch-time TX engine queues
# ~AVB_TX_SCHED_LEAD_NS / 125us Class-A intervals ahead, each consuming
# num_streams descriptor GROUPS (2 descriptors per launched packet).  If the ring
# can't hold (lead_intervals + 1) * num_streams groups it back-pressures and
# tx_xrun climbs -- this was the old ~7-stream ceiling on the 256-descriptor
# default.  Size the ring from --num-streams with headroom for softirq-jitter
# bursts, align to 128 descriptors (i210 TDLEN alignment), clamp to [256, 4096].
LEAD_INTERVALS=8                                    # 1ms lead / 125us (AVB_TX_SCHED_LEAD_NS)
TX_GROUPS=$(( (LEAD_INTERVALS + 1) * NUM_STREAMS * 3 / 2 ))   # +50% burst headroom
TX_SIZE=$(( TX_GROUPS * 2 ))                        # 2 descriptors per group
TX_SIZE=$(( (TX_SIZE + 127) / 128 * 128 ))          # round up to a 128-desc boundary
[ "$TX_SIZE" -lt 256 ]  && TX_SIZE=256
[ "$TX_SIZE" -gt 4096 ] && TX_SIZE=4096
export TX_SIZE
echo "TX ring: num_streams=$NUM_STREAMS -> tx_size=$TX_SIZE descriptors ($((TX_SIZE/2)) groups)"

insmod $AVBHOME/snd_igb_avb.ko samplerate=$SAMPLERATE \
    ${OWN_MAC:+own_mac=$OWN_MAC} \
    ${AVB_DEVICE_SOURCE_MAC:+avb_device_source_mac=$AVB_DEVICE_SOURCE_MAC} \
    ${AVB_DEVICE_TALKER_MAC_BASE:+avb_device_talker_mac=$AVB_DEVICE_TALKER_MAC_BASE} \
    ${NUM_STREAMS:+num_streams=$NUM_STREAMS} \
    ${TX_SIZE:+tx_size=$TX_SIZE} \
    ${TX_MIRROR:+avb_tx_mirror=$TX_MIRROR}

ethtool -i $INTERFACE

sleep 1
ifconfig $INTERFACE down
echo 0 > /sys/class/net/$INTERFACE/queues/tx-0/xps_cpus
echo 0 > /sys/class/net/$INTERFACE/queues/tx-1/xps_cpus
echo f > /sys/class/net/$INTERFACE/queues/tx-2/xps_cpus
echo f > /sys/class/net/$INTERFACE/queues/tx-3/xps_cpus
ifconfig $INTERFACE up promisc

# Wait for interface to be fully ready before starting daemons
sleep 2

echo "Starting daemons on "$INTERFACE

# Route the stock daemons' output to syslog (tagged) instead of this console,
# so avb-user owns the terminal for its status screen.  Inspect their detail
# with e.g.  journalctl -t gptp -t mrpd -t maap -t shaper -t avb-user -f
groupadd ptp > /dev/null 2>&1
# GPTP_SYNTONIZE: pass -S to daemon_cl so it DISCIPLINES the i210 PHC to the gPTP
# grandmaster (step + slew). This makes the dashboard CLOCK go LOCKED. The
# snd_igb_avb data plane now uses a self-clocked TX engine that derives
# the media clock from the PHC, not from the RX stream. Default ON (-S) since
# the RX->TX coupling has been removed.
# (-S MUST come after $INTERFACE: daemon_cl uses argv[1] as ifname.)
#
# For A/B testing sync stability, GPTP_DISCIPLINE=0 forces -S OFF (PHC free-runs).
# NOTE: pass it AFTER sudo (`sudo GPTP_DISCIPLINE=0 ./avb_up.sh ...`) -- sudo strips
# env vars set before it.  Uses `:-` only for the ON default; the OFF case is empty.
if [ "${GPTP_DISCIPLINE:-1}" = "0" ]; then
    GPTP_SYNTONIZE=""
else
    GPTP_SYNTONIZE="${GPTP_SYNTONIZE:--S}"
fi
$AVBHOME/daemon_cl $INTERFACE $GPTP_SYNTONIZE   2>&1 | logger -t gptp   &
$AVBHOME/mrpd -mvs -i $INTERFACE           2>&1 | logger -t mrpd   &
# maap_daemon is the odd one out: its log goes to stderr, and `-d <file>` makes
# it DAEMONIZE (double-fork + close all fds + reopen its log to <file>).  The old
# "-d /dev/null 2>&1 | logger" therefore detached it from the pipe at startup and
# sent every line to /dev/null, so `journalctl -t maap` was always empty (the
# daemon itself was running fine, just mute).  Keep its proper daemonized mode but
# point the log at a FIFO we drain into journald; the reader hits EOF and exits by
# itself when the daemon is killed, so avb_down.sh's by-name reaping still works.
MAAP_FIFO=$AVBHOME/maap.fifo
[ -p "$MAAP_FIFO" ] || { rm -f "$MAAP_FIFO"; mkfifo "$MAAP_FIFO"; }
logger -t maap < "$MAAP_FIFO" &
$AVBHOME/maap_daemon -i $INTERFACE -d "$MAAP_FIFO"
$AVBHOME/shaper_daemon -d                  2>&1 | logger -t shaper &

sleep 10

# Real-time priorities (generic -- needed on every machine).  A DAW's audio
# threads run SCHED_FIFO (REAPER defaults to 40) and would otherwise preempt the
# AVB data plane, so the stack is ordered above them:
#   ktimers/N      AVB_KTIMERS_PRIO  60  TX engine: a soft hrtimer, which runs in
#                                        ktimers/N on threadirqs kernels (FIFO 1
#                                        by default -- below any DAW)
#   irq/N-<iface>  AVB_IRQ_PRIO      50  RX ring + NIC interrupts (kernel default)
#   daemon_cl      AVB_GPTP_PRIO     45  gPTP; starved -> sync lost -> device unlocks
#   DAW audio      < AVB_GPTP_PRIO       e.g. REAPER RT priority 40
# mrpd / maap / shaper / avb-user only run protocol timers (seconds), so they
# get a nice level instead of an RT class.  Override any of these after sudo,
# e.g. `sudo AVB_GPTP_PRIO=55 ./avb_up.sh ...`; per-machine tuning goes in
# $AVB_TUNING (below), which runs afterwards and can override these too.
AVB_KTIMERS_PRIO=${AVB_KTIMERS_PRIO:-60}
AVB_IRQ_PRIO=${AVB_IRQ_PRIO:-50}
AVB_GPTP_PRIO=${AVB_GPTP_PRIO:-45}
AVB_DAEMON_NICE=${AVB_DAEMON_NICE:--10}
export AVB_KTIMERS_PRIO AVB_IRQ_PRIO AVB_GPTP_PRIO AVB_DAEMON_NICE

# set_fifo <prio> <label> <pid>...  -- prints how many threads it moved
set_fifo() {
    local prio=$1 label=$2 pid n=0
    shift 2
    for pid in "$@"; do
        chrt -f -p "$prio" "$pid" >/dev/null && n=$((n + 1)) ||
            echo "WARNING: chrt -f $prio failed for $label (pid $pid)" >&2
    done
    echo "  $label: SCHED_FIFO $prio ($n thread(s))"
}

echo "Real-time priorities:"
KTIMERS=$(pgrep -x 'ktimers/[0-9]+')
if [ -n "$KTIMERS" ]; then
    set_fifo "$AVB_KTIMERS_PRIO" ktimers $KTIMERS
else
    echo "  NOTE: no ktimers threads (kernel without threadirqs?) -- the TX engine" >&2
    echo "        runs in softirq/ksoftirqd and cannot be prioritised above a DAW" >&2
fi
# IRQ thread names are truncated to 15 chars, so match by IRQ number, not iface.
IRQ_THREADS=""
for irq in $(awk -v i="$INTERFACE" '$NF ~ "^" i { sub(":", "", $1); print $1 }' /proc/interrupts); do
    IRQ_THREADS="$IRQ_THREADS $(pgrep "^irq/$irq-")"
done
if [ -n "${IRQ_THREADS// /}" ]; then
    set_fifo "$AVB_IRQ_PRIO" "irq/$INTERFACE" $IRQ_THREADS
else
    echo "  NOTE: no threaded IRQs for $INTERFACE (boot with threadirqs)" >&2
fi
set_fifo "$AVB_GPTP_PRIO" daemon_cl $(pgrep -x daemon_cl)
for d in mrpd maap_daemon shaper_daemon; do
    renice -n "$AVB_DAEMON_NICE" -p $(pgrep -x $d) >/dev/null 2>&1 ||
        echo "WARNING: renice failed for $d" >&2
done
echo "  mrpd/maap/shaper/avb-user: nice $AVB_DAEMON_NICE"
echo "  -> keep DAW audio threads below FIFO $AVB_GPTP_PRIO"

# Per-machine tuning (CPU governor, IRQ affinity, ...) that every user adapts to
# their own box.  `make install` seeds it from avb_tuning.example.sh and never
# overwrites it; avb_down.sh calls it again with "down" to undo.  It runs as a
# child with INTERFACE / SAMPLERATE / NUM_STREAMS / AVB_*_PRIO exported, so a
# failure there cannot stop the stack.  AVB_TUNING= (empty) skips it.
AVB_TUNING=${AVB_TUNING-$AVBHOME/avb-tuning.sh}
if [ -n "$AVB_TUNING" ] && [ -f "$AVB_TUNING" ]; then
    echo "Per-machine tuning: $AVB_TUNING up"
    bash "$AVB_TUNING" up || echo "WARNING: $AVB_TUNING up exited with $?" >&2
fi

# avb-user runs in the foreground and owns the console (status screen); its
# detailed per-layer log also goes to syslog (tag avb-user).
nice -n "$AVB_DAEMON_NICE" $AVBHOME/avb-user $INTERFACE $SAMPLERATE $NUM_STREAMS