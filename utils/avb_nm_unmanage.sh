#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Hand a dedicated AVB port to a single owner by telling NetworkManager to leave
# it alone.  This is a ONE-TIME setup step, not something to run on every start:
# it writes a persistent keyfile drop-in so NM never auto-activates the interface
# again (across reboots and NM restarts).
#
# Why: snd_igb_avb wires the whole AVB device lifecycle onto the netdev open/close
# path (__igb_open -> snd_avb_probe, __igb_close -> snd_avb_remove).  If NM is
# managing the port it will up/down it on its own schedule -- carrier changes,
# DHCP/RA, connectivity checks, profile reloads -- and EVERY such transition tears
# down and rebuilds the ALSA card, DMA buffers and RX flex filter underneath a
# running stream.  Removing the port from NM removes that second controller, so
# the only up/down transitions are the ones avb_up.sh / avb_down.sh make.
#
# This is declarative and reversible -- the opposite of toggling `nmcli device set
# ... managed no` imperatively each run, which races the very auto-activation it
# is trying to suppress.
#
# Usage:
#   sudo ./utils/avb_nm_unmanage.sh <interface>        # release the port from NM
#   sudo ./utils/avb_nm_unmanage.sh -r <interface>     # restore NM management of it
#
# Pass the SAME <interface> you give avb_up.sh.  We match on interface-name, and
# the predictable name (e.g. enp5s0) is derived from the PCI slot by udev -- it is
# identical whether the stock igb or the realtime igb_avb driver created the port,
# and it does NOT change with the driver's own_mac= parameter.  (That is why we do
# NOT match on mac:: own_mac can rewrite the MAC, which would silently break the
# match and let NM grab the port back.)  The brief "eth0 -> enp5s0" rename is
# internal to udev; NM waits for udev to settle the name before deciding managed
# state, so the rule is evaluated against the final name.
#
# If NetworkManager is not the active network manager (systemd-networkd, ifupdown,
# netplan, ...), this prints a note and does nothing -- assert "leave this port
# alone" against whatever manager your host actually runs.

set -e

DROPIN=/etc/NetworkManager/conf.d/99-avb-unmanaged.conf
KEY=unmanaged-devices

usage() { echo "usage: $0 [-r|--restore] <interface>" >&2; exit 2; }

RESTORE=0
case "$1" in
    -r|--restore) RESTORE=1; shift ;;
    -h|--help)    usage ;;
esac
IFACE=$1
[ -n "$IFACE" ] || usage

# Re-exec under sudo if not root (we write under /etc and talk to NM).
if [ "$(id -u)" -ne 0 ]; then
    [ "$RESTORE" -eq 1 ] && exec sudo "$0" -r "$IFACE"
    exec sudo "$0" "$IFACE"
fi

# No NetworkManager -> nothing for us to do.  Don't pretend we secured the port.
if ! command -v nmcli >/dev/null 2>&1; then
    echo "NetworkManager (nmcli) not found; nothing to do." >&2
    echo "If another manager (systemd-networkd/netplan/ifupdown) owns $IFACE," >&2
    echo "mark it unmanaged there so it does not auto-up the AVB port." >&2
    exit 0
fi

# --- read the interfaces we currently keep unmanaged (from OUR drop-in only) ---
# Stored as "interface-name:eth0;interface-name:eth1".  We only ever touch the
# entries in this file, so a hand-rolled NM config elsewhere is left intact.
declare -a IFACES=()
if [ -f "$DROPIN" ]; then
    line=$(grep -E "^[[:space:]]*$KEY[[:space:]]*=" "$DROPIN" | head -1 | cut -d= -f2-)
    IFS=';' read -r -a ENTRIES <<< "$line"
    for e in "${ENTRIES[@]}"; do
        e="${e#"${e%%[![:space:]]*}"}"; e="${e%"${e##*[![:space:]]}"}"   # trim
        [ -n "$e" ] && IFACES+=("${e#interface-name:}")
    done
fi

# --- add or remove $IFACE from that set --------------------------------------
declare -a NEW=()
present=0
for i in "${IFACES[@]}"; do
    if [ "$i" = "$IFACE" ]; then present=1; [ "$RESTORE" -eq 1 ] && continue; fi
    NEW+=("$i")
done
if [ "$RESTORE" -eq 0 ] && [ "$present" -eq 0 ]; then NEW+=("$IFACE"); fi

# --- rewrite (or remove) the drop-in -----------------------------------------
if [ "${#NEW[@]}" -eq 0 ]; then
    rm -f "$DROPIN"
else
    val=""
    for i in "${NEW[@]}"; do val="${val:+$val;}interface-name:$i"; done
    install -d -m 755 "$(dirname "$DROPIN")"
    cat > "$DROPIN" <<EOF
# Managed by avb_nm_unmanage.sh -- do not edit by hand.
# Keeps the dedicated AVB port(s) out of NetworkManager so it does not up/down
# the link (and thereby tear down the running AVB device) on its own schedule.
[keyfile]
$KEY=$val
EOF
fi

# Reload NM so the drop-in takes effect, and converge the CURRENT session too:
# on first setup NM already holds the port (managed), so reload alone won't drop
# it until the next event -- set it explicitly.  This is a one-time setup action,
# not a per-run toggle.
nmcli general reload 2>/dev/null || systemctl reload NetworkManager 2>/dev/null || true
if [ "$RESTORE" -eq 1 ]; then
    nmcli device set "$IFACE" managed yes 2>/dev/null || true
    echo "Restored NetworkManager management of $IFACE."
else
    nmcli device set "$IFACE" managed no  2>/dev/null || true
    echo "Released $IFACE from NetworkManager (persistent: $DROPIN)."
fi
