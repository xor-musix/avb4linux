#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BASELINE=${WARN_BASELINE_FILE:-"$ROOT/warning-baseline.txt"}
LOG=$(mktemp "${TMPDIR:-/tmp}/snd-igb-avb-build.XXXXXX")
WARN=$(mktemp "${TMPDIR:-/tmp}/snd-igb-avb-warnings.XXXXXX")

cleanup() {
	rm -f "$LOG" "$WARN"
}
trap cleanup EXIT HUP INT TERM

if ! make -C "$ROOT" modules >"$LOG" 2>&1; then
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
echo "** built $ROOT/build/snd_igb_avb.ko"
echo "** warning gate: $count warnings matched baseline"
