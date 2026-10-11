#!/bin/bash
# MOTU configuration helper: applies, checks and restores multi-device AVB
# topologies through the MOTU web datastore (http://<host>/datastore).
#
# A MOTU acts as ACMP controller for writes to its datastore: writing
# avb/<listener>/cfg/0/input_streams/<i>/talker connects that input, also when
# the listener is us.  Routing (ext/obank) and the clock are device-local.
#
#   motu_ds.sh                           interactive menu
#   motu_ds.sh status                    clock, lock, connections, routing per host
#   motu_ds.sh [--dry-run] apply <topo>  write the topology (differences only)
#   motu_ds.sh check <topo>              read-only: one ok/FAIL line per device
#   motu_ds.sh diagnose [<topo>]         why device inputs fed by us are not
#                                        connected (all such inputs without topo),
#                                        and gPTP faults (hung switch, split domain)
#   motu_ds.sh [--dry-run] restore       write back the restore snapshot
#   motu_ds.sh get <host> <key>
#   motu_ds.sh [--dry-run] set <host> <key> <json-value>
#   motu_ds.sh [--dry-run] connect <host> <listener-eid> <in-idx> <talker-eid> <uid>
#   motu_ds.sh [--dry-run] loopback <host> <n>
#   motu_ds.sh [--dry-run] clock <host> [<source-name>]
#
# <topo> is a name in tests/motu/topologies/ (n7, n24, ...) or a path.  An eid
# argument may also be `us` or a host name.  Talker `0000000000000000 0`
# disconnects.  --dry-run prints every write as `host key: old -> new` and sends
# nothing.  Every write first saves the old value to RESTORE_FILE, which is
# extended, never overwritten, so `restore` returns to the state before the
# first write.
#
# Topology file: one line per range, `#` starts a comment.
#   # our-streams  host   device-streams
#   0-3            828es  0-3
#   4-6            ulavb  0-2
# Each pair (k, j): device input j <- us:k; device AVB-out j <- AVB-in j; our
# input k <- device uid j; every listed device clocks from CLOCK_SOURCE.  apply
# writes in that order and waits for clock lock before connecting our inputs.
#
# Knobs:
#   MOTU_HOSTS="16a 24ai 242ai 828es ulavb"   hosts to query
#   SWITCH_HOSTS="avbswitch1 avbswitch2"      MOTU AVB Switches (GET /params); the
#                              one with the best priority must be everybody's
#                              gPTP grandmaster
#   RATE=48000                 sample rate check expects (and apply prints)
#   LOCK_SECS=10               wait for ext/clockLocked=1 after a clock write
#                              (restore: 3x, re-locking to a MOTU master is slower)
#   CLOCK_SOURCE="AVB Input Stream 1"   clock source every topology device takes
#   OUR_EID=                   our entity id; default: the entity whose
#                              model_name is OUR_MODEL ("Linux AVB"), else
#                              from /proc/snd_igb_avb/streams
#   RESTORE_FILE=~/.avb/motu-restore.txt
#   READBACK_SECS=10           how long a write may take to read back / connect
#                              (restore: 3x for connecting, a MOTU re-locking
#                              to its master connects 15-25 s late)
#   CURL_TIMEOUT=3             per-request timeout
#   CONNECT_TIMEOUT=1          connect timeout (an offline host costs this once)
#   IFACE=<iface>              the printed avb_up.sh command; with the stack
#                              down also our entity id (from its MAC)
#   EXTRA_STREAMS=0            streams the stack runs beyond the topology's pairs
#                              (fanout.sh: receive-only copies); N must then be
#                              exactly pairs + EXTRA_STREAMS
#   JOURNAL_FILE=              avb-user log text for diagnose; default: the
#                              journal of the running avb-user (journalctl _PID=)
#   DS_DIR=                    read <host>.json from this directory instead of
#                              http://<host>/datastore, and <switch>.params.json
#                              instead of http://<switch>/params (offline diagnose)
#   PROCDIR=/proc/snd_igb_avb  SYSPARAM=/sys/module/snd_igb_avb/parameters
#
# Exit codes:
#   0 ok
#   1 FAIL (a check or a write failed)
#   2 setup error (usage, bad topology, host offline)

# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>

set -u

MOTU_HOSTS=${MOTU_HOSTS:-16a 24ai 242ai 828es ulavb}
SWITCH_HOSTS=${SWITCH_HOSTS:-avbswitch1 avbswitch2}
RATE=${RATE:-48000}
LOCK_SECS=${LOCK_SECS:-10}
CLOCK_SOURCE=${CLOCK_SOURCE:-AVB Input Stream 1}
OUR_EID=${OUR_EID:-}
OUR_MODEL=${OUR_MODEL:-Linux AVB}
RESTORE_FILE=${RESTORE_FILE:-$HOME/.avb/motu-restore.txt}
READBACK_SECS=${READBACK_SECS:-10}
CURL_TIMEOUT=${CURL_TIMEOUT:-3}
CONNECT_TIMEOUT=${CONNECT_TIMEOUT:-1}
IFACE=${IFACE:-<iface>}
PROCDIR=${PROCDIR:-/proc/snd_igb_avb}
EXTRA_STREAMS=${EXTRA_STREAMS:-0}
JOURNAL_FILE=${JOURNAL_FILE:-}
DS_DIR=${DS_DIR:-}
SYSPARAM=${SYSPARAM:-/sys/module/snd_igb_avb/parameters}
TOPODIR=$(cd "$(dirname "$0")" && pwd)/topologies
NULL_EID=0000000000000000
DRY_RUN=0

usage() {
  awk '
    NR == 1 { next }
    /^#/ { sub(/^# ?/, ""); print; next }
    { exit }
  ' "$0"
}

say(){  printf '\033[1m== %s\033[0m\n' "$*"; }
ok(){   printf '\033[32mok\033[0m   %s\n' "$*"; }
fail(){ printf '\033[31mFAIL\033[0m %s\n' "$*"; }
warn(){ printf '\033[33m!! %s\033[0m\n' "$*" >&2; }
err(){  printf '\033[31merror: %s\033[0m\n' "$*" >&2; }

for t in awk curl jq sort; do
  command -v "$t" >/dev/null || { err "missing required tool: $t"; exit 2; }
done

CACHE=$(mktemp -d)
trap 'rm -rf "$CACHE"' EXIT
declare -A HOST_EID=()

# ---------------------------------------------------------------- datastore --

# The full datastore of <host>, cached for one command; returns 1 when offline.
ds_load() {
  local h=$1 f=$CACHE/$1.json
  [ -s "$f" ] && return 0
  [ -e "$CACHE/$h.off" ] && return 1
  if [ -n "$DS_DIR" ]; then
    [ -s "$DS_DIR/$h.json" ] && cp "$DS_DIR/$h.json" "$f" && return 0
    : > "$CACHE/$h.off"
    return 1
  fi
  if curl -sf -m "$CURL_TIMEOUT" --connect-timeout "$CONNECT_TIMEOUT" "http://$h/datastore" -o "$f.tmp" &&
     jq -e 'type == "object"' "$f.tmp" >/dev/null 2>&1; then
    mv "$f.tmp" "$f"
    return 0
  fi
  rm -f "$f.tmp"
  : > "$CACHE/$h.off"
  return 1
}

ds_flush() { rm -f "$CACHE"/*; HOST_EID=(); }

# One cached key: as raw text ("" when absent) / whether it exists.
ds_str() { jq -r --arg k "$2" '.[$k] // "" | tostring' "$CACHE/$1.json"; }
ds_has() { jq -e --arg k "$2" 'has($k)' "$CACHE/$1.json" >/dev/null; }

# One key read live, as JSON.  Only for keys that exist: the datastore holds a
# GET of an absent key open until the timeout.
ds_get_live() { curl -sf -m "$CURL_TIMEOUT" "http://$1/datastore/$2" | jq -c '.value'; }

host_eid() {
  [ -n "${HOST_EID[$1]:-}" ] || { ds_load "$1" && HOST_EID[$1]=$(ds_str "$1" uid); } || return 1
  echo "${HOST_EID[$1]}"
}

online_hosts() { local h; for h in $MOTU_HOSTS; do ds_load "$h" && echo "$h"; done; }

# eid -> entity name, from every datastore that answers.
eid_name() {
  local f=$CACHE/names h
  if [ ! -e "$f" ]; then
    for h in $MOTU_HOSTS; do
      ds_load "$h" || continue
      jq -r 'to_entries[] | select(.key | test("^avb/[0-9a-f]{16}/entity_name$"))
             | "\(.key | split("/")[1])\t\(.value)"' "$CACHE/$h.json"
    done | sort -u -t$'\t' -k1,1 > "$f"
  fi
  awk -F'\t' -v e="$1" '$1 == e { print $2; f = 1; exit } END { if (!f) print e }' "$f"
}

# eid (MAC with fffe in the middle) -> aa:bb:cc:dd:ee:ff
eid_mac() {
  local m=${1:0:6}${1:10:6}
  echo "${m:0:2}:${m:2:2}:${m:4:2}:${m:6:2}:${m:8:2}:${m:10:2}"
}

# "<eid>:<uid>" -> "<name>:<uid>", "-" when unconnected.
talker_fmt() {
  local v=${1//\"/}
  case $v in ''|null|$NULL_EID:*) echo "(none)" ;; *) echo "$(eid_name "${v%%:*}"):${v#*:}" ;; esac
}

our_eid() {
  local f=$CACHE/our_eid e h sid
  if [ ! -s "$f" ]; then
    if [ -n "$OUR_EID" ]; then
      e=$OUR_EID
    else
      e=$(for h in $MOTU_HOSTS; do
            ds_load "$h" || continue
            jq -r --arg m "$OUR_MODEL" '. as $d | (.["avb/devs"] // "" | split(":"))[]
              | select($d["avb/\(.)/model_name"] == $m)' "$CACHE/$h.json"
          done | sort -u)
      if [ -z "$e" ] && [ -r "$PROCDIR/streams" ]; then
        sid=$(sed -n 's/^stream_id=\([0-9a-f]\{12\}\).*/\1/p' "$PROCDIR/streams" | head -1)
        [ -n "$sid" ] && e=${sid:0:6}fffe${sid:6:6}
      fi
      # stack down: the entity id is the NIC MAC with fffe in the middle
      if [ -z "$e" ] && [ -r "/sys/class/net/$IFACE/address" ]; then
        sid=$(tr -d ':\n' < "/sys/class/net/$IFACE/address")
        e=${sid:0:6}fffe${sid:6:6}
      fi
      case $e in
        '') err "cannot find our entity (no '$OUR_MODEL' entity on any MOTU, no $PROCDIR, no IFACE); set OUR_EID or IFACE"; return 2 ;;
        *[!0-9a-f]*) err "several '$OUR_MODEL' entities ($(echo $e)); set OUR_EID"; return 2 ;;
      esac
    fi
    echo "$e" > "$f"
  fi
  cat "$f"
}

# `us`, a host name or an eid -> eid
resolve_eid() {
  case $1 in
    us) our_eid ;;
    0|$NULL_EID) echo $NULL_EID ;;
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) echo "$1" ;;
    *) host_eid "$1" 2>/dev/null || { err "'$1' is not us, an eid or a host that answers"; return 2; } ;;
  esac
}

# "<io>\t<bank>\t<numCh>\t<name>" for every ext/ibank|obank of <host>.
banks() {
  local f=$CACHE/$1.banks
  [ -s "$f" ] || jq -r '. as $d | to_entries[] | select(.key | test("^ext/[io]bank/[0-9]+/name$"))
      | (.key | split("/")) as $p
      | [$p[1], $p[2], ($d["ext/\($p[1])/\($p[2])/numCh"] // $d["ext/\($p[1])/\($p[2])/maxCh"] // 0), .value]
      | @tsv' "$CACHE/$1.json" > "$f"
  cat "$f"
}

# <host> ibank|obank <j> -> "<bank> <numCh>" of bank "AVB Stream j+1"
avb_bank() {
  banks "$1" | awk -F'\t' -v io="$2" -v n="AVB Stream $(( $3 + 1 ))" \
    '$1 == io && $4 == n { print $2, $3; exit }'
}

# <host> <source-name> -> clock source index;  <host> <index> -> name
clock_index() {
  local e; e=$(host_eid "$1") || return 1
  jq -r --arg e "$e" --arg n "$2" 'to_entries[]
    | select(.key | test("^avb/\($e)/cfg/0/clock_sources/[0-9]+/object_name$"))
    | select(.value == $n) | .key | split("/")[5]' "$CACHE/$1.json" | head -1
}
clock_name() {
  local e; e=$(host_eid "$1") || return 1
  ds_str "$1" "avb/$e/cfg/0/clock_sources/$2/object_name"
}

# ------------------------------------------------------------------- plans --
# A plan line is "<phase>\t<host>\t<key>\t<new-json>\t<old-json>".

# Wanted lines ("<phase>\t<host>\t<key>\t<json>") on stdin -> the same lines
# with the cached live value appended, in input order.
with_live() {
  local w=$CACHE/wanted.$BASHPID h
  awk -v OFS='\t' '{ print NR, $0 }' > "$w"
  for h in $(cut -f3 "$w" | awk '!s[$0]++'); do
    ds_load "$h" || { awk -F'\t' -v OFS='\t' -v h="$h" '$3 == h { print $0, "null" }' "$w"; continue; }
    awk -F'\t' -v h="$h" '$3 == h' "$w" |
      jq -rR --slurpfile d "$CACHE/$h.json" 'split("\t") as $f | $f + [$d[0][$f[3]] | tojson] | join("\t")'
  done | sort -t$'\t' -k1,1n | cut -f2-
  rm -f "$w"
}

# Wanted lines -> plan lines that differ from the live state.
plan_diff() { with_live | awk -F'\t' '$4 != $5'; }

old_fmt() { [ "$1" = null ] && echo '(absent)' || echo "$1"; }

print_plan() {
  local p h k new old
  while IFS=$'\t' read -r p h k new old <&3; do
    printf '%s %s: %s -> %s\n' "$h" "$k" "$(old_fmt "$old")" "$new"
  done 3< "$1"
}

# Saves the old value of every key in the plan, unless the snapshot already
# has that key: the snapshot always holds the state before the first write.
# A clock source change also saves master_clock/uid after it: leaving "MOTU
# AVB Master Clock" clears it on the device, and a MOTU AVB Master Clock
# without a master falls back to Internal.  restore runs newest first, so the
# master is back before the clock source.
snapshot_plan() {
  local p h k new old e
  { [ "$DRY_RUN" = 1 ] || [ -n "${NO_SNAPSHOT:-}" ]; } && return 0
  mkdir -p "$(dirname "$RESTORE_FILE")"
  [ -e "$RESTORE_FILE" ] || printf '# motu_ds.sh restore snapshot, started %s\n' "$(date '+%F %T')" > "$RESTORE_FILE"
  while IFS=$'\t' read -r p h k new old <&3; do
    printf '%s\t%s\t%s\n' "$h" "$k" "$old"
    case $k in
      avb/*/cfg/0/clock_source_index)
        e=${k#avb/}; e=${e%%/*}
        ds_load "$h" && printf '%s\tavb/%s/master_clock/uid\t%s\n' "$h" "$e" \
          "$(jq -c --arg k "avb/$e/master_clock/uid" '.[$k]' "$CACHE/$h.json")" ;;
    esac
  done 3< "$1" > "$CACHE/snap.cand"
  awk -F'\t' 'NR == FNR { if (!/^#/) seen[$1 FS $2] = 1; next }
       !(($1 FS $2) in seen) { seen[$1 FS $2] = 1; print }' \
    "$RESTORE_FILE" "$CACHE/snap.cand" > "$CACHE/snap.new"
  cat "$CACHE/snap.new" >> "$RESTORE_FILE"
}

# POSTs json={"<key>":<value>} and polls the key until it reads back.
ds_write() {
  local h=$1 k=$2 v=$3 code got="" deadline
  code=$(curl -s -m "$CURL_TIMEOUT" -o /dev/null -w '%{http_code}' \
           --data-urlencode "json={\"$k\":$v}" "http://$h/datastore")
  case $code in 2??) ;; *) fail "$h $k: POST answered $code"; return 1 ;; esac
  rm -f "$CACHE/$h.json" "$CACHE/$h.banks"
  deadline=$(( $(date +%s) + READBACK_SECS ))
  while :; do
    got=$(ds_get_live "$h" "$k")
    if [ "$got" = "$v" ]; then
      printf '  %s %s = %s\n' "$h" "$k" "$v"
      return 0
    fi
    [ "$(date +%s)" -lt "$deadline" ] || break
    sleep 0.2
  done
  fail "$h $k: wrote $v, reads back ${got:-nothing}"
  return 1
}

# Executes the plan lines of one phase (all when no phase is given).
write_plan() {
  local plan=$1 phase=${2:-} p h k new old rc=0
  while IFS=$'\t' read -r p h k new old <&3; do
    [ -n "$phase" ] && [ "$p" != "$phase" ] && continue
    if [ "$DRY_RUN" = 1 ]; then
      printf '%s %s: %s -> %s\n' "$h" "$k" "$(old_fmt "$old")" "$new"
    else
      ds_write "$h" "$k" "$new" || rc=1
    fi
  done 3< "$plan"
  return $rc
}

# Snapshot + write a whole plan; prints "0 writes" when it is empty.
run_plan() {
  local what=$1 n
  n=$(wc -l < "$CACHE/plan")
  if [ "$n" -eq 0 ]; then say "$what: 0 writes, already in place"; return 0; fi
  snapshot_plan "$CACHE/plan"
  write_plan "$CACHE/plan" || { say "$what: FAIL"; return 1; }
  if [ "$DRY_RUN" = 1 ]; then say "$what: $n writes (dry run, nothing sent)"; else say "$what: $n writes"; fi
}

wait_lock() {
  local h=$1 secs=${2:-$LOCK_SECS} deadline
  [ "$DRY_RUN" = 1 ] && return 0
  sleep 0.5
  deadline=$(( $(date +%s) + secs ))
  while [ "$(ds_get_live "$h" ext/clockLocked)" != 1 ]; do
    [ "$(date +%s)" -lt "$deadline" ] || { fail "$h clock not locked after ${secs}s"; return 1; }
    sleep 0.5
  done
  ok "$h clock locked"
}

# After a talker write: waits for the listener to report the connection.  A
# MOTU listener has connection_status; for us, the kernel's published
# avb_listener_src_macs entry shows that avb-user saw the CONNECT_RX.
# QUIET=1 prints only failures; WAIT_SECS overrides READBACK_SECS.
wait_connection() {
  local h=$1 l=$2 i=$3 t=$4 key want got deadline
  [ "$DRY_RUN" = 1 ] && return 0
  ds_load "$h" || return 1
  key="avb/$l/cfg/0/input_streams/$i/connection_status"
  deadline=$(( $(date +%s) + ${WAIT_SECS:-$READBACK_SECS} ))
  if ds_has "$h" "$key"; then
    # A disconnect only has to stop being connected: an input that was trying
    # a talker which no longer exists keeps reporting "connecting" for a while.
    want='"connected"'
    while got=$(ds_get_live "$h" "$key")
          if [ "$t" = $NULL_EID ]; then [ "$got" = '"connected"' ]; else [ "$got" != "$want" ]; fi; do
      [ "$(date +%s)" -lt "$deadline" ] ||
        { fail "$h in $i of $(eid_name "$l"): $got (want $( [ "$t" = $NULL_EID ] && echo 'not connected' || echo "$want"))"; return 1; }
      sleep 0.3
    done
    [ -n "${QUIET:-}" ] || ok "$h in $i of $(eid_name "$l"): $got"
  elif [ "$l" = "$(our_eid)" ] && [ -r "$SYSPARAM/avb_listener_src_macs" ]; then
    [ "$t" = $NULL_EID ] && want="" || want=$(eid_mac "$t")
    while got=$(cut -d, -f$((i + 1)) "$SYSPARAM/avb_listener_src_macs"); [ "$got" != "$want" ]; do
      [ "$(date +%s)" -lt "$deadline" ] || { fail "kernel listener $i src '${got}' (want '$want')"; return 1; }
      sleep 0.3
    done
    [ -n "${QUIET:-}" ] || ok "kernel listener $i src '${want}' (avb-user published it)"
  fi
}

# ---------------------------------------------------------------- topology --

topo_file() { case $1 in */*|*.topo) echo "$1" ;; *) echo "$TOPODIR/$1.topo" ;; esac; }

range() {
  case $1 in
    ''|*[!0-9-]*|-*|*-|*-*-*) return 1 ;;
    *-*) [ "${1%-*}" -le "${1#*-}" ] || return 1; seq "${1%-*}" "${1#*-}" ;;
    *) echo "$1" ;;
  esac
}

# Fills TK[] (our stream), TH[] (host), TJ[] (device stream): one per pair.
topo_load() {
  local f n=0 line ours host devs extra ks js i
  f=$(topo_file "$1")
  [ -r "$f" ] || { err "no topology '$1' ($f)"; return 2; }
  TOPO=$(basename "$f" .topo)
  TK=() TH=() TJ=()
  while IFS= read -r line || [ -n "$line" ]; do
    n=$((n + 1))
    line=${line%%#*}
    read -r ours host devs extra <<< "$line"
    [ -z "$ours" ] && continue
    [ -n "$devs" ] && [ -z "$extra" ] || { err "$f:$n: want '<our-streams> <host> <device-streams>'"; return 2; }
    ks=$(range "$ours") && js=$(range "$devs") || { err "$f:$n: bad range"; return 2; }
    ks=($ks) js=($js)
    [ ${#ks[@]} -eq ${#js[@]} ] || { err "$f:$n: $ours and $devs differ in length"; return 2; }
    for i in "${!ks[@]}"; do TK+=("${ks[i]}"); TH+=("$host"); TJ+=("${js[i]}"); done
  done < "$f"
}

topo_validate() {
  local n=${#TK[@]} i h e nin nout ci cs seen=" " got want rc=0
  [ "$n" -gt 0 ] || { err "$TOPO: no streams"; return 2; }
  got=$(printf '%s\n' "${TK[@]}" | sort -n | tr '\n' ' ')
  want=$(seq 0 $((n - 1)) | tr '\n' ' ')
  [ "$got" = "$want" ] || { err "$TOPO: our streams must be 0..$((n - 1)) once each, got: $got"; return 2; }
  for i in "${!TK[@]}"; do
    case $seen in *" ${TH[i]}:${TJ[i]} "*) err "$TOPO: ${TH[i]} stream ${TJ[i]} used twice"; return 2 ;; esac
    seen+="${TH[i]}:${TJ[i]} "
  done
  TOPO_HOSTS=$(printf '%s\n' "${TH[@]}" | awk '!s[$0]++' | tr '\n' ' ')
  for h in $TOPO_HOSTS; do
    ds_load "$h" || { err "$TOPO: $h is offline"; rc=2; continue; }
    e=$(host_eid "$h")
    nin=$(ds_str "$h" "avb/$e/cfg/0/input_streams/num")
    nout=$(ds_str "$h" "avb/$e/cfg/0/output_streams/num")
    for i in "${!TK[@]}"; do
      [ "${TH[i]}" = "$h" ] || continue
      if [ "${TJ[i]}" -ge "${nin:-0}" ] || [ "${TJ[i]}" -ge "${nout:-0}" ]; then
        err "$TOPO: $h has $nin input / $nout output streams, no stream ${TJ[i]}"; rc=2; continue
      fi
      for got in ibank obank; do
        read -r _ want <<< "$(avb_bank "$h" $got "${TJ[i]}")"
        [ "${want:-0}" -gt 0 ] || { err "$TOPO: $h has no $got 'AVB Stream $((TJ[i] + 1))' (numCh 0)"; rc=2; }
      done
    done
    ci=$(clock_index "$h" "$CLOCK_SOURCE")
    [ -n "$ci" ] || { err "$TOPO: $h has no clock source '$CLOCK_SOURCE'"; rc=2; continue; }
    # A stream clock source needs that input in the topology, or it never locks.
    cs=$(ds_str "$h" "avb/$e/cfg/0/clock_sources/$ci/stream_id")
    if [ "${cs:--1}" -ge 0 ] && ! printf '%s\n' "$seen" | grep -q " $h:$cs "; then
      err "$TOPO: $h clocks from '$CLOCK_SOURCE' = its input $cs, which the topology does not connect"; rc=2
    fi
  done
  return $rc
}

# Wanted lines for the loaded topology, in write order (phases 1-4).
topo_wanted() {
  local us i h e j ob onc ib inc c
  us=$(our_eid) || return 2
  for i in "${!TK[@]}"; do
    e=$(host_eid "${TH[i]}")
    printf '1\t%s\tavb/%s/cfg/0/input_streams/%s/talker\t"%s:%s"\n' "${TH[i]}" "$e" "${TJ[i]}" "$us" "${TK[i]}"
  done
  for i in "${!TK[@]}"; do
    loopback_wanted "${TH[i]}" "${TJ[i]}"
  done
  for h in $TOPO_HOSTS; do
    printf '3\t%s\tavb/%s/cfg/0/clock_source_index\t%s\n' "$h" "$(host_eid "$h")" "$(clock_index "$h" "$CLOCK_SOURCE")"
  done
  for i in "${!TK[@]}"; do
    e=$(host_eid "${TH[i]}")
    printf '4\t%s\tavb/%s/cfg/0/input_streams/%s/talker\t"%s:%s"\n' "${TH[i]}" "$us" "${TK[i]}" "$e" "${TJ[i]}"
  done
}

# AVB-in j -> AVB-out j on <host>, every channel.
loopback_wanted() {
  local h=$1 j=$2 ob onc ib inc c
  read -r ob onc <<< "$(avb_bank "$h" obank "$j")"
  read -r ib inc <<< "$(avb_bank "$h" ibank "$j")"
  [ "${onc:-0}" -gt 0 ] && [ "${inc:-0}" -gt 0 ] || { err "$h: no AVB Stream $((j + 1)) bank (numCh 0)"; return 2; }
  [ "$onc" = "$inc" ] || warn "$h AVB Stream $((j + 1)): $inc in / $onc out channels, routing $(( onc < inc ? onc : inc ))"
  for ((c = 0; c < (onc < inc ? onc : inc); c++)); do
    printf '2\t%s\text/obank/%s/ch/%s/src\t"%s:%s"\n' "$h" "$ob" "$c" "$ib" "$c"
  done
}

# The stack command the topology needs, and a warning when the running one differs.
stack_hint() {
  local n=$(( ${#TK[@]} + EXTRA_STREAMS )) i cur live
  for i in "${!TK[@]}"; do [ "${TK[i]}" = 0 ] && break; done
  live=$(seq -s, 0 $((n - 1)))
  say "stack:  sudo AVB_DEVICE_SOURCE_MAC=$(eid_mac "$(host_eid "${TH[i]}")") ./avb_up.sh $IFACE $RATE --num-streams $n"
  say "gate:   CH=$((8 * n)) EXPECT_LIVE=$live ./tests/test_rx_streams.sh"
  if [ -r "$PROCDIR/streams" ]; then
    cur=$(grep -c '^stream_id=' "$PROCDIR/streams")
    [ "$cur" = "$n" ] || warn "the running stack has N=$cur, $TOPO needs --num-streams $n"
  else
    warn "no AVB stack running ($PROCDIR/streams missing): our inputs cannot connect"
  fi
}

# -------------------------------------------------------------------- gPTP --
# A hung MOTU AVB Switch keeps its web page and sends Pdelay_Req, but answers
# none and sends no Announce/Sync: its neighbours elect themselves grandmaster.

# The /params of switch <host>, cached; returns 1 when offline.
sw_load() {
  local h=$1 f=$CACHE/$1.params
  [ -s "$f" ] && return 0
  [ -e "$f.off" ] && return 1
  if [ -n "$DS_DIR" ]; then
    [ -s "$DS_DIR/$h.params.json" ] && cp "$DS_DIR/$h.params.json" "$f" && return 0
  elif curl -sf -m "$CURL_TIMEOUT" --connect-timeout "$CONNECT_TIMEOUT" "http://$h/params" -o "$f.tmp" &&
       jq -e '.mac' "$f.tmp" >/dev/null 2>&1; then
    mv "$f.tmp" "$f"
    return 0
  fi
  rm -f "$f.tmp"
  : > "$f.off"
  return 1
}

# switch <host> -> its gPTP clock id (MAC with fffe in the middle)
sw_clock() { local m; m=$(jq -r '.mac' "$CACHE/$1.params" | tr -d ':'); echo "${m:0:6}fffe${m:6:6}"; }

# clock id -> switch name, "us", or entity name
gptp_name() {
  local h
  for h in $SWITCH_HOSTS; do
    sw_load "$h" && [ "$(sw_clock "$h")" = "$1" ] && { jq -r '.name' "$CACHE/$h.params"; return; }
  done
  [ "$1" = "$(our_eid 2>/dev/null)" ] && { echo us; return; }
  eid_name "$1"
}

# The switch that should be grandmaster: lowest priority1, priority2, clock id.
best_switch() {
  local h
  for h in $SWITCH_HOSTS; do
    sw_load "$h" || continue
    printf '%03d %03d %s %s\n' "$(jq -r '.priority1' "$CACHE/$h.params")" \
      "$(jq -r '.priority2' "$CACHE/$h.params")" "$(sw_clock "$h")" "$h"
  done | sort | awk 'NR == 1 { print $4 }'
}

# Our grandmaster as avb-user last logged it ("" without journal).
our_gm() { journal | sed -n 's/^\[gPTP\] grandmaster selected: \([0-9a-f]\{16\}\).*/\1/p' | tail -n 1; }

# One line per gPTP fault: avb-user's own link/split/talker [DIAG] lines, then
# every group of hosts (and us) following a grandmaster other than the
# best-priority switch, or, without switches, a split between hosts and us.
gptp_issues() {
  local h e gm best want l x id
  declare -A who=()
  journal > /dev/null
  active_diags | while IFS= read -r l; do
    case $l in
      "gPTP link: "*|"AVTP talker: "*)
        x=${l% \[*} id=${l##* \[} id=${id%]}
        [ "$id" = $NULL_EID ] || x="$x ($(gptp_name "$id"))"
        echo "us $x" ;;
    esac
  done
  for h in $(online_hosts); do
    gm=$(ds_str "$h" avb/grandmaster)
    [ -n "$gm" ] && who[$gm]+=" $h"
  done
  gm=$(our_gm)
  [ -n "$gm" ] && who[$gm]+=" us"
  [ ${#who[@]} -gt 0 ] || return 0
  best=$(best_switch)
  if [ -n "$best" ]; then
    want=$(sw_clock "$best")
    for gm in "${!who[@]}"; do
      [ "$gm" = "$want" ] && continue
      l=${who[$gm]# }
      [ "$l" = "${l#* }" ] && x=s || x=""
      echo "gPTP: ${l// /, } follow$x grandmaster $(gptp_name "$gm"), not $(jq -r '.name' "$CACHE/$best.params") (best priority of SWITCH_HOSTS): if that switch is up, its gPTP is hung, power-cycle it"
    done
  elif [ ${#who[@]} -gt 1 ]; then
    l=""
    for gm in "${!who[@]}"; do l+="; ${who[$gm]# } -> $(gptp_name "$gm")"; done
    echo "gPTP split domain: ${l#; } (no SWITCH_HOSTS answer)"
  fi
}

# ---------------------------------------------------------------- diagnose --
# Names the cause per device input fed by us that is not connected.  avb-user
# logs what it learns over AVDECC as "[DIAG] <device>[ <scope>]: <cause> [<eid>]"
# and "[DIAG] cleared: ..."; device stream numbers there count from 1.

journal() {
  local f=$CACHE/journal pid
  if [ ! -e "$f" ]; then
    if [ -n "$JOURNAL_FILE" ]; then
      cat "$JOURNAL_FILE" > "$f" 2>/dev/null || : > "$f"
    elif pid=$(pgrep -x avb-user | head -1) && [ -n "$pid" ]; then
      journalctl _PID="$pid" -o cat --no-pager > "$f" 2>/dev/null || : > "$f"
    else
      : > "$f"
    fi
  fi
  cat "$f"
}

# [DIAG] bodies not cleared later in the journal, oldest first.
active_diags() {
  journal | awk '
    /^\[DIAG\] cleared: / { delete on[substr($0, 17)]; next }
    /^\[DIAG\] / { b = substr($0, 8); if (!(b in on)) { on[b] = ++n; ord[n] = b } }
    END { for (i = 1; i <= n; i++) if (ord[i] in on && on[ord[i]] == i) print ord[i] }'
}

# "1-3,5" (1-based) -> "0 1 2 4";  "0 1 2 4" -> "0-2,4"
list0() { local r; for r in ${1//,/ }; do range "$r"; done | awk '{ printf "%s%d", s, $1 - 1; s = " " }'; }
compress() {
  tr ' ' '\n' | awk 'NF' | sort -n | awk '
    function out() { printf "%s%s", (o++ ? "," : ""), (s == p ? s : s "-" p) }
    NR > 1 && $1 == p + 1 { p = $1; next }
    NR > 1 { out() }
    { s = p = $1 }
    END { if (NR) out(); print "" }'
}

# The active [DIAG] line that explains input <j> (0-based) of <eid>: one that
# lists the input, or a device-wide one.  Clock lines are consequences, not causes.
diag_for() {
  local e=$1 j=$2 l text
  while IFS= read -r l; do
    [ "${l##* \[}" = "$e]" ] || continue
    text=${l% \[*}
    case $text in *": clock "*) continue ;; esac
    if [[ $text =~ \ in\ ([0-9,-]+)(\ →\ us:[0-9,-]+)?(,\ out\ [0-9,-]+)?:\  ]]; then
      case " $(list0 "${BASH_REMATCH[1]}") " in *" $j "*) echo "$text"; return 0 ;; esac
    elif [[ ! $text =~ \ out\ [0-9,-]+:\  ]]; then
      echo "$text"
      return 0
    fi
  done < "$CACHE/diags"
  return 1
}

# TK/TH/TJ from every MOTU input whose talker is us.
pairs_from_devices() {
  local us h e j k
  us=$(our_eid) || return 2
  TK=() TH=() TJ=()
  for h in $(online_hosts); do
    e=$(host_eid "$h")
    while read -r j k; do
      TK+=("$k"); TH+=("$h"); TJ+=("$j")
    done < <(jq -r --arg e "$e" --arg us "$us" '. as $d | "avb/\($e)/cfg/0/input_streams" as $c
               | range(0; $d["\($c)/num"] // 0) as $j | ($d["\($c)/\($j)/talker"] // "") as $t
               | select($t | startswith($us + ":")) | "\($j) \($t | split(":")[1])"' "$CACHE/$h.json")
  done
}

# Diagnoses the loaded pairs; prints one FAIL line per device and cause.
diagnose() {
  local topo=$1 us n="" i h e j k st t name why mac want key js ks ci cs l x rc=0 grc=0
  local -a keys=()
  declare -A G_J=() G_K=() G_OURS=() G_HOST=() bad=()
  us=$(our_eid) || return 2
  [ -r "$PROCDIR/streams" ] && n=$(grep -c '^stream_id=' "$PROCDIR/streams")
  if [ -n "$topo" ] && [ -r "$SYSPARAM/avb_device_source_mac" ]; then
    for i in "${!TK[@]}"; do [ "${TK[i]}" = 0 ] && break; done
    want=$(eid_mac "$(host_eid "${TH[i]}")")
    mac=$(cat "$SYSPARAM/avb_device_source_mac")
    [ "$mac" = "$want" ] ||
      warn "the stack runs with AVB_DEVICE_SOURCE_MAC=$mac, $topo wants $want (${TH[i]}): restart it with the hinted MAC"
  fi
  journal > /dev/null
  active_diags > "$CACHE/diags"
  [ -s "$CACHE/journal" ] || warn "no avb-user journal (stack down? JOURNAL_FILE=): ACMP and [DIAG] causes unknown"
  while IFS= read -r l; do
    fail "$l"
    grc=1
  done < <(gptp_issues)
  for i in "${!TK[@]}"; do
    h=${TH[i]} j=${TJ[i]} k=${TK[i]}
    ds_load "$h" || continue
    e=$(host_eid "$h")
    st=$(ds_str "$h" "avb/$e/cfg/0/input_streams/$j/connection_status")
    t=$(ds_str "$h" "avb/$e/cfg/0/input_streams/$j/talker")
    [ "$st" = connected ] && [ "$t" = "$us:$k" ] && continue
    bad[$h]+=" $j"
    name=$(eid_name "$e")
    if [ "$t" != "$us:$k" ]; then
      why="talker is $(talker_fmt "$t"), want us:$k (apply not run, or overwritten)"
    elif [ -z "$n" ]; then
      why="no AVB stack ($PROCDIR missing)"
    elif [ "$k" -ge "$n" ]; then
      why="the stack runs N=$n, too few for these inputs (restart with the --num-streams apply prints)"
    elif grep -q "ACMP CONNECT_TX uid=$k peer=$e:$j " "$CACHE/journal"; then
      why="we answered its CONNECT_TX, yet it is ${st:-not connected}: our side or MSRP (journal below)"
    elif why=$(diag_for "$e" "$j"); then
      why="avb-user saw: $why"
    elif [ "$(ds_str "$h" host/connected)" = 1 ]; then
      why="${st:-not connected}; a computer host is attached (host/connected=1), it can force rates or stream formats: check them in the MOTU app or detach it, else power-cycle $h"
    else
      why="listener stuck: $name sent no ACMP for in @J@; power-cycle it"
    fi
    key="$h|$why"
    [ -n "${G_J[$key]+x}" ] || keys+=("$key")
    G_J[$key]+=" $j"
    G_K[$key]+=" $k"
    case $why in "we answered"*) G_OURS[$key]=1 ;; esac
    case $why in "avb-user saw: "*)
      [ "$(ds_str "$h" host/connected)" = 1 ] && G_HOST[$key]=1 ;;
    esac
  done
  for key in "${keys[@]}"; do
    h=${key%%|*} why=${key#*|}
    js=$(compress <<< "${G_J[$key]}") ks=$(compress <<< "${G_K[$key]}")
    fail "$h in $js <- us:$ks: ${why//@J@/$js}"
    rc=1
    [ -n "${G_HOST[$key]:-}" ] &&
      echo "     hint: a computer host is attached to $h (host/connected=1); it can force rates or stream formats"
    if [ -n "${G_OURS[$key]:-}" ]; then
      for k in ${G_K[$key]}; do
        grep -E "uid=$k([^0-9]|\$)|(talker|listener)\[$k\]" "$CACHE/journal" | tail -n 4 | sed 's/^/       /'
      done
    fi
  done
  for h in "${!bad[@]}"; do
    e=$(host_eid "$h")
    while IFS= read -r l; do
      [ "${l##* \[}" = "$e]" ] || continue
      x=${l% \[*}
      case $x in *": clock from in "*) ;; *": clock "*) echo "     note $h: ${x#*: }"; continue ;; *) continue ;; esac
      cs=${x##*clock from in }; cs=$(( ${cs%%,*} - 1 ))
      case " ${bad[$h]} " in
        *" $cs "*) echo "     note $h: clocks from in $cs, which is not connected: fix in $cs first" ;;
        *) echo "     note $h: clocks from in $cs, which is not connected" ;;
      esac
    done < "$CACHE/diags"
    if [ "$(ds_str "$h" ext/clockLocked)" != 1 ]; then
      ci=$(ds_str "$h" "avb/$e/cfg/0/clock_source_index")
      cs=$(ds_str "$h" "avb/$e/cfg/0/clock_sources/$ci/stream_id")
      case " ${bad[$h]} " in
        *" ${cs:--1} "*) echo "     note $h: clock not locked: consequence of in $cs" ;;
        *) echo "     note $h: clock not locked (source '$(clock_name "$h" "$ci")')" ;;
      esac
    fi
  done
  [ $rc = 0 ] && ok "${#TK[@]} device inputs fed by us are connected"
  [ $grc = 0 ] || rc=1
  return $rc
}

# ---------------------------------------------------------------- commands --

cmd_get() {
  [ $# -eq 2 ] || { err "usage: get <host> <key>"; return 2; }
  ds_load "$1" || { err "$1 is offline"; return 2; }
  ds_has "$1" "$2" || { err "$1 has no key $2"; return 1; }
  ds_get_live "$1" "$2"
}

cmd_set() {
  local v
  [ $# -eq 3 ] || { err "usage: set <host> <key> <json-value>"; return 2; }
  v=$(jq -c . <<< "$3" 2>/dev/null) && [ -n "$v" ] || { err "not a JSON value: $3 (a string needs quotes: '\"6:0\"')"; return 2; }
  ds_load "$1" || { err "$1 is offline"; return 2; }
  printf '0\t%s\t%s\t%s\n' "$1" "$2" "$v" | plan_diff > "$CACHE/plan"
  run_plan "set $1 $2"
}

cmd_connect() {
  local h=$1 l t key
  [ $# -eq 5 ] || { err "usage: connect <host> <listener-eid> <in-idx> <talker-eid> <uid>"; return 2; }
  case $3$5 in ''|*[!0-9]*) err "in-idx and uid must be numbers"; return 2 ;; esac
  ds_load "$h" || { err "$h is offline"; return 2; }
  l=$(resolve_eid "$2") && t=$(resolve_eid "$4") || return 2
  [ "$t" = $NULL_EID ] && set -- "$1" "$2" "$3" "$4" 0
  key="avb/$l/cfg/0/input_streams/$3/talker"
  ds_has "$h" "$key" || { err "$h does not see input $3 of $(eid_name "$l") ($key)"; return 2; }
  printf '0\t%s\t%s\t"%s:%s"\n' "$h" "$key" "$t" "$5" | plan_diff > "$CACHE/plan"
  run_plan "connect $(eid_name "$l") in $3 <- $(talker_fmt "$t:$5")" || return 1
  wait_connection "$h" "$l" "$3" "$t"
}

cmd_loopback() {
  local h=$1 j
  [ $# -eq 2 ] && [ -n "${2##*[!0-9]*}" ] || { err "usage: loopback <host> <n>"; return 2; }
  ds_load "$h" || { err "$h is offline"; return 2; }
  for ((j = 0; j < $2; j++)); do loopback_wanted "$h" "$j" || return 2; done > "$CACHE/wanted"
  plan_diff < "$CACHE/wanted" > "$CACHE/plan"
  run_plan "loopback $h AVB in 0..$(( $2 - 1 )) -> AVB out"
}

cmd_clock() {
  local h=$1 name=${2:-$CLOCK_SOURCE} ci
  [ $# -ge 1 ] && [ $# -le 2 ] || { err "usage: clock <host> [<source-name>]"; return 2; }
  ds_load "$h" || { err "$h is offline"; return 2; }
  ci=$(clock_index "$h" "$name")
  [ -n "$ci" ] || { err "$h has no clock source '$name'"; return 2; }
  printf '0\t%s\tavb/%s/cfg/0/clock_source_index\t%s\n' "$h" "$(host_eid "$h")" "$ci" | plan_diff > "$CACHE/plan"
  run_plan "clock $h <- $name" || return 1
  wait_lock "$h"
}

cmd_apply() {
  local us h i rc=0
  [ $# -eq 1 ] || { err "usage: apply <topo>"; return 2; }
  topo_load "$1" && topo_validate || return 2
  us=$(our_eid) || return 2
  topo_wanted > "$CACHE/wanted" || return 2
  plan_diff < "$CACHE/wanted" > "$CACHE/plan"
  stack_hint
  # Writes per device; our inputs are written through the device but counted as us.
  for h in $TOPO_HOSTS; do
    printf '%s %s\n' "$h" "$(awk -F'\t' -v h="$h" -v us="avb/$us/" '$2 == h && index($3, us) != 1' "$CACHE/plan" | wc -l)"
  done | { printf 'writes:'; while read -r h n; do printf ' %s %s,' "$h" "$n"; done
           printf ' us %s\n' "$(awk -F'\t' -v us="avb/$us/" 'index($3, us) == 1' "$CACHE/plan" | wc -l)"; }
  if [ ! -s "$CACHE/plan" ]; then say "apply $TOPO: 0 writes, already applied"; return 0; fi
  if [ "$DRY_RUN" = 1 ]; then
    print_plan "$CACHE/plan"
    say "apply $TOPO: $(wc -l < "$CACHE/plan") writes (dry run, nothing sent)"
    return 0
  fi
  snapshot_plan "$CACHE/plan"
  write_plan "$CACHE/plan" 1 && write_plan "$CACHE/plan" 2 && write_plan "$CACHE/plan" 3 ||
    { say "apply $TOPO: FAIL (devices half-written; '$0 restore' undoes it)"; return 1; }
  for i in "${!TK[@]}"; do
    QUIET=1 wait_connection "${TH[i]}" "$(host_eid "${TH[i]}")" "${TJ[i]}" "$us" || rc=1
  done
  [ $rc = 0 ] && ok "${#TK[@]} device inputs connected"
  for h in $TOPO_HOSTS; do wait_lock "$h" || rc=1; done
  if [ $rc != 0 ]; then
    ds_flush
    diagnose "$TOPO"
    say "apply $TOPO: FAIL, devices not connected/locked, our inputs left as they were"
    return 1
  fi
  write_plan "$CACHE/plan" 4 || { say "apply $TOPO: FAIL connecting our inputs"; return 1; }
  for i in "${!TK[@]}"; do
    QUIET=1 wait_connection "${TH[i]}" "$us" "${TK[i]}" "$(host_eid "${TH[i]}")" || rc=1
  done
  [ $rc = 0 ] || { say "apply $TOPO: FAIL, avb-user did not publish every listener"; return 1; }
  ok "${#TK[@]} kernel listener entries published"
  say "apply $TOPO: $(wc -l < "$CACHE/plan") writes. Next: $0 check $TOPO"
}

cmd_check() {
  local us h e i p k new old who rc=0 nproc
  declare -A why=() routed=() badtalker=()
  [ $# -eq 1 ] || { err "usage: check <topo>"; return 2; }
  topo_load "$1" && topo_validate || return 2
  us=$(our_eid) || return 2
  {
    topo_wanted
    for i in "${!TK[@]}"; do
      printf 'c\t%s\tavb/%s/cfg/0/input_streams/%s/connection_status\t"connected"\n' "${TH[i]}" "$(host_eid "${TH[i]}")" "${TJ[i]}"
    done
    for h in $TOPO_HOSTS; do
      printf 'c\t%s\text/clockLocked\t1\n' "$h"
      printf 'c\t%s\tavb/%s/cfg/0/current_sampling_rate\t%s\n' "$h" "$(host_eid "$h")" "$RATE"
    done
  } > "$CACHE/wanted" || return 2
  plan_diff < "$CACHE/wanted" > "$CACHE/plan"
  while IFS=$'\t' read -r p h k new old <&3; do
    who=$h
    case $k in "avb/$us/"*) who=us ;; esac
    case $k in
      */input_streams/*/talker)
        i=${k%/talker}; i=${i##*/}
        badtalker[$who:$i]=1
        why[$who]+="in $i <- $(talker_fmt "$old") (want $(talker_fmt "$new")); " ;;
      */connection_status)
        i=${k%/connection_status}; i=${i##*/}
        [ -n "${badtalker[$who:$i]:-}" ] || why[$who]+="in $i ${old//\"/}; " ;;
      ext/obank/*/src)
        i=${k#ext/obank/}; i=${i%%/*}
        routed[$h:$i]=$(( ${routed[$h:$i]:-0} + 1 )) ;;
      */clock_source_index)
        why[$who]+="clock '$(clock_name "$h" "$old")' (want '$CLOCK_SOURCE'); " ;;
      ext/clockLocked)
        why[$who]+="clockLocked=$old; " ;;
      */current_sampling_rate)
        why[$who]+="rate $old (want $RATE; AECP sets only AVB_DEVICE_SOURCE_MAC's rate); " ;;
    esac
  done 3< "$CACHE/plan"
  for k in $(printf '%s\n' "${!routed[@]}" | sort -t: -k1,1 -k2,2n); do
    why[${k%%:*}]+="obank ${k#*:}: ${routed[$k]} ch not AVB-in -> AVB-out; "
  done
  # Our side: the kernel must run N streams and publish each listener's talker MAC.
  if [ -r "$PROCDIR/streams" ]; then
    nproc=$(grep -c '^stream_id=' "$PROCDIR/streams")
    [ "$nproc" = $(( ${#TK[@]} + EXTRA_STREAMS )) ] || why[us]+="stack N=$nproc (want $(( ${#TK[@]} + EXTRA_STREAMS ))); "
  else
    why[us]+="no AVB stack ($PROCDIR missing); "
  fi
  if [ -r "$SYSPARAM/avb_listener_src_macs" ]; then
    for i in "${!TK[@]}"; do
      new=$(eid_mac "$(host_eid "${TH[i]}")")
      old=$(cut -d, -f$((TK[i] + 1)) "$SYSPARAM/avb_listener_src_macs")
      [ "$old" = "$new" ] || why[us]+="kernel listener ${TK[i]} src '$old' (want $new); "
    done
  fi
  for h in $TOPO_HOSTS us; do
    if [ "$h" = us ]; then
      e=$us
      p="${#TK[@]} inputs <- $(for i in $TOPO_HOSTS; do eid_name "$(host_eid "$i")"; done | paste -sd, | sed 's/,/, /g'), kernel lists published"
    else
      e=$(host_eid "$h")
      p="$(grep -c "^$h\$" < <(printf '%s\n' "${TH[@]}")) inputs <- us + AVB loopback, clock '$CLOCK_SOURCE' locked, $RATE Hz"
    fi
    if [ -n "${why[$h]:-}" ]; then
      fail "$h $(eid_name "$e"): ${why[$h]%; }"
      rc=1
    else
      ok "$h $(eid_name "$e"): $p"
    fi
  done
  if [ $rc = 0 ]; then
    say "check $TOPO: ok"
  else
    diagnose "$TOPO"
    say "check $TOPO: FAIL"
  fi
  return $rc
}

cmd_diagnose() {
  [ $# -le 1 ] || { err "usage: diagnose [<topo>]"; return 2; }
  if [ $# -eq 1 ]; then
    topo_load "$1" && topo_validate || return 2
    diagnose "$TOPO"
  else
    pairs_from_devices || return 2
    diagnose ""
  fi
}

cmd_restore() {
  local rc=0 p h k new old dest us t i l touched=" " NO_SNAPSHOT=1
  [ -s "$RESTORE_FILE" ] || { say "restore: no snapshot ($RESTORE_FILE)"; return 0; }
  # Newest first: our inputs, then clocks, routing, device inputs -- apply in reverse.
  # A talker key that did not exist yet (the stream count was lower) restores
  # as unconnected; any other absent key is left alone.
  grep -v '^#' "$RESTORE_FILE" | tac | while IFS=$'\t' read -r h k old; do
    if [ "$old" = null ]; then
      case $k in */talker) old="\"$NULL_EID:0\"" ;; *) continue ;; esac
    fi
    printf '0\t%s\t%s\t%s\n' "$h" "$k" "$old"
  done > "$CACHE/wanted"
  plan_diff < "$CACHE/wanted" > "$CACHE/plan"
  # Our inputs do not survive a stack restart, so their snapshot value depends
  # on when the stack was last started.  Only disconnect those whose talker is
  # a device this restore changes back; a connection to an untouched device
  # (the 828ES at N=4 under n7) stays valid.
  us=$(our_eid 2>/dev/null) || us=""
  for h in $(awk -F'\t' -v us="avb/$us/" 'index($3, us) != 1 { print $2 }' "$CACHE/plan" | sort -u); do
    touched+="$(host_eid "$h" 2>/dev/null) "
  done
  while IFS=$'\t' read -r p h k new old <&3; do
    # A key that is absent now (our input k at a lower N) cannot be written.
    [ "$old" = null ] && continue
    if [ -n "$us" ] && [[ $k == avb/$us/*/talker ]] && [ "$new" = "\"$NULL_EID:0\"" ]; then
      t=${old#\"}; t=${t%%:*}
      case $touched in
        *" $t "*) ;;
        *) i=${k%/talker}; i=${i##*/}
           echo "  keeping our input $i <- $(talker_fmt "$old"): $(eid_name "$t") is not changed back" >&2
           continue ;;
      esac
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$p" "$h" "$k" "$new" "$old"
  done 3< "$CACHE/plan" > "$CACHE/plan.keep"
  mv "$CACHE/plan.keep" "$CACHE/plan"
  run_plan "restore" || rc=1
  if [ "$DRY_RUN" != 1 ]; then
    for h in $(awk -F'\t' '$3 ~ /clock_source_index$/ { print $2 }' "$CACHE/plan" | sort -u); do
      wait_lock "$h" $((3 * LOCK_SECS)) || warn "$h: restored clock source does not lock"
    done
    while IFS=$'\t' read -r p h k new old <&3; do
      case $k in avb/*/cfg/0/input_streams/*/talker) ;; *) continue ;; esac
      t=${new#\"}; t=${t%%:*}
      i=${k%/talker}; i=${i##*/}
      l=${k#avb/}; l=${l%%/*}
      QUIET=1 WAIT_SECS=$((3 * READBACK_SECS)) wait_connection "$h" "$l" "$i" "$t" || rc=1
    done 3< "$CACHE/plan"
  fi
  [ "$DRY_RUN" = 1 ] && return $rc
  if [ $rc = 0 ]; then
    dest=${RESTORE_FILE%.txt}.$(date +%Y%m%d-%H%M%S).txt
    mv "$RESTORE_FILE" "$dest"
    say "restore: snapshot moved to $dest"
  else
    say "restore: FAIL, snapshot kept ($RESTORE_FILE); run restore again"
  fi
  return $rc
}

cmd_status() {
  local h gm="" fol us cur l
  fol=$(for h in $MOTU_HOSTS; do
          ds_load "$h" || continue
          jq -r '. as $d | .uid as $e | "avb/\($e)/cfg/0" as $c | $d["\($c)/clock_source_index"] as $ci
            | select($d["\($c)/clock_sources/\($ci)/object_name"] == "MOTU AVB Master Clock")
            | "\($d["avb/\($e)/master_clock/uid"] // "")\t\($d["avb/\($e)/entity_name"] // $e)"' "$CACHE/$h.json"
        done)
  for h in $MOTU_HOSTS; do ds_load "$h" && { gm=$(ds_str "$h" avb/grandmaster); break; }; done
  us=$(our_eid 2>/dev/null) || us="?"
  cur="no stack"
  [ -r "$PROCDIR/streams" ] && cur="N=$(grep -c '^stream_id=' "$PROCDIR/streams")"
  say "us $(eid_name "$us") $us ($cur)   gPTP grandmaster ${gm:-?}$( [ -n "$gm" ] && [ "$(gptp_name "$gm")" != "$gm" ] && echo " = $(gptp_name "$gm")")"
  gptp_issues | while IFS= read -r l; do printf '   \033[31m!! %s\033[0m\n' "$l"; done
  # Our inputs as the first MOTU that answers sees them (we have no connection_status).
  for h in $MOTU_HOSTS; do
    ds_load "$h" || continue
    jq -r --arg e "$us" '. as $d | "avb/\($e)/cfg/0" as $c
      | range(0; $d["\($c)/input_streams/num"] // 0) as $k | "\($k)\t\($d["\($c)/input_streams/\($k)/talker"] // "")"' \
      "$CACHE/$h.json" | while IFS=$'\t' read -r k t; do printf '   in %s <- %s\n' "$k" "$(talker_fmt "$t")"; done
    break
  done
  for h in $MOTU_HOSTS; do
    if ! ds_load "$h"; then say "$h  offline"; continue; fi
    jq -r --arg host "$h" --arg fol "$fol" --arg gm "$gm" '
      . as $d | .uid as $e | "avb/\($e)/cfg/0" as $c
      | def nm($x): if ($x // "") == "" then "?" else ($d["avb/\($x)/entity_name"] // $x) end;
        def talker($t): ($t // "" | split(":")) as $p
          | if ($p | length) < 2 or $p[0] == "0000000000000000" then "(none)" else "\(nm($p[0])):\($p[1])" end;
        def bank($io; $name): [$d | to_entries[] | select(.key | test("^ext/\($io)/[0-9]+/name$"))
                               | select(.value == $name) | .key | split("/")[2]] | first;
      $d["\($c)/clock_source_index"] as $ci
      | ($d["\($c)/clock_sources/\($ci)/object_name"] // "index \($ci)") as $cn
      | ($d["\($c)/clock_sources/\($ci)/stream_id"] // -1) as $cs
      | (if $cs >= 0 then " (= \(talker($d["\($c)/input_streams/\($cs)/talker"])))"
         elif $cn == "MOTU AVB Master Clock" then " (= \(nm($d["avb/\($e)/master_clock/uid"])))"
         else "" end) as $cx
      | "\u001b[1m== \($host)  \(nm($e))  \($e)  \($d["\($c)/current_sampling_rate"]) Hz  clock: \($cn)\($cx)  \(if $d["ext/clockLocked"] == 1 then "locked" else "NOT LOCKED" end)\u001b[0m",
        (range(0; $d["\($c)/input_streams/num"] // 0) as $j
         | "   in \($j) <- \(talker($d["\($c)/input_streams/\($j)/talker"])) \($d["\($c)/input_streams/\($j)/connection_status"] // "?")"),
        ([to_entries[] | select(.key | test("^ext/obank/[0-9]+/name$")) | select(.value | test("^AVB Stream [0-9]+$"))
          | {o: (.key | split("/")[2]), name: .value, j: ((.value | ltrimstr("AVB Stream ") | tonumber) - 1)}]
         | sort_by(.j)[]
         | . as $b | ($d["ext/obank/\($b.o)/numCh"] // 0) as $nc | select($nc > 0)
         | bank("ibank"; $b.name) as $i
         | [range(0; $nc) as $ch | $d["ext/obank/\($b.o)/ch/\($ch)/src"] // ""] as $src
         | if $src == [range(0; $nc) as $ch | "\($i):\($ch)"] then "   AVB out \($b.j) <- AVB in \($b.j)"
           elif all($src[]; . == "") then "   AVB out \($b.j) unrouted"
           else "   AVB out \($b.j) custom: \($src | map(if . == "" then "-" else . end) | join(" "))" end),
        ([$fol | split("\n")[] | split("\t") | select(.[0] == $e) | .[1]] | select(length > 0)
         | "   media-clock master (MOTU AVB Master Clock) of: \(join(", "))"),
        (select($gm == $e) | "   gPTP grandmaster"),
        (select($d["avb/grandmaster"] != $gm) | "   gPTP grandmaster here: \($d["avb/grandmaster"])")
      ' "$CACHE/$h.json"
  done
}

# ------------------------------------------------------------- interactive --

# select from a list; sets PICK (the chosen index) or returns 1 for back/EOF.
pick() {
  local prompt=$1 o; shift
  PS3="$prompt> "
  select o in "$@" "(back)"; do
    [ -n "$o" ] || continue
    [ "$o" = "(back)" ] && return 1
    PICK=$((REPLY - 1))
    return 0
  done
  return 1
}

pick_host() {
  local hs=() labels=() h
  for h in $(online_hosts); do hs+=("$h"); labels+=("$h ($(eid_name "$(host_eid "$h")"))"); done
  [ ${#hs[@]} -gt 0 ] || { err "no MOTU host answers ($MOTU_HOSTS)"; return 1; }
  pick "host" "${labels[@]}" || return 1
  PICKED_HOST=${hs[PICK]}
}

# The entities <host> sees (itself, us, the other MOTUs).
pick_entity() {
  local h=$1 what=$2 es=() labels=() e
  for e in $(ds_str "$h" avb/devs | tr ':' ' '); do es+=("$e"); labels+=("$(eid_name "$e") ($e)"); done
  pick "$what" "${labels[@]}" || return 1
  PICKED_EID=${es[PICK]}
}

pick_stream() {
  local h=$1 e=$2 io=$3 n j labels=() c="avb/$2/cfg/0"
  n=$(ds_str "$h" "$c/${io}_streams/num")
  for ((j = 0; j < ${n:-0}; j++)); do
    if [ "$io" = input ]; then
      labels+=("in $j <- $(talker_fmt "$(ds_str "$h" "$c/input_streams/$j/talker")") $(ds_str "$h" "$c/input_streams/$j/connection_status")")
    else
      labels+=("out $j $(ds_str "$h" "$c/output_streams/$j/object_name")")
    fi
  done
  [ ${#labels[@]} -gt 0 ] || { err "$(eid_name "$e") has no $io streams"; return 1; }
  pick "$io stream" "${labels[@]}" || return 1
  PICKED_IDX=$PICK
}

pick_topo() {
  local fs=("$TOPODIR"/*.topo) names=() f
  [ -e "${fs[0]}" ] || { err "no topologies in $TOPODIR"; return 1; }
  for f in "${fs[@]}"; do names+=("$(basename "$f" .topo)"); done
  pick "topology" "${names[@]}" || return 1
  PICKED_TOPO=${names[PICK]}
}

confirm() { local a; read -r -p "$1 [y/N] " a; [ "$a" = y ] || [ "$a" = Y ]; }

# Every item calls the sub-command's function, so a manual session makes the
# same writes as a scripted one.
interactive() {
  local items=("status" "apply topology" "check topology" "diagnose" "connect input" "disconnect input"
               "set clock" "loopback" "restore" "quit")
  local item h l n srcs=() e
  while :; do
    ds_flush
    echo
    PS3="motu> "
    select item in "${items[@]}"; do break; done
    [ -n "${item:-}" ] || return 0
    case $item in
      status) cmd_status ;;
      "apply topology")
        pick_topo || continue
        ( DRY_RUN=1; cmd_apply "$PICKED_TOPO" ) || continue
        [ "$DRY_RUN" = 1 ] && continue
        ds_flush
        confirm "apply $PICKED_TOPO?" && cmd_apply "$PICKED_TOPO" ;;
      "check topology") pick_topo && cmd_check "$PICKED_TOPO" ;;
      diagnose) cmd_diagnose ;;
      "connect input")
        pick_host || continue; h=$PICKED_HOST
        pick_entity "$h" "listener" || continue; l=$PICKED_EID
        pick_stream "$h" "$l" input || continue; n=$PICKED_IDX
        pick_entity "$h" "talker" || continue
        pick_stream "$h" "$PICKED_EID" output || continue
        cmd_connect "$h" "$l" "$n" "$PICKED_EID" "$PICKED_IDX" ;;
      "disconnect input")
        pick_host || continue; h=$PICKED_HOST
        pick_entity "$h" "listener" || continue; l=$PICKED_EID
        pick_stream "$h" "$l" input || continue
        cmd_connect "$h" "$l" "$PICKED_IDX" $NULL_EID 0 ;;
      "set clock")
        pick_host || continue; h=$PICKED_HOST; e=$(host_eid "$h")
        mapfile -t srcs < <(jq -r --arg e "$e" '. as $d | range(0; $d["avb/\($e)/cfg/0/clock_sources/num"] // 0)
                              | $d["avb/\($e)/cfg/0/clock_sources/\(.)/object_name"]' "$CACHE/$h.json")
        pick "clock source" "${srcs[@]}" || continue
        cmd_clock "$h" "${srcs[PICK]}" ;;
      loopback)
        pick_host || continue; h=$PICKED_HOST
        read -r -p "loop back AVB streams 0..n-1, n = " n
        cmd_loopback "$h" "$n" ;;
      restore)
        ( DRY_RUN=1; cmd_restore ) || continue
        [ "$DRY_RUN" = 1 ] && continue
        [ -s "$RESTORE_FILE" ] && { ds_flush; confirm "restore?" && cmd_restore; } ;;
      quit) return 0 ;;
    esac
  done
}

# -------------------------------------------------------------------- main --

args=()
for a in "$@"; do
  case $a in
    --dry-run) DRY_RUN=1 ;;
    -h|--help) usage; exit 0 ;;
    *) args+=("$a") ;;
  esac
done
set -- "${args[@]+"${args[@]}"}"

[ $# -eq 0 ] && { interactive; exit $?; }
cmd=$1; shift
case $cmd in
  status|apply|check|diagnose|restore|get|set|connect|loopback|clock) "cmd_$cmd" "$@"; exit $? ;;
  *) err "unknown command '$cmd'"; usage; exit 2 ;;
esac
