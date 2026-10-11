# Network Layer Regression Tests

This directory contains network-layer validation tests for the AVB4Linux stack.

## Overview

The AVB4Linux stack has two test tiers:

1. **Audio tests** (`tests/audio/`): End-to-end roundtrip validation through ALSA
2. **Network tests** (`tests/network/`): Network layer frame validation

## Prerequisites

Before running network tests:

1. **Load the AVB stack with multi-stream support:**
   ```bash
   # For 2-stream test (default for test_multistream.sh)
   sudo avb_up.sh enp5s0 48000 --num-streams 2
   
   # For 4-stream test
   sudo avb_up.sh enp5s0 48000 --num-streams 4
   ```

2. **Verify the driver is loaded:**
   ```bash
   ls /proc/snd_igb_avb/
   ls /sys/module/snd_igb_avb/parameters/avb_tx_mirror
   ```

3. **Ensure TX mirror is enabled (if not built-in):**
   ```bash
   # Check if TX mirror is available
   cat /sys/module/snd_igb_avb/parameters/avb_tx_mirror
   ```

4. **Bring up the interface (if not done by avb_up.sh):**
   ```bash
   sudo ip link set enp5s0 up
   ```

## Test Suite

### `test_multistream.sh`

Validates multi-stream TX by examining TX frames captured via the in-driver TX mirror tap (`avbtap0`).

**Features:**
- Validates per-stream `stream_id = (mac << 16) | s` encoding
- Checks 8-channel slice payload size (`8 × spp × 4` bytes)
- Verifies N packets per PHC tick (one per stream)
- Confirms seq/dbc continuity per stream
- Reports SYT cadence per stream

**Usage:**
```bash
# Quick validation with 2 streams
sudo ./tests/network/test_multistream.sh -n 2 -d 5

# Full validation (establish baseline)
sudo ./tests/network/test_multistream.sh --establish-baseline /tmp/baseline.txt -n 8

# With specific ALSA device (audio is played separately)
sudo ./tests/network/test_multistream.sh -n 4 -w test.wav -D hw:igbavb
```

### `test_stress.sh`

Long-duration stress test (5-60 minutes) that validates:
- No XRUN under sustained load
- Consistent packet emission
- No MAC filter issues
- No bandwidth violations

**Usage:**
```bash
sudo ./tests/network/test_stress.sh -n 2 -t 300
```

### `test_mac_filter.sh`

Validates MAC address configuration:
- Talker dest MAC matches expected
- Listener source MAC matches expected
- Domain MAC registration

**Usage:**
```bash
sudo ./tests/network/test_mac_filter.sh -i enp5s0 --validate
```

### `test_network.sh`

Orchestrator that runs all network tests in sequence.

**Usage:**
```bash
sudo ./tests/network/test_network.sh -n 2 -d 10 -t 60
```

## Architecture

```
tests/network/
├── test_multistream.sh      # Main multistream network validation
├── test_stress.sh           # Long-duration stress test
├── test_mac_filter.sh       # MAC address filtering validation
├── test_network.sh          # Orchestrator script
├── tools/
│   ├── pcap_analyze.py      # Shared pcap analysis library
│   └── avtp_rx_ts_check.py  # Peer RX timestamp model vs the driver's RX alignment
└── README.md                # This file
```

### `tools/avtp_rx_ts_check.py` — peer timestamp model

The driver's RX alignment assumes the peer timestamps AVTP 61883-6 the IEC way. The timestamp marks the
event whose `dbc` is a multiple of SYT_INTERVAL, and only packets holding one have `tv=1` (3 in 4 at
6 frames per packet). This tool checks that on the peer's real streams and replays the driver's
integer offset math in wire order. Only streams the driver does not consume reach the netdev and
are visible to `tcpdump`.

```bash
sudo ./avb_up.sh enp5s0 --samplerate 48000 --num-streams 4      # MOTU sinks connected
sudo tcpdump -i enp5s0 -nn -c 3000 -w ~/tmp/motu-n4.pcap 'ether proto 0x22f0 or vlan'
./tests/network/tools/avtp_rx_ts_check.py ~/tmp/motu-n4.pcap    # --rate, --ref <sid hex>
./tests/network/tools/avtp_rx_ts_check.py --selftest            # synthetic captures, no hardware
```

It also prints each stream's src/dst MAC and stream id. Exit 0 = the driver's
model holds and every replayed offset is within ±96 frames, 1 = FAIL, 2 = INCONCLUSIVE.

## Design Principles

1. **Audio tests run as regular user** (in audio group) - no root required
2. **Network tests require root** (need `tcpdump` + module param access)
3. **Deterministic output** (exit 0=PASS, 1=FAIL, 2=INCONCLUSIVE)
4. **Self-documenting** (each check prints PASS/FAIL with details)
5. **Extensible** (Python-based analysis can add new checks easily)

## Integration with CI

The network tests are designed to run on:
- Local development machines (pre-push validation)
- CI runners with AVB hardware
- Docker containers with network namespaces

## See Also

- `tests/audio/test_loopback.sh` for audio E2E validation