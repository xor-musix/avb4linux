#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#
# test_network.sh -- orchestrator for AVB4Linux network-layer regression tests
#
# Runs all network tests in sequence:
#   1. test_mac_filter.sh    - MAC address configuration
#   2. test_multistream.sh   - Multi-stream frame validation
#   3. test_stress.sh        - Long-duration stress test
#
# Usage:
#   sudo ./tests/network/test_network.sh [-i iface] [-n streams] [-d dur] [-t stress_secs]
#
#   -i  interface             (default: enp5s0)
#   -n  num_streams           (default: 2)
#   -d  multistream duration  (default: 10)
#   -t  stress test duration  (default: 60)
#
# Exit codes:
#   0 = all tests passed
#   1 = at least one test failed
#   2 = preflight failed (missing dependencies, etc.)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$ROOT/tests/network"

IFACE=enp5s0
NUM_STREAMS=2
MULTI_DUR=10
STRESS_DUR=60
FAILED=0

while getopts "i:n:d:t:h" opt; do
  case "$opt" in
    i) IFACE=$OPTARG ;;
    n) NUM_STREAMS=$OPTARG ;;
    d) MULTI_DUR=$OPTARG ;;
    t) STRESS_DUR=$OPTARG ;;
    h) echo "Usage: $0 [-i iface] [-n streams] [-d dur] [-t stress_secs]"; exit 0 ;;
    *) echo "bad option; -h for help" >&2; exit 2 ;;
  esac
done

die() { echo "FATAL: $*" >&2; exit 2; }
warn() { echo "WARN: $*" >&2; }
ok() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; }
summary(){ echo ""; echo "=== SUMMARY ==="; echo "  mac_filter: ${MAC_RESULT:-not run}"; echo "  multistream: ${MULTI_RESULT:-not run}"; echo "  stress: ${STRESS_RESULT:-not run}"; echo "  total failures: $FAILED"; }

echo "== AVB4Linux Network Regression Test Suite =="
echo "   interface=$IFACE  streams=$NUM_STREAMS"
echo "   multistream duration=${MULTI_DUR}s  stress duration=${STRESS_DUR}s"
echo ""

# Preflight checks
echo "=== Preflight ==="
for cmd in tcpdump python3 ethtool; do
    command -v "$cmd" >/dev/null || die "missing $cmd"
done

[ -e /proc/snd_igb_avb ] || die "/proc/snd_igb_avb not found - is snd_igb_avb loaded?"

# Check for TX mirror parameter
[ -e /sys/module/snd_igb_avb/parameters/avb_tx_mirror ] || \
    warn "TX mirror not available - some tests may be limited"

ip link show "$IFACE" >/dev/null 2>&1 || die "interface $IFACE not found"

# Test 1: MAC Filter
echo ""
echo "=== Test 1: MAC Filter ==="
if bash "$HERE/test_mac_filter.sh" -i "$IFACE" --validate 2>&1 | tee /tmp/mac_filter.log; then
    MAC_RESULT=PASS
else
    MAC_RESULT=FAIL
    FAILED=$((FAILED + 1))
fi

# Test 2: Multi-stream
echo ""
echo "=== Test 2: Multi-stream ==="
if bash "$HERE/test_multistream.sh" -i avbtap0 -n "$NUM_STREAMS" -d "$MULTI_DUR" 2>&1 | tee /tmp/multistream.log; then
    MULTI_RESULT=PASS
else
    MULTI_RESULT=FAIL
    FAILED=$((FAILED + 1))
fi

# Test 3: Stress
echo ""
echo "=== Test 3: Stress Test ==="
if bash "$HERE/test_stress.sh" -n "$NUM_STREAMS" -t "$STRESS_DUR" -i avbtap0 2>&1 | tee /tmp/stress.log; then
    STRESS_RESULT=PASS
else
    STRESS_RESULT=FAIL
    FAILED=$((FAILED + 1))
fi

# Summary
summary

if [ "$FAILED" -eq 0 ]; then
    ok "All network tests passed"
    exit 0
else
    fail "$FAILED test(s) failed"
    exit 1
fi