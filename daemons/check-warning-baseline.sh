#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BASELINE=${WARN_BASELINE_FILE:-"$ROOT/warning-baseline.txt"}
LOG=$(mktemp "${TMPDIR:-/tmp}/shaper-build.XXXXXX")
WARN=$(mktemp "${TMPDIR:-/tmp}/shaper-warnings.XXXXXX")

cleanup() {
	rm -f "$LOG" "$WARN"
}
trap cleanup EXIT HUP INT TERM

# Build every daemon in ONE make process so `stage` runs exactly once and never
# re-runs its `rm -rf` against a sibling daemon that was already built (which is
# what left build/daemons/mrpd/mrpd missing at install time).
if ! make -C "$ROOT" all >"$LOG" 2>&1; then
	cat "$LOG" >&2
	exit 1
fi

# Only shaper carries a patch overlay, so only its warnings are gated. mrpd and
# maap are staged/built pristine and their upstream warnings are not baselined.
awk '
/warning:/ {
	line = $0
	sub(/:[0-9]+:[0-9]+: warning:/, ": warning:", line)
	sub(/^.*\//, "", line)
	if (line ~ /^shaper/)
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
echo "** built $ROOT/OpenAvnu/build/daemons/shaper/shaper_daemon"
echo "** warning gate: $count warnings matched baseline"