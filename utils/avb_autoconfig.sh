#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# One-shot auto-detection of the AVB device MACs, written to the runtime conf
# ($AVBHOME/avb-macs.conf) via avb_apply_mac.sh.  No rebuild -- the values are
# applied by avb_up.sh on the next module load.
# Works with any AVB/IEEE-1722.1 endpoint.
#
#   OWN_MAC                    = the i210's own MAC                       -> read from sysfs
#   AVB_DEVICE_SOURCE_MAC      = the device's MAC (it announces via ADP)  -> sniffed
#   AVB_DEVICE_TALKER_MAC_BASE = its talker stream multicast              -> sniffed
#
# Run as root (raw capture).  Usage:
#   sudo ./utils/avb_autoconfig.sh <interface> [seconds] [name-match]
#     interface   NIC to read OWN_MAC from and sniff on   (default enp5s0)
#     seconds     capture duration                         (default 60)
#     name-match  preferred AVDECC entity-name substring   (default: none -> busiest)
#
# Prereqs: the AVB device powered, cabled to the i210, AVB enabled on that port, and
#          the stock igb driver still loaded -- run this BEFORE avb_up.sh.  Needs tshark.
#
# Device pick: any endpoint that emits ADP (AVTP subtype 0xfa, or 0x7a on older
# Wireshark that masks the control bit) is a candidate. If several answer, the one
# whose AVDECC entity name matches name-match is chosen; otherwise the busiest one
# that actually advertises a talker stream. The talker base is the MSRP-advertised
# stream destination (stream_da), else the MAAP-announced reserved base.
set -e
IFACE=${1:-enp5s0}
SECS=${2:-60}
WANT=${3:-}
HERE="$(cd "$(dirname "$0")" && pwd)"

norm() { echo "$1" | tr -d ':-' | tr 'A-F' 'a-f'; }

# --- OWN_MAC straight from the interface ---------------------------------
OWN=$(cat "/sys/class/net/$IFACE/address" 2>/dev/null) || {
  echo "no such interface: $IFACE" >&2; exit 1; }
[ ${#OWN} -eq 17 ] || { echo "could not read MAC for $IFACE" >&2; exit 1; }
echo "OWN_MAC ($IFACE) = $OWN"

# --- one capture (AVDECC 0x22f0 + MSRP 0x22ea), parsed several times ------
command -v tshark >/dev/null || { echo "tshark not found" >&2; exit 1; }
PCAP=$(mktemp --suffix=.pcap)
trap 'rm -f "$PCAP"' EXIT
echo "Sniffing AVB (AVDECC 0x22f0 + MSRP 0x22ea) on $IFACE for ${SECS}s ..."
# Promiscuous (no -p): MAAP and ADP use different multicast groups, and a NIC
# that isn't in promisc mode may drop the MAAP announces we need for the talker base.
timeout "$SECS" tshark -i "$IFACE" -f "ether proto 0x22f0 or ether proto 0x22ea" \
  -w "$PCAP" 2>/dev/null || true

# ADP frames carry AVTP subtype 0xfa (full first byte, control bit set); older
# Wireshark masks the control bit and reports 0x7a, so accept both.
ADP_FILTER="ieee1722.subtype == 0xfa || ieee1722.subtype == 0x7a"

# Candidate devices = sources of ADP (AVDECC discovery) frames, busiest first
mapfile -t SRCS < <(tshark -r "$PCAP" -Y "$ADP_FILTER" -T fields -e eth.src \
  2>/dev/null | grep -E '^([0-9a-f]{2}:){5}[0-9a-f]{2}$' | sort | uniq -c | sort -rn \
  | awk '{print $2}')
[ ${#SRCS[@]} -gt 0 ] || {
  echo "No AVB endpoint announced itself (no ADP / subtype 0xfa). AVTP sources seen:" >&2
  tshark -r "$PCAP" -T fields -e eth.src -e ieee1722.subtype 2>/dev/null \
    | sort | uniq -c | sort -rn >&2
  exit 1; }

# Talker stream multicast advertised by <mac>.  Parsed from tshark's default
# text output (the human-readable Info, e.g. "MAAP_ANNOUNCE: req_start=...,") so
# it does not depend on version-specific field names.  -n keeps the source column
# as a raw MAC so we can match it; the req_start=/stream_da= prefix means we never
# mistake the frame's destination multicast for the talker base.
talker_base_for() {  # mac -> talker MAC (most frequent), or empty
  tshark -n -r "$PCAP" 2>/dev/null \
    | grep -iF "$1" \
    | grep -oiE '(req_start=|stream_da[ =:]+)([0-9a-f]{2}:){5}[0-9a-f]{2}' \
    | grep -oiE '([0-9a-f]{2}:){5}[0-9a-f]{2}' \
    | sort | uniq -c | sort -rn | head -1 | awk '{print $NF}'
}

# Talker base per candidate (used for selection and for the multi-device list).
declare -A TB
for mac in "${SRCS[@]}"; do TB[$mac]=$(talker_base_for "$mac"); done

# WANT matches an AVDECC entity name OR (always reliable) a MAC substring.
match_want() {  # -> matching SRC, or empty
  local mac name
  for mac in "${SRCS[@]}"; do
    case "${mac,,}" in *"${WANT,,}"*) echo "$mac"; return;; esac
  done
  while IFS=$'\t' read -r mac name; do
    [ -n "$name" ] || continue
    case "${name,,}" in *"${WANT,,}"*) echo "$mac"; return;; esac
  done < <(tshark -r "$PCAP" -Y "ieee17221.entity_name" \
            -T fields -e eth.src -e ieee17221.entity_name 2>/dev/null)
}

# Choose the device: name/MAC match if asked, else the busiest source that
# advertises a talker stream, else just the busiest.
SRC=""
[ -n "$WANT" ] && { SRC=$(match_want); [ -n "$SRC" ] && echo "Matched \"$WANT\" -> $SRC"; }
if [ -z "$SRC" ]; then
  for mac in "${SRCS[@]}"; do [ -n "${TB[$mac]}" ] && { SRC="$mac"; break; }; done
  [ -n "$SRC" ] || SRC="${SRCS[0]}"
fi
TALK="${TB[$SRC]}"

if [ ${#SRCS[@]} -gt 1 ]; then
  echo "Multiple AVB devices seen:"
  for mac in "${SRCS[@]}"; do printf '  %s  talker=%s\n' "$mac" "${TB[$mac]:-<none>}"; done
  echo "Using $SRC. To target another, re-run with a name/MAC match arg"
  echo "(e.g. '${SRCS[1]##*:}') or call avb_apply_mac.sh to set them by hand."
fi
echo "AVB_DEVICE_SOURCE_MAC = $SRC"

[ -n "$TALK" ] || {
  echo "No talker multicast advertised by $SRC (no MAAP announce / MSRP stream_da)." >&2
  echo "Set it by hand once known, e.g.:" >&2
  echo "  $HERE/avb_apply_mac.sh $SRC 91:e0:f0:00:ae:53 $OWN" >&2
  exit 1; }
echo "AVB_DEVICE_TALKER_MAC_BASE = $TALK"
echo

# --- write all three to the runtime conf (no rebuild) --------------------
echo "$HERE/avb_apply_mac.sh" "$SRC" "$TALK" "$OWN"
IFACE="$IFACE" exec "$HERE/avb_apply_mac.sh" "$SRC" "$TALK" "$OWN"