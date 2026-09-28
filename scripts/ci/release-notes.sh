#!/bin/sh
# Render the release notes from .github/release-notes.md.
#   scripts/ci/release-notes.sh <tag> <dist directory> > notes.md
# <dist directory> holds what the build jobs produced: *.img.xz, *.info (one
# per board, from build-board.sh package), legal-info-*.
# Placeholders of the template:
#   inline:            {{VERSION}} {{TAG}} {{DATE}} {{REPO_URL}} {{COMMIT}} {{BUILDROOT}}
#   on their own line: {{PRERELEASE}} {{BOARDS_TABLE}} {{HOMEBREW}} {{UPDATES}} {{LEGAL_FILES}} {{CHANGELOG}}
# The changelog is the commit list since the previous v* tag (needs the full
# history: actions/checkout with fetch-depth 0).
set -eu
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
# shellcheck disable=SC1091
. "$HERE/buildroot.env"
TAG=${1:?usage: release-notes.sh <tag> <dist directory>}
DIST=$(cd "${2:?dist directory}" && pwd)
TEMPLATE=${RSOS_CI_NOTES_TEMPLATE:-$REPO/.github/release-notes.md}
VERSION=${TAG#v}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

mib() { awk -v b="$1" 'BEGIN { printf "%.0f MiB", b / 1048576 }'; }

# ---- boards table (hardware-tested boards first)
: > "$T/rows"
hb_yes=
hb_no=
for info in "$DIST"/*.info; do
	[ -f "$info" ] || continue
	(
		# shellcheck disable=SC1090
		. "$info"
		case $STATUS in "tested on hardware") o=0 ;; *) o=1 ;; esac
		if [ "$HOMEBREW" = y ]; then hb=yes; else hb=no; fi
		# shellcheck disable=SC2016 # Markdown backquotes
		printf '%s\t| %s | `%s` | %s | %s | %s |\n' "$o$NAME" "$NAME" "$FILE" "$(mib "$SIZE")" \
			"$STATUS" "$hb" >> "$T/rows"
		echo "$HOMEBREW $NAME" >> "$T/hb"
	)
done
[ -s "$T/rows" ] || { echo "release-notes: no *.info in $DIST" >&2; exit 1; }
{
	echo "| Board | Image | Download size | Status | Homebrew games |"
	echo "|---|---|---|---|---|"
	sort "$T/rows" | cut -f 2-
} > "$T/BOARDS_TABLE"
hb_yes=$(grep -c '^y ' "$T/hb" || true)
hb_no=$(grep -c '^n ' "$T/hb" || true)
if [ "$hb_no" = 0 ]; then
	echo "- A few free homebrew games come with the images: the first boot copies them to \`roms/\`." > "$T/HOMEBREW"
elif [ "$hb_yes" = 0 ]; then
	echo "- These images were built **without** the bundled homebrew games; \`roms/\` starts empty." > "$T/HOMEBREW"
else
	# shellcheck disable=SC2016 # Markdown backquotes
	printf -- '- The bundled homebrew games (copied to `roms/` at the first boot) are only in some images: %s.\n' \
		"$(sed 's/^y \(.*\)/\1: yes/; s/^n \(.*\)/\1: no/' "$T/hb" | paste -sd ';' - | sed 's/;/; /g')" > "$T/HOMEBREW"
fi

# ---- system update packages (docs/updates.md)
{
	found=0
	for info in "$DIST"/*.info; do
		[ -f "$info" ] || continue
		(
			RSU=
			RSU_SIGNED=no
			RSU_SIZE=0
			# shellcheck disable=SC1090
			. "$info"
			[ -n "$RSU" ] || exit 1
			if [ "$RSU_SIGNED" = yes ]; then s=""; else s=" (**not signed**: only development builds install it)"; fi
			echo "- $NAME: \`$RSU\` ($(mib "$RSU_SIZE"))$s"
		) && found=1
	done
	if [ "$found" = 0 ]; then
		echo "- (no update package in this release: flash the image)"
	fi
	true
} > "$T/UPDATES"

# ---- source offer files
{
	found=0
	for f in "$DIST"/legal-info-*; do
		[ -f "$f" ] || continue
		case $f in *.sha256) continue ;; esac
		found=1
		echo "- \`${f##*/}\` ($(mib "$(stat -c %s "$f")"))"
	done
	[ "$found" = 1 ] || echo "- (no legal-info archive in this release)"
	for f in "$DIST"/*-source.tar.*; do
		[ -f "$f" ] && echo "- \`${f##*/}\`: source of a bundled GPL homebrew game"
	done
	true
} > "$T/LEGAL_FILES"

# ---- pre-release
case $TAG in
*-rc* | *-beta* | *-alpha*)
	echo "> **Pre-release** ($TAG): for testers. Expect rough edges; the latest stable release is on the Releases page." > "$T/PRERELEASE" ;;
*) : > "$T/PRERELEASE" ;;
esac

# ---- changelog
PREV=$(git -C "$REPO" describe --tags --abbrev=0 --match 'v*' "$TAG^" 2>/dev/null || true)
{
	if [ -n "$PREV" ]; then
		echo "Changes since [$PREV](${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-PaddleStroke/RetroStoneOS}/compare/$PREV...$TAG):"
		echo
		git -C "$REPO" log --no-merges --pretty='- %s (%h)' "$PREV..$TAG"
	else
		echo "First release. Latest commits:"
		echo
		git -C "$REPO" log --no-merges --pretty='- %s (%h)' -n 100 "$TAG"
	fi
} > "$T/CHANGELOG" 2>/dev/null || echo "(changelog unavailable)" > "$T/CHANGELOG"

COMMIT=$(git -C "$REPO" rev-parse --short "$TAG^{commit}" 2>/dev/null || git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)
REPO_URL=${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-PaddleStroke/RetroStoneOS}

VERSION=$VERSION TAG=$TAG DATE=$(date -u +%Y-%m-%d) REPO_URL=$REPO_URL COMMIT=$COMMIT \
	BUILDROOT=$BR_VERSION BLOCKS=$T awk '
function repl(s, k, v,    out, i) {
	out = ""
	while ((i = index(s, k)) > 0) { out = out substr(s, 1, i - 1) v; s = substr(s, i + length(k)) }
	return out s
}
{
	line = $0
	if (line ~ /^\{\{[A-Z_]+\}\}$/) {
		f = ENVIRON["BLOCKS"] "/" substr(line, 3, length(line) - 4)
		while ((getline l < f) > 0) print l
		close(f)
		next
	}
	n = split("VERSION TAG DATE REPO_URL COMMIT BUILDROOT", keys, " ")
	for (j = 1; j <= n; j++) line = repl(line, "{{" keys[j] "}}", ENVIRON[keys[j]])
	print line
}' "$TEMPLATE"
