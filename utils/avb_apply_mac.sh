#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Write the AVB device MACs into the runtime conf ($AVBHOME/avb-macs.conf).
# avb_up.sh reads them at module-load time -- no rebuild needed.
#
# Usually called for you by avb_autoconfig.sh.  Use it directly to override the
# auto-detected values by hand.
#
# Usage: ./utils/avb_apply_mac.sh <AVB_DEVICE_SOURCE_MAC> <AVB_DEVICE_TALKER_MAC_BASE> [OWN_MAC]
#   MACs as 12 hex digits, separators optional, e.g.:
#     ./utils/avb_apply_mac.sh 00:01:f2:aa:bb:cc 91:e0:f0:00:1c:86 00:1b:21:aa:bb:cc
#   OWN_MAC is optional (the i210's own MAC / AVTP source); if omitted it is left
#   out of the conf and the kernel module defaults to the NIC's hardware MAC.
set -e
AVBHOME=${AVBHOME:-$HOME/.avb}
norm()  { echo "$1" | tr -d ':-' | tr 'A-F' 'a-f'; }
colon() { norm "$1" | sed 's/../&:/g; s/:$//'; }   # 12 hex digits -> aa:bb:cc:dd:ee:ff
SRC=$(norm "${1:-}"); TALK=$(norm "${2:-}"); OWN=$(norm "${3:-}")
{ [ ${#SRC} -eq 12 ] && [ ${#TALK} -eq 12 ]; } || {
  echo "usage: $0 <SOURCE_MAC> <TALKER_BASE> [OWN_MAC]  (12 hex digits each)"; exit 1; }
[ -n "$OWN" ] && [ ${#OWN} -ne 12 ] && {
  echo "OWN_MAC must be 12 hex digits if given"; exit 1; }

mkdir -p "$AVBHOME"
CONF="$AVBHOME/avb-macs.conf"
{
  echo "# AVB device MACs, read at runtime by avb_up.sh."
  echo "# Re-run avb_apply_mac.sh / avb_autoconfig.sh, or edit by hand."
  echo "AVB_DEVICE_SOURCE_MAC=$(colon "$SRC")"
  echo "AVB_DEVICE_TALKER_MAC_BASE=$(colon "$TALK")"
  [ -n "$OWN" ] && echo "OWN_MAC=$(colon "$OWN")"
} > "$CONF"
echo "Wrote $CONF:"; cat "$CONF"; echo
echo "No rebuild needed -- the values are applied on the next avb_up.sh."
echo
echo "Next:  sudo avb_up.sh ${IFACE:-<interface>} 48000   (run from the physical console)"