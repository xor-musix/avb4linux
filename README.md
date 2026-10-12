# AVB4linux

ALSA driver and supporting daemons for Audio Video Bridging on Intel i210 NICs.
It exposes the NIC as one ALSA PCM device carrying up to 32 8-channel AVB streams
in each direction, to and from one or more AVB endpoints (see
[Current limits](#current-limits)).

## Structure

Upstream code is pulled in as git submodules whose sources are never edited. The
build copies the kernel driver, gptp and the shaper into staging trees
(`kernel-module/snd-igb-avb/build`, `daemons/gptp-build`, `daemons/OpenAvnu/build`)
and applies this repository's patch overlay there. The one exception is
`utils/build-tcpdump-for-motu.sh`, which patches `utils/libpcap` in place.

| Path | Submodule | Role |
|------|-----------|------|
| `kernel-module/igb_avb`     | Avnu/igb_avb | Intel/Avnu igb_avb driver, base of the kernel module |
| `kernel-module/snd-igb-avb` | —            | ALSA/AVTP sources (`src/`) + patches → `snd_igb_avb.ko` |
| `daemons/OpenAvnu`          | Avnu/OpenAvnu | `mrpd`, `maap`, `shaper` (shaper patched from `daemons/patches/`) |
| `daemons/gptp`              | Avnu/gptp    | gPTP daemon `daemon_cl` (patched from `daemons/gptp-patches/`) |
| `jdksavdecc-c`              | jdkoftinoff/jdksavdecc-c | AVDECC library for `avb-user` |
| `include`                   | —            | `stream_id.h`, shared by the kernel module and `avb-user` |
| `tools`                     | —            | `avb-user` user-space app (ADP/AECP/ACMP, MSRP/MAAP clients, status screen) |
| `tests`                     | —            | test harnesses and throughput tools, see [tests/README.md](tests/README.md) |
| `utils`                     | —            | setup and developer helpers: MAC detection, NetworkManager hand-over, [network-config](utils/network-config/README.md), pcap/WAV analysis, static tcpdump build for MOTU devices |
| `utils/libpcap`, `utils/tcpdump` | the-tcpdump-group | sources for the MOTU tcpdump build (`utils/build-tcpdump-for-motu.sh`) |

Scripts:

| Script | Purpose |
|--------|---------|
| `avb_up.sh`, `avb_down.sh` | start / stop the whole stack (installed to `/usr/local/bin`) |
| `avb_tuning.example.sh` | template for the per-machine tuning hook |
| `utils/avb_autoconfig.sh`, `utils/avb_apply_mac.sh` | detect / set the AVB device MACs (run from the repo) |
| `utils/avb_nm_unmanage.sh` | hand the AVB port over from NetworkManager (installed) |

Known issues: [kernel-module/snd-igb-avb/KNOWN_ISSUES.md](kernel-module/snd-igb-avb/KNOWN_ISSUES.md).

## Prerequisites

- Kernel headers for the running kernel.
- The kernel must provide PTP 1588 clock and i2c-algo-bit support (as modules or built-in).
- A kernel booted with `threadirqs` for the real-time priorities (see below).
- Build: `gcc`, `g++`, `make`, `cmake`, `patch`.
- Runtime: `ethtool`, `ifconfig` (net-tools), `killall` (psmisc), `chrt`/`renice`
  (util-linux), `logger`.
- `tshark` for `utils/avb_autoconfig.sh`; `curl` and `jq` for the MOTU test helpers.

## Build

```sh
git submodule update --init jdksavdecc-c kernel-module/igb_avb daemons/gptp daemons/OpenAvnu

make
sudo make install
```

- `make` builds the kernel module, the daemons, gptp and `avb-user`. The
  jdksavdecc-c library is built first if it is missing.
- `make` runs three warning-baseline checks (kernel module, shaper, gptp). New
  compiler warnings fail the build.
- `make install` copies the binaries to `$AVBHOME` and `avb_up.sh`, `avb_down.sh`
  and `avb_nm_unmanage.sh` to `/usr/local/bin`.
- The other submodules (`utils/libpcap`, `utils/tcpdump`) are only needed for
  `utils/build-tcpdump-for-motu.sh`, which initializes them itself.

### Where the runtime files live (`$AVBHOME`)

All scripts and `make install` use `AVBHOME`, default `$HOME/.avb` of the user
running the command. Under `sudo`, `$HOME` is normally reset to root's home, so
**`sudo make install`, `sudo ./utils/avb_autoconfig.sh` and `sudo avb_up.sh` all use
`/root/.avb`**. Edit `avb-macs.conf` and `avb-tuning.sh` there, or set the same
`AVBHOME` for every step (`sudo AVBHOME=/path ...`).

## Configure the AVB device MACs

Do this once before the first `avb_up.sh`. `avb-user` refuses to start without
the remote device's MACs, which have no default. They are runtime parameters,
read from `$AVBHOME/avb-macs.conf` by `avb_up.sh`, passed to the kernel module
and exported for `avb-user`. No rebuild is needed when they change.

| conf key (`avb-macs.conf`) | kernel module param | meaning |
|----------------------------|---------------------|---------|
| `AVB_DEVICE_SOURCE_MAC`      | `avb_device_source_mac` | remote AVB device MAC: AVDECC/AECP target, RX stream-0 fallback (required) |
| `AVB_DEVICE_TALKER_MAC_BASE` | `avb_device_talker_mac` | remote device's talker stream multicast: RX stream-0 fallback (required) |
| `OWN_MAC`                    | `own_mac`               | the i210's own MAC / AVTP source (default: NIC hardware MAC) |

- **Talker (TX):** `avb-user` allocates the stream destination addresses via
  MAAP at runtime, starting at `MAAP_LOCALLY_ADMINISTRATORED_BASE`
  (`avb-config.h`). If MAAP fails, or with `AVB_MAAP=0`, it uses that fixed base
  plus the stream index.
- **Listener (RX):** each RX stream is bound at runtime from the ACMP connection
  that `avb-user` publishes to the kernel. The two device MACs are the kernel's
  identity for RX stream 0 until then. Capture `START` is refused with `-ENODEV`
  while RX stream 0 has no identity.
- A source MAC given on the command line wins over the conf:
  `sudo AVB_DEVICE_SOURCE_MAC=... avb_up.sh ...`.

### Why one device's MACs are still required

All TX streams and RX streams 1..N-1 are set up at runtime (MAAP, ACMP). Only one
remote device, the talker of our RX stream 0, still has to be configured:

- **Capture clock:** RX stream 0 drives the ALSA capture pointer, so the kernel
  refuses capture `START` until stream 0 has an identity. ACMP only binds a
  stream once a controller connects it, which may be after the DAW opens the
  card, or never. The two MACs give stream 0 a static identity at module load:
  destination `AVB_DEVICE_TALKER_MAC_BASE`, source `AVB_DEVICE_SOURCE_MAC`,
  stream ID = source MAC + index 0. With that identity `avb-user` can also
  attach the MSRP listener to the stream without a controller.
- **AECP target:** `avb-user` sends its AEM commands (such as setting the sample
  rate) only to the entity of `AVB_DEVICE_SOURCE_MAC`. It is also the default
  peer of every stream. In multi-device setups it must be the device behind RX
  stream 0, and the other devices' rates are set by hand.

Nothing selects this device from ADP/ACMP discovery yet, so `avb-user` refuses to
start without both MACs.

### Detect them automatically (experimental)

Run with the AVB device connected and the stock driver still loaded (BEFORE
`avb_up.sh`). It works with any AVB/IEEE-1722.1 endpoint.

```sh
sudo ./utils/avb_autoconfig.sh [interface] [seconds] [name-match]   # needs tshark
```

- `OWN_MAC` is read from the interface.
- `AVB_DEVICE_SOURCE_MAC` is the endpoint that announces itself via AVDECC ADP.
  If several answer, the one whose entity name or MAC matches `name-match` is
  chosen. Otherwise it is the busiest source that advertises a talker stream,
  else the busiest source.
- `AVB_DEVICE_TALKER_MAC_BASE` is the most frequent stream destination seen in
  that device's MSRP and MAAP traffic. If none is seen, nothing is written and
  the script tells you how to set the values by hand.

The values are written by `avb_apply_mac.sh`, which you can also call directly:

```sh
sudo ./utils/avb_apply_mac.sh <AVB_DEVICE_SOURCE_MAC> <AVB_DEVICE_TALKER_MAC_BASE> [OWN_MAC]
```

### Take the AVB port away from NetworkManager (recommended, one-time)

`snd_igb_avb` ties the AVB device lifecycle to the netdev open/close path, so any
link up/down rebuilds the ALSA card, DMA buffers and RX filter. If NetworkManager
manages the interface it will up/down the port on its own schedule (carrier
changes, DHCP/RA, connectivity checks, profile reloads) and tear down a running
stream. For a dedicated AVB port, hand it to a single owner once:

```sh
sudo ./utils/avb_nm_unmanage.sh <interface>        # persistent: writes an NM drop-in
sudo ./utils/avb_nm_unmanage.sh -r <interface>     # undo: give the port back to NM
```

This writes `/etc/NetworkManager/conf.d/99-avb-unmanaged.conf` (survives reboots)
rather than toggling `nmcli` each run. It is a no-op if NetworkManager is not the
active manager — if `systemd-networkd`/`netplan`/`ifupdown` owns the port, mark it
unmanaged there instead. `avb_down.sh <interface> --restore-nm` (or
`AVB_RESTORE_NM=1`) runs the `-r` undo for you on a final teardown.

## Run

```sh
sudo avb_up.sh <interface> [<samplerate> [<num_streams>]]
sudo avb_up.sh <interface> [--samplerate <rate>] [--num-streams <N>] [--tx-mirror]
sudo avb_down.sh <interface> [--restore-nm]
```

- The sample rate defaults to 48000 and the stream count to 1.
- `avb_up.sh` stays in the foreground: `avb-user` owns the terminal with its
  status screen. Run `avb_down.sh` from a second terminal.
- The daemons log to syslog:
  `journalctl -t gptp -t mrpd -t maap -t shaper -t avb-user -f`.
- `GPTP_DISCIPLINE=0` lets the PHC free-run (for A/B tests of sync stability).
- Environment overrides go after `sudo`, because sudo drops them otherwise:
  `sudo GPTP_DISCIPLINE=0 avb_up.sh ...`.

The card is `hw:igbavb`. If PipeWire/WirePlumber opens it, a DAW gets "device
busy". `avb_tuning.example.sh` shows how to disable the card in WirePlumber.

### Real-time priorities and per-machine tuning

`avb_up.sh` ranks the AVB stack above a DAW's audio threads (REAPER defaults to
SCHED_FIFO 40), so a busy DAW cannot starve the data plane. This needs a kernel
booted with `threadirqs`:

| Thread | Priority | Override | Role |
|--------|----------|----------|------|
| `ktimers/N` | FIFO 60 | `AVB_KTIMERS_PRIO` | TX engine (soft hrtimer) |
| `irq/N-<iface>` | FIFO 50 | `AVB_IRQ_PRIO` | RX ring, NIC interrupts |
| `daemon_cl` | FIFO 45 | `AVB_GPTP_PRIO` | gPTP |
| mrpd, maap, shaper, avb-user | nice -10 | `AVB_DAEMON_NICE` | protocol timers |

Keep the DAW's RT priority **below `AVB_GPTP_PRIO`**. Example:
`sudo AVB_GPTP_PRIO=55 avb_up.sh ...`.

Machine-specific settings live in `$AVBHOME/avb-tuning.sh`. `make install` seeds
it from `avb_tuning.example.sh` and never overwrites it. `avb_up.sh` runs it with
`up` right after the priorities are set (so it can override them), and
`avb_down.sh` runs it with `down`. The template sets the CPU governor to
`performance` and restores it on `down`, and can pin the NIC interrupts to the
NIC's NUMA node. Skip it for one run with `sudo AVB_TUNING= avb_up.sh ...`.

### TX Tap Debugging (TX Mirror)

For AVTP frame analysis with `tcpdump`, enable TX frame mirroring:

```sh
sudo avb_up.sh <interface> 48000 --tx-mirror
sudo ip link set avbtap0 up
sudo tcpdump -i avbtap0 -w tx_frames.pcap
```

When enabled, each built AVTP frame is copied and injected into the dummy netdev
`avbtap0` via `netif_rx()`, making it visible to `tcpdump`. The TX descriptors,
CBS shaping and the frames on the wire are unchanged, but enabling it adds a
per-frame copy in the TX timer path.

`avbtap0` always exists while the module is loaded. Mirroring is off by default
and can also be toggled at runtime via
`/sys/module/snd_igb_avb/parameters/avb_tx_mirror`.
`/proc/snd_igb_avb/tx_engine` shows the `tx_mirror_frames` count.

## Current limits

| Limit | Value | Where it comes from |
|-------|-------|---------------------|
| Streams | one count N for **both** directions, 1-32 (`avb_up.sh --num-streams N`) | `MAX_STREAMS` in `avb-config.h` |
| Stream format | 8 ch AM824 (IEC 61883-6), class A, sample rate fixed at load | |
| Proven | N=31: 248 ch each way, 248k pps RX, five MOTU devices, 48 kHz | `tests/motu` topology `n31` |
| Talker devices we receive from | at most **8** different devices | one i210 flex filter per device above N=8, one per stream at N≤8 |
| Remote listeners per stream of ours | 16 devices | `ACMP_MAX_LISTENERS` |
| Bandwidth | SRP reserves at most 75% of the link per direction: 44 streams at 48 kHz, 25 at 96 kHz, 13 at 192 kHz | the kernel only warns when TX goes over |
| RX ring | 256 descriptors ≈ 0.9 ms of IRQ stall at N=31 | `IGB_DEFAULT_RXD` and `MAXDESCRIPTORS` in `alsa.c` (change both together) |
| Capture reference | RX stream 0 drives the capture pointer: disconnecting our input 0 stalls capture | by design |

Planned: replace the count limits with separate TX/RX counts bounded only by bandwidth (44 + 44
at 48 kHz), lift the 8-device limit and size the RX ring from the RX count.

## AI Tools Usage

This project was developed with the help and usage of AI tools.

## Licensing

- User-space code (`tools/`, `tests/`, `utils/`, `include/`, top-level scripts): BSD 2-Clause,
  see [LICENSE](LICENSE). Files with their own copyright header keep it.
- Kernel module sources and patches (`kernel-module/snd-igb-avb/`): GPL-2.0, see
  [COPYING](kernel-module/snd-igb-avb/COPYING). `include/stream_id.h` is shared by both and
  dual-licensed.
- Submodules keep their own licenses.

Every file carries its copyright holders and SPDX license identifier, either in its header
or in [REUSE.toml](REUSE.toml); the license texts are in [LICENSES/](LICENSES/). The repository
follows the [REUSE](https://reuse.software) specification: `pipx run reuse lint` checks it.

## Disclaimer

THIS SOFTWARE IS PROVIDED "AS IS" AND WITHOUT ANY WARRANTIES.
ANY USE OF THIS SOFTWARE IS AT YOUR ABSOLUTE OWN RISK.
