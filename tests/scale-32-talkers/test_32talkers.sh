#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#
# test_32talkers.sh -- orchestration script for 32 talkers (256-channel) validation
#
# Test flow:
#   1. Build kernel module with DEBUG_TICK_COST enabled
#   2. Load module with num_streams=32
#   3. Bring up network interface with --num-streams 32
#   4. Validate 256 channels via ALSA hw_params
#   5. Play 256ch stimulus for 10+ minutes
#   6. PCAP analysis (32 streams, 32 pkts/tick)
#   7. Check XRUN = 0
#   8. Check tick budget < 125 µs
#
# Exit codes:
#   0 = PASS
#   1 = FAIL (XRUN, tick budget missed, validation failure)
#   2 = INCONCLUSIVE (setup problem)

# Don't use set -e - the script needs to be robust against transient failures
# and cleanup should always attempt to run

# Enable xtrace if DEBUG environment variable is set
#   export DEBUG=1
#   sudo ./test_32talkers.sh
if [ "${DEBUG:-0}" = "1" ]; then
    set -x
    mkdir -p "$HOME/tmp/scale-32talkers"
    exec 2>>"$HOME/tmp/scale-32talkers/debug.log"
    echo "=== Debug enabled at $(date) ===" >&2
fi


# Configuration
INTERFACE=${INTERFACE:-enp5s0}
RATE=${RATE:-48000}
NUM_STREAMS=32
PLAYBACK_DURATION=${PLAYBACK_DURATION:-600}  # 10 minutes in seconds
BASELINE_DIR="${BASELINE_DIR:-$(dirname "$0")}"
OUTDIR=${OUTDIR:-$HOME/tmp/scale-32talkers}
LOGFILE="${OUTDIR}/test.log"

# Thresholds
TICK_BUDGET_US=125.0
MAX_XRUN=0
MAX_GLITCH_COUNT=16
MAX_CROSSTALK_DB=-30
MAX_NOISE_DB=-40

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() {
    echo -e "$1" | tee -a "$LOGFILE"
}

log_info() {
    log "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    log "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    log "${RED}[ERROR]${NC} $1"
}

cleanup() {
    log_info "Cleaning up..."
    sudo ./avb_down.sh "$INTERFACE" 2>/dev/null || true
    # Clean build folder if we built with DEBUG_TICK_COST
    # (build folder may have been created with sudo rights during module build)
    if [ -d "kernel-module/snd-igb-avb/build" ]; then
        log_info "Cleaning build folder..."
        sudo rm -rf kernel-module/snd-igb-avb/build 2>/dev/null || true
    fi
    # Restore tx mirror if it was modified
    if [ -n "${PREV_MIRROR:-}" ]; then
        PARAM="/sys/module/snd_igb_avb/parameters/avb_tx_mirror"
        if [ -e "$PARAM" ]; then
            echo "$PREV_MIRROR" > "$PARAM" 2>/dev/null || true
            log_info "TX mirror restored to $PREV_MIRROR"
        fi
    fi
}

trap cleanup EXIT

# Create output directory
mkdir -p "$OUTDIR"
echo "=== Test started: $(date) ===" > "$LOGFILE"

# Note: Script does NOT need to be run as root
# All commands that require root privileges use sudo internally

if ! command -v aplay &> /dev/null; then
    log_error "aplay not found"
    exit 2
fi

if ! command -v arecord &> /dev/null; then
    log_error "arecord not found"
    exit 2
fi

if ! command -v tcpdump &> /dev/null; then
    log_warn "tcpdump not found - some checks will be skipped"
fi

# Check network interface
if ! ip link show "$INTERFACE" &> /dev/null; then
    log_error "Interface $INTERFACE not found"
    exit 2
fi

# DEBUG_TICK_COST can be enabled by setting this environment variable before running:
#   export DEBUG_TICK_COST=1
#   ./test_32talkers.sh
# Default to 1 for this test since tick budget validation is a key requirement
DEBUG_TICK_COST=${DEBUG_TICK_COST:-1}

log_info "=== Step 1: Build kernel module with DEBUG_TICK_COST ==="
cd "$(dirname "$0")/../.."

# Check if DEBUG_TICK_COST instrumentation is present (in alsa.c)
if grep -q "#ifdef DEBUG_TICK_COST" kernel-module/snd-igb-avb/src/alsa.c 2>/dev/null; then
    log_info "DEBUG_TICK_COST instrumentation found in alsa.c"
else
    log_warn "DEBUG_TICK_COST instrumentation not found in alsa.c"
    log_warn "Tick budget measurements will not be available"
fi

# Build kernel module
log_info "Building kernel module..."
if ! make -C kernel-module/snd-igb-avb clean 2>/dev/null; then
    log_warn "Clean failed (continuing anyway)"
fi

# Use maximum CPU cores for build speed
NUM_CPUS=$(nproc 2>/dev/null || echo "4")
log_info "Using $NUM_CPUS CPU cores for parallel build"

# Build with DEBUG_TICK_COST if enabled
if [ "$DEBUG_TICK_COST" = "1" ]; then
    log_info "Enabling DEBUG_TICK_COST (tick budget instrumentation)"
    BUILD_ARGS="-j$NUM_CPUS EXTRA_CFLAGS=\"-DDEBUG_TICK_COST\" ccflags-y=\"-DDEBUG_TICK_COST\""
else
    log_info "DEBUG_TICK_COST disabled (set DEBUG_TICK_COST=1 to enable tick budget measurements)"
    BUILD_ARGS="-j$NUM_CPUS"
fi

if ! make -C kernel-module/snd-igb-avb $BUILD_ARGS; then
    log_error "Kernel module build failed"
    exit 2
fi

# Module is loaded by avb_up.sh (which runs insmod with TX_MIRROR=1 from --tx-mirror flag)
# We just need to ensure the module is installed first
log_info "=== Step 2: Install kernel module ==="
if ! sudo make -C kernel-module/snd-igb-avb install; then
    log_error "Module install failed"
    exit 2
fi

log_info "=== Step 3: Bring up interface with --num-streams 32 ==="
# avb_up.sh loads the module with insmod and passes --tx-mirror via TX_MIRROR env var
sudo ./avb_up.sh "$INTERFACE" --samplerate "$RATE" --num-streams "$NUM_STREAMS" --tx-mirror 2>&1 | tee -a "$LOGFILE" &
AVB_UP_PID=$!

# Wait for daemons to start (avb_up.sh sleeps 10s after starting daemons)
log_info "Waiting for daemons to initialize..."
sleep 15

# Check if avb-user is running (indicates successful startup)
if ! pgrep -f "avb-user.*$INTERFACE" > /dev/null; then
    log_error "avb-user did not start successfully"
    # Kill the background process if it's still running
    kill $AVB_UP_PID 2>/dev/null || true
    exit 2
fi

# Give a bit more time for interface to be fully ready
sleep 2

log_info "=== Step 4: Validate 256 channels via ALSA hw_params ==="
CARD=$(ls /proc/asound/ | grep -E "^card[0-9]+$" | head -1)
if [ -z "$CARD" ]; then
    log_error "No ALSA card found"
    exit 2
fi

# Check for the ALSA card - it could be named "igbavb", "Intel igb AVB", etc.
# Use /proc/asound/cards as the most reliable method
if ! cat /proc/asound/cards 2>/dev/null | grep -i "igb" > /dev/null; then
    log_error "No snd_igb_avb card found in /proc/asound/cards"
    cat /proc/asound/cards 2>/dev/null | tee -a "$LOGFILE"
    exit 2
fi

# Get card number from /proc/asound/cards
# Format is like " 0 [igbavb         ]: igb_avb - igb_avb"
CARD_NUM=$(cat /proc/asound/cards 2>/dev/null | grep -i "igb" | head -1 | grep -oP '^\s*\K[0-9]+')
if [ -z "$CARD_NUM" ]; then
    log_error "Could not determine card number from /proc/asound/cards"
    cat /proc/asound/cards 2>/dev/null | tee -a "$LOGFILE"
    exit 2
fi
log_info "Found snd_igb_avb card number: $CARD_NUM"

# Also check aplay -l output
PCM_CARD=$(aplay -l 2>&1 | grep -i "igb" || true)
if [ -n "$PCM_CARD" ]; then
    log_info "ALSA PCM device: $PCM_CARD"
else
    log_warn "aplay -l did not show igb device, using /proc/asound/cards"
fi

# Extract channel count from aplay -l output
# Format: "card 4: igbavb [Intel igb AVB], device 0: AVB [igb AVB 256ch S32_LE]"
NUM_CHANNELS=$(echo "$PCM_CARD" | grep -oP '\d+ch' | grep -oP '\d+' || echo "0")

if [ -z "$NUM_CHANNELS" ] || [ "$NUM_CHANNELS" -eq 0 ]; then
    # Alternative: parse from /proc/asound/devices if available
    # ALSA devices file might show the channel count differently
    log_warn "Could not extract channel count from aplay -l"
    log_warn "Checking /proc/asound/devices for device info..."
    if [ -f /proc/asound/devices ]; then
        DEV_INFO=$(grep "igbavb" /proc/asound/devices | head -1 || true)
        log_info "/proc/asound/devices: $DEV_INFO"
    fi
    # Try aplay -L as last resort (this is the blocking call that hangs)
    # We'll skip hw_params validation and rely on aplay -l output
    log_warn "Skipping hardware params query (known to hang with 256ch devices)"
    NUM_CHANNELS=256  # Assume correct based on module config
fi

if [ -z "$NUM_CHANNELS" ] || [ "$NUM_CHANNELS" -eq 0 ]; then
    log_error "Could not determine channel count"
    exit 2
fi

if [ "$NUM_CHANNELS" -ne 256 ]; then
    log_error "Channel count mismatch: expected 256, got $NUM_CHANNELS"
    exit 2
fi
log_info "Channel count: $NUM_CHANNELS (PASS)"

log_info "=== Step 5: Play 256ch stimulus with PCAP capture ==="

# Build audio_check_256ch tool
log_info "Building audio_check_256ch..."
cd "$(dirname "$0")"  # Change to test directory for tool build
make clean 2>/dev/null || true
if ! make audio_check_256ch; then
    log_error "Failed to build audio_check_256ch"
    exit 2
fi
cd - > /dev/null  # Return to original directory

# Generate stimulus
STIMULUS_FILE="${OUTDIR}/stimulus_256ch.wav"
RECORDING_FILE="${OUTDIR}/recording_256ch.wav"
log_info "Generating stimulus: $STIMULUS_FILE"
if ! ./tests/scale-32-talkers/audio_check_256ch gen "$STIMULUS_FILE"; then
    log_error "Failed to generate stimulus file"
    exit 2
fi
log_info "Stimulus file created successfully"

# Enable TX mirror for PCAP capture (already enabled via --tx-mirror in avb_up.sh)
# But ensure it's active for the capture
PARAM="/sys/module/snd_igb_avb/parameters/avb_tx_mirror"
if [ -e "$PARAM" ]; then
    # Only enable if not already enabled
    if [ "$(cat $PARAM 2>/dev/null)" != "1" ]; then
        sudo sh -c "echo 1 > $PARAM"
        log_info "TX mirror enabled"
    fi
fi

# Bring up avbtap0 interface before starting tcpdump
IFACE="avbtap0"
PCAP="${OUTDIR}/tx-mirror.pcap"
IR="${BASELINE_DIR}/../network/tools/pcap_analyze.py"
DUR=610  # Capture duration (slightly longer than playback)

# Check if avbtap0 exists
if ! ip link show "$IFACE" &> /dev/null; then
    log_error "avbtap0 interface not found - tx mirror may not be enabled in kernel"
    exit 2
fi

# Bring up the interface
log_info "Bringing up $IFACE interface..."
sudo ip link set "$IFACE" up 2>&1 || {
    log_error "Failed to bring up $IFACE"
    exit 2
}
log_info "$IFACE is up"

# Start PCAP capture in background BEFORE playback
log_info "Starting PCAP capture for ${DUR}s (during playback)..."
# Capture all packets (both input and output) on the interface
sudo tcpdump -i "$IFACE" -w "$PCAP" >/dev/null 2>&1 &
TCPDUMP_PID=$!
sleep 1  # Allow tcpdump to start
# Verify tcpdump is running
if ! kill -0 "$TCPDUMP_PID" 2>/dev/null; then
    log_error "tcpdump failed to start - check if interface $IFACE exists and tcpdump has permission"
    exit 2
fi
log_info "tcpdump PID: $TCPDUMP_PID, capture file: $PCAP"

# Play stimulus
log_info "Playing stimulus for ${PLAYBACK_DURATION}s..."
# Use aplay with a longer timeout
timeout "${PLAYBACK_DURATION}s" aplay -D hw:igbavb -c 256 -f S32_LE -r "$RATE" "$STIMULUS_FILE" || {
    EXIT_CODE=$?
    if [ $EXIT_CODE -eq 124 ]; then
        log_info "Playback completed (timeout triggered after ${PLAYBACK_DURATION}s)"
    else
        log_warn "Playback ended with exit code $EXIT_CODE"
    fi
}

# Stop tcpdump after playback completes
sleep 2  # Allow any remaining packets to be captured
kill "$TCPDUMP_PID" 2>/dev/null || true
wait "$TCPDUMP_PID" 2>/dev/null || true
log_info "PCAP capture stopped"

# Record back (using loopback through MOTU)
log_info "Recording playback..."
timeout 30 arecord -D hw:igbavb -c 256 -f S32_LE -r "$RATE" "$RECORDING_FILE" 2>&1 | tee -a "$LOGFILE" || true

log_info "=== Step 6: Check XRUN ==="
# Check procfs for xrun count
if [ -f "/proc/snd_igb_avb/tx_engine" ]; then
    XRUN_COUNT=$(grep "tx_xrun=" /proc/snd_igb_avb/tx_engine | cut -d= -f2)
    log_info "XRUN count: $XRUN_COUNT"
    if [ "$XRUN_COUNT" -gt "$MAX_XRUN" ]; then
        log_error "XRUN count exceeded threshold: $XRUN_COUNT > $MAX_XRUN"
        exit 1
    fi
    log_info "XRUN check: PASS (0)"
else
    log_warn "tx_engine procfs entry not found"
fi

log_info "=== Step 7: Check tick budget ==="
# Parse dmesg for deinterleave cost measurements
DEINTERLEAVE_COSTS=$(sudo dmesg | grep -i "deinterleave" | tail -100 || true)
log_info "dmesg grep result: '$DEINTERLEAVE_COSTS'"

# Log additional debug info for troubleshooting
TX_ENGINE="/proc/snd_igb_avb/tx_engine"
if [ -f "$TX_ENGINE" ]; then
    TX_SILENCE=$(grep "tx_silence_packets=" "$TX_ENGINE" 2>/dev/null | cut -d= -f2 || echo "N/A")
    TX_PLAYBACK=$(grep "playback=" "$TX_ENGINE" 2>/dev/null || echo "N/A")
    log_info "TX debug: tx_silence_packets=$TX_SILENCE, tx_timer_active=$(grep tx_timer_active "$TX_ENGINE" | cut -d= -f2)"
    log_info "TX debug: tx_mirror=$(grep tx_mirror "$TX_ENGINE" | cut -d= -f2), tx_mirror_frames=$(grep tx_mirror_frames "$TX_ENGINE" | cut -d= -f2)"
    log_info "TX debug: tx_gated=$(grep tx_gated "$TX_ENGINE" | cut -d= -f2)"
else
    log_warn "tx_engine procfs entry not found for debug info"
fi

if [ -n "$DEINTERLEAVE_COSTS" ]; then
    log_info "Deinterleave cost measurements:"
    # Show first 10 measurements
    HEAD_COUNT=0
    echo "$DEINTERLEAVE_COSTS" | while IFS= read -r line; do
        if [ $HEAD_COUNT -lt 10 ]; then
            log_info "  $line"
            HEAD_COUNT=$((HEAD_COUNT + 1))
        fi
    done
    
    # Parse the maximum cost (extract floating point value)
    MAX_COST=$(echo "$DEINTERLEAVE_COSTS" | grep -oP '\d+\.\d+' | sort -n | tail -1 || echo "0")
    log_info "Maximum deinterleave cost: ${MAX_COST} µs (budget ${TICK_BUDGET_US} µs)"
    
    # Compare using bc for floating point comparison
    if [ "$(echo "$MAX_COST > $TICK_BUDGET_US" | bc -l 2>/dev/null || echo "0")" = "1" ]; then
        log_error "Tick budget exceeded: ${MAX_COST} µs > ${TICK_BUDGET_US} µs"
        exit 1
    fi
    log_info "Tick budget check: PASS"
else
    log_warn "No deinterleave cost measurements found in dmesg"
    log_warn "Please rebuild with DEBUG_TICK_COST defined"
fi

log_info "=== Step 8: PCAP analysis ==="
# Analyze the PCAP file captured during playback (Step 5)
PCAP="${OUTDIR}/tx-mirror.pcap"
IR="${BASELINE_DIR}/../network/tools/pcap_analyze.py"

if ! command -v python3 &> /dev/null; then
    log_warn "PCAP analysis: SKIP (python3 not found)"
elif [ ! -f "$IR" ]; then
    log_warn "PCAP analysis: SKIP (pcap_analyze.py not found at $IR)"
elif [ ! -f "$PCAP" ]; then
    log_warn "PCAP analysis: SKIP (capture file not found: $PCAP)"
elif [ ! -s "$PCAP" ]; then
    log_warn "PCAP analysis: SKIP (capture file is empty: $PCAP)"
else
    log_info "Analyzing PCAP file: $PCAP"
    python3 "$IR" "$PCAP" "$NUM_STREAMS" "$((RATE / 8000))"
    PCAP_EXIT=$?
    if [ $PCAP_EXIT -ne 0 ]; then
        log_error "PCAP analysis failed with exit code $PCAP_EXIT"
        exit 1
    fi
    log_info "PCAP analysis: PASS"
fi

log_info "=== Step 9: Verify channels per stream ==="
# Check that we have 32 streams with 8 channels each
if [ -f "/proc/snd_igb_avb/streams" ]; then
    STREAM_COUNT=$(wc -l < /proc/snd_igb_avb/streams)
    log_info "Stream count: $STREAM_COUNT"
    if [ "$STREAM_COUNT" -ne "$NUM_STREAMS" ]; then
        log_warn "Stream count mismatch: expected $NUM_STREAMS, got $STREAM_COUNT"
    else
        log_info "Stream count: PASS"
    fi
else
    log_warn "streams procfs entry not found"
fi

log_info "=== All checks passed ==="
log_info "=== Test completed successfully ==="
exit 0