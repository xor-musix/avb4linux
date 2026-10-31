#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 mnemonic powerplay <mnemonic@xor-musix.de>
#
# Warn when a submodule is no longer pinned to the latest upstream commit, so
# the dev knows to check the upstream repo for important updates. "Latest" is
# the tip of submodule.<name>.branch from .gitmodules, or the remote's default
# branch (HEAD) when none is set.
#
# Advisory only: always exits 0. Remotes are queried in parallel with
# git ls-remote; an unreachable remote is reported and skipped.
# Set AVB_SKIP_SUBMODULE_CHECK=1 to skip the check (e.g. offline builds).
set -u

[ -n "${AVB_SKIP_SUBMODULE_CHECK:-}" ] && exit 0

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TIMEOUT=${AVB_SUBMODULE_CHECK_TIMEOUT:-15}

cd "$ROOT" || exit 0
# Not a git checkout (e.g. a source tarball): nothing to compare.
git rev-parse --git-dir >/dev/null 2>&1 || exit 0
[ -f .gitmodules ] || exit 0

TMP=$(mktemp -d "${TMPDIR:-/tmp}/avb-submodules.XXXXXX")
cleanup() {
	rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

if command -v timeout >/dev/null 2>&1; then
	run_timeout() { timeout "$TIMEOUT" "$@"; }
else
	run_timeout() { "$@"; }
fi

# One background ls-remote per submodule; results land in $TMP/<n>.
n=0
while read -r key path; do
	[ -n "$path" ] || continue
	name=${key#submodule.}
	name=${name%.path}
	url=$(git config -f .gitmodules "submodule.$name.url")
	branch=$(git config -f .gitmodules "submodule.$name.branch" || true)
	# The pin is the gitlink in the index, i.e. what the next commit records.
	pinned=$(git ls-files -s -- "$path" | awk '$1 == "160000" { print $2 }')
	[ -n "$pinned" ] || continue
	case $branch in
	"" | .) ref=HEAD ;;
	*) ref=refs/heads/$branch ;;
	esac
	n=$((n + 1))
	(
		upstream=$(GIT_TERMINAL_PROMPT=0 run_timeout \
			git ls-remote "$url" "$ref" 2>/dev/null | awk 'NR == 1 { print $1 }')
		printf '%s %s %s %s %s\n' "$path" "$url" "$ref" "$pinned" "${upstream:--}" >"$TMP/$n"
	) &
done <<EOF
$(git config -f .gitmodules --get-regexp '^submodule\..*\.path$')
EOF
wait

total=0
latest=0
i=0
while [ "$i" -lt "$n" ]; do
	i=$((i + 1))
	read -r path url ref pinned upstream <"$TMP/$i" || continue
	if [ "$upstream" = - ]; then
		echo "** submodule check: could not reach $url, skipped $path"
		continue
	fi
	total=$((total + 1))
	if [ "$pinned" = "$upstream" ]; then
		latest=$((latest + 1))
		continue
	fi
	# Tag name and commit count are only known if the submodule is checked out
	# (and, for the count, has already fetched the upstream commit).
	desc=
	behind=
	if [ -e "$path/.git" ]; then
		desc=$(git -C "$path" describe --tags "$pinned" 2>/dev/null) && desc=" ($desc)"
		behind=$(git -C "$path" rev-list --count "$pinned..$upstream" 2>/dev/null) &&
			behind=", $behind commits behind"
	fi
	short_pinned=$(printf '%.12s' "$pinned")
	short_upstream=$(printf '%.12s' "$upstream")
	echo "WARNING: submodule $path is pinned to $short_pinned$desc, upstream $ref is $short_upstream$behind" >&2
	echo "         check $url for important updates" >&2
done

[ "$total" -gt 0 ] && echo "** submodule check: $latest of $total submodules pinned to latest upstream"
exit 0
