#!/bin/sh
# Put the owner-approved homebrew games where rsos-homebrew looks for them
# (homebrew/license ok/ at the top of the repository, gitignored).
#   scripts/ci/stage-homebrew.sh <checkout of PaddleStroke/RetroStoneOS-homebrew>
# The private repository may hold the files at its root, or a "license ok/"
# folder (a copy of the whole local homebrew/ folder): the latter wins. Only
# regular files and folders are copied, never .git. The files are checked
# against package/rsos-homebrew/rsos-homebrew.hash by the package itself; this
# only reports what is missing, so that a wrong upload fails early and clearly.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SRC=${1:?usage: stage-homebrew.sh <directory>}
[ -d "$SRC/license ok" ] && SRC="$SRC/license ok"
DEST="$REPO/homebrew/license ok"
HASH=$REPO/buildroot-external/package/rsos-homebrew/rsos-homebrew.hash

mkdir -p "$DEST"
(cd "$SRC" && tar --exclude='./.git' --exclude='./.github' -cf - .) | (cd "$DEST" && tar -xf -)

missing=0
LIST=$(mktemp)
sed -n 's/^sha256  *[0-9a-f]\{64\}  *\(.*[^ ]\) *$/\1/p' "$HASH" > "$LIST"
while IFS= read -r f; do
	if [ ! -f "$DEST/$f" ]; then
		echo "stage-homebrew: missing $f"
		missing=$((missing + 1))
	fi
done < "$LIST"
rm -f "$LIST"
n=$(find "$DEST" -type f | wc -l)
if [ "$missing" != 0 ]; then
	echo "stage-homebrew: $missing file(s) of rsos-homebrew.hash missing in the homebrew repository" >&2
	exit 1
fi
echo "stage-homebrew: $n files in $DEST, every file of rsos-homebrew.hash present"
