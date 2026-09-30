#!/bin/sh
# RetroStoneOS post-build script, the part every board shares. Run by
# Buildroot on the target directory before the root filesystem image is
# made, before the board's own post-build script (the defconfig lists both:
# BR2_ROOTFS_POST_BUILD_SCRIPT="board/common/post-build.sh board/<board>/post-build.sh").
# Called as: post-build.sh <TARGET_DIR>; HOST_DIR, BINARIES_DIR... are in the
# environment.
set -e

TARGET="$1"
COMMON_DIR="$(cd "$(dirname "$0")" && pwd)"

# --- Mount point of the user data partition (the root filesystem is read-only)
mkdir -p "$TARGET/data"

# --- Modes of the overlay files (Windows checkouts lose the executable bit)
for f in etc/init.d/rcS etc/init.d/rcK usr/bin/rsos-net usr/bin/rsos-boot-ok \
	usr/libexec/rsos/data-partition usr/libexec/rsos/bootlog usr/libexec/rsos/uart-shell \
	usr/share/udhcpc/default.script.d/rsos-ntp; do
	chmod 0755 "$TARGET/$f"
done
chmod 0644 "$TARGET/etc/inittab" "$TARGET/etc/fstab" "$TARGET/usr/libexec/rsos/board.sh"
[ -f "$TARGET/etc/fw_env.config" ] && chmod 0644 "$TARGET/etc/fw_env.config"

# --- The board profile (/etc/rsos/board.ini, from the board's rootfs
# overlay; docs/porting.md) as shell variables for the init scripts:
# /etc/rsos/board.env, "key = value" -> RSOS_BOARD_<KEY>='value', read by
# /usr/libexec/rsos/board.sh without parsing the ini at boot.
mkdir -p "$TARGET/etc/rsos"
{
	echo "# Generated at build time from /etc/rsos/board.ini (board/common/post-build.sh)."
	if [ -f "$TARGET/etc/rsos/board.ini" ]; then
		chmod 0644 "$TARGET/etc/rsos/board.ini"
		tr -d '\r' < "$TARGET/etc/rsos/board.ini" | awk '
			/^[ \t]*[#;[]/ { next }
			index($0, "=") == 0 { next }
			{
				k = substr($0, 1, index($0, "=") - 1)
				v = substr($0, index($0, "=") + 1)
				gsub(/^[ \t]+|[ \t]+$/, "", k)
				sub(/[ \t]+[#;].*$/, "", v)
				gsub(/^[ \t]+|[ \t]+$/, "", v)
				if (v ~ /^".*"$/)
					v = substr(v, 2, length(v) - 2)
				if (k !~ /^[A-Za-z0-9_]+$/)
					next
				gsub(/\047/, "", v)
				printf "RSOS_BOARD_%s=\047%s\047\n", toupper(k), v
			}'
	else
		echo "# (no board.ini: every value is auto-detected)"
	fi
} > "$TARGET/etc/rsos/board.env"
chmod 0644 "$TARGET/etc/rsos/board.env"
[ -f "$TARGET/etc/rsos/board-hooks.sh" ] && chmod 0644 "$TARGET/etc/rsos/board-hooks.sh"
SERIAL_CONSOLE=$(sed -n "s/^RSOS_BOARD_SERIAL_CONSOLE='\(.*\)'$/\1/p" "$TARGET/etc/rsos/board.env")

# --- Build variant (buildroot-external/Config.in, "RetroStoneOS build";
# docs/build.md "Release build"): /etc/rsos/build.env, read by the boot
# logger and the UART shell wrapper.
# cfg NAME: the value of a Buildroot option (without the quotes of a string)
cfg() {
	sed -n "s/^$1=\"\{0,1\}\([^\"]*\)\"\{0,1\}\$/\1/p" "$BR2_CONFIG" 2> /dev/null
}
RELEASE=0
[ "$(cfg BR2_RETROSTONE_RELEASE)" = y ] && RELEASE=1
UART_SHELL=open
[ "$(cfg BR2_RETROSTONE_UART_SHELL_PASSWORD)" = y ] && UART_SHELL=password
[ "$(cfg BR2_RETROSTONE_UART_SHELL_DISABLED)" = y ] && UART_SHELL=disabled
[ "$SERIAL_CONSOLE" = none ] && UART_SHELL=disabled
{
	echo "# Generated at build time (board/common/post-build.sh)."
	echo "RSOS_RELEASE=$RELEASE"
	echo "RSOS_UART_SHELL=$UART_SHELL"
} > "$TARGET/etc/rsos/build.env"
chmod 0644 "$TARGET/etc/rsos/build.env"

# --- Shell on the debug UART (/usr/libexec/rsos/uart-shell): the board's
# serial_console (ttyS0 by default); "open" = a root shell, "password" = a
# root login (the password is set here), "disabled" = no inittab line.
case "$UART_SHELL" in
disabled)
	sed -i '/^[A-Za-z0-9]*::respawn:\/usr\/libexec\/rsos\/uart-shell$/d' "$TARGET/etc/inittab"
	;;
*)
	case "$SERIAL_CONSOLE" in
	"" | ttyS0) ;;
	*) sed -i "s|^ttyS0::respawn:/usr/libexec/rsos/uart-shell\$|$SERIAL_CONSOLE::respawn:/usr/libexec/rsos/uart-shell|" \
		"$TARGET/etc/inittab" ;;
	esac
	;;
esac
if [ "$UART_SHELL" = password ]; then
	PW=$(cfg BR2_RETROSTONE_UART_PASSWORD)
	if [ -z "$PW" ] || [ "$PW" = CHANGE-ME ]; then
		echo "post-build.sh: set BR2_RETROSTONE_UART_PASSWORD (menuconfig: External options >" \
			"RetroStoneOS build) for an image with a password on the UART (docs/build.md, \"Release build\")" >&2
		exit 1
	fi
	HASH=$("$HOST_DIR/bin/mkpasswd" -m sha-512 "$PW")
	case "$HASH" in
	'$6$'*) ;;
	*) echo "post-build.sh: mkpasswd failed" >&2; exit 1 ;;
	esac
	sed -i "s|^root:[^:]*:|root:$HASH:|" "$TARGET/etc/shadow"
fi

# --- Start the frontend (respawned by init) when it is part of the image.
# It goes right after the ::sysinit line, before the UART shell: BusyBox init
# starts the respawn entries in file order once rcS has finished, so the menu
# is forked first and the shell's start-up does not delay it. (Only the
# respawn line is removed first, not the comments that mention it.) Through
# /usr/libexec/rsos/frontend-respawn: no endless respawn of a crashing menu.
sed -i -e '/^::respawn:\/usr\/bin\/rsos-frontend$/d' \
	-e '/^::respawn:\/usr\/libexec\/rsos\/frontend-respawn$/d' "$TARGET/etc/inittab"
if [ -x "$TARGET/usr/bin/rsos-frontend" ]; then
	sed -i '/^::sysinit:/a ::respawn:/usr/libexec/rsos/frontend-respawn' "$TARGET/etc/inittab"
	grep -q '^::respawn:/usr/libexec/rsos/frontend-respawn$' "$TARGET/etc/inittab"
fi

# --- roms/<system> folders for a fresh data partition: every system listed by
# an installed core ("systems =" in the [core] section of
# /usr/share/rsos/cores/*.ini), plus folders for systems whose cores are on
# their way. Read by /usr/libexec/rsos/data-partition. A core whose games are
# built in ("no_content = true", the RetroStone VC games) has no ROM folder:
# its entries are in /usr/share/rsos/games/<system>/ (docs/vc-games.md).
EXTRA_ROM_FOLDERS="atari2600 pcengine"
mkdir -p "$TARGET/usr/share/rsos"
{
	for ini in "$TARGET"/usr/share/rsos/cores/*.ini; do
		[ -f "$ini" ] || continue
		if grep -Eq '^[[:space:]]*no_content[[:space:]]*=[[:space:]]*(true|yes|1)' "$ini"; then
			continue
		fi
		awk '
			/^\[/ { core = ($0 == "[core]") }
			core && /^[ \t]*systems[ \t]*=/ {
				sub(/^[^=]*=/, ""); gsub(/[ \t]/, ""); n = split($0, s, ",")
				for (i = 1; i <= n; i++) if (s[i] != "") print s[i]
			}' "$ini"
	done
	for s in $EXTRA_ROM_FOLDERS; do
		echo "$s"
	done
} | sort -u > "$TARGET/usr/share/rsos/rom-folders"

# --- README.txt of the data partition (seed image and first boot). Normally
# installed by the frontend package; fall back to its source.
README_SRC="$COMMON_DIR/../../../frontend/src/transfer/data-README.txt"
if [ ! -f "$TARGET/usr/share/rsos/data-README.txt" ] && [ -f "$README_SRC" ]; then
	install -D -m 0644 "$README_SRC" "$TARGET/usr/share/rsos/data-README.txt"
fi

# --- /media -> /run/media: USB sticks are mounted there by the frontend
# (read-only root, docs/rom-transfer.md C4)
rm -rf "$TARGET/media"
ln -s /run/media "$TARGET/media"

# --- BlueZ state: a mount point for the ext4 loop image
# /data/rsos/bluetooth.img that rsos-net mounts while Bluetooth is on
# (exFAT cannot hold BlueZ's "AA:BB:..." names).
if [ -d "$TARGET/usr/libexec/bluetooth" ]; then
	rm -rf "$TARGET/var/lib/bluetooth"
	mkdir -p "$TARGET/var/lib/bluetooth"
	chmod 0700 "$TARGET/var/lib/bluetooth"
fi
