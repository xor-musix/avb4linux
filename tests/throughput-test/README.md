# AVB Throughput Test

How many AVB **listener** streams can a single i210 + this host actually sink with
zero drops? This tool answers that on the real hardware. It loads the i210 with
synthetic AVTP talker streams from a second NIC and sweeps the stream count up to
the ceiling.

Each stream models one real listener — **8 channels @ 48 kHz = 8000 packets/sec** —
so the headline result is *"this system can host N such 8-channel AVB listeners on
one i210"*.

> Background: earlier one-off RX-throughput spikes proved a single i210 sinks
> 16×8ch = 128k pps with zero drops; this is the permanent, repeatable tool.

## What you need

```
second box / 2nd NIC   --- direct ethernet cable ---   AVB host i210 ($I210, e.g. enp5s0)
   runs avtp-blaster                                      runs snd_igb_avb avb_rx_bench=1
```

- **Direct cable, no switch/MOTU** — isolates the i210's raw pps sink. A managed
  switch may filter/shape the 802.1Q-tagged AVTP (ethertype 0x22f0).
- No IP needed (raw L2). The blaster sends on a bound NIC; frames really egress it.
- The i210 must run `snd_igb_avb` loaded in **measurement mode** (next section).
- Two NICs in one box works (same-host); a separate sender box is cleaner. With the
  same-host setup the blaster competes for CPU with the RX path, so the script pins
  the i210 RX-q1 IRQ, the blaster NIC IRQ, and the blaster process to separate cores.

## 1. Load the driver in measurement mode

```bash
sudo rmmod snd_igb_avb 2>/dev/null
sudo modprobe snd_igb_avb avb_rx_bench=1        # (or set avb_rx_bench=1 in avb_up.sh)
dmesg | tail            # expect: "AVB RX benchmark mode -- capturing ALL AVTP on RX-q1"
```

`avb_rx_bench=1` (default 0) flips the driver into measurement mode:

- the RX-q1 flex filter is **broadened** to match every AVTP frame (any MAC), so a
  bare blaster needs no MSRP/MAC configuration to be received;
- the listener is force-enabled (no `avb_device_*_mac` params required);
- every packet the custom datapath drains from RX-q1 is **counted** at
  `/sys/kernel/debug/snd_igb_avb/rx_bench` (the datapath bypasses igb's NAPI
  accounting, so `ethtool` rx counters don't move — this node is the received-rate
  source of truth). `cat` it for the running total + per-stream histogram + the live
  drop accumulators; write any byte to reset.

In normal operation (`avb_rx_bench=0`) none of this is active — the production audio
path is unchanged. The measurement code lives entirely in `../kernel-module/.../src/avb-bench.c`;
`alsa.c` only carries a few `if (avb_rx_bench)` guards.

## 2. Run the sweep

```bash
sudo ./measure-listeners.sh --prep --sanity     # first time: clean the link, gate, sweep
sudo ./measure-listeners.sh                      # subsequent runs
```

Output is a per-step table and a headline:

```
  streams   sent(pps)    recv(pps)    Mbps   drops    CPU%   result
  16        128000       128000       272    0        12     ok
  32        256000       256000       544    0        24     ok
  48        384000       340000       723    0        ...    TX-LIMIT
== verdict
  Max AVB listeners sustained with ZERO drops on enp5s0 = 32 (= 32 x 8ch = 256 channels, ~544 Mbps on-wire)
  Wall at 48 listeners: blaster under-delivered ... bandwidth/TX wall (gigabit link), NOT an i210 drop limit.
```

It stops at the first step that shows **DROPS** (the i210's real RX limit) or
**TX-LIMIT** (the sender / gigabit link ran out of bandwidth — *not* an i210 result;
re-run from a faster or second sender to find the true ceiling).

### Knobs (env vars)

| var | default | meaning |
|-----|---------|---------|
| `I210` | `enp5s0` | i210 under test |
| `BLAST` | `eno1` | sender NIC (direct-cabled to `$I210`) |
| `STEPS` | `1 2 4 8 12 16 24 32 48 64` | stream counts to ramp |
| `PPS` | `8000` | per-stream pps (8000 = one 8-ch @ 48 kHz listener) |
| `DWELL` | `8` | seconds per sweep step |
| `IRQ_CPU`/`NIC_CPU`/`APP_CPU` | `2`/`4`/`6` | isolated cores for RX-q1 IRQ / sender NIC IRQ / blaster |
| `BVID` | `2` | blaster VLAN id (must be registered on the i210) |

To push past 64 listeners, raise `STEPS` **and** `MAX_STREAMS` in `avtp-blaster.c`.

### `--fixed` (single config spot check)

```bash
sudo STREAMS=16 SECS=60 ./measure-listeners.sh --fixed
```

Runs one `STREAMS × PPS` load for `SECS` and prints PASS/FAIL with full stats —
the old spike-2 behavior, for confirming a known-good point.

## `--prep` and `--sanity` (read these — they encode the gotchas)

A direct NIC↔NIC cable is correct but the link still needs cleaning, and the i210
silently drops frames in several non-obvious ways. `--prep` handles all of them:

- **Stale DHCP IPs** on the NICs make the kernel route/ARP — `--prep` flushes them
  (internet on those ports goes away until you replug; that's the proof you're
  point-to-point, not on a switch).
- **802.3x PAUSE flow control:** if the i210's RX backs up it sends PAUSE and the
  sender *throttles instead of dropping*, hiding the very thing we measure. `--prep`
  disables flow control on the **blaster NIC only** — never on the i210 (see below).
- **`rx-vlan-filter`** drops tagged frames whose VID isn't registered. The blaster
  sends VID `$BVID` (2); `--prep` registers it by adding a VLAN subif on the i210
  (programs the VFTA without resetting the NIC).

`--sanity` blasts a low rate for 5s, captures the on-wire bytes (must be
`81 00 60 02 22 f0` at offset 0x0c), and aborts unless RX-q1 actually receives —
so the real sweep only runs once the path is confirmed.

**Never run `ethtool -A/-s/-G/-K` on the i210 after loading the module.** Any NIC
reset **wipes the AVB flex filter** (it's installed once at probe), after which RX-q1
goes unserviced: you'll see `rx_missed_errors`/`rx_fifo_errors` climb by every packet
while RX-q1 IRQs stay 0. Fix = reload the module (`../../avb_up.sh`). If `rx-vlan-offload`
is on `[fixed]` and frames still aren't steered, `ethtool -K $I210 rxvlan off` **then
reload** the module (the `-K` itself resets and wipes the filter).

## Files

- `avtp-blaster.c` — raw-socket AVTP load source (frame byte-identical to the talker's
  `AVPDU_HEADER`); `--streams`/`--pps`/`--secs`/`--blast`. Build with `make`.
- `measure-listeners.sh` — the sweep/fixed harness (root; pins IRQs, prep/sanity, verdict).
- `../../Makefile` — builds the blaster.

## Roadmap: TX / Talker capacity (planned)

This measures the **listener (RX)** ceiling. The complementary question — how many
**talker (TX)** streams the i210 can source on time — is not yet implemented. The
intended shape, mirroring this tool:

- a `measure-talkers.sh` that drives the i210's TX path at increasing stream counts;
- a **receiver/counter on the second box** (a raw-socket AVTP sink, the mirror of
  `avtp-blaster.c`) that measures delivered pps, launch-time jitter, and gaps —
  the TX-side equivalent of the `rx_bench` debugfs counter;
- an `avb_tx_bench` module param symmetric to `avb_rx_bench` if the TX path likewise
  needs to be driven without a real DAW client.

Until then, use this tool for the listener-count answer.