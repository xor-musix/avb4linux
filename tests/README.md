# AVB audio regression tests

Bench, hardware-in-the-loop checks that a code change did not break the i210 AVB
**audio data plane**. The headline tool is the loopback regression gate.

## Why play-and-record (and not packet capture)

The talker's AVTP frames are injected straight into the i210 DMA ring and **bypass
the Linux netdev** — `tcpdump`/`tshark` see nothing of the audio stream. The only
way to observe the data plane is to play a known signal out the talker and record
what comes back through the listener. That round trip exercises TX framing → media
clock → wire → RX demux → de-interleave → PCM in one shot, so a regression in any
of those stages surfaces as a failed check.

## What it does

`tests/audio/test_loopback.sh` plays a generated **multitone** — one distinct continuous
sine per channel (channel *k* → `661 + 181·k` Hz) — out `hw:igbavb`, records it back
through the loopback, and the C oracle (`audio_check`) proves the recording is
faithful:

| check | catches |
|-------|---------|
| frequency per channel | wrong sample rate, media-clock drift/slip |
| channel mapping (k carries *fₖ*, argmax) | channel swaps / reordering in de-interleave |
| crosstalk (foreign tones down ≥30 dB) | channel bleed, demux leakage |
| amplitude band (vs baseline) | S32 format/scaling errors, gain/attenuation, clipping |
| noise floor + DC | sustained corruption, stuck samples, DC bias |
| glitch count (small budget, **always reported**) | dropouts, packet loss, clock slips |
| silence on channels `>= LIVE_CH` (`FAIL LEAK`) | audio landing in a slice whose stream cannot arrive |

> One distinct frequency per channel (instead of `speaker-test`'s one-channel-at-a-
> time sweep) means every channel is always active, so the oracle needs no timing
> sync with the recorder — a single Goertzel pass per channel proves frequency,
> mapping, and crosstalk at once. `speaker-test -D hw:igbavb -c 8 -r 48000 -F S32_LE
> -t sine` is still fine for a quick manual listen; it just isn't the scored path.

## Precondition — the loopback (your responsibility)

Using an AVDECC controller (e.g. the MOTU's own / Hive), route **every i210 talker
channel 1:1, unity gain, back to the corresponding i210 listener channel** through
the MOTU (AVB-in → AVB-out), and make sure **gPTP is locked**. The realisation here
is the MOTU reflecting the i210's talker stream back to its listener — a single NIC
port can't receive its own egress, so the MOTU is the reflector.

The script can't create this; it runs a **liveness probe** first and stops with
`INCONCLUSIVE` (not `FAIL`) if no audio comes back, so a broken setup never looks
like a code regression.

## Run it

```bash
# 1. bring up the AVB stack in NORMAL mode (NOT avb_rx_bench=1) -- this needs root
sudo ../avb_up.sh enp5s0 48000

# 2. ONE TIME per channel count, on known-good code: record the amplitude band
./tests/audio/test_loopback.sh --establish-baseline      # writes tests/audio/baseline.txt
CH=32 ./tests/audio/test_loopback.sh --establish-baseline  # writes tests/audio/baseline-32ch.txt

# 3. every time you want to gate a change:
./tests/audio/test_loopback.sh
```

## RX multi-stream gate

`tests/test_rx_streams.sh` is the counter-level gate for the listener path. It runs a
background `arecord -D $DEV -c $CH -r $RATE -f S32_LE /dev/null` (the RX datapath only counts
while capture is open), samples `/proc/snd_igb_avb/{streams,rx_engine}` once capture is flowing
and again `DURATION` s later, reads the kernel journal for the window, and judges named checks:

| check | PASS when |
|-------|-----------|
| `pkts` | every stream in `EXPECT_LIVE` gains `rx_pkts` at `PKT_RATE` (8000/s, class A) ±5%; every other stream gains 0 |
| `drops` | `rx_drops` delta = 0 on all streams |
| `clamp` | no new `align clamp` kernel lines |
| `fps` | `capture_frames_total` delta / elapsed = `RATE` ±0.5% — the shared capture pointer tracks the sample rate whatever N |
| `xrun` | 0 `overrun!!!` from the background `arecord`, which is SIGSTOPped for `STALL_MS` mid-window (a `/dev/null` reader never stalls by itself, so an undersized capture buffer could not show otherwise) |
| `engine` | `rx_seq_errors`, `rx_clock_slips`, `rx_budget_hits`, `rx_ring_near_lap` deltas = 0; no new `descriptor backlog` |
| `filters` | every stream with `filter>=0` has non-zero `src_mac` and `dst_mac` (an enabled filter that matches nothing is a FAIL) |

It prints a per-stream delta table, one row per check (`ok` / `FAIL <reason>`), then a single
summary line:

```
== RX: pkts=ok drops=FAIL clamp=FAIL fps=ok xrun=FAIL engine=ok filters=FAIL
```

```bash
sudo ./avb_up.sh enp5s0 --samplerate 48000 --num-streams 1
./tests/test_rx_streams.sh                                   # N=1
sudo ./avb_up.sh enp5s0 --samplerate 48000 --num-streams 4
CH=32 EXPECT_LIVE=0,1,2,3 ./tests/test_rx_streams.sh         # N=4, all four streams
```

| var | default | meaning |
|-----|---------|---------|
| `CH` | `8` | capture channels (8 per listener stream) |
| `RATE` | `48000` | sample rate |
| `DURATION` | `10` | seconds between the two `/proc` samples (`arecord` runs `DURATION+2`) |
| `EXPECT_LIVE` | `0` | comma list of stream indices that must receive packets |
| `DEV` | `hw:igbavb` | ALSA capture device |
| `CHECKS` | all | subset to run, e.g. `CHECKS=pkts,drops` |
| `PKT_RATE` | `8000` | expected packets/s per live stream |
| `STALL_MS` | `10` | consumer stall injected mid-window; `0` = none. Usable headroom is ~1/3 of the buffer: measured 3–5 ms at 32 ch/512 frames (today), ~32 ms at 2048 frames |
| `PROCDIR` | `/proc/snd_igb_avb` | procfs directory |

Exit codes: **0** PASS, **1** FAIL, **2** INCONCLUSIVE — `/proc/snd_igb_avb` missing (or a module
without the `capture_frames_total` / `align=` exports), `arecord` cannot open the device (busy, `CH`
≠ `8 × num_streams`, permission), the kernel journal is unreadable while `clamp`/`engine` are
selected, or no `EXPECT_LIVE` stream receives anything (loopback/ACMP not connected — setup, not
code). Run as the normal user; it needs the `audio` group and a journal-reading group (`wheel`).

## MOTU configuration helper

`tests/motu/motu_ds.sh` configures the MOTU devices for a multi-device run through each
device's web datastore (`http://<host>/datastore`, bash + curl + jq). It replaces clicking through
the MOTU web UI. A MOTU acts as ACMP controller for writes to its datastore, also for *our*
inputs: writing `avb/<listener>/cfg/0/input_streams/<i>/talker` = `"<talker-eid>:<uid>"`
connects, `"0000000000000000:0"` disconnects.

| command | does |
|---------|------|
| (none) | interactive `select` menu: status, apply / check topology, connect / disconnect one input, set clock, loopback, restore. Every item calls the sub-command's function, so a manual session writes exactly what a script would |
| `status` | read-only, every host in `MOTU_HOSTS`: name, eid, rate, clock source by name, lock; `in j <- <talker>:<uid> <status>`; `AVB out j <- AVB in j` / `custom` / `unrouted`; which devices follow it as MOTU AVB master clock; our own inputs |
| `apply <topo>` | writes only the differences, in order: device inputs ← us, routing, clock (+ wait for lock), our inputs ← device. Prints the `avb_up.sh` command the topology needs and warns when the running N differs |
| `check <topo>` | read-only, one `ok` / `FAIL` line per device + one for us: inputs connected to the right talker, routing, `clockLocked=1`, `current_sampling_rate=$RATE`; for us also the stack N and the kernel's published `avb_listener_src_macs` |
| `restore` | writes back the snapshot (newest first), waits for lock and connections, then renames it `motu-restore.<date>.txt`; kept when a write fails, so `restore` can be re-run |
| `get <host> <key>`, `set <host> <key> <json>` | one datastore key; `set` reads it back |
| `connect <host> <listener> <in> <talker> <uid>` | one input; `<listener>`/`<talker>` = an eid, `us` or a host name. Waits for `connection_status` (a MOTU) or the kernel list entry (us). |
| `loopback <host> <n>` | AVB-in j → AVB-out j, all channels, j < n (banks found by name "AVB Stream j+1") |
| `clock <host> [<source>]` | clock source by name (default "AVB Input Stream 1"), waits `LOCK_SECS` for lock |

`--dry-run` (anywhere on the line) prints every write as `host key: old -> new` and sends nothing.
Every write first saves the old value to `~/.avb/motu-restore.txt`; an existing snapshot is
extended, never overwritten, so `restore` always returns to the state before the first write.
Two refinements:
- A clock source change also saves the device's `master_clock/uid`: leaving "MOTU AVB Master
  Clock" clears it, and a device back on that source with no master falls to Internal.
- Our own inputs do not survive a stack restart, so their snapshot value depends on when the
  stack last started. `restore` only disconnects our inputs whose talker is a device it changes
  back (n7: inputs 4-6 ← UltraLite go, 0-3 ← 828ES stay).
Run it as the normal user. Exit codes: **0** ok, **1** FAIL, **2** setup error (usage, bad
topology, used host offline).

**Topology files** (`tests/motu/topologies/<name>.topo`): one line per range, `#` comments.

```
# our-streams  host   device-streams
0-3            828es  0-3
4-6            ulavb  0-2
```

Each pair (k, j): device input j ← us:k; device AVB-out j ← AVB-in j; our input k ← device uid j;
every listed device clocks from "AVB Input Stream 1" (its input 0, which the topology must use).
Our streams must be exactly 0..N-1, no (host, j) twice, j below the device's stream counts.

| topology | N | mapping | proves |
|----------|---|---------|--------|
| `n7` | 7 (56ch) | 0-3 ↔ 828ES 0-3, 4-6 ↔ UltraLite 0-2 | two devices, uid ≠ k, HW filters |
| `n12-crossmap-16a` | 12 (96ch) | 0-8 ↔ 16A 0-8, 9 ↔ 16A 10, 10 ↔ 16A 9, 11 ↔ 16A 11 | uid ≠ k in SW demux |
| `n11` | 11 (88ch) | 0-3 ↔ 16A, 4-7 ↔ 828ES, 8-10 ↔ UltraLite | three talker filters |
| `n24` | 24 (192ch) | 0-15 ↔ 16A, 16-19 ↔ 24Ai 0-3, 20-23 ↔ 242Ai 0-3 | scale |
| `n4-16a`, `n16-16a`, `n4-828es` | 4 / 16 / 4 | 1:1 on one device | regression |
| `n31` | 31 (248ch) | 0-15 ↔ 16A, 16-19 ↔ 24Ai, 20-23 ↔ 242Ai, 24-27 ↔ 828ES, 28-30 ↔ UltraLite | the setup's maximum |
| `n31-swap` | 31 (248ch) | as `n31`, 24Ai and 242Ai swapped (16-19 ↔ 242Ai, 20-23 ↔ 24Ai) | device vs stream-index fault |
| `n4-24ai` | 4 (32ch) | 0-3 ↔ 24Ai 0-3 | one device without network load |

Recipe (same for every topology; `apply` prints the first two lines for it):

```bash
./tests/motu/motu_ds.sh --dry-run apply n7
sudo AVB_DEVICE_SOURCE_MAC=<828ES MAC> ./avb_up.sh enp5s0 48000 --num-streams 7
./tests/motu/motu_ds.sh apply n7
./tests/motu/motu_ds.sh check n7
CH=56 EXPECT_LIVE=0,1,2,3,4,5,6 ./tests/test_rx_streams.sh
CH=56 ./tests/audio/test_loopback.sh
./tests/motu/motu_ds.sh restore
```

`AVB_DEVICE_SOURCE_MAC` (the AECP rate target and the stream-0 fallback) is stream 0's device:
828ES for `n7`, 16A for `n12-crossmap-16a`, `n11` and `n24`. Passed as `sudo VAR=... cmd` it
overrides `avb-macs.conf`; `VAR=... sudo cmd` would be dropped by sudo. `apply` does not change
rates: AECP SET_SAMPLING_RATE reaches only that one device, so `check` reports a rate mismatch.

**Full-setup run, N=31** (16A ×16, 24Ai ×4, 242Ai ×4, 828ES ×4, UltraLite ×3 = 248 ch; stream 0
is on the 16A, so `AVB_DEVICE_SOURCE_MAC` is the 16A's MAC). No `#` comments in the lines: zsh
does not take them interactively.

```bash
sudo AVB_DEVICE_SOURCE_MAC=<16A MAC> ./avb_up.sh enp5s0 48000 --num-streams 31
./tests/motu/motu_ds.sh apply n31
./tests/motu/motu_ds.sh check n31
CH=248 EXPECT_LIVE=$(seq -s, 0 30) ./tests/test_rx_streams.sh
SECS=1200 ./tests/motu/device_check.sh n31
CH=248 OUTDIR=/dev/shm SECS=240 ./tests/audio/test_loopback.sh
./tests/motu/motu_ds.sh restore
```

1. Restart the stack at the topology's N. `apply` prints the exact line.
2. `apply`, then `check`. Also look at the dashboard: TALKER/LISTEN READY 31/31 and OUT/IN 31/31.
   `check` cannot see our talker state.
3. The RX counter gate. Pass `CH` and `EXPECT_LIVE` every time: the defaults are 8 ch / stream 0 only.
4. `device_check.sh`: a live glitch check of every device, verdict after `SECS`, no recording. At
   ~0.2 events/min a clean 1200 s run leaves ≈2 % chance that a fault is hiding (600 s: ≈13 %).
5. The full audio gate, against `baseline-248ch.txt`. 240 s at 248 ch is an 11.4 GB raw
   recording on tmpfs plus ~42 min of analysis. For a new channel count, establish the baseline
   once on known-good code: `CH=… ./tests/audio/test_loopback.sh --establish-baseline`.
6. `restore` puts the devices back as they were before the first `apply`.

Both `n31` and `n24` reach 248k / 192k pps. Before blaming the stack for audio glitches on one
device, run `device_check.sh` or `fanout.sh` on it. Repeated `copy-64` events on
one 24Ai were traced to its network link (cable / jack / switch port), not to the unit or to us.

**Your own setup.** Write a topology for your devices and run the same chain:

1. `./tests/motu/motu_ds.sh status` lists every reachable MOTU (set `MOTU_HOSTS` to your host
   names): eid, clock and one line per AVB input (`in j`) and output (`AVB out j`). Those lines
   show how many streams each device has.
2. Write `tests/motu/topologies/<name>.topo`: one `<our-streams> <host> <device-streams>` line
   per device. Our streams must cover 0..N-1 exactly. Per device use at most
   min(AVB in, AVB out) streams and start at device stream 0 (its clock follows input 0).
3. `./tests/motu/motu_ds.sh --dry-run apply <name>` shows every write and the `avb_up.sh` line
   (stream 0's device MAC, N), and sends nothing.
4. Then run steps 1-6 above with your N: `CH=8N`, `EXPECT_LIVE=$(seq -s, 0 N-1)`, and
   `device_check.sh <name>`.

| var | default | meaning |
|-----|---------|---------|
| `MOTU_HOSTS` | `16a 24ai 242ai 828es ulavb` | hosts to query; an offline one is an error only when the topology uses it |
| `SWITCH_HOSTS` | `avbswitch1 avbswitch2` | MOTU AVB Switches (`GET /params`); the one with the best priority must be everybody's gPTP grandmaster |
| `RATE` | `48000` | rate `check` expects |
| `LOCK_SECS` | `10` | wait for `ext/clockLocked=1` |
| `CLOCK_SOURCE` | `AVB Input Stream 1` | clock source of every topology device |
| `OUR_EID` | auto | our entity: the one with `model_name` "Linux AVB", else from `/proc/snd_igb_avb/streams` |
| `RESTORE_FILE` | `~/.avb/motu-restore.txt` | snapshot |
| `READBACK_SECS` | `10` | how long a write may take to read back / connect |
| `EXTRA_STREAMS` | `0` | streams the stack runs beyond the topology's pairs (`fanout.sh` copies); `check` then wants N = pairs + this exactly |
| `JOURNAL_FILE` | journal of the running avb-user | log text `diagnose` reads (`[DIAG]`, ACMP lines) |
| `DS_DIR` | – | read `<host>.json` (switches: `<host>.params.json`) from here instead of the devices (offline `diagnose`) |

### Why did apply/check fail? (`motu_ds.sh diagnose`)

`apply` (when device inputs do not connect) and `check` (on FAIL) end with a diagnosis; run it
alone with `./tests/motu/motu_ds.sh diagnose [<topo>]` (without a topology: every MOTU input
whose talker is us). It prints one `FAIL <host> in <j> <- us:<k>: <cause>` line per device and
cause; `j` counts from 0 like the datastore. The causes come from the datastores, the running
avb-user's journal and the `[DIAG]` lines avb-user logs from asking every device over AVDECC
(sampling rate, clock source, stream formats, stream state). The same findings are on the
dashboard in the device's DEVICES row, in place of serial/group/avail/ADP; there device streams
count from 1 like the MOTU UI ("AVB Stream 1"), our streams from 0.

| cause printed | meaning | fix |
|---------------|---------|-----|
| `talker is X, want us:k` | the input was never written, or something else rewrote it | `apply <topo>` |
| `no AVB stack` / `the stack runs N=…` | stack down or started with too few streams | restart with the line `apply` prints |
| warning `the stack runs with AVB_DEVICE_SOURCE_MAC=…` | stack started without the hinted MAC (stream 0's device) | restart: `sudo AVB_DEVICE_SOURCE_MAC=… ./avb_up.sh …` |
| `we answered its CONNECT_TX, yet …` | the device asked us for the stream and we answered; the reservation failed | read the printed `talker[k]` / `uid=k` journal lines; dashboard TALKER row |
| `avb-user saw: … sample rate …` | the device runs another rate | set the rate in the MOTU app (AECP sets only `AVB_DEVICE_SOURCE_MAC`'s rate) |
| `avb-user saw: … stream format …` | the device's stream formats do not match ours, e.g. 192 kHz-sized at 48 kHz | detach the computer host or reset the rate in the MOTU app; a reboot alone did not clear it |
| `avb-user saw: … MSRP failure …` | the bridge refused the reservation (code, bridge id) | bandwidth / switch port |
| `avb-user saw: … not connected although we answered …` | as "we answered" above, seen by avb-user | as above |
| `… a computer host is attached (host/connected=1) …` | nothing else explains it; a USB host can force rates and formats | check them in the MOTU app or detach the host, else power-cycle |
| `listener stuck: X sent no ACMP for in …` | the device never asked us for the stream | power-cycle the device |
| note `clocks from in j, which is not connected` / `clock not locked: consequence of in j` | its clock follows a failing input | fix in j first |
| `gPTP: X follows grandmaster Y, not <switch> …` | X (a host or `us`) is outside the best switch's gPTP domain; all of them at once = that switch's gPTP is hung (its web page still answers) | power-cycle the switch, then restart the stack |
| `us gPTP link: the link partner answers none of our Pdelay_Req …` | avb-user watches PTP on our port: the bridge sends its own Pdelay_Req but answers none of ours = hung gPTP | power-cycle the named switch |
| `us gPTP link: no gPTP frames from the link partner …` / `… N ports answer our Pdelay_Req …` | our port is not on an AVB bridge port, or a plain switch sits in between | cabling |
| `us gPTP link: Ethernet link down …` | our port has no carrier (a switch reboots, or the cable); TX, split and stall checks pause meanwhile | wait, or cabling |
| `us AVTP talker: TX engine armed but launching no packets …` | the kernel TX schedule is stuck (a module without the PHC-step re-align) | restart the stack |

`[DIAG]` lines are logged at WARNING when an anomaly appears and as `[DIAG] cleared: …` when it
goes away: `journalctl -t avb-user | grep DIAG`. The gPTP lines are printed before the input
lines, because a gPTP fault explains every input behind it; `status` shows them too. The
dashboard's CLOCK row names our link partner and turns red while it does not answer.

A grandmaster change while the stack runs steps the PHC, by decades when we were our own
grandmaster (Unix time) and a freshly booted switch takes over. The kernel TX schedule then
re-aligns (`tx schedule re-aligned: PHC stepped back …` in the kernel log, counted in
`tx_late_launch`).

### Device check (`tests/motu/device_check.sh`)

A short live loopback test of one device inside a topology, for fault hunting
in short cycles. It plays the `audio_check` stimulus on all of our streams and
pipes `arecord -t raw -` straight into `glitch_monitor.py`, so no recording is
written. Events print as they happen, and the verdict comes `SECS` after the
start. (A 240 s `test_loopback.sh` run at 248 ch needs ~11 GB of tmpfs and a
42-minute analysis.) The topology maps each stream to its device: every event
names the device and its output stream, and the devices not judged serve as
controls.

```bash
./tests/motu/motu_ds.sh apply n31-swap
SECS=300 ./tests/motu/device_check.sh n31-swap 24ai      # judge the 24Ai, the rest are controls
```

```
  t= 142.308s  24ai   stream 21 (24ai Output Stream 2)  copy-64  6 frames x 8 ch = the frames 64 earlier
  24ai   judged  EVENTS   1 event(s)   0.25/min  streams 21  copy-64=1
  242ai  control clean    0 event(s)   0.00/min  streams -
```

- `copy-L`: the glitched frames equal the frames L earlier on all 8 channels
  (the 24Ai link fault above is `copy-64`, one packet). `glitch`: anything else.
- An event on ≥ 75 % of the streams at once is on our side, not a device
  fault. The summary names the cause: a TX schedule resync when
  `tx_late_launch` moved within ±2 s (every device then misses the same
  packets), else the capture side. An `arecord` overrun counts the same way. Either one makes the
  verdict INCONCLUSIVE, as does a stream below −20 dBFS (a dead connection or
  route).
- Our side is polled once per second: `tx_late_launch`, `tx_xrun`,
  `rx_seq_errors`, `rx_clock_slips`, `rx_budget_hits` and `rx_ring_near_lap`.
  Every change prints live as `OUR`, and an event within ±2 s of a change is
  flagged "may be ours, not the device".
- Checked: glitches and silence only. Amplitude, crosstalk and bit-exactness
  stay with `test_loopback.sh`.
- Env: `SECS` (120), `SKIP` (3 s settle), `CH` (8 × the stack's N), `CHECK=0`
  skips `motu_ds.sh check`, `DEV`, `RATE`, `LOGDIR` (`/dev/shm`). Exit: 0
  PASS, 1 FAIL, 2 INCONCLUSIVE.
- The monitor runs at ~3× real time at 248 ch. A reader thread drains the pipe
  and `arecord` starts 1 s late, so the 44 ms ALSA capture buffer cannot
  overrun while the analysis runs.

#### Fan-out (`tests/motu/fanout.sh`): input side or output side?

A MOTU with more AVB outputs than inputs (24Ai: 4 in, 8 out) can send each
input j to output j **and** to outputs j+4, …. We receive every output, so
each output carries a copy of the same source data. An event on all copies at
once (`COINCIDENT`) means the data was bad before the device split it: input
side or router. An event on one copy only (`SINGLE`) means it went bad on that
output stream.

```bash
sudo AVB_DEVICE_SOURCE_MAC=<24Ai MAC> ./avb_up.sh enp5s0 48000 --num-streams 8
./tests/motu/fanout.sh 24ai apply     # loopback pairs 0-3 + routing out 5-8 <- in 1-4 + our inputs 4-7 <- out 5-8
SECS=1200 ./tests/motu/fanout.sh 24ai run
./tests/motu/fanout.sh 24ai undo      # (or motu_ds.sh restore: every write is in the snapshot)
```

`run` checks the setup (`fanout.sh <host> check`, read-only), then calls
`device_check.sh` with `MAP=0-7=24ai:0 SRC=4=0,5=1,6=2,7=3`. Those two env
vars describe streams that a `.topo` cannot (receive-only copies), and the
monitor lists every fan-out event as `COINCIDENT` or `SINGLE`. The device's
input/output counts and bank indices come from its datastore, so the same
command works for the 242Ai as a control.

#### Shared listeners (`tests/motu/shared.sh`): K of our inputs on one talker stream

Fan-out without the device: each copy input of ours connects to the device
uid that its source input already receives. The talker sends one stream, and
the kernel writes every packet into each listener's slice. `SHARE`
names the copies: `4-7=0-3` = our inputs 4-7 share the streams of inputs 0-3,
and `4-7=0-3,8-11=0-3` = three listeners per stream. The stack runs N = topology
pairs + copies.

```bash
IFACE=enp5s0 EXTRA_STREAMS=4 ./tests/motu/motu_ds.sh --dry-run apply n4-828es    # prints the avb_up.sh line (N=8), also with the stack down
./tests/motu/shared.sh n4-828es apply   # n4-828es + our inputs 4-7 <- 828ES uid 0-3
./tests/motu/shared.sh n4-828es check   # read-only: connections + the same kernel expected_sid per copy
./tests/motu/shared.sh n4-828es exact   # 3 s: every copy's slice == its source's, bit for bit
SECS=120 ./tests/motu/shared.sh n4-828es run    # check + exact + device_check.sh (copies as twins)
./tests/motu/shared.sh n4-828es leave 4 # disconnect input 4: it goes idle, input 0 keeps receiving
./tests/motu/shared.sh n4-828es join 4
./tests/motu/shared.sh n4-828es undo    # disconnect the copies (or motu_ds.sh restore)
```

Pass the same `SHARE` to every command, and run `apply` again after a stack
restart (our inputs reset). `exact` refuses to run unless every copy holds its
source's kernel stream id. `leave`/`join` check only the affected group: inputs
left earlier are allowed and judged idle, so several can leave in turn. A
capture that stalls during the window (arecord EIO) is a FAIL: stream 0, the
capture reference, receives nothing.

## Timer / Shadow Period Check

`tests/test_phc_times.sh` validates the TX timer engine while playback is
active. In the default shadow mode (`tx_engine_enabled=0`) it samples
`/proc/snd_igb_avb/tx_engine` and fails if the timer is not active, ticks/PHC
values do not move, the shadow playback pointer does not advance, drift exceeds
the configured sample bound, `tx_xrun` grows, or fresh kernel WARN/BUG/locking
messages appear. In live mode (`tx_engine_enabled=1`) it keeps the
PHC-vs-RX comparison using fresh `avb phc ... delta-rx ...` kernel logs.

```bash
GPTP_SYNTONIZE=-S sudo ./avb_up.sh enp5s0 48000
MODE=shadow ./tests/test_phc_times.sh
```

Useful knobs:

| var | default | meaning |
|-----|---------|---------|
| `MODE` | `auto` | `auto`, `shadow`, or `live` |
| `DURATION` | `10` | sample window in seconds |
| `MAX_SHADOW_DRIFT_SAMPLES` | `1200` | max absolute timer-vs-legacy drift in shadow mode |
| `MAX_ABS_DELTA_NS` | `1000000` | max absolute PHC-vs-RX delta, default 1 ms |
| `MAX_SPREAD_NS` | `1000000` | max delta range during the sample |
| `MIN_LOG_LINES` | `3` | minimum fresh mode-specific kernel log lines required |

Exit codes match the audio test convention: **0** = PASS, **1** = FAIL, **2** =
INCONCLUSIVE/setup not ready.

The audio test itself needs **no root** -- it only plays/records on the ALSA device (you
must be in the `audio` group) and builds a small C tool. Run it as your normal user
so the stimulus/recording/baseline land in your own `~/tmp`, not `/root/tmp`. (Only
`avb_up.sh`, which swaps the kernel module, needs `sudo`.)

Frequency, mapping, crosstalk and glitches are judged against **absolute** criteria
(no baseline needed). Only **amplitude** needs the baseline, because the loopback
gain through the MOTU isn't knowable a priori — hence the one-time step 2. Without a
baseline the tool still runs and judges amplitude against a loose absolute sanity
band (with a warning).

### Knobs (env)

| var | default | meaning |
|-----|---------|---------|
| `DEV` | `hw:igbavb` | ALSA device |
| `RATE` | `48000` | sample rate |
| `CH` | `8` | channel count, 8 per listener stream (`8 × num_streams`) |
| `LIVE_CH` | `CH` | channels `< LIVE_CH` are judged as usual; channels `>= LIVE_CH` must be **silent** (see below) |
| `SECS` | `25` | scored capture seconds (any length; see *Long runs*) |
| `GLITCH_BUDGET` | `16` | discontinuities tolerated before FAIL (count is always reported) |
| `BASELINE` | `baseline.txt` (`CH=8`), else `baseline-${CH}ch.txt` | amplitude baseline file, in `tests/audio/` — one per channel count, so a 32ch run never compares against the 8ch band |
| `OUTDIR` | `$HOME/tmp` | where the recording/metrics go (under `sudo -E`, your home). Set explicitly, it also takes a raw recording |
| `RAMDIR` | `/dev/shm` | tmpfs for a raw (> 2 GiB) recording when `OUTDIR` is not set; `RAMDIR=` keeps it in `OUTDIR` (see *Long runs*) |

### Long runs

A WAV's sizes are 32-bit, so one file holds < 4 GiB, and `arecord` splits a WAV recording at
2 GiB. That is ~87 s at 128ch, ~116 s at 96ch, ~174 s at 64ch and ~349 s at 32ch. **arecord's
split is lossy**: it drops the rest of the current read chunk (~1 period, 1011 frames at 128ch)
at each split. Neither limit applies to the harness:
- The stimulus is streamed: `audio_check gen - --raw | aplay -t raw`. It repeats every
  second, so `gen` writes one period over and over, and no stimulus file is written.
- A recording that would exceed 2 GiB is taken as one raw S32_LE file
  (`avb-loopback.raw`, `arecord -t raw`). `audio_check check` reads `*.raw` given
  `--ch`/`--rate`. Shorter runs still produce `avb-loopback.wav`.
- The raw file goes to `RAMDIR` (`/dev/shm`) when it is a tmpfs with room for it plus
  1 GiB and `MemAvailable` covers it plus 4 GiB. Otherwise it goes to `OUTDIR`, and the
  preflight line says why. On disk, `arecord` writing 37 MB/s (192ch) overran its ALSA
  buffer (57 ms at 192ch) on writeback stalls: N=24 `SECS=240` on ext4 had overruns
  up to 1.6 s, and tmpfs had none. An explicit `OUTDIR` is used as given. A RAM recording is
  deleted on PASS and kept on FAIL/INCONCLUSIVE (the result line says so). The next run
  removes it.
- `arecord` overruns are counted from its stderr. Any overrun makes the run INCONCLUSIVE
  (host-side gaps, not a data-plane fault) and skips the analysis unless `ANALYZE_OVERRUN=1`.
- Each run deletes the previous run's recordings (`.wav`, `.raw`, old `-NN.wav` parts, and a
  kept RAM recording) first.

Analysis cost grows with channels² × seconds: about 3 min per 100 s at 128ch. The whole
recording is held in memory (5.9 GB for 240 s at 128ch). `audio_check check` reports
progress on stderr, so the metrics file stays clean: a `reading` line for a large file,
then `analysing: channel c/N (p%)  <elapsed>, ~<left> left`. On a terminal that is one line
updating in place; in a log it is a line every ~10%. Analyses under 2 s and the liveness
probe print nothing.

### Live channels (`LIVE_CH`)

At N>1 only some listener streams may be able to arrive (e.g. before per-stream listener
identities exist, only stream 0 → ch0-7). `CH=32 LIVE_CH=8` then judges ch0-7 as usual and
requires ch8-31 to be **silent**: total level (tone + noise + DC) below `TONE_DBFS +
NOISE_FLOOR_DB` = −46 dBFS, reported `ok (silent)`, else `FAIL LEAK <level>dBFS`. The stimulus
still drives all `CH` channels, so a leak carries a *foreign* tone and the verdict names its source
(`(ch3 tone)`); a smear of several channels is reported `(broadband)`. The liveness probe only
considers channels `< LIVE_CH`, so "only stream 0 arrives" is not misreported as INCONCLUSIVE.
`--establish-baseline` requires `LIVE_CH = CH`.

### Reading the result

```
  ch  freqHz    ampdBFS   xtalkdB   noisedB   dcdBFS    glitch  verdict
  0   660.0     -6.0      -67       -71       -120      0       ok
  ...
== glitches: 3 total (budget 16) within
== VERDICT: PASS -- no audio regression
```

Exit codes: **0** = PASS, **1** = FAIL (regression — recording kept for inspection),
**2** = INCONCLUSIVE (setup/loopback problem, not a code verdict). Per-channel `FAIL`
rows name the reason (`MAP->chN`, `FREQ off`, `XTALK`, `NOISE`, `AMP`, `LEAK`).
An INCONCLUSIVE keeps its evidence: arecord's stderr (`~/tmp/avb-loopback.arecord.log`) and
every stream's `rx_pkts` sampled at 1 Hz during each recording (`~/tmp/avb-loopback.rx.log`).
The message shows arecord's error, how far into the recording it failed, and the last 8 s of
per-stream packets/s (all streams at 0 = capture/reference stall, some = one talker device);
a failed probe names the silent channels.

> First real run may need a one-time tolerance nudge: the `FREQ_TOL_HZ`,
> `CROSSTALK_DB_MAX`, `NOISE_FLOOR_DB` and `AMP_BAND_DB` constants at the top of
> `audio_check.c` are set for a clean path; widen if a known-good loopback trips them.

## Files

- `audio_check.c` — stimulus generator + WAV content oracle (Goertzel + a 2nd-order
  sine-predictor for glitches; no FFT lib, no Python). `gen` (WAV, or `- --raw` stream) /
  `check` (WAV or `*.raw` recording; several contiguous files are joined) subcommands.
- `Makefile` — builds `audio_check`.
- `test_loopback.sh` — the loopback orchestrator (normal user; preflight, probe, run, verdict).
- `../test_rx_streams.sh` — RX multi-stream counter gate (see *RX multi-stream gate*).
- `test_phc_times.sh` — TX timer PHC and shadow period timing check.
- `test_talker_dest.sh` — unrelated control-plane check (talker dest MAC across planes).
- `../motu/motu_ds.sh`, `../motu/topologies/*.topo` — MOTU configuration helper and the
  multi-device topologies (see *MOTU configuration helper*).
- `../motu/device_check.sh`, `../motu/glitch_monitor.py`, `../motu/fanout.sh` — live per-device
  glitch check and the fan-out input/output-side test (see *Device check*).
- `../motu/shared.sh` — K of our inputs on one talker stream: apply/check/bit-exact/join/leave.
- `baseline.txt`, `baseline-${CH}ch.txt` — generated by `--establish-baseline`, one per channel
  count (git-ignore or commit a known-good one).
```