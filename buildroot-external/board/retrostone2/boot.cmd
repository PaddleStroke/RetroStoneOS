# RetroStoneOS boot script (A/B). post-build.sh compiles it to /boot/boot.scr
# in each root filesystem. U-Boot's bootcmd runs the one of the current slot
# (falling back to the other slot's copy if it cannot be loaded).
#
# Boot state, in the redundant U-Boot environment (raw, at 1 MiB on the card;
# fw_printenv/fw_setenv from Linux; docs/build.md "A/B slots"):
#   rsos_slot      a|b  slot to boot (default a)
#   rsos_ok        1    the slot is confirmed (it reached a stable menu once)
#                  0    freshly updated slot, on trial
#   rsos_tries     N    trial slot: boots left before falling back
#   rsos_fails     N    confirmed slot: boots in a row that did not reach
#                       rsos-boot-ok (panic, hang, power cut before the
#                       stability window)
#   rsos_fallback  a|b  the slot this one fell back from (no ping-pong);
#                       cleared when the running slot is confirmed
#   rsos_maxfails  N    fallback threshold for a confirmed slot (default 3;
#                       0 = never count, no environment write on a normal
#                       boot)
#
# Every boot is counted before the kernel starts, and Linux undoes the count:
#   - trial slot (rsos_ok=0): rsos_tries - 1 at each boot; at 0, fall back;
#   - confirmed slot: rsos_fails + 1 at each boot; once it has reached
#     rsos_maxfails, fall back.
#   /usr/bin/rsos-boot-ok confirms the running slot once the menu is up, the
#   network drivers are loaded and 30 s have passed without a problem
#   (rsos_ok=1 rsos_tries=0 rsos_fails=0, rsos_fallback cleared); an orderly
#   shutdown before that (rcK: "rsos-boot-ok refund") gives the try back. So a
#   kernel that panics (panic=10), hangs (hardware watchdog, lockup
#   detectors) or loses power before the window, N times in a row, makes
#   U-Boot boot the other slot, even if it was confirmed before.
#   Cost: one saveenv here per counted boot (+ one fw_setenv from Linux about
#   40 s later, in the background). TODO(hw): measure the saveenv time
#   (bootstage marks "boot.scr" -> "env-saved").
# Fall back = switch to the other slot if it has a kernel, mark it confirmed
# and remember rsos_fallback; if the other slot is the one that failed
# before (or has no kernel), stay and count again from 1.
# The kernel command line says rsos.boot=pending when this boot was counted.
# If the kernel or the device tree cannot be loaded, or bootz returns, the
# script switches to the other slot at once (reset), or powers off when no
# slot can be booted: it never stops at a U-Boot prompt.
#
# Updater contract: write the inactive slot, then in one "fw_setenv -s":
#   rsos_slot <new>, rsos_ok 0, rsos_tries 3, rsos_fails 0, rsos_fallback (empty)
# Counters: 1..9 (setexpr works in hex).
# The saved environment is a full copy of U-Boot's (including bootcmd and the
# transient "silent", which keeps the console quiet from the environment load
# on): an updater that replaces U-Boot must rewrite it, keeping the rsos_*
# variables. Updates never touch U-Boot itself (not A/B).
#
# Optional settings, also in the U-Boot environment (fw_setenv):
#   rsos_overlays   e.g. "emmc sata": /boot/overlays/<name>.dtbo to apply
#   rsos_extraargs  appended to the kernel command line (e.g. "initcall_debug")
#   rsos_verbose    1: U-Boot prints on the UART (silent otherwise, see bootcmd)
#
# Boot timing: "bootstage mark" records a timestamp (microseconds since
# power-on); Linux gets them all in /proc/device-tree/bootstage/ and the boot
# logger writes rsos/logs/boot<N>/bootstage.txt (docs/build.md, "Boot time").
# An older U-Boot without the command just prints an error and goes on.
#
# The script uses hush local variables (l_*): they are never written to the
# environment. (An environment variable of the same name shadows a hush
# variable, as bootcmd's old "for rsos_p" loop showed on QEMU: hence the
# prefix.)

bootstage mark boot.scr

l_save=0
# First boot (no saved environment yet): store the defaults, so that
# fw_setenv in Linux always finds a valid environment written by this U-Boot
# (otherwise it would start from its own built-in default environment,
# including a foreign bootcmd).
if test -z "${rsos_slot}"; then
	setenv rsos_slot a
	setenv rsos_ok 1
	setenv rsos_tries 0
	l_save=1
fi

if test "${rsos_slot}" != "b"; then
	setenv rsos_slot a
fi
if test "${rsos_ok}" != "0"; then
	setenv rsos_ok 1
fi
l_maxfails=3
if test -n "${rsos_maxfails}"; then
	l_maxfails=${rsos_maxfails}
fi

l_fallback=0
l_state=ok
if test "${rsos_ok}" = "0"; then
	if test -n "${rsos_tries}" && test ${rsos_tries} -gt 0; then
		setexpr rsos_tries ${rsos_tries} - 1
		echo "rsos: trying unconfirmed slot ${rsos_slot}, ${rsos_tries} tries left after this one"
	else
		l_fallback=1
	fi
	l_save=1
	l_state=pending
elif test ${l_maxfails} -gt 0; then
	if test -z "${rsos_fails}"; then
		setenv rsos_fails 0
	fi
	if test ${rsos_fails} -ge ${l_maxfails}; then
		l_fallback=1
	else
		setexpr rsos_fails ${rsos_fails} + 1
	fi
	l_save=1
	l_state=pending
fi

if test "${rsos_slot}" = "b"; then
	l_other=a
	l_opart=2
else
	l_other=b
	l_opart=3
fi
if test ${l_fallback} = 1; then
	if test "${rsos_fallback}" = "${l_other}"; then
		echo "rsos: slot ${rsos_slot} fails too, and slot ${l_other} failed before: staying on ${rsos_slot}"
	elif test -e mmc 0:${l_opart} /boot/zImage; then
		echo "rsos: slot ${rsos_slot} failed too often, falling back to slot ${l_other}"
		setenv rsos_fallback ${rsos_slot}
		setenv rsos_slot ${l_other}
	else
		echo "rsos: slot ${l_other} has no kernel: staying on ${rsos_slot}"
	fi
	# This boot is the first counted boot of the slot now selected.
	setenv rsos_ok 1
	setenv rsos_tries 0
	setenv rsos_fails 1
fi

if test "${rsos_slot}" = "b"; then
	l_part=3
	l_other=a
	l_opart=2
else
	l_part=2
	l_other=b
	l_opart=3
fi

if test ${l_save} = 1; then
	# (the bootcmd of older images left its variable rsos_p there)
	setenv rsos_p
	saveenv
	bootstage mark env-saved
fi

# "quiet": printing the whole kernel log on the 115200 baud UART cost ~1.4 s
# on the first hardware boot. The full log is still in dmesg (and in
# RETROSTONE/rsos/logs/). For a verbose console: fw_setenv rsos_extraargs loglevel=7
# driver_async_probe: probe these drivers in a worker thread, in parallel with
# the rest of the boot (the microSD and the root filesystem mount do not wait
# for them): the two UARTs (console, Bluetooth serdev), the OTG controller
# (host-only port) and the thermal sensor. The EHCI/OHCI and SD/SDIO host
# drivers already probe asynchronously. The audio codec is left out on
# purpose: it must stay ALSA card 0 (rcS, the frontend), and an asynchronous
# probe could let the HDMI audio card take that number.
# axp20x-i2c (boot round 3): the AXP209 MFD probe (regulators, ADC, battery,
# power key; tens of I2C transfers) ran synchronously inside the I2C
# controller's initcall. Nothing on the way to the root filesystem and init
# needs it (the SD card is on the fixed 3.3 V rail, rsos-bootreason reads the
# AXP through /dev/i2c-0); its consumers (cpufreq, the analog stick) wait for
# it through deferred probing.
#
# DEV IMAGE ONLY (boot round 3): "initcall_debug log_buf_len=1M" make the
# kernel log every initcall and driver probe with its duration (at debug
# level: the console stays quiet) and the boot logger writes
# rsos/logs/boot<N>/initcalls.txt. The logging itself costs some time (a few
# tens of ms in total, spread over the initcalls). The release build
# (BR2_RETROSTONE_RELEASE, docs/build.md "Release build") removes both words:
# post-build.sh deletes " initcall_debug log_buf_len=1M" from this line.
setenv bootargs "console=ttyS0,115200 root=/dev/mmcblk0p${l_part} rootfstype=ext4 rootwait ro quiet panic=10 consoleblank=0 driver_async_probe=dw-apb-uart,musb-sunxi,sun4i-ts,axp20x-i2c initcall_debug log_buf_len=1M rsos.slot=${rsos_slot} rsos.boot=${l_state} ${rsos_extraargs}"

bootstage mark load-zImage
if load mmc 0:${l_part} ${kernel_addr_r} /boot/zImage; then
	bootstage mark zImage-loaded
	if load mmc 0:${l_part} ${fdt_addr_r} /boot/sun7i-a20-retrostone2.dtb; then
		bootstage mark dtb-loaded
		if test -n "${rsos_overlays}"; then
			fdt addr ${fdt_addr_r}
			fdt resize 65536
			for l_ov in ${rsos_overlays}; do
				if load mmc 0:${l_part} ${fdtoverlay_addr_r} /boot/overlays/${l_ov}.dtbo; then
					if fdt apply ${fdtoverlay_addr_r}; then
						echo "rsos: overlay ${l_ov} applied"
					else
						# A failed fdt apply leaves the DT in an undefined state
						echo "rsos: overlay ${l_ov} failed, reloading the base DT"
						load mmc 0:${l_part} ${fdt_addr_r} /boot/sun7i-a20-retrostone2.dtb
						fdt addr ${fdt_addr_r}
						fdt resize 65536
					fi
				fi
			done
			bootstage mark overlays-applied
		fi
		bootz ${kernel_addr_r} - ${fdt_addr_r}
	fi
fi

# Only reached when the kernel or the DTB cannot be loaded, or bootz fails
# (missing or corrupt kernel): never stop at a prompt. Switch to the other
# slot once (unless it is the one that failed before, or has no kernel),
# else power off.
setenv silent
echo "rsos: cannot boot slot ${rsos_slot} (partition ${l_part})"
if test "${rsos_fallback}" != "${l_other}" && test -e mmc 0:${l_opart} /boot/zImage; then
	echo "rsos: switching to slot ${l_other}"
	setenv rsos_fallback ${rsos_slot}
	setenv rsos_slot ${l_other}
	setenv rsos_ok 1
	setenv rsos_tries 0
	setenv rsos_fails 0
	setenv rsos_p
	setenv silent 1
	saveenv
	reset
fi
echo "rsos: no slot can be booted, powering off"
poweroff
