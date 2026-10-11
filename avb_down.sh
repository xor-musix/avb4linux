#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2019 Ralf Beck
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Stop all daemons and unload snd_igb_avb.
#
# The daemons are started as root by avb_up.sh, so this must run as root too;
# otherwise killall silently fails to signal them and they keep running, which
# in turn makes "sudo make install" fail with "Text file busy" when it tries to
# overwrite a still-executing binary.

# Re-exec under sudo if not already root.
if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$@"
fi

export INTERFACE=$1

# Optional: hand the port back to NetworkManager.  avb_down.sh is normally just
# "unload the realtime driver" (e.g. to re-stage binaries), after which you run
# avb_up.sh again -- so by default we LEAVE the port unmanaged and avoid churning
# its NM state.  Pass --restore-nm (or set AVB_RESTORE_NM=1) on a final teardown
# when you actually want normal networking back on this interface.
case "$2" in --restore-nm) AVB_RESTORE_NM=1 ;; esac

# Send SIGTERM, then poll until the process is really gone, escalating to
# SIGKILL.  Returns only once no process of that name remains, so a following
# "make install" can safely overwrite the binary.
stop_daemon() {
    local name=$1 i

    pgrep -x "$name" >/dev/null 2>&1 || return 0

    killall -q "$name" 2>/dev/null
    for i in $(seq 1 10); do            # up to ~5s for a graceful exit
        pgrep -x "$name" >/dev/null 2>&1 || return 0
        sleep 0.5
    done

    echo "$name did not exit on SIGTERM; sending SIGKILL" >&2
    killall -q -9 "$name" 2>/dev/null
    for i in $(seq 1 10); do
        pgrep -x "$name" >/dev/null 2>&1 || return 0
        sleep 0.5
    done

    echo "WARNING: $name is still running" >&2
    return 1
}

stop_daemon avb-user
stop_daemon shaper_daemon
stop_daemon maap_daemon
stop_daemon mrpd
stop_daemon daemon_cl

# remove snd_igb_avb module

if [ -n "$INTERFACE" ]; then
    ifconfig "$INTERFACE" down
else
    echo "No interface given; skipping 'ifconfig <iface> down'." >&2
fi

rmmod snd_igb_avb 2>/dev/null || true

modprobe igb

sleep 5

# Re-binding to igb leaves the interface administratively down; bring it back
# up.  After modprobe the netdev may take a moment to reappear, so wait for it.
if [ -n "$INTERFACE" ]; then
    for i in $(seq 1 10); do
        [ -e "/sys/class/net/$INTERFACE" ] && break
        sleep 0.5
    done
    if [ -e "/sys/class/net/$INTERFACE" ]; then
        ip link set "$INTERFACE" up
    else
        echo "WARNING: $INTERFACE did not reappear after 'modprobe igb'" >&2
    fi
fi

# Undo the per-machine tuning avb_up.sh applied (see AVB_TUNING there).
export AVBHOME=${AVBHOME:-$HOME/.avb}
AVB_TUNING=${AVB_TUNING-$AVBHOME/avb-tuning.sh}
if [ -n "$AVB_TUNING" ] && [ -f "$AVB_TUNING" ]; then
    echo "Per-machine tuning: $AVB_TUNING down"
    bash "$AVB_TUNING" down || echo "WARNING: $AVB_TUNING down exited with $?" >&2
fi

# Give the interface back to NetworkManager only when explicitly asked (see top).
if [ -n "$INTERFACE" ] && [ "${AVB_RESTORE_NM:-0}" = "1" ]; then
    helper="$(dirname "$0")/avb_nm_unmanage.sh"
    [ -x "$helper" ] || helper="$(dirname "$0")/utils/avb_nm_unmanage.sh"
    [ -x "$helper" ] || helper="$(command -v avb_nm_unmanage.sh)"
    if [ -n "$helper" ]; then
        "$helper" -r "$INTERFACE"
    else
        echo "avb_nm_unmanage.sh not found; cannot restore NM management of $INTERFACE." >&2
    fi
fi

ip link show
