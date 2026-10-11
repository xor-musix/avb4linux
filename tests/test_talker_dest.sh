#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
# Verification: the resolved talker base must drive the kernel TX header,
# the ACMP/MSRP advertisement, AND the /sys param -- byte-identical, across all
# four scenarios.  Runs each scenario end-to-end, forces a playback so the
# kernel stamps + transmits, and cross-checks every plane.
#
# Planes (stream 0 of the per-stream avb_talker_dest_macs):
#   /sys param       first entry of avb_talker_dest_macs (avb-user writes it)
#   user published   avb-user 'talker dest block base=0x... count=N (MAAP|fixed)'
#   user advertised  avb-user 'stream[0] ... dest=...'
#   kernel TX header /proc/snd_igb_avb/streams first 'dest_mac=' (stamped per packet)
# The kernel must also not log 'TX dest FALLBACK' (published list unusable).
#
#   sudo ./tests/test_talker_dest.sh <iface> [samplerate]
#
# Needs: tcpdump, aplay (alsa-utils).  Re-execs under sudo automatically.
# OWNS THE STACK: every scenario runs avb_down.sh + avb_up.sh, and the stack is
# left DOWN on exit -- restart it with avb_up.sh afterwards.
# Exit: 0 PASS, 1 FAIL, 2 INCONCLUSIVE (avb_up.sh did not come up).
# Run from anywhere: avb_up.sh / avb_down.sh are resolved relative to the repo
# root (this script's parent dir), not the current working directory.

set -u
[ "$(id -u)" -eq 0 ] || exec sudo "$0" "$@"

IFACE=${1:?usage: $0 <iface> [samplerate]}
RATE=${2:-48000}
ROOT=$(cd "$(dirname "$0")/.." && pwd)   # repo root (this script lives in tests/)
SYSP=/sys/module/snd_igb_avb/parameters/avb_talker_dest_macs
PROCS=/proc/snd_igb_avb/streams
FIXED=91e0f000fe00                 # MAAP_LOCALLY_ADMINISTRATORED_BASE
TMP=$(mktemp -d)
FAIL=0

for t in tcpdump tshark aplay; do command -v "$t" >/dev/null || { echo "missing $t"; exit 1; }; done

norm() { echo "${1:-}" | tr -d ':' | tr 'A-F' 'a-f'; }   # MAC -> bare lowercase hex
cleanup() { "$ROOT/avb_down.sh" "$IFACE" >/dev/null 2>&1; rm -rf "$TMP"; }
trap cleanup EXIT

# Run one scenario.  $1 name, $2 AVB_MAAP value ("-" = unset; MAAP is on by
# default), $3 "kill"|"keep" (kill the daemon to force fallback),
# $4 expected user-tag, $5 expected base ("fixed"|"dyn").
scenario() {
    local name=$1 maap=$2 kill=$3 want_tag=$4 want_base=$5
    echo "=================================================================="
    echo "SCENARIO: $name"
    "$ROOT/avb_down.sh" "$IFACE" >/dev/null 2>&1
    local since; since=$(date '+%Y-%m-%d %H:%M:%S')

    local env=""; [ "$maap" != - ] && env="AVB_MAAP=$maap"
    env $env "$ROOT/avb_up.sh" "$IFACE" --samplerate "$RATE" >"$TMP/up.log" 2>&1 &
    local up=$!

    # Force fallback: keep maap_daemon dead through avb-user's acquire window.
    if [ "$kill" = kill ]; then
        ( for _ in $(seq 1 60); do killall -q -9 maap_daemon 2>/dev/null; sleep 0.5; done ) &
    fi

    # Wait (<=45s) for avb-user to resolve + publish the dest.
    local i
    for i in $(seq 1 45); do
        journalctl -t avb-user --since "$since" 2>/dev/null | grep -q 'talker dest' && break
        # avb_up.sh runs avb-user in the foreground: if it is gone, the stack
        # never came up and every plane below would read <none> -- a setup
        # failure, not a talker-dest verdict.
        if ! kill -0 "$up" 2>/dev/null; then
            echo "  INCONCLUSIVE: avb_up.sh exited before avb-user resolved the dest:"
            tail -n 15 "$TMP/up.log" | sed 's/^/    | /'
            exit 2
        fi
        sleep 1
    done

    # Capture control-plane frames (ACMP + MSRP carry the advertised dest) and
    # force a playback prepare so the kernel stamps the header.  Window covers
    # steady state (avb-user sends ACMP connect / starts MSRP ~6s after resolve).
    # The AVTP *data* stream is DMA-ring-injected and bypasses the netdev, so it
    # is invisible to local capture -- it needs an external mirror under audio.
    tcpdump -i "$IFACE" -w "$TMP/cap.pcap" -n -s0 >/dev/null 2>&1 &
    local td=$!
    timeout 6 aplay -D hw:CARD=igbavb -c 8 -f S32_LE -r "$RATE" -t raw /dev/zero >/dev/null 2>&1
    sleep 8; kill "$td" 2>/dev/null; wait "$td" 2>/dev/null

    # Harvest the host-observable planes.
    local sys user user_line user_tag user_count adv kern kfall acmp_da msrp_da
    sys=$(norm "$(cut -d, -f1 "$SYSP" 2>/dev/null)")
    user_line=$(journalctl -t avb-user --since "$since" 2>/dev/null \
            | grep -oE 'talker dest block base=0x[0-9a-f]+ count=[0-9]+ \((MAAP|fixed)\)' | tail -1)
    user=$(echo "$user_line" | grep -oE 'base=0x[0-9a-f]+' | cut -dx -f2)
    user_count=$(echo "$user_line" | grep -oE 'count=[0-9]+' | cut -d= -f2)
    user_tag=$(echo "$user_line" | grep -oE 'MAAP|fixed')
    adv=$(norm "$(journalctl -t avb-user --since "$since" 2>/dev/null \
            | grep -oE 'stream\[0\].* dest=[0-9a-f]+' | tail -1 | grep -oE 'dest=[0-9a-f]+' | cut -d= -f2)")
    kern=$(norm "$(grep -m1 -oE '^stream_id=[0-9a-f]+ dest_mac=([0-9a-f]{2}:){5}[0-9a-f]{2}' "$PROCS" 2>/dev/null \
            | cut -d= -f3)")
    kfall=$(journalctl -k --since "$since" 2>/dev/null | grep -c 'TX dest FALLBACK')
    # Decode the advertised dest straight out of the dissected frames.
    acmp_da=$(tshark -r "$TMP/cap.pcap" -Y ieee17221 -T fields -e ieee17221.dest_mac 2>/dev/null \
            | tr 'A-F' 'a-f' | tr -d ':' | grep -E '.' | sort -u | paste -sd, -)
    msrp_da=$(tshark -r "$TMP/cap.pcap" -Y mrp-msrp -T fields -e mrp-msrp.stream_da 2>/dev/null \
            | tr 'A-F' 'a-f' | tr -d ':' | grep -E '.' | sort -u | paste -sd, -)

    printf '  /sys param           : %s\n' "${sys:-<none>}"
    printf '  user published       : %s count=%s (%s)\n' "${user:-<none>}" "${user_count:-?}" "${user_tag:-?}"
    printf '  user advertised      : %s\n' "${adv:-<none>}"
    printf '  kernel TX header     : %s\n' "${kern:-<none>}"
    printf '  kernel FALLBACK warns: %s\n' "$kfall"
    printf '  ACMP dest_mac (wire) : %s\n' "${acmp_da:-<none>}"
    printf '  MSRP stream_da (wire): %s\n' "${msrp_da:-<none>}"

    # Checks (the AVTP data plane on the wire is out of scope -- see note above).
    local ok=1
    [ -n "$sys" ] && [ "$user" = "$sys" ] && [ "$adv" = "$sys" ] \
        && [ "$kern" = "$sys" ] || { echo "  FAIL: host planes disagree"; ok=0; }
    [ "$kfall" = 0 ] || { echo "  FAIL: kernel fell back to an unreserved TX dest"; ok=0; }
    case ",$acmp_da,$msrp_da," in
        *",$sys,"*) ;;
        *) echo "  NOTE: advertised dest not seen on the wire in the capture window" ;;
    esac
    [ "$user_tag" = "$want_tag" ] || { echo "  FAIL: tag $user_tag, want $want_tag"; ok=0; }
    if [ "$want_base" = fixed ]; then
        [ "$sys" = "$FIXED" ] || { echo "  FAIL: base $sys, want fixed $FIXED"; ok=0; }
    else
        { [ "${sys#91e0f000}" != "$sys" ] && [ "$sys" != "$FIXED" ]; } \
            || { echo "  FAIL: base $sys not a dynamic MAAP-pool addr"; ok=0; }
    fi
    [ $ok = 1 ] && echo "  PASS" || FAIL=1
}

scenario "default (no AVB_MAAP) -> dynamic base"        -  keep MAAP  dyn
scenario "AVB_MAAP=0 -> fixed base"                     0  keep fixed fixed
scenario "AVB_MAAP=1, daemon up -> dynamic base"        1  keep MAAP  dyn
scenario "AVB_MAAP=1, daemon down -> fixed fallback"    1  kill fixed fixed

echo "=================================================================="
[ $FAIL = 0 ] && echo "ALL SCENARIOS PASSED" || echo "SOME SCENARIOS FAILED"
exit $FAIL
