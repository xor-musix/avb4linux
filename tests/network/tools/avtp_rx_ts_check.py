#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
"""
avtp_rx_ts_check.py -- check the RX timestamp model against a peer's real streams

The RX alignment (kernel-module/snd-igb-avb/src/alsa.c avb_rx_track_media_time /
avb_rx_align_frames) assumes the peer timestamps AVTP 61883-6 packets the IEC way:
the avtp_timestamp is the presentation time of the event whose dbc is a multiple of
SYT_INTERVAL (8/16/32 frames at 48/96/192 kHz), and only packets holding such an event
carry one (tv=1) -- at 6 frames per packet, 3 packets in 4. The driver refers every
timestamp back to the packet's first frame and extrapolates across unstamped packets.
If the peer instead stamps frame 0 of every packet, that correction mis-places streams
by up to 4 frames. This tool settles which model the peer follows.

tcpdump only sees streams that no RX flex filter claims; streams the driver consumes
directly are NOT in the capture. All of a peer's streams come from the same talker,
so their timestamp semantics are the same.

Capture (stack up at --num-streams 4, MOTU sinks connected):
    sudo tcpdump -i enp5s0 -nn -c 3000 -w ~/tmp/motu-n4.pcap 'ether proto 0x22f0 or vlan'

Usage:
    avtp_rx_ts_check.py <pcap_file> [--rate 48000] [--ref <stream_id hex>]
    avtp_rx_ts_check.py --selftest

Checks, per 61883 stream:
    tv pattern   tv set exactly on packets holding an SYT_INTERVAL event
    timeline     step between consecutive stamped packets, referred to frame 0, equals
                 the dbc distance in frames (< half a frame off) -- for the SYT model the
                 driver uses, and for the frame-0 model as the alternative
    align        the driver's integer alignment math replayed in capture (= wire) order against
                 a reference stream: every offset within +-ALIGN_MAX_FRAMES (96)
It also prints each stream's src/dst/stream id.

Exit codes: 0 PASS (driver model confirmed), 1 FAIL, 2 INCONCLUSIVE (no usable capture).
"""

import os
import struct
import sys
import tempfile

NSEC = 1_000_000_000
ALIGN_MAX_FRAMES = 96          # kernel-module/snd-igb-avb/src/avb-config.h
AVTP_ETHERTYPE = 0x22f0
SUBTYPE_61883 = 0x00
TIMESTAMP_VALID = 0x01

RED, GREEN, YELLOW, CYAN, NC = "\033[0;31m", "\033[0;32m", "\033[0;33m", "\033[0;36m", "\033[0m"
if not sys.stdout.isatty():
    RED = GREEN = YELLOW = CYAN = NC = ""


def s32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v


def div_trunc(num, den):
    """div_s64(): C division, truncates toward zero."""
    q = abs(num) // den
    return q if num >= 0 else -q


def mac(b):
    return ":".join(f"{x:02x}" for x in b)


# ---------------------------------------------------------------- pcap parsing

def read_pcap(path):
    """Yield raw frames from a classic pcap file (any endianness, us or ns)."""
    with open(path, "rb") as f:
        data = f.read()
    magic = data[:4]
    if magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        endian = ">"
    elif magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        endian = "<"
    elif magic == b"\x0a\x0d\x0d\x0a":
        raise ValueError("pcapng file -- capture with tcpdump -w, or convert: editcap -F pcap in out")
    else:
        raise ValueError("not a pcap file")
    linktype = struct.unpack(endian + "I", data[20:24])[0]
    if linktype != 1:
        raise ValueError(f"link type {linktype}, expected Ethernet (1) -- capture on the NIC, not 'any'")
    off = 24
    while off + 16 <= len(data):
        _, _, incl, _ = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        yield data[off:off + incl]
        off += incl


def parse_avtp(pkt):
    """Return a dict for an AVTP 61883 stream packet, else None.  Accepts the frame with
    the 802.1Q tag in place or stripped by RX VLAN offload."""
    if len(pkt) >= 18 and pkt[12:14] == b"\x81\x00" and \
            struct.unpack(">H", pkt[16:18])[0] == AVTP_ETHERTYPE:
        a = 18
    elif len(pkt) >= 14 and struct.unpack(">H", pkt[12:14])[0] == AVTP_ETHERTYPE:
        a = 14
    else:
        return None
    if len(pkt) < a + 28:
        return None
    subtype = pkt[a] & 0x7f
    if subtype != SUBTYPE_61883:
        return {"subtype": subtype} if subtype < 0x7a else None   # 0x7a+ = control (ADP/AECP/ACMP/MAAP)
    return {
        "subtype": subtype,
        "dst": pkt[0:6], "src": pkt[6:12],
        "tv": bool(pkt[a + 1] & TIMESTAMP_VALID),
        "seq": pkt[a + 2],
        "sid": struct.unpack(">Q", pkt[a + 4:a + 12])[0],
        "ts": struct.unpack(">I", pkt[a + 12:a + 16])[0],
        "dbc": pkt[a + 27],        # CIP quadlet 0 byte 3 (alsa.c: data_block_continuity)
    }


# ---------------------------------------------------------------- driver model (alsa.c)

class DriverStream:
    """avb_rx_track_media_time(), integer for integer."""

    def __init__(self, spp, rate):
        self.spp, self.rate = spp, rate
        self.syt = spp * 4 // 3
        self.pkt_ts = 0
        self.seen = False

    def track(self, p):
        if p["tv"] and p["ts"]:
            idx = (self.syt - p["dbc"] % self.syt) % self.syt
            if idx < self.spp:
                self.pkt_ts = (p["ts"] - (idx * NSEC) // self.rate) & 0xffffffff
                self.seen = True
                return
        if self.seen:
            self.pkt_ts = (self.pkt_ts + (self.spp * NSEC) // self.rate) & 0xffffffff


def align_frames(stream, ref, rate):
    """avb_rx_align_frames(); None while either side has no media time."""
    if stream is ref:
        return 0
    if not ref.seen or not stream.seen:
        return None
    num = s32(stream.pkt_ts - ref.pkt_ts) * rate
    num += -(NSEC // 2) if num < 0 else NSEC // 2
    return div_trunc(num, NSEC)


# ---------------------------------------------------------------- analysis

def timeline_residuals(pkts, spp, rate, model):
    """Residual (ns) of each step between consecutive stamped packets with no sequence gap
    between them: frame-0 time step minus the dbc distance in frames."""
    syt = spp * 4 // 3
    res = []
    prev = None          # (ts0, dbc, seq)
    last_seq = None
    for p in pkts:
        gap = last_seq is not None and p["seq"] != (last_seq + 1) & 0xff
        last_seq = p["seq"]
        if gap:
            prev = None
        if not (p["tv"] and p["ts"]):
            continue
        if model == "syt":
            idx = (syt - p["dbc"] % syt) % syt
            if idx >= spp:
                continue        # the driver ignores such a stamp too
            ts0 = p["ts"] - idx * NSEC / rate
        else:
            ts0 = p["ts"]
        if prev is not None:
            frames = (p["dbc"] - prev[1]) & 0xff
            res.append(s32(int(round(ts0 - prev[0]))) - frames * NSEC / rate)
        prev = (ts0, p["dbc"], p["seq"])
    return res


def analyze(path, rate, ref_sid=None, quiet=False):
    def out(*a):
        if not quiet:
            print(*a)

    spp = rate // 8000 if rate % 8000 == 0 else 6        # 44.1k: 6, as the driver uses
    syt = spp * 4 // 3
    npf = NSEC / rate
    tol = npf / 2

    try:
        frames = list(read_pcap(path))
    except (OSError, ValueError) as e:
        out(f"{YELLOW}INCONCLUSIVE{NC}: {e}")
        return 2

    order = []                     # capture (= wire) order of 61883 packets
    streams = {}
    other_subtypes = set()
    for fr in frames:
        p = parse_avtp(fr)
        if p is None:
            continue
        if p["subtype"] != SUBTYPE_61883:
            other_subtypes.add(p["subtype"])
            continue
        streams.setdefault(p["sid"], []).append(p)
        order.append(p)

    out(f"== {path}: {len(frames)} frames, {len(order)} AVTP 61883 packets, "
        f"{len(streams)} streams  (rate={rate} spp={spp} SYT_INTERVAL={syt})")
    if other_subtypes:
        out(f"{YELLOW}WARN{NC}: non-61883 AVTP stream subtypes seen: "
            f"{sorted(hex(s) for s in other_subtypes)} (the driver parses 61883 only)")
    if not streams:
        out(f"{YELLOW}INCONCLUSIVE{NC}: no AVTP 61883 stream packets -- stack up at "
            f"--num-streams >= 2? MOTU sinks connected? (stream 0 never shows: the driver eats it)")
        return 2

    fails, warns = [], []

    # -- identities
    out("\n== identities")
    out(f"  {'stream_id':16}  {'src_mac':17}  {'dst_mac':17}  {'pkts':>5}  sid==src<<16|k")
    for sid in sorted(streams):
        p0 = streams[sid][0]
        src_sid = int.from_bytes(p0["src"], "big") << 16
        k = sid & 0xffff
        pattern = "yes (k=%d)" % k if sid & ~0xffff == src_sid else "NO"
        out(f"  {sid:016x}  {mac(p0['src'])}  {mac(p0['dst'])}  {len(streams[sid]):>5}  {pattern}")

    # -- per-stream tv pattern + timeline
    out("\n== timestamp model per stream")
    out(f"  {'stream_id':16}  {'stamped':>9}  {'tv!=SYT':>7}  "
        f"{'SYT model max|r|':>17}  {'frame-0 model max|r|':>20}  pairs")
    usable = 0
    syt_ok_all = True
    first_ok_any = False
    for sid in sorted(streams):
        ps = streams[sid]
        stamped = sum(1 for p in ps if p["tv"] and p["ts"])
        mism = sum(1 for p in ps
                   if bool(p["tv"] and p["ts"]) != (((syt - p["dbc"] % syt) % syt) < spp))
        r_syt = timeline_residuals(ps, spp, rate, "syt")
        r_f0 = timeline_residuals(ps, spp, rate, "first")
        m_syt = max((abs(r) for r in r_syt), default=None)
        m_f0 = max((abs(r) for r in r_f0), default=None)
        fmt = lambda m: "-" if m is None else f"{m / 1000:.3f} us"
        out(f"  {sid:016x}  {stamped:>4}/{len(ps):<4}  {mism:>7}  {fmt(m_syt):>17}  "
            f"{fmt(m_f0):>20}  {len(r_syt)}")
        if len(r_syt) < 10:
            continue
        usable += 1
        if m_syt >= tol:
            syt_ok_all = False
        if m_f0 is not None and m_f0 < tol:
            first_ok_any = True
        if mism:
            warns.append(f"{sid:016x}: tv disagrees with the SYT rule on {mism} packets")

    if not usable:
        out(f"\n{YELLOW}INCONCLUSIVE{NC}: fewer than 10 stamped packet pairs on every stream "
            f"-- capture more packets (-c)")
        return 2
    if not syt_ok_all:
        if first_ok_any:
            fails.append("timestamps refer to frame 0 of the packet, not to the SYT event -- "
                         "the driver's dbc correction (avb_rx_track_media_time) is wrong for this peer")
        else:
            fails.append("timestamps fit neither the SYT nor the frame-0 model "
                         f"(tolerance {tol / 1000:.1f} us = half a frame)")

    # -- replay the driver's alignment offsets in wire order
    ref = ref_sid if ref_sid is not None else min(streams)
    if ref not in streams:
        out(f"{YELLOW}INCONCLUSIVE{NC}: --ref {ref:016x} not in the capture")
        return 2
    drv = {sid: DriverStream(spp, rate) for sid in streams}
    hist = {sid: {} for sid in streams}
    for p in order:
        st = drv[p["sid"]]
        st.track(p)
        off = align_frames(st, drv[ref], rate)
        if off is not None:
            h = hist[p["sid"]]
            h[off] = h.get(off, 0) + 1

    out(f"\n== driver align replay (reference {ref:016x}, capture order)")
    out("   offset in frames -> packets; aligned streams read 0 (after the reference's packet)"
        f" or +{spp} (before it)")
    for sid in sorted(streams):
        h = hist[sid]
        desc = "  ".join(f"{o:+d}:{n}" for o, n in sorted(h.items())) or "-"
        out(f"  {sid:016x}  {desc}")
        bad = [o for o in h if abs(o) > ALIGN_MAX_FRAMES]
        if bad:
            fails.append(f"{sid:016x}: {sum(h[o] for o in bad)} packets beyond "
                         f"+-{ALIGN_MAX_FRAMES} frames -> would clamp (rx_drops)")
        elif len(h) > 2 or (h and max(h) - min(h) > spp):
            warns.append(f"{sid:016x}: offsets spread {sorted(h)} -- more than one packet "
                         "of wobble against the reference")

    out("")
    for w in warns:
        out(f"{YELLOW}WARN{NC}: {w}")
    for f in fails:
        out(f"{RED}FAIL{NC}: {f}")
    if fails:
        return 1
    out(f"{GREEN}PASS{NC}: peer follows the SYT timestamp model the driver's alignment assumes; "
        "all replayed offsets within the clamp")
    return 0


# ---------------------------------------------------------------- self-test

def write_synthetic(path, model, rate=48000, nstreams=3, npkts=600, skew_frames=0):
    """A MOTU-like capture: nstreams 61883 streams presented together, wire order
    interleaved and rotating, timestamps crossing the u32 wrap.  model 'syt' stamps the
    SYT event (IEC); 'first' stamps frame 0 of every packet."""
    spp = rate // 8000
    syt = spp * 4 // 3
    src = bytes.fromhex("0001f2aabbcc")
    t0 = (1 << 32) - 40 * 125000 + 1234    # wraps after 40 packets, never exactly 0
    dbc0 = [0x5a, 0x13, 0xc0][:nstreams] + [0] * max(0, nstreams - 3)
    recs = []
    for n in range(npkts):
        for k in [(n + j) % nstreams for j in range(nstreams)]:    # rotate arrival order
            dbc = (dbc0[k] + n * spp) & 0xff
            frame0_ns = t0 + n * 125000 + (skew_frames * NSEC // rate if k == 1 else 0)
            if model == "syt":
                idx = (syt - dbc % syt) % syt
                tv = idx < spp
                ts = (frame0_ns + idx * NSEC // rate) & 0xffffffff if tv else 0
            else:
                tv, ts = True, frame0_ns & 0xffffffff
            sid = (int.from_bytes(src, "big") << 16) | (k + 1)
            avtp = bytes([SUBTYPE_61883, 0x80 | (TIMESTAMP_VALID if tv else 0), n & 0xff, 0]) + \
                struct.pack(">QII", sid, ts, 0) + struct.pack(">HH", 8 + spp * 32, 0) + \
                bytes([0, 8, 0, dbc, 0x90, 0x02, 0xff, 0xff]) + bytes(spp * 32)
            eth = bytes.fromhex("91e0f000ae54") + src + b"\x81\x00\x60\x02" + \
                struct.pack(">H", AVTP_ETHERTYPE) + avtp
            recs.append(eth)
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHiIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
        for r in recs:
            f.write(struct.pack("<IIII", 0, 0, len(r), len(r)) + r)


def selftest():
    cases = [("syt", 0, 0), ("first", 0, 1), ("syt", 3, 0), ("syt", 200, 1)]
    ok = True
    with tempfile.TemporaryDirectory() as d:
        for model, skew, want in cases:
            p = os.path.join(d, f"{model}-{skew}.pcap")
            write_synthetic(p, model, skew_frames=skew)
            got = analyze(p, 48000, quiet=True)
            good = got == want
            ok &= good
            print(f"  {'ok  ' if good else 'FAIL'} model={model:5} skew={skew:3}fr  "
                  f"exit={got} (want {want})")
        # replayed offsets for presented-together streams must be exactly {0, +spp}
        p = os.path.join(d, "offsets.pcap")
        write_synthetic(p, "syt")
        order = [parse_avtp(fr) for fr in read_pcap(p)]
        drv = {}
        for q in order:
            drv.setdefault(q["sid"], DriverStream(6, 48000))
        ref = min(drv)
        seen = set()
        for q in order:
            drv[q["sid"]].track(q)
            o = align_frames(drv[q["sid"]], drv[ref], 48000)
            if o is not None and q["sid"] != ref:
                seen.add(o)
        good = seen == {0, 6}
        ok &= good
        print(f"  {'ok  ' if good else 'FAIL'} aligned offsets {sorted(seen)} (want [0, 6])")
    print("SELFTEST " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    args = [a for a in argv[1:]]
    rate, ref = 48000, None
    path = None
    i = 0
    while i < len(args):
        if args[i] == "--rate":
            rate = int(args[i + 1]); i += 2
        elif args[i] == "--ref":
            ref = int(args[i + 1], 16); i += 2
        elif path is None and not args[i].startswith("-"):
            path = args[i]; i += 1
        else:
            print(__doc__, file=sys.stderr)
            return 2
    if path is None:
        print(__doc__, file=sys.stderr)
        return 2
    return analyze(os.path.expanduser(path), rate, ref)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
