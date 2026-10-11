#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#
# test_stress.sh -- long-duration stress test for AVB4Linux network layer
#
# Validates sustained multi-stream operation for 5-60 minutes checking:
#   - No XRUN under sustained load
#   - Consistent packet emission
#   - No MAC filter issues
#   - No bandwidth violations
#
# Usage:
#   sudo ./tests/network/test_stress.sh [-n streams] [-t seconds] [-i iface]
#
#   -n  num_streams           (default: 2)
#   -t  test duration (sec)   (default: 300 = 5 min)
#   -i  capture interface     (default: avbtap0)
#   --baseline <file>         save baseline metrics to file
#
# Requires: root, tcpdump, python3, snd_igb_avb loaded with TX mirror.
# Audio must be playing during the capture period for meaningful results.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
HERE="$ROOT/tests/network"
IR="$HERE/tools/pcap_analyze.py"
PROC=/proc/snd_igb_avb

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

NUM_STREAMS=2
DUR=300
IFACE=avbtap0
PARAM=/sys/module/snd_igb_avb/parameters/avb_tx_mirror
OUTDIR=$AVB_STRESS_DIR

# First pass: extract --baseline file if present
BASELINE_FILE=""
TEMP_ARGS=()
FOUND_BASELINE=0
for arg in "$@"; do
    if [ "$FOUND_BASELINE" -eq 1 ]; then
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

while getopts "n:t:i:h" opt; do
  case "$opt" in
    n) NUM_STREAMS=$OPTARG ;;
    t) DUR=$OPTARG ;;
    i) IFACE=$OPTARG ;;
    h) echo "Usage: $0 [-n streams] [-t seconds] [-i iface]";
       echo "       $0 [--baseline file]";
       echo "";
       echo "  -n  num_streams           (default: 2)";
       echo "  -t  test duration (sec)   (default: 300 = 5 min)";
       echo "  -i  capture interface     (default: avbtap0)";
       echo "";
       echo "Audio must be playing during the capture period.";
       exit 0 ;;
    *) echo "bad option; -h for help" >&2; exit 2 ;;
  esac
done

die() { echo "FATAL: $*" >&2; exit 1; }
warn() { echo "WARN: $*" >&2; }
ok() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; }
inconc() { echo "INCONCLUSIVE: $*" >&2; exit 2; }

[ "$(id -u)" -eq 0 ] || die "must run as root"
command -v tcpdump >/dev/null || die "tcpdump not found"
command -v python3 >/dev/null || die "python3 not found"
[ -e "$PARAM" ] || die "$PARAM missing -- is snd_igb_avb loaded?"
[ -f "$IR" ] || die "pcap_analyze.py not found at $IR"

mkdir -p "$OUTDIR"

# Validate num_streams parameter
if ! echo "$NUM_STREAMS" | grep -qE '^[0-9]+$' || [ "$NUM_STREAMS" -lt 1 ] || [ "$NUM_STREAMS" -gt 16 ]; then
    die "num_streams must be between 1 and 16"
fi

echo "== AVB4Linux Stress Test =="
echo "   streams=$NUM_STREAMS  duration=${DUR}s  iface=$IFACE"
echo "   output=$OUTDIR"

# Check driver state
if [ "$NUM_STREAMS" -ne "$(sed -n 's/^num_streams=\([0-9]\+\).*/\1/p' "$PROC/tx_engine" 2>/dev/null || echo 0)" ]; then
    die "num_streams mismatch: driver has $(sed -n 's/^num_streams=\([0-9]\+\).*/\1/p' "$PROC/tx_engine" 2>/dev/null || echo 'unknown'), expected $NUM_STREAMS"
fi

RATE=$(sed -n 's/.*samplerate=\([0-9]\+\).*/\1/p' "$PROC/streams" 2>/dev/null | head -1 || true)
[ -n "$RATE" ] || RATE=48000
SPP=$(( RATE / 8000 ))

echo "   rate=$RATE  spp=$SPP  expected bandwidth=$(( NUM_STREAMS * 8 * SPP * 4 * 8000 / 1000000 )) Mbps"

# Arm TX mirror
PREV_MIRROR=$(cat "$PARAM")
cleanup() {
    echo 0 > "$PARAM" 2>/dev/null || true
    [ "${PREV_MIRROR:-0}" = "Y" -o "${PREV_MIRROR:-0}" = "1" ] && echo 1 > "$PARAM" 2>/dev/null || true
}
trap cleanup EXIT

ip link set "$IFACE" up 2>/dev/null || die "interface $IFACE not present"

echo "   arm TX mirror"
echo 1 > "$PARAM"

# Capture packets
PCAP="$OUTDIR/stress_test.pcap"
echo "   capture for ${DUR}s..."
tcpdump -i "$IFACE" -w "$PCAP" -Q in >/dev/null 2>&1 &
TCPDUMP_PID=$!
sleep 1

# Start playback (use loopback if available)
if aplay -l 2>/dev/null | grep -qi 'igbavb'; then
    # Build tool if needed (same as loopback test)
    AUDIO_HERE="$ROOT/tests/audio"
    TOOL="$AUDIO_HERE/audio_check"
    if [ ! -x "$TOOL" ] || [ "$AUDIO_HERE/audio_check.c" -nt "$TOOL" ]; then
        echo "   building audio_check..."
        make -C "$AUDIO_HERE" >/dev/null 2>&1 || die "audio_check build failed"
    fi
    
    # Generate stimulus using audio_check tool (same as loopback test)
    WAV="$OUTDIR/stress_test.wav"
    GEN_SECS=$((DUR + 6))
    CHANS=$((NUM_STREAMS * 8))
    echo "   generating ${GEN_SECS}s stimulus (${CHANS}ch, $RATE Hz)..."
    "$TOOL" gen "$WAV" --ch "$CHANS" --rate "$RATE" --secs "$GEN_SECS" || die "stimulus generation failed"
    
    aplay -q -D hw:igbavb "$WAV" &
    APLAY_PID=$!
else
    echo "   >>> play audio now (${DUR}s window) <<<"
fi

# Monitor procfs stats during test
STATS_FILE="$OUTDIR/stress_stats.txt"
START_TIME=$(date +%s)

while [ $(( $(date +%s) - START_TIME )) -lt "$DUR" ]; do
    REMAINING=$(( DUR - $(date +%s) + START_TIME ))
    if [ $REMAINING -le 0 ]; then
        break
    fi
    
    # Sample procfs
    cat "$PROC/tx_engine" >> "$STATS_FILE" 2>/dev/null || true
    cat "$PROC/streams" >> "$STATS_FILE" 2>/dev/null || true
    
    # Show progress every 30 seconds
    if [ $(( $(date +%s) % 30 )) -eq 0 ]; then
        TX_XRUN=$(grep -o 'tx_xrun=[0-9]*' "$PROC/tx_engine" 2>/dev/null | cut -d= -f2 || echo 0)
        LATE=$(grep -o 'late_launch=[0-9]*' "$PROC/tx_engine" 2>/dev/null | cut -d= -f2 || echo 0)
        echo "   [${REMAINING}s remaining] tx_xrun=$TX_XRUN late_launch=$LATE"
    fi
    
    sleep 1
done

# Stop capture
kill "$TCPDUMP_PID" 2>/dev/null || true
wait "$TCPDUMP_PID" 2>/dev/null || true
kill "$APLAY_PID" 2>/dev/null || true
wait "$APLAY_PID" 2>/dev/null || true 2>/dev/null || true

# Cleanup
echo 0 > "$PARAM" 2>/dev/null || true

echo "   captured $(du -h "$PCAP" | cut -f1)"

# Analyze pcap
echo "   analyzing pcap..."
python3 "$IR" "$PCAP" "$NUM_STREAMS" "$SPP" > "$OUTDIR/pcap_analysis.txt" 2>&1 || true
cat "$OUTDIR/pcap_analysis.txt"

# Save baseline if requested
if [ -n "$BASELINE_FILE" ]; then
    echo "   saving baseline to $BASELINE_FILE..."
    python3 "$IR" "$PCAP" "$NUM_STREAMS" "$SPP" "$BASELINE_FILE"
fi

# Check for XRUNs
TX_XRUN=$(grep -o 'tx_xrun=[0-9]*' "$PROC/tx_engine" 2>/dev/null | cut -d= -f2 || echo 0)
LATE=$(grep -o 'late_launch=[0-9]*' "$PROC/tx_engine" 2>/dev/null | cut -d= -f2 || echo 0)

echo ""
echo "== Stress Test Results =="
echo "   duration: ${DUR}s"
echo "   tx_xrun: $TX_XRUN"
echo "   late_launch: $LATE"

FAILS=0

if [ "$TX_XRUN" != "0" ] 2>/dev/null; then
    fail "XRUN count: $TX_XRUN"
    FAILS=$((FAILS + 1))
else
    ok "No XRUNs"
fi

if [ "$LATE" != "0" ] 2>/dev/null; then
    warn "Late launch packets: $LATE (may indicate timing issues)"
else
    ok "No late launch packets"
fi

# Check if pcap analysis passed
if grep -q "^PASS:" "$OUTDIR/pcap_analysis.txt"; then
    ok "PCAP analysis: PASS"
else
    if grep -q "^FAIL:" "$OUTDIR/pcap_analysis.txt"; then
        fail "PCAP analysis: FAIL"
        FAILS=$((FAILS + 1))
    fi
fi

echo ""
if [ "$FAILS" -eq 0 ]; then
    ok "Stress test passed (${DUR}s, $NUM_STREAMS streams)"
    exit 0
else
    fail "Stress test failed ($FAILS failures)"
    exit 1
fi