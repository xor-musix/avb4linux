#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Per-machine tuning hook for the AVB stack -- TEMPLATE.
#
# `make install` copies this file to $AVBHOME/avb-tuning.sh (~/.avb) once and
# never overwrites it, so edit THAT copy for your own machine; this template
# only changes with the repo.  The generic real-time ordering (ktimers / NIC
# IRQs / gPTP above the DAW) is already applied by avb_up.sh -- put here only
# what depends on your hardware and taste.
#
# Called as root, as a child process:
#   avb_up.sh   -> avb-tuning.sh up    (stack running, just before avb-user)
#   avb_down.sh -> avb-tuning.sh down  (after the module is unloaded)
# Exported by avb_up.sh: INTERFACE SAMPLERATE NUM_STREAMS AVBHOME
#   AVB_KTIMERS_PRIO AVB_IRQ_PRIO AVB_GPTP_PRIO AVB_DAEMON_NICE
# avb_down.sh exports only AVBHOME (and INTERFACE).
# Skip the hook for one run with `sudo AVB_TUNING= avb_up.sh ...`.

# ---- adapt these ------------------------------------------------------------

# CPU frequency governor while AVB runs ("" = leave alone).  `performance`
# avoids clock ramp-up latency on bursty RT work (TX engine, DAW blocks);
# the previous governor is restored on "down".
TUNE_GOVERNOR=performance

# CPUs for the NIC's interrupts ("" = leave alone, "numa" = the CPUs of the
# NIC's NUMA node, or an explicit list such as "2-3").  Only matters on
# multi-socket machines; IRQs are re-created on every module load, so "down"
# has nothing to undo.
TUNE_IRQ_CPUS=""

# Desktop sound servers: if PipeWire/WirePlumber opens hw:igbavb, a DAW gets
# "device busy".  Disable the card in WirePlumber (user session, not root), e.g.
# ~/.config/wireplumber/wireplumber.conf.d/51-igbavb-off.conf with a rule
# matching alsa.card_name = "IGB-AVB" -> device.disabled = true.

# ---- implementation ----------------------------------------------------------

AVBHOME=${AVBHOME:-$HOME/.avb}
GOV_SAVED=$AVBHOME/.tuning-governor.saved

governor_up() {
    local f
    [ -n "$TUNE_GOVERNOR" ] || return 0
    # Save once: a second "up" without "down" must not save "performance".
    [ -f "$GOV_SAVED" ] || for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
        echo "$f $(cat "$f")"
    done > "$GOV_SAVED"
    for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
        echo "$TUNE_GOVERNOR" > "$f" 2>/dev/null
    done
    echo "  governor: $TUNE_GOVERNOR"
}

governor_down() {
    local f gov
    [ -f "$GOV_SAVED" ] || return 0
    while read -r f gov; do
        echo "$gov" > "$f" 2>/dev/null
    done < "$GOV_SAVED"
    rm -f "$GOV_SAVED"
    echo "  governor: restored"
}

irq_affinity_up() {
    local cpus=$TUNE_IRQ_CPUS node irq
    [ -n "$cpus" ] && [ -n "$INTERFACE" ] || return 0
    if [ "$cpus" = numa ]; then
        node=$(cat "/sys/class/net/$INTERFACE/device/numa_node" 2>/dev/null)
        [ -n "$node" ] && [ "$node" -ge 0 ] || { echo "  irq affinity: no NUMA node for $INTERFACE" >&2; return 0; }
        cpus=$(cat "/sys/devices/system/node/node$node/cpulist")
    fi
    for irq in $(awk -v i="$INTERFACE" '$NF ~ "^" i { sub(":", "", $1); print $1 }' /proc/interrupts); do
        echo "$cpus" > "/proc/irq/$irq/smp_affinity_list" ||
            echo "  irq affinity: IRQ $irq refused $cpus" >&2
    done
    echo "  irq affinity: $INTERFACE -> CPUs $cpus"
}

case "$1" in
    up)
        governor_up
        irq_affinity_up
        ;;
    down)
        governor_down
        ;;
    *)
        echo "usage: $0 up|down" >&2
        exit 1
        ;;
esac
