# 32 Talkers (256-channel) Scale Validation Test

This directory contains the test harness for scaling AVB to 32 talker streams (256 channels).

## Overview

The test validates that the AVB implementation can sustain 32 simultaneous talker streams with 8 channels each (256 total channels) at 48kHz sample rate, meeting:
- **Zero XRUN**: No buffer underruns during 10+ minutes of playback
- **Tick budget**: De-interleave cost stays within 125µs (Class A interval)
- **Audio quality**: No crosstalk, frequency errors, or dropouts

## Test Structure

```
tests/scale-32-talkers/
├── test_32talkers.sh          # Main orchestration script
├── audio_check_256ch.c        # 256-channel audio generator/checker
├── audio_check_256ch          # Compiled binary (after make)
├── Makefile                   # Build the C tool
├── baseline_32talkers.txt     # Expected metrics thresholds
└── README.md                  # This file
```

## Usage

### Prerequisites

- Linux kernel with AVB support (snd_igb_avb)
- ALSA utilities (aplay, arecord)
- tcpdump (for PCAP analysis)
- Root access

### Running the Test

```bash
cd tests/scale-32-talkers
make
sudo ./test_32talkers.sh
```

**Note:** DEBUG_TICK_COST is enabled by default for tick budget validation. To disable:

```bash
export DEBUG_TICK_COST=0
sudo ./test_32talkers.sh
```

The test performs:
1. Build kernel module with DEBUG_TICK_COST enabled
2. Load module with `num_streams=32`
3. Bring up interface with `--num-streams 32 --tx-mirror`
4. Validate 256 channels via ALSA hw_params
5. Play 256ch stimulus for 10+ minutes
6. PCAP analysis (32 streams, 32 pkts/tick)
7. Check XRUN = 0
8. Check tick budget < 125 µs

### Manual Build of Audio Tool

```bash
cd tests/scale-32-talkers
make
# Generate stimulus: ./audio_check_256ch gen output.wav
# Check recording:   ./audio_check_256ch check recording.wav
```

## Exit Codes

| Code | Meaning |
|------|---------|
| 0    | PASS - All validations passed |
| 1    | FAIL - XRUN, tick budget missed, or validation failure |
| 2    | INCONCLUSIVE - Setup problem (missing interface, tool, etc.) |

## Audio Stimulus Design

The stimulus generator creates 256 channels with distinct sine waves:
- **Channel k**: frequency = 660 + 90×k Hz
- **Frequency range**: 660 Hz to 23,610 Hz (all below 24kHz Nyquist)
- **Level**: -6 dBFS per channel
- **Duration**: 30 seconds (configurable)
- **Spacing**: 90 Hz between adjacent channels (avoids harmonics)

The checker validates:
- Frequency accuracy (±3 Hz tolerance)
- No crosstalk (>30 dB rejection)
- Noise floor (<-40 dBFS)
- Glitch detection (predictor-based)

## Tick Budget Instrumentation

The kernel module includes rate-limited tick cost measurement:

```c
#ifdef DEBUG_TICK_COST
start_ns = ktime_to_ns(ktime_get());
// ... de-interleave operation ...
end_ns = ktime_to_ns(ktime_get());
pr_info_ratelimited("deinterleave cost: %llu.%03lll µs (budget 125.000 µs)\n",
                    (end_ns - start_ns) / 1000, ...);
#endif
```

To enable:
1. Ensure `DEBUG_TICK_COST` is defined in `alsa.c` (via patch)
2. Run the test - measurements appear in dmesg

## Baseline Metrics

See `baseline_32talkers.txt` for expected thresholds:
- Tick budget: 125 µs (max)
- XRUN count: 0 (max)
- Crosstalk: -30 dB (min)
- Noise floor: -40 dBFS (max)

## PCAP Analysis

When `--tx-mirror` is enabled, the test captures AVTP packets via a tap interface. Analyze with:

```bash
tshark -r output.pcap -Y "avtp.protocol_type == 1" -T fields -e frame.time -e avtp.stream_id
```

Expected: 32 distinct stream IDs, packets at ~125 µs intervals.

## Regression Testing

To verify N=1 still works (no regression):

```bash
# With num_streams=1
sudo ./avb_up.sh enp5s0 48000
# Should show 8 channels, same audio quality
```

## Troubleshooting

### "Interface not found"
- Check `ip link show` for available interfaces
- Ensure the interface is a supported Intel i210/i211

### "Module load failed"
- Check `dmesg` for kernel errors
- Verify own_mac is set correctly

### "Channel count mismatch"
- Verify `num_streams=32` is passed to module
- Check `/proc/asound/card*/pcm0p/sub0/hw_params`

### "Tick budget exceeded"
- This indicates the de-interleave path needs optimization
- Consider enabling SSE/AVX variant (if available)
- Reduce num_streams or sample rate as workaround

## History

- **2025-06-15**: Initial validation run on HP Z840 (scalar path passed)
- **2025-06-16**: Added PCAP analysis, glitch detection improvements
