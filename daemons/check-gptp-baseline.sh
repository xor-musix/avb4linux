#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BASELINE=${GPTP_WARN_BASELINE_FILE:-"$ROOT/gptp-warning-baseline.txt"}

# Patch overlay for the gptp daemon.  The daemons/gptp submodule is a pristine
# upstream checkout and is NEVER edited in place (same rule as OpenAvnu and
# kernel-module/snd-igb-avb).  We stage a fresh copy of the parts needed to
# build daemon_cl into gptp-build/, apply gptp-patches/*.patch there, and build
# from the staged copy.  gptp-build/ is git-ignored.
SUBMODULE="$ROOT/gptp"
PATCHES="$ROOT/gptp-patches"
STAGE="$ROOT/gptp-build"
BUILD="$STAGE/linux/build"

LOG=$(mktemp "${TMPDIR:-/tmp}/gptp-build.XXXXXX")
WARN=$(mktemp "${TMPDIR:-/tmp}/gptp-warnings.XXXXXX")

cleanup() {
	rm -f "$LOG" "$WARN"
}
trap cleanup EXIT HUP INT TERM

test -f "$SUBMODULE/common/common_port.cpp" || {
	echo "ERROR: $SUBMODULE is empty - run: git submodule update --init --recursive" >&2
	exit 1; }

# Stage pristine submodule (only common/ + linux/ are needed to build daemon_cl).
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -a "$SUBMODULE/common" "$STAGE/"
cp -a "$SUBMODULE/linux"  "$STAGE/"

# Apply the overlay.  -p1 so a/common/... maps onto the staged tree root.
if [ -d "$PATCHES" ]; then
	for p in "$PATCHES"/*.patch; do
		[ -e "$p" ] || continue
		echo "applying $(basename "$p")"
		( cd "$STAGE" && patch -p1 < "$p" )
	done
fi

# gptp is a pristine submodule with no incremental warning tracking, so force a
# full rebuild to guarantee every warning is (re)emitted for the gate - matching
# the always-from-scratch behaviour of the kernel-module / OpenAvnu daemon gates.
make -C "$BUILD" clean >/dev/null 2>&1 || true
if ! make -C "$BUILD" >"$LOG" 2>&1; then
	cat "$LOG" >&2
	exit 1
fi

awk '
/warning:/ {
	line = $0
	sub(/:[0-9]+:[0-9]+: warning:/, ": warning:", line)
	sub(/^.*\//, "", line)
	print line
}
' "$LOG" | sed -e "s/[‘’]/'/g" -e 's/[“”]/"/g' | LC_ALL=C sort >"$WARN"

if [ ! -f "$BASELINE" ]; then
	echo "missing warning baseline: $BASELINE" >&2
	exit 1
fi

if ! diff -u "$BASELINE" "$WARN"; then
	echo "warning gate failed" >&2
	exit 1
fi

count=$(wc -l <"$WARN")
echo "** built $BUILD/obj/daemon_cl"
echo "** warning gate: $count warnings matched baseline"
