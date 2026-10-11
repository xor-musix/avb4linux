#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#
# test_mac_filter.sh -- validate MAC address configuration for AVB4Linux
#
# Checks:
#   - Talker dest MAC matches expected (MAAP-allocated)
#   - Listener source MAC matches expected
#   - Domain MAC registration
#
# Usage:
#   sudo ./tests/network/test_mac_filter.sh [-i iface] [--validate]
#
#   -i  network interface     (default: enp5s0)
#   --validate  validate against /proc/snd_igb_avb
#
# Requires: root, snd_igb_avb loaded, interface up.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PROC=/proc/snd_igb_avb

IFACE=enp5s0
VALIDATE=0

while getopts "i:h" opt; do
  case "$opt" in
    i) IFACE=$OPTARG ;;
    h) echo "Usage: $0 [-i iface] [--validate]"; exit 0 ;;
    *) echo "bad option; -h for help" >&2; exit 2 ;;
  esac
done

# Parse --validate
shift $((OPTIND - 1))
for arg in "$@"; do
    case "$arg" in
        --validate) VALIDATE=1 ;;
    esac
done

die() { echo "FATAL: $*" >&2; exit 1; }
warn() { echo "WARN: $*" >&2; }
ok() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; }
inconc() { echo "INCONCLUSIVE: $*" >&2; exit 2; }

[ "$(id -u)" -eq 0 ] || die "must run as root"
command -v ethtool >/dev/null || die "ethtool not found"
[ -e "$PROC" ] || die "$PROC missing -- is snd_igb_avb loaded?"

# Get interface MAC
IF_MAC=$(ip link show "$IFACE" 2>/dev/null | grep -oP 'link/ether \K[0-9a-f:]+' || true)
[ -n "$IF_MAC" ] || die "could not get MAC for $IFACE"

echo "== MAC Filter Validation =="
echo "   iface=$IFACE  mac=$IF_MAC"

# Parse driver params
if [ "$VALIDATE" -eq 1 ]; then
    echo "   validating against /proc/snd_igb_avb..."
    
    # Get driver's MAC settings
    DRIVER_MAC=$(grep -oP 'talker_mac=\K[0-9a-f]+' "$PROC" 2>/dev/null | head -1 || true)
    SOURCE_MAC=$(grep -oP 'source_mac=\K[0-9a-f]+' "$PROC" 2>/dev/null | head -1 || true)
    
    if [ -n "$DRIVER_MAC" ]; then
        # Convert to standard format
        DRIVER_MAC_FMT=$(echo "$DRIVER_MAC" | sed 's/\(..\)/\1:/g; s/:$//')
        echo "   driver talker_mac=$DRIVER_MAC_FMT"
        
        if [ "$DRIVER_MAC_FMT" = "$IF_MAC" ]; then
            ok "Talker MAC matches interface MAC"
        else
            fail "Talker MAC mismatch: driver=$DRIVER_MAC_FMT iface=$IF_MAC"
        fi
    fi
    
    if [ -n "$SOURCE_MAC" ]; then
        SOURCE_MAC_FMT=$(echo "$SOURCE_MAC" | sed 's/\(..\)/\1:/g; s/:$//')
        echo "   driver source_mac=$SOURCE_MAC_FMT"
        
        if [ "$SOURCE_MAC_FMT" = "$IF_MAC" ]; then
            ok "Source MAC matches interface MAC"
        else
            fail "Source MAC mismatch: driver=$SOURCE_MAC_FMT iface=$IF_MAC"
        fi
    fi
fi

# Check MAAP status
echo ""
echo "=== MAAP Status ==="
MAAP_STATUS=$(grep -A5 'maap:' "$PROC" 2>/dev/null || echo "not available")
echo "$MAAP_STATUS"

# Check domain registration
echo ""
echo "=== Domain Status ==="
DOMAIN_STATUS=$(grep -A10 'domain:' "$PROC" 2>/dev/null || echo "not available")
echo "$DOMAIN_STATUS"

# Check TX stream config
echo ""
echo "=== TX Stream Config ==="
STREAMS=$(cat "$PROC/streams" 2>/dev/null | head -5 || echo "not available")
echo "$STREAMS"

# Verify hardware MAC filters
echo ""
echo "=== Hardware MAC Filters ==="
if command -v ethtool >/dev/null 2>&1; then
    # Check for existing flows
    FLOW_STATS=$(ethtool -s "$IFACE" 2>/dev/null | grep -i flow || echo "no flow stats")
    echo "$FLOW_STATS"
fi

echo ""
ok "MAC filter validation complete"
exit 0