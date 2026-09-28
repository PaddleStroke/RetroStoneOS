#!/bin/sh
# Run shellcheck on the shell scripts of the tree:
#   buildroot-external/board/**/*.sh, every script with a sh/bash shebang in
#   a rootfs-overlay or package directory, and scripts/ci/*.sh.
#   scripts/ci/shellcheck.sh [--strict] [--severity=<level>]
# Without --strict it reports and exits 0 (the findings of the existing
# scripts are warnings for now, docs/ci.md), except for scripts/ci/ itself,
# which must stay clean. On GitHub each finding becomes an annotation.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
STRICT=0
SEV=style
for a in "$@"; do
	case $a in
	--strict) STRICT=1 ;;
	--severity=*) SEV=${a#--severity=} ;;
	*) echo "shellcheck.sh: unknown option $a" >&2; exit 2 ;;
	esac
done
command -v shellcheck > /dev/null || { echo "shellcheck is not installed" >&2; exit 2; }
cd "$REPO" || exit 2

LIST=$(mktemp)
trap 'rm -f "$LIST" "$LIST.out"' EXIT
{
	find buildroot-external/board -type f -name '*.sh'
	find buildroot-external/board buildroot-external/package -type f ! -name '*.sh' \
		! -name '*.mk' ! -name '*.hash' ! -name '*.patch' ! -name 'Config.in*' |
		while IFS= read -r f; do
			case $(head -c 64 "$f" | head -n 1) in
			'#!'*/sh | '#!'*/sh\ * | '#!'*bash*) echo "$f" ;;
			esac
		done
	find scripts/ci -type f -name '*.sh'
} | sort -u > "$LIST"

n=$(wc -l < "$LIST")
# -x: follow sourced files; -f gcc: file:line:col: level: message [SCxxxx]
tr '\n' '\0' < "$LIST" | xargs -0 shellcheck -x -f gcc -S "$SEV" > "$LIST.out" 2>&1
total=$(grep -c ': \(error\|warning\|note\|info\|style\):' "$LIST.out" || true)
ci=$(grep -c '^scripts/ci/' "$LIST.out" || true)
echo "shellcheck ($SEV and above): $n scripts, $total findings ($ci in scripts/ci)"
for l in error warning note; do
	printf '  %-8s %s\n' "$l" "$(grep -c ": $l:" "$LIST.out" || true)"
done
echo "by file:"
cut -d: -f1 "$LIST.out" | sort | uniq -c | sort -rn | sed 's/^/  /'
echo "by check:"
sed -n 's/.*\[\(SC[0-9]*\)\]$/\1/p' "$LIST.out" | sort | uniq -c | sort -rn | head -n 20 | sed 's/^/  /'
if [ "${GITHUB_ACTIONS:-}" = true ]; then
	# file:line:col: level: message  ->  ::warning file=,line=,col=::message
	sed -n 's/^\([^:]*\):\([0-9]*\):\([0-9]*\): \([a-z]*\): \(.*\)$/\4 \1 \2 \3 \5/p' "$LIST.out" |
		while read -r level file line col msg; do
			case $level in error) a=error ;; warning) a=warning ;; *) a=notice ;; esac
			case $file in scripts/ci/*) a=error ;; esac
			echo "::$a file=$file,line=$line,col=$col::$msg"
		done
	{
		echo "### shellcheck"
		echo "$n scripts, **$total findings** ($SEV and above)"
		echo '```'
		cut -d: -f1 "$LIST.out" | sort | uniq -c | sort -rn
		echo '```'
	} >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
else
	cat "$LIST.out"
fi
[ "$ci" = 0 ] || exit 1
[ "$STRICT" = 0 ] || [ "$total" = 0 ]
