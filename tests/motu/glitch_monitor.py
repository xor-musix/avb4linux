#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
"""Live glitch monitor for the audio_check sine stimulus (used by device_check.sh).

Reads a raw S32_LE interleaved capture from stdin (arecord ... -t raw -) one
second at a time and runs a sine predictor per channel:
    r[n] = x[n] - 2 cos(w) x[n-1] + x[n-2]
which is ~0 for a clean sine and spikes at every discontinuity.  Glitches are
grouped into events per stream (8 channels) and each event is classified:
    copy-L   the glitched frames equal, on all 8 channels, the frames L earlier
             (the 24Ai fault: one packet = copy of 64 frames earlier)
    glitch   anything else (dropout, splice, corruption)
Events that hit every stream at once are on our side, not a device fault: a
TX schedule resync (tx_late_launch, every device misses the same packets), an
RX loss, or a capture overrun.  Nothing is written to disk, so a cycle costs SECS, not an
analysis pass over a multi-GB recording.

--map  "<our-streams>=<host>:<device-stream>,..."  e.g. "0-15=16a:0,16-19=24ai:0"
--judge <host>[,<host>]  hosts whose events make the verdict FAIL (default all)
--src  "<streams>=<source-streams>,..."  streams that carry another stream's
       tones (a device fan-out: "4-7=0-3" = stream 4 carries stream 0's tones).
       Streams sharing a source are twins: an event on all twins at the same
       time is `coincident` (the data was bad before the device split it), an
       event on one twin only is `single` (it went bad on that output).

Exit: 0 PASS (no event on a judged host), 1 FAIL, 2 INCONCLUSIVE (silent
channels, host-side gaps, short input)."""
import argparse
import collections
import fcntl
import os
import sys
import threading
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("--ch", type=int, required=True)
ap.add_argument("--rate", type=int, default=48000)
ap.add_argument("--f0", type=float, required=True, help="ch k tone = f0 + df*k Hz")
ap.add_argument("--df", type=float, required=True)
ap.add_argument("--secs", type=int, required=True, help="expected capture length")
ap.add_argument("--skip", type=float, default=3.0, help="settle window at the start (s)")
ap.add_argument("--map", required=True)
ap.add_argument("--judge", default="")
ap.add_argument("--src", default="")
ap.add_argument("--proc", default="/proc/snd_igb_avb",
                help="poll our TX/RX counters once per second ('' = off)")
a = ap.parse_args()

CPS = 8                                  # channels per stream
RATE, CH = a.rate, a.ch
THR = 1e-4 * 2 ** 31                     # far above quantisation noise of a -6 dBFS sine
HIST = 256                               # frames kept across chunks (>= 2*64 for copy-L)
LAGS = (64, 128, 32, 6, 12, 24, 48, 96)  # copy distances tested, most likely first

def span(txt):
    lo, _, hi = txt.partition("-")
    return range(int(lo), int(hi or lo) + 1)


# stream -> (host, device stream)
smap = {}
for item in a.map.split(","):
    ours, rest = item.split("=")
    host, dev0 = rest.split(":")
    for i, k in enumerate(span(ours)):
        smap[k] = (host, int(dev0) + i)
# stream -> stream whose tones it carries
srcmap = {}
for item in filter(None, a.src.split(",")):
    dst, src = item.split("=")
    d, sr = span(dst), span(src)
    if len(d) != len(sr):
        sys.exit("--src %s: ranges differ in length" % item)
    srcmap.update(zip(d, sr))
streams = sorted(s for s in smap if (s + 1) * CPS <= CH)
hosts = sorted({smap[s][0] for s in streams})
judge = set(a.judge.split(",")) - {""} or set(hosts)
cols = [c for s in streams for c in range(s * CPS, (s + 1) * CPS)]
tone = np.array([srcmap.get(c // CPS, c // CPS) * CPS + c % CPS for c in cols])
c2 = (2 * np.cos(2 * np.pi * (a.f0 + a.df * tone) / RATE)).astype(np.float64)
twins = {}                               # source stream -> streams carrying its tones
for s in streams:
    twins.setdefault(srcmap.get(s, s), []).append(s)
twins = {k: v for k, v in twins.items() if len(v) > 1}

events = {h: [] for h in hosts}          # host -> [(t, stream, kind)]
host_side = []                           # events on (nearly) every stream at once
silent = {}                              # stream -> seconds below -20 dBFS
done_upto = int(a.skip * RATE)           # absolute frame: everything before is judged
carry = np.zeros((0, len(cols)), dtype=np.int32)
g0 = 0                                   # absolute frame of carry[0]
frames_in = 0
lvl = 2 ** 31 * 10 ** (-20 / 20) / np.sqrt(2)   # rms of a -20 dBFS sine


# arecord must never block on the pipe: its ALSA buffer is only ~44 ms at
# 248 ch, the pipe ~1.4 ms, and one chunk's analysis takes ~0.3 s.  A reader
# thread drains stdin into memory while the main thread analyses.
FD = sys.stdin.fileno()
try:
    fcntl.fcntl(FD, 1031, 1 << 20)       # F_SETPIPE_SZ: 1 MiB (best effort)
except OSError:
    pass
_q, _qcv, _eof = collections.deque(), threading.Condition(), [False]


def _reader():
    while True:
        b = os.read(FD, 1 << 20)
        with _qcv:
            if not b:
                _eof[0] = True
            else:
                _q.append(b)
            _qcv.notify()
        if not b:
            return


threading.Thread(target=_reader, daemon=True).start()
_pend = bytearray()


def read_chunk(n):
    want = n * CH * 4
    while len(_pend) < want:
        with _qcv:
            while not _q and not _eof[0]:
                _qcv.wait()
            if not _q:
                break
            while _q:
                _pend.extend(_q.popleft())
    take = min(len(_pend), want) // (CH * 4) * CH * 4
    b = bytes(_pend[:take])
    del _pend[:take]
    return np.frombuffer(b, dtype="<i4").reshape(-1, CH)


def classify(y, s_idx, i0, i1):
    """copy-L if frames [i0,i1] of stream s_idx equal the frames L earlier on all 8 ch"""
    blk = slice(s_idx * CPS, (s_idx + 1) * CPS)
    lo, hi = max(i0 - 8, 0), min(i1 + 8, len(y))
    for L in LAGS:
        if lo - L < 0:
            continue
        same = np.all(y[lo:hi, blk] == y[lo - L:hi - L, blk], axis=1)
        if same.sum() >= 3:
            idx = np.flatnonzero(same)
            return "copy-%d" % L, int(same.sum()), lo + idx[0]
    return "glitch", 0, i0


def report(t, s, kind, n, nch):
    host, dev = smap[s]
    what = ("%d frames x 8 ch = the frames %s earlier" % (n, kind[5:])) if kind.startswith("copy") \
        else "%d of 8 ch, no exact repeat" % nch
    print("  t=%8.3fs  %-6s stream %2d (%s Output Stream %d)  %-8s %s"
          % (t, host, s, host, dev + 1, kind, what), flush=True)


# Our side, polled once per second: an event within +-2 s of one of these
# counters moving may be ours, not the device's.
WATCH = (("tx_engine", "tx_late_launch"), ("tx_engine", "tx_xrun"),
         ("rx_engine", "rx_seq_errors"), ("rx_engine", "rx_clock_slips"),
         ("rx_engine", "rx_budget_hits"), ("rx_engine", "rx_ring_near_lap"))


def counters():
    out = {}
    if not a.proc:
        return out
    for f, k in WATCH:
        try:
            for line in open("%s/%s" % (a.proc, f)):
                if line.startswith(k + "="):
                    out[k] = int(line.split("=", 1)[1])
        except (OSError, ValueError):
            pass
    return out


ctr0 = ctr_prev = counters()
ctr_moves = []                           # (t, counter, delta)

sec = 0
while True:
    x = read_chunk(RATE)
    if len(x) == 0:
        break
    frames_in += len(x)
    xs = x[:, cols]
    # silence check per stream, once per full second after the settle window
    if len(x) == RATE and (g0 + len(carry)) >= a.skip * RATE:
        rms = np.sqrt(np.mean(xs.astype(np.float64) ** 2, axis=0))
        for si, s in enumerate(streams):
            if rms[si * CPS:(si + 1) * CPS].min() < lvl:
                silent[s] = silent.get(s, 0) + 1
    y = np.concatenate([carry, xs])
    yf = y.astype(np.float64)
    r = yf[2:] - c2 * yf[1:-1] + yf[:-2]                 # r[i-2] belongs to frame i
    bad = np.abs(r) > THR
    lim = g0 + len(y) - 64                               # finalise up to here; the rest is re-seen
    fr = np.arange(2, len(y)) + g0
    ok = (fr >= done_upto) & (fr < lim)
    bad &= ok[:, None]
    hit = []                                             # (stream idx, first, last) local frames
    for si in range(len(streams)):
        rows = np.flatnonzero(bad[:, si * CPS:(si + 1) * CPS].any(axis=1)) + 2
        if not len(rows):
            continue
        start = prev = rows[0]
        for i in list(rows[1:]) + [None]:
            if i is not None and i - prev <= 64:
                prev = i
                continue
            nch = int(bad[start - 2:prev - 1, si * CPS:(si + 1) * CPS].any(axis=0).sum())
            hit.append((si, start, prev, nch))
            if i is not None:
                start = prev = i
    # an event on (nearly) every stream within the same 64 frames is the host, not a device
    by_time = {}
    for h in hit:
        by_time.setdefault((g0 + h[1]) // 64, []).append(h)
    for slot, hs in sorted(by_time.items()):
        if len(streams) >= 4 and len(hs) >= 0.75 * len(streams):
            host_side.append((g0 + hs[0][1]) / RATE)
            print("  t=%8.3fs  ALL    %d/%d streams at once -- our side (TX resync / RX / capture), not a device"
                  % ((g0 + hs[0][1]) / RATE, len(hs), len(streams)), flush=True)
            continue
        for si, i0, i1, nch in hs:
            s = streams[si]
            kind, n, at = classify(y, si, i0, i1)
            t = (g0 + at) / RATE
            events[smap[s][0]].append((t, s, kind))
            report(t, s, kind, n, nch)
    ctr = counters()
    for k in ctr:
        if k in ctr_prev and ctr[k] != ctr_prev[k]:
            t = (g0 + len(y)) / RATE
            ctr_moves.append((t, k, ctr[k] - ctr_prev[k]))
            print("  t~%7.1fs  OUR    %s %+d" % (t, k, ctr[k] - ctr_prev[k]), flush=True)
    ctr_prev = ctr or ctr_prev
    done_upto = max(done_upto, lim)
    carry = y[-HIST:]
    g0 += len(y) - len(carry)
    sec += 1
    if sec % 30 == 0:
        print("  .. %4ds  events: %s" % (sec, "  ".join("%s=%d" % (h, len(events[h])) for h in hosts)),
              flush=True)

# ---- summary ----
secs_in = frames_in / RATE
judged = max(secs_in - a.skip, 1e-9)
print("== %.0fs captured, %.0fs judged, %d channels of %d streams" % (secs_in, judged, len(cols), len(streams)))
fail = False
for h in hosts:
    ev = events[h]
    sts = sorted({e[1] for e in ev})
    kinds = {}
    for e in ev:
        kinds[e[2]] = kinds.get(e[2], 0) + 1
    tag = "judged " if h in judge else "control"
    if ev:
        verdict = "EVENTS"
        fail |= h in judge
    else:
        verdict = "clean"
    print("  %-6s %s %-6s %3d event(s)  %5.2f/min  streams %s  %s"
          % (h, tag, verdict, len(ev), len(ev) * 60 / judged,
             ",".join(map(str, sts)) or "-",
             " ".join("%s=%d" % kv for kv in sorted(kinds.items()))))
if a.proc:
    moved = {k: ctr_prev[k] - ctr0[k] for k in ctr_prev if k in ctr0 and ctr_prev[k] != ctr0[k]}
    print("  our counters during the run: %s" % (", ".join("%s %+d" % kv for kv in sorted(moved.items())) or "unchanged"))
    for h in hosts:
        for t, s_, kind in events[h]:
            near = [m for m in ctr_moves if abs(m[0] - t) <= 2.0]
            if near:
                print("  t=%8.3fs  %s stream %d: within 2 s of %s -- may be ours, not the device"
                      % (t, h, s_, ", ".join("%s %+d" % (m[1], m[2]) for m in near)))
if twins:
    # the copies of one source can reach us up to a packet or two apart
    win = 96 / RATE
    allev = [e for h in hosts for e in events[h]]
    seen, coinc, single = set(), 0, 0
    print("== fan-out: %s" % "  ".join("src %d -> %s" % (k, ",".join(map(str, v)))
                                       for k, v in sorted(twins.items())))
    for t, s, kind in sorted(allev):
        src = srcmap.get(s, s)
        if src not in twins or (t, s) in seen:
            continue
        group = [(t2, s2, k2) for t2, s2, k2 in allev
                 if srcmap.get(s2, s2) == src and abs(t2 - t) <= win]
        hit = sorted({g[1] for g in group})
        for g in group:
            seen.add((g[0], g[1]))
        clean = [x for x in twins[src] if x not in hit]
        if clean:
            single += 1
            verdict = "SINGLE      bad on %s only, twin %s clean -> output side" % (
                ",".join(map(str, hit)), ",".join(map(str, clean)))
        else:
            coinc += 1
            verdict = "COINCIDENT  bad on all twins %s -> input side / router" % ",".join(map(str, hit))
        print("  t=%8.3fs  src %d  %s  (%s)" % (t, src, verdict, kind))
    print("  fan-out events: %d coincident, %d single-copy" % (coinc, single))
if silent:
    print("  silent (< -20 dBFS) seconds per stream: %s" % ", ".join("%d:%d" % kv for kv in sorted(silent.items())))
if host_side:
    print("  gaps on all streams (our side): %d" % len(host_side))
    for t in host_side:
        near = [m for m in ctr_moves if abs(m[0] - t) <= 2.0]
        print("  t=%8.3fs  all streams: %s" % (t, ", ".join("%s %+d" % (m[1], m[2]) for m in near)
              or "no counter moved -- capture side (arecord overrun?)"))

if secs_in < a.secs - 1:
    print("== INCONCLUSIVE: capture ended after %.1fs of %ds" % (secs_in, a.secs))
    sys.exit(2)
if silent or host_side:
    print("== INCONCLUSIVE: %s" % ("silent streams (routing/connection?)" if silent else "gaps on all streams (our side)"))
    sys.exit(2)
if fail:
    print("== FAIL: events on %s" % ", ".join(h for h in hosts if h in judge and events[h]))
    sys.exit(1)
print("== PASS: no events on %s" % ", ".join(sorted(judge)))
sys.exit(0)
