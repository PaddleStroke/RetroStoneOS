#!/bin/sh
# Lint .github/workflows/*.yml with actionlint (and shellcheck on their run:
# steps when shellcheck is installed). Uses an actionlint from PATH, else
# downloads the pinned release and checks its sha256.
#   scripts/ci/actionlint.sh
set -eu
V=1.7.12
SHA=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8 # actionlint_1.7.12_linux_amd64.tar.gz
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
AL=$(command -v actionlint || true)
if [ -z "$AL" ]; then
	D=${RSOS_CI_WORK:-$HOME/rsos-ci}/actionlint-$V
	AL=$D/actionlint
	if [ ! -x "$AL" ]; then
		mkdir -p "$D"
		T=$D/actionlint.tar.gz
		wget -q -O "$T" "https://github.com/rhysd/actionlint/releases/download/v$V/actionlint_${V}_linux_amd64.tar.gz"
		echo "$SHA  $T" | sha256sum -c -
		tar -xzf "$T" -C "$D" actionlint
	fi
fi
cd "$REPO"
"$AL" -version | head -n 1
"$AL" -no-color
echo "actionlint: OK"
