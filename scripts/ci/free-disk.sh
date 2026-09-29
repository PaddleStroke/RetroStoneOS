#!/bin/sh
# GitHub-hosted Ubuntu runner: remove the preinstalled SDKs a Buildroot build
# does not need (Android, .NET, Haskell, Swift, CodeQL, browsers, Docker
# images; ~30 GB) and pick the work directory: /mnt when that disk has more
# room, else ~/rsos-ci. Appends RSOS_CI_WORK=... to $GITHUB_ENV when set.
# The runner's swap is kept (16 GB of RAM, and the LLVM/Clang links of the
# Orange Pi 5 build use a lot); without about 4 GiB of swap, a 4 GiB
# swapfile is added on the work disk.
# Only for CI runners: it deletes system directories with sudo.
set -u
[ "${GITHUB_ACTIONS:-}" = true ] || [ "${RSOS_CI_FORCE_FREE_DISK:-}" = 1 ] || {
	echo "free-disk.sh: only runs on a CI runner (set RSOS_CI_FORCE_FREE_DISK=1 to force)" >&2
	exit 1
}
echo "before:"
df -h / /mnt 2>/dev/null
t0=$(date +%s)
sudo rm -rf \
	/usr/local/lib/android \
	/usr/share/dotnet \
	/opt/ghc /usr/local/.ghcup \
	/usr/share/swift \
	/opt/hostedtoolcache/CodeQL \
	/usr/local/share/boost \
	/usr/local/share/powershell \
	/usr/local/share/chromium \
	/opt/microsoft /opt/google /opt/az \
	/usr/local/lib/node_modules \
	/usr/lib/jvm \
	/usr/share/miniconda \
	2>/dev/null
command -v docker > /dev/null && sudo docker image prune -a -f > /dev/null 2>&1
echo "after ($(($(date +%s) - t0)) s):"
df -h / /mnt 2>/dev/null

avail() { df -Pk "$1" 2>/dev/null | awk 'NR == 2 { print int($4 / 1048576) }'; }
root=$(avail /)
mnt=$(avail /mnt || echo 0)
: "${mnt:=0}"
if [ "$mnt" -gt "$root" ] && sudo mkdir -p /mnt/rsos-ci && sudo chown "$(id -u):$(id -g)" /mnt/rsos-ci; then
	WORK=/mnt/rsos-ci
else
	WORK=$HOME/rsos-ci
	mkdir -p "$WORK"
fi

swap_mib=$(awk '/^SwapTotal:/ { print int($2 / 1024) }' /proc/meminfo)
if [ "${swap_mib:-0}" -lt 3500 ] && [ "$(avail "$WORK")" -ge 45 ]; then
	SW=$(dirname "$WORK")/rsos-swapfile
	if sudo fallocate -l 4G "$SW" && sudo chmod 600 "$SW" && sudo mkswap "$SW" > /dev/null && sudo swapon "$SW"; then
		echo "swap: added $SW (4 GiB)"
	else
		sudo rm -f "$SW"
		echo "swap: could not add $SW"
	fi
fi
swapon --show 2>/dev/null || true

echo "work directory: $WORK ($(avail "$WORK") GiB free)"
[ -n "${GITHUB_ENV:-}" ] && echo "RSOS_CI_WORK=$WORK" >> "$GITHUB_ENV"
[ "$(avail "$WORK")" -ge 25 ] ||
	echo "::warning::only $(avail "$WORK") GiB free for the build (about 20-25 GiB needed per board, 35 for the Orange Pi 5)"
exit 0
