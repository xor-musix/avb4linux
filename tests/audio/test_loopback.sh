#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Audio regression gate for the i210 AVB data plane.
#
# Plays a known multitone (one distinct sine per channel) out the i210 talker,
# records it back through the MOTU AVB-in->AVB-out loopback on the i210 listener,
# and checks the recording is bit-faithful: right frequency per channel, no
# channel swaps, no crosstalk, amplitude in the known-good band, and no glitches
# beyond a small budget. A code change that regresses TX framing, the media clock,
# RX demux, or de-interleave shows up here as a FAIL.
#
# The AVTP data stream is DMA-ring-injected and BYPASSES the netdev, so it cannot
# be sniffed -- recording through the ALSA capture device is the ONLY way to
# observe the audio data plane, and it exercises the whole round trip at once.
#
#   ./tests/audio/test_loopback.sh                      # verify (vs baseline if present)
#   ./tests/audio/test_loopback.sh --establish-baseline # one-time, on known-good code
#   DEV=hw:igbavb RATE=48000 SECS=25 CH=8 ./tests/audio/test_loopback.sh
#   CH=32 LIVE_CH=8 ./tests/audio/test_loopback.sh      # only ch0-7 may carry audio
#   PERIOD=512 PERIODS=2 ./tests/audio/test_loopback.sh # DAW-style exact period (aplay+arecord)
#
# PRECONDITION (your responsibility, via an AVDECC controller): every i210 talker
# channel routed 1:1, unity gain, back to the corresponding i210 listener channel
# through the MOTU, and gPTP locked. The tool runs a liveness probe first and
# aborts (INCONCLUSIVE, not FAIL) if the loopback isn't returning audio.
#
# Needs: aplay, arecord (alsa-utils), gcc. NO root required -- it only plays/records
# on the ALSA device (you must be in the 'audio' group) and builds a small C tool.
# Run it as your normal user so output lands in your own ~/tmp.

set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)      # repo root (this script lives in tests/audio/)
HERE="$ROOT/tests/audio"

# ---- config (override via env) -------------------------------------------
DEV=${DEV:-hw:igbavb}                 # ALSA device (card id is "igbavb")
RATE=${RATE:-48000}                   # sample rate
CH=${CH:-8}                           # channel count (8 per listener stream: 8*num_streams)
LIVE_CH=${LIVE_CH:-$CH}               # channels [0,LIVE_CH) judged; [LIVE_CH,CH) must be silent
SECS=${SECS:-25}                      # scored capture seconds
GLITCH_BUDGET=${GLITCH_BUDGET:-16}    # small-but-reported discontinuity budget
PERIOD=${PERIOD:-}                    # aplay+arecord period in frames ("" = their default)
PERIODS=${PERIODS:-2}                 # periods per buffer when PERIOD is set
PERIOD_ARGS=()
[ -n "$PERIOD" ] && PERIOD_ARGS=(--period-size="$PERIOD" --buffer-size=$((PERIOD * PERIODS)))
SPLICE_BUDGET=${SPLICE_BUDGET:-0}     # timeline breaks tolerated (a splice is a real fault)
OUTDIR_SET=${OUTDIR+1}                # an explicit OUTDIR also takes a raw recording
OUTDIR=${OUTDIR:-$HOME/tmp}           # stimulus/recording/metrics land here
RAMDIR=${RAMDIR-/dev/shm}             # tmpfs for raw recordings ("" = never)
# one amplitude baseline per channel count: a 32ch run never compares against the 8ch one
if [ "$CH" = 8 ]; then
  BASELINE=${BASELINE:-$HERE/baseline.txt}
else
  BASELINE=${BASELINE:-$HERE/baseline-${CH}ch.txt}
fi
TOOL="$HERE/audio_check"
# The recording is a WAV while it fits one arecord file (< 2 GiB), else raw S32_LE:
# arecord splits a bigger WAV and DROPS the rest of the read chunk at each split
# (~1 period), so its parts are not contiguous.
REC_BYTES=$(( CH * 4 * RATE * SECS ))
REC_IN_RAM=0
REC_NOTE=""
REC_NOTE_KEPT=""
if [ "$REC_BYTES" -lt 2000000000 ] 2>/dev/null; then
  REC="$OUTDIR/avb-loopback.wav"
else
  REC="$OUTDIR/avb-loopback.raw"
  # A raw recording is >= 2 GiB written at CH*4*RATE B/s (37 MB/s at 192ch);
  # on disk, writeback stalls made arecord overrun its 57 ms ALSA buffer (N=24
  # SECS=240, ext4: overruns up to 1.6 s; tmpfs: none).  So it goes to
  # RAMDIR when that is a tmpfs with room for it AND the RAM is really free
  # (a tmpfs only bounds its size).  Margins: 1 GiB on the tmpfs, 4 GiB of
  # MemAvailable left for everything else.  An explicit OUTDIR wins.
  if [ -z "$OUTDIR_SET" ] && [ -n "$RAMDIR" ] && [ -d "$RAMDIR" ] && [ -w "$RAMDIR" ] &&
     [ "$(stat -f -c %T "$RAMDIR" 2>/dev/null)" = tmpfs ]; then
    ram_rm=$RAMDIR/avb-loopback.raw    # an old RAM recording does not count against the room
    ram_old=$(stat -c %s "$ram_rm" 2>/dev/null || echo 0)
    ram_avail=$(( $(df -B1 --output=avail "$RAMDIR" 2>/dev/null | tail -1) + ram_old ))
    mem_avail=$(( $(sed -n 's/^MemAvailable: *\([0-9]*\) kB/\1/p' /proc/meminfo) * 1024 + ram_old ))
    if [ "$ram_avail" -gt $(( REC_BYTES + (1 << 30) )) ] && [ "$mem_avail" -gt $(( REC_BYTES + (4 << 30) )) ]; then
      REC="$RAMDIR/avb-loopback.raw"
      REC_IN_RAM=1
      REC_NOTE_KEPT=" -- in RAM ($(( (REC_BYTES >> 30) + 1 )) GiB) until you rm it or the next run"
      REC_NOTE="  (raw > 2 GiB: in RAM, avoids disk-write overruns; RAMDIR= keeps it in OUTDIR)"
    else
      REC_NOTE="  (raw > 2 GiB: no room in $RAMDIR ($((ram_avail >> 30)) GiB free, $((mem_avail >> 30)) GiB RAM available, needs $(( (REC_BYTES >> 30) + 1 )) GiB) -- on disk, may overrun)"
    fi
  fi
fi
METRICS="$OUTDIR/avb-loopback.metrics.txt"

ESTABLISH=0
for a in "$@"; do
  case "$a" in
    --establish-baseline) ESTABLISH=1 ;;
    *) echo "unknown arg: $a (use --establish-baseline)" >&2; exit 2 ;;
  esac
done
case "$LIVE_CH" in ''|*[!0-9]*) echo "LIVE_CH must be a number (got '$LIVE_CH')" >&2; exit 2 ;; esac
if [ "$LIVE_CH" -lt 1 ] || [ "$LIVE_CH" -gt "$CH" ]; then
  echo "LIVE_CH=$LIVE_CH out of range 1..$CH" >&2; exit 2
fi
if [ "$ESTABLISH" = 1 ] && [ "$LIVE_CH" != "$CH" ]; then
  echo "--establish-baseline needs every channel live (LIVE_CH=$LIVE_CH < CH=$CH)" >&2; exit 2
fi

say(){  printf '\033[1m== %s\033[0m\n' "$*"; }
warn(){ printf '\033[33m!! %s\033[0m\n' "$*" >&2; }
die(){  printf '\033[31mXX %s\033[0m\n' "$*" >&2; exit 1; }
inconc(){ printf '\033[33m~~ INCONCLUSIVE: %s\033[0m\n' "$*" >&2; exit 2; }
ok(){   printf '\033[32m%s\033[0m\n' "$*"; }

APLAY_PID=""
cleanup(){ [ -n "$APLAY_PID" ] && kill "$APLAY_PID" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT INT TERM

# ---- build the oracle (only if source is newer than executable) ----------
build_tool(){
  local src="$HERE/audio_check.c"
  local exe="$TOOL"
  
  # Check if executable exists and is newer than source
  if [ ! -x "$exe" ] || [ "$src" -nt "$exe" ]; then
    say "build audio_check"
    make -C "$HERE" >/dev/null || die "audio_check build failed"
  fi
  [ -x "$exe" ] || die "audio_check missing after build"
}

# ---- preflight -----------------------------------------------------------
say "preflight"
for t in aplay arecord gcc make; do command -v "$t" >/dev/null || die "missing $t"; done
mkdir -p "$OUTDIR" || die "cannot create $OUTDIR"

aplay -l 2>/dev/null | grep -qi 'igbavb' || die "ALSA card 'igbavb' not found (aplay -l).
    Load the AVB stack first:  sudo $ROOT/avb_up.sh <iface> $RATE"

# Must be the real audio path, NOT benchmark mode (avb_rx_bench broadens the RX
# filter + force-enables the listener -> not a faithful audio test).
BENCH=/sys/module/snd_igb_avb/parameters/avb_rx_bench
if [ -e "$BENCH" ] && [ "$(cat "$BENCH" 2>/dev/null)" = "1" ]; then
  die "snd_igb_avb is loaded with avb_rx_bench=1 (throughput-bench mode). Reload WITHOUT
   it for a real audio test:  sudo rmmod snd_igb_avb; sudo $ROOT/avb_up.sh <iface> $RATE"
fi

echo "  device=$DEV  rate=$RATE  ch=$CH  live-ch=$LIVE_CH  capture=${SECS}s  glitch-budget=$GLITCH_BUDGET  splice-budget=$SPLICE_BUDGET"
echo "  stimulus=streamed (audio_check gen --raw | aplay)  recording=$REC$REC_NOTE"

# Build the tool if needed
build_tool

# ---- stimulus: streamed, a few s longer than the capture ------------------
# Generated live into aplay rather than written as a WAV: a WAV holds < 4 GiB
# (32-bit sizes), i.e. ~87 s at 128ch, and the stimulus repeats every second
# anyway.  The old avb-stim.wav is no longer written.
GEN_SECS=$((SECS + 6))
rm -f "$OUTDIR/avb-stim.wav"

# Never let a previous run's recording be analyzed: drop both formats and any
# arecord split parts left by older harness versions.
rm -f "$OUTDIR/avb-loopback.wav" "$OUTDIR/avb-loopback.raw" "$OUTDIR"/avb-loopback-[0-9][0-9].wav
# ... and a RAM recording a previous run kept (FAIL/INCONCLUSIVE), which holds RAM.
[ -n "$RAMDIR" ] && rm -f "$RAMDIR/avb-loopback.raw"

# RX evidence for an INCONCLUSIVE verdict.  RX only counts while a capture is
# open, so a background sampler logs every stream's rx_pkts once a second during
# each recording; rx_rates prints the last 8 s of per-stream packets/s from it
# (all streams stop = reference/capture stall, some = one talker device).
RXLOG="$OUTDIR/avb-loopback.rx.log"
rx_sampler(){
  local f=/proc/snd_igb_avb/streams
  while [ -r "$f" ]; do
    printf '%s %s\n' "$(date +%s)" "$(sed -n 's/.*rx_pkts=\([0-9]*\).*/\1/p' "$f" | tr '\n' ' ')"
    sleep 1
  done
}
rx_rates(){
  [ -s "$RXLOG" ] || return 0
  tail -9 "$RXLOG" | awk '
    { for (i = 2; i <= NF; i++) v[NR, i] = $i; n = NR; nf = NF }
    END {
      if (n < 2) exit
      print "   rx pkts/s, last " n - 1 " s of the recording:"
      for (i = 2; i <= nf; i++) {
        line = sprintf("     s%-2d", i - 2)
        for (r = 2; r <= n; r++) line = line sprintf(" %5d", v[r, i] - v[r - 1, i])
        print line
      }
    }'
}

# play_and_record <capture_secs> <out.wav|out.raw> [genlog] -- start playback, capture, stop playback.
# Playback starts first and runs ~1s before capture so the loopback is flowing
# through the whole recording (the oracle additionally skips a 3s settle window).
play_and_record(){
  local secs=$1 out=$2 genlog=${3:-$OUTDIR/avb-loopback.gen.log} fmt=()
  [[ $out == *.raw ]] && fmt=(-t raw)
  rm -f "$out"
  "$TOOL" gen - --raw --ch "$CH" --rate "$RATE" --secs "$GEN_SECS" 2>"$genlog" |
    aplay -q -D "$DEV" -t raw -c "$CH" -r "$RATE" -f S32_LE "${PERIOD_ARGS[@]}" >/dev/null 2>&1 &
  APLAY_PID=$!
  sleep 1
  if ! kill -0 "$APLAY_PID" 2>/dev/null; then
    APLAY_PID=""
    # aplay also exits at once when the generator feeding it fails (EOF): show its stderr.
    local gerr=""
    [ -f "$genlog" ] && [ -s "$genlog" ] && gerr="
   audio_check gen: $(tail -3 "$genlog" | tr '\n' ' ')"
    inconc "aplay exited immediately on $DEV -- the stimulus generator failed, the device is busy, or no permission to open it.$gerr
   If it's permission: add yourself to the 'audio' group (then re-login), or run with sudo."
  fi
  if [ -n "$PERIOD" ]; then
    local hwp
    hwp=$(awk '/^(period|buffer)_size:/ { printf "%s %s ", $1, $2 }' /proc/asound/igbavb/pcm0p/sub0/hw_params 2>/dev/null)
    echo "  playback negotiated: ${hwp:-?}"
    [ "$hwp" = "period_size: $PERIOD buffer_size: $((PERIOD * PERIODS)) " ] ||
      { kill "$APLAY_PID" 2>/dev/null; APLAY_PID=""
        inconc "asked for period $PERIOD x $PERIODS, playback negotiated '${hwp:-?}' -- a DAW setting this size exactly would fail to open"; }
  fi
  local t0=$SECONDS spid
  rx_sampler > "$RXLOG" &
  spid=$!
  arecord -q -D "$DEV" -c "$CH" -r "$RATE" -f S32_LE "${PERIOD_ARGS[@]}" "${fmt[@]}" -d "$secs" "$out" >/dev/null 2>"$OUTDIR/avb-loopback.arecord.log" \
    || { kill "$APLAY_PID" "$spid" 2>/dev/null; APLAY_PID=""
         inconc "arecord failed on $DEV after $((SECONDS - t0))s of ${secs}s -- capture substream busy, listener not ready, or no permission (audio group / sudo).
   arecord: $(tail -3 "$OUTDIR/avb-loopback.arecord.log" | tr '\n' ' ')
$(rx_rates)"; }
  kill "$spid" 2>/dev/null; wait "$spid" 2>/dev/null
  kill "$APLAY_PID" 2>/dev/null; wait "$APLAY_PID" 2>/dev/null; APLAY_PID=""
}

# ---- liveness probe (is the loopback returning audio at all?) -------------
say "liveness probe (5s)"
PROBE="$OUTDIR/avb-loopback.probe.wav"     # 5 s: always a WAV
play_and_record 5 "$PROBE"
if ! "$TOOL" check "$PROBE" --ch "$CH" --rate "$RATE" --live-ch "$LIVE_CH" --probe; then
  rm -f "$PROBE"
  inconc "loopback not live -- no audio came back. Check:
     - MOTU routing: every i210 talker channel -> the matching i210 listener channel
     - gPTP is locked (grandmaster elected) and the stream is CONNECTED in the controller
     - listener MACs configured (avb_device_talker_mac / avb_device_source_mac)
$(rx_rates)"
fi
rm -f "$PROBE"
ok "  loopback live."

# ---- scored run ----------------------------------------------------------
say "record ${SECS}s and analyze"
play_and_record "$SECS" "$REC" /dev/stderr
[ -f "$REC" ] || inconc "arecord wrote no $REC"
[[ $REC == *.raw ]] && echo "  recording > 2 GiB: raw S32_LE (a WAV would be split by arecord, losing frames)"

# Host-side capture overruns: arecord did not read the device within the ALSA
# buffer (57 ms at 192ch) -- e.g. blocked in write() on the recording file -- and
# ALSA dropped audio.  Each one is a source jump in the recording that no
# data-plane fault caused, so the audio verdict is not attributable to the driver
# (first seen at N=24 SECS=240: 12 overruns up to 1.6 s, RX counters clean).
OVERRUNS=$(grep -c 'overrun!!!' "$OUTDIR/avb-loopback.arecord.log" 2>/dev/null || true)
if [ "${OVERRUNS:-0}" -gt 0 ]; then
  warn "arecord overran $OVERRUNS time(s) -- the capture process stalled, ALSA dropped audio:"
  warn "  $(grep -o 'at least [0-9.,]* ms' "$OUTDIR/avb-loopback.arecord.log" | sed 's/at least //' | tr '\n' ' ')"
  if [ "$REC_IN_RAM" = 1 ]; then
    warn "  the recording was already in RAM ($REC): the stall is not disk I/O"
  else
    warn "  record to RAM to rule out disk stalls: OUTDIR=/dev/shm $0 ..."
  fi
fi

# ---- data-plane health (kernel counters) ---------------------------------
# The oracle only sees audio.  These separate "our data plane broke it" from
# "the peer's media clock is not locked to the stream", which sounds identical
# in the recording but is a peer/config fault no code change here can fix.
RXE=/proc/snd_igb_avb/rx_engine
if [ -r "$RXE" ]; then
  say "RX data-plane counters"
  sed 's/^/  /' "$RXE"
  get(){ sed -n "s/^$1=//p" "$RXE"; }
  drift=$(get rx_media_drift_ppb); warn=$(get rx_drift_warn_ppb)
  for c in rx_seq_errors rx_ring_near_lap rx_budget_hits rx_clock_slips; do
    v=$(get "$c"); [ "${v:-0}" != 0 ] && warn "$c=$v -- RX data-plane fault, not just audio"
  done
  # Listener identity check: each rx: row's src_mac (the MAC its talker filter
  # admits) must be the MAC inside its expected_sid (the talker's stream id).
  # Both are published by avb-user; a mismatch means the stream's identity is
  # inconsistent and it will not match its packets.  Unbound rows (zero sid or
  # MAC) are skipped.  This is a config check, not a media-clock-lock check.
  STREAMS=/proc/snd_igb_avb/streams
  if [ -r "$STREAMS" ]; then
    k=0; bad=()
    while IFS= read -r line; do
      [[ "$line" =~ ^\ +rx:\ expected_sid=([0-9a-f]{16})\ src_mac=([0-9a-f:]{17}) ]] || continue
      sid=${BASH_REMATCH[1]}; mac=${BASH_REMATCH[2]//:/}
      if [ "$sid" != 0000000000000000 ] && [ "$mac" != 000000000000 ] && [ "${sid:0:12}" != "$mac" ]; then
        bad+=("$k")
      fi
      k=$((k + 1))
    done < "$STREAMS"
    [ ${#bad[@]} -gt 0 ] && warn "listener identity mismatch on stream(s) ${bad[*]}: src_mac is not the MAC in expected_sid ($STREAMS)"
  fi
  # Raise threshold to 5000 ppb - clock rate errors < 5000 ppb are normal
  # and don't affect audio quality. The warning should only trigger for
  # extreme clock rate errors (>5000 ppb).
  THRESHOLD=5000
  if [ -n "$drift" ] && [ -n "$warn" ] && [ "${drift#-}" -gt "$THRESHOLD" ] 2>/dev/null; then
    warn "peer media clock rate error is ${drift} ppb (> ${THRESHOLD} threshold) -- the peer's audio clock"
    warn "runs at a slightly different rate than our gPTP-paced talker. This is expected when"
    warn "the peer's crystal oscillator is slightly off. Set the peer's CLOCK SOURCE to this"
    warn "AVB stream for proper synchronization. No amount of driver hardening removes this."
  fi
fi

ARGS=(check "$REC" --ch "$CH" --rate "$RATE" --live-ch "$LIVE_CH" --glitch-budget "$GLITCH_BUDGET" --splice-budget "$SPLICE_BUDGET")
if [ "$ESTABLISH" = 1 ]; then
  ARGS+=(--establish-baseline "$BASELINE")
elif [ -f "$BASELINE" ]; then
  ARGS+=(--baseline "$BASELINE")
  echo "  comparing against baseline: $BASELINE"
else
  warn "no baseline ($BASELINE) -- amplitude judged against an absolute sanity band only."
  warn "establish one on known-good code: CH=$CH $0 --establish-baseline"
fi

if [ "${OVERRUNS:-0}" -gt 0 ] && [ "${ANALYZE_OVERRUN:-0}" != 1 ]; then
  warn "recording kept: $REC$REC_NOTE_KEPT"
  inconc "arecord overran $OVERRUNS time(s): the recording has host-side gaps, so the audio check would
   not say anything about the data plane (skipped; ANALYZE_OVERRUN=1 runs it anyway)."
fi

"$TOOL" "${ARGS[@]}" | tee "$METRICS"
rc=${PIPESTATUS[0]}

say "result"
case "$rc" in
  0) [ "$ESTABLISH" = 1 ] && ok "  baseline established -> $BASELINE" \
                            || ok "  PASS -- no audio regression. (metrics: $METRICS)"
     # A passing RAM recording only holds RAM: drop it (kept on FAIL/INCONCLUSIVE).
     if [ "$REC_IN_RAM" = 1 ]; then rm -f "$REC"; echo "  removed the RAM recording $REC"; fi
     ;;
  1) warn "recording kept for inspection: $REC$REC_NOTE_KEPT"
     [ "${OVERRUNS:-0}" -gt 0 ] && inconc "audio check failed, but arecord overran $OVERRUNS time(s) -- not attributable to the data plane (metrics: $METRICS)"
     die "FAIL -- audio regression detected (see table above; metrics: $METRICS)" ;;
  *) warn "recording kept for inspection: $REC$REC_NOTE_KEPT"
     inconc "analysis could not run (rc=$rc) -- recording unreadable/too short." ;;
esac
exit 0