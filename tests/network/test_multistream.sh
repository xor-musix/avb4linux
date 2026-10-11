#!/usr/bin/env bash
#
# test_multistream.sh -- validate multi-stream AVB TX behavior using the
# in-driver TX capture tap (avb_tx_mirror -> avbtap0).
#
# The self-clocked TX engine DMAs AVTP frames straight to the i210, bypassing the
# kernel network stack, so tcpdump on the sending host normally sees nothing.
# The driver's avb_tx_mirror gate copies each *built* frame onto a dummy netdev
# (avbtap0) so we can decode exactly what was packed and check:
#
#   * N AVTP packets per PHC tick (N = num_streams)
#   * each carries an 8-channel slice  (payload = 8 * samples_per_packet * 4)
#   * per-stream stream_id = (mac << 16) | s   for s in [0, N)
#   * seq/dbc advance once per tick, contiguous per stream
#   * SYT (TIMESTAMP_VALID) appears on a regular cadence, not every packet
#
# It verifies what the driver BUILT, not what the PHC launched on the wire --
# for on-wire launch-time validation you still need a second machine / tap.
#
# Usage:
#   sudo ./tests/network/test_multistream.sh [-i iface] [-d secs] [-o pcap] [-n streams]
#                                            [-w file.wav [-D alsa_dev]]
#
#   -i  capture interface            (default: avbtap0)
#   -d  capture duration in seconds  (default: 6)
#   -o  output pcap path             (default: $TMP_DIR/tx-mirror.pcap)
#   -n  expected num_streams         (default: read from /proc/snd_igb_avb)
#   -w  wav to aplay during capture  (optional; else play audio yourself)
#   -D  ALSA device for aplay        (default: default)
#
# Requires: root, tcpdump, python3, and the snd_igb_avb module loaded.

# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$ROOT/tests/network"
IR="$HERE/tools/pcap_analyze.py"

# Load environment configuration
if [ -f "$HERE/.env" ]; then
    # shellcheck source=tests/network/.env
    . "$HERE/.env"
else
    # Fallback defaults
    USER_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
    TMP_DIR="$USER_HOME/tmp"
    AVB_STRESS_DIR="$TMP_DIR/avb_stress_test"
fi

IFACE=avbtap0
DUR=6
PCAP=$TMP_DIR/tx-mirror.pcap
EXPECT_N=""
WAV=""
ADEV=default
PROC=/proc/snd_igb_avb
PARAM=/sys/module/snd_igb_avb/parameters/avb_tx_mirror

# First pass: extract --baseline file if present
BASELINE_FILE=""
TEMP_ARGS=()
FOUND_BASELINE=0
for arg in "$@"; do
    if [ "$FOUND_BASELINE" -eq 1 ]; then
        # Next arg after --baseline
        if [[ "$arg" != -* ]]; then
            BASELINE_FILE="$arg"
        else
            echo "FATAL: --baseline requires a file argument" >&2
            exit 2
        fi
        FOUND_BASELINE=0
    elif [ "$arg" = "--baseline" ]; then
        FOUND_BASELINE=1
    else
        TEMP_ARGS+=("$arg")
    fi
done
set -- "${TEMP_ARGS[@]}"

while getopts "i:d:o:n:w:D:h" opt; do
  case "$opt" in
    i) IFACE=$OPTARG ;;
    d) DUR=$OPTARG ;;
    o) PCAP=$OPTARG ;;
    n) EXPECT_N=$OPTARG ;;
    w) WAV=$OPTARG ;;
    D) ADEV=$OPTARG ;;
    h) awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"; exit 0 ;;
    *) echo "bad option; -h for help" >&2; exit 2 ;;
  esac
done

die() { echo "FATAL: $*" >&2; exit 1; }
warn() { echo "WARN: $*" >&2; }
ok() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; }
inconc() { echo "INCONCLUSIVE: $*" >&2; exit 2; }

[ "$(id -u)" -eq 0 ] || die "must run as root (capture + module param)"
command -v tcpdump >/dev/null || die "tcpdump not found"
command -v python3 >/dev/null || die "python3 not found"
[ -e "$PARAM" ] || die "$PARAM missing -- is snd_igb_avb loaded (with the B1 tap build)?"
[ -f "$IR" ] || die "pcap_analyze.py not found at $IR"

# --- expected params from the driver -----------------------------------------
if [ -z "$EXPECT_N" ]; then
  EXPECT_N=$(sed -n 's/^num_streams=\([0-9]\+\).*/\1/p' "$PROC/tx_engine" 2>/dev/null || true)
fi
[ -n "$EXPECT_N" ] || die "could not determine num_streams; pass -n"

RATE=$(sed -n 's/.*samplerate=\([0-9]\+\).*/\1/p' "$PROC/streams" 2>/dev/null | head -1 || true)
[ -n "$RATE" ] || RATE=48000
SPP=$(( RATE / 8000 ))   # samples per 125us Class-A interval

echo "== TX-mirror verification =="
echo "   iface=$IFACE  duration=${DUR}s  expect num_streams=$EXPECT_N  rate=$RATE  spp=$SPP"
echo "   pcap=$PCAP"

# --- arm the tap --------------------------------------------------------------
PREV_MIRROR=$(cat "$PARAM")
cleanup() {
  echo 0 > "$PARAM" 2>/dev/null || true
  [ "${PREV_MIRROR:-0}" = "Y" -o "${PREV_MIRROR:-0}" = "1" ] && echo 1 > "$PARAM" 2>/dev/null || true
}
trap cleanup EXIT

ip link show "$IFACE" >/dev/null 2>&1 || die "interface $IFACE not present (tap failed to register?)"
ip link set "$IFACE" up || die "could not bring $IFACE up"
echo 1 > "$PARAM"
echo "   mirror armed; $IFACE up"

# --- capture ------------------------------------------------------------------
# avbtap0 only ever carries our mirrored frames, so capture unfiltered.
tcpdump -i "$IFACE" -w "$PCAP" -Q in >/dev/null 2>&1 &
TCPDUMP_PID=$!
sleep 0.5

if [ -n "$WAV" ]; then
  echo "   playing $WAV on $ADEV ..."
  aplay -D "$ADEV" "$WAV" >/dev/null 2>&1 || echo "   (aplay failed -- continuing capture)"
else
  echo "   >>> play audio now (${DUR}s window) <<<"
fi
sleep "$DUR"

kill "$TCPDUMP_PID" 2>/dev/null || true
wait "$TCPDUMP_PID" 2>/dev/null || true
echo "   captured $(du -h "$PCAP" | cut -f1) to $PCAP"
echo

# --- analyse ------------------------------------------------------------------
python3 "$IR" "$PCAP" "$EXPECT_N" "$SPP"

# --- baseline mode (optional) -----------------------------------------------
if [ -n "$BASELINE_FILE" ]; then
    echo "   saving baseline to $BASELINE_FILE..."
    python3 "$IR" "$PCAP" "$EXPECT_N" "$SPP" "$BASELINE_FILE"
fi