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
#   rsos_bad       a|b  an updated slot that never got confirmed (its trial
#                       ran out): U-Boot never falls back to it. Only the
#                       updater's next write of the boot state (a new
#                       system in a slot) clears it, never a confirmation
#   rsos_good      a|b  the last slot that rsos-boot-ok confirmed
#   rsos_rounds    N    slots that ran out of boots (or could not be loaded)
#                       since the last confirmed boot: the failure budget
#   rsos_maxfails  N    fallback threshold for a confirmed slot (default 3;
#                       0 = never count, no environment write on a normal
#                       boot)
#   rsos_maxrounds N    failure budget (default 4, about two full rounds of
#                       both slots): power off when rsos_rounds reaches it
#   rsos_lowvolt   trial|failed  the cpu-lowvolt test overlay (see below)
#
# Every boot is counted before the kernel starts, and Linux undoes the count:
#   - trial slot (rsos_ok=0): rsos_tries - 1 at each boot; at 0, fall back;
#   - confirmed slot: rsos_fails + 1 at each boot; once it has reached
#     rsos_maxfails, fall back.
#   /usr/bin/rsos-boot-ok confirms the running slot once the menu is up, the
#   network drivers are loaded and 30 s have passed without a problem
#   (rsos_ok=1 rsos_tries=0 rsos_fails=0 rsos_good=<slot>, rsos_fallback and
#   rsos_rounds cleared); an orderly shutdown before that (rcK: "rsos-boot-ok
#   refund") gives the try back. So a kernel that panics (panic=10), hangs
#   (hardware watchdog, lockup detectors) or loses power before the window,
#   N times in a row, makes U-Boot boot the other slot, even if it was
#   confirmed before.
#   Cost: one saveenv here per counted boot (+ one fw_setenv from Linux about
#   40 s later, in the background). TODO(hw): measure the saveenv time
#   (bootstage marks "boot.scr" -> "env-saved").
# A slot that runs out of boots ("exhausted") counts one round
# (rsos_rounds + 1); a trial slot (rsos_ok=0) is also marked rsos_bad. Then:
#   - the budget is spent (rsos_rounds = rsos_maxrounds): power off. The next
#     power-on starts a fresh budget on the last confirmed slot (rsos_good);
#   - else switch to the other slot when it has a kernel, is not rsos_bad,
#     and either did not fail before in this episode (rsos_fallback) or is
#     the last confirmed slot (both slots failed: prefer rsos_good); mark it
#     confirmed and remember rsos_fallback;
#   - else stay and count again from 1.
# The kernel command line says rsos.boot=pending when this boot was counted.
# If the kernel or the device tree cannot be loaded, or bootz returns, the
# script switches to the other slot at once (reset) when it has a kernel and
# is not rsos_bad (one round of the budget), or powers off (and resets, in
# case the power-off returns): it never stops at a U-Boot prompt.
#
# Updater contract: write the inactive slot, then in one "fw_setenv -s":
#   rsos_slot <new>, rsos_ok 0, rsos_tries 3, rsos_fails 0, rsos_fallback,
#   rsos_bad and rsos_rounds (empty)
# The protection against a bad slot needs this script in the slot that runs
# (U-Boot sources the selected slot's boot.scr): a slot with an older script
# may still fall back to a slot marked bad.
# Counters: 1..9 (setexpr works in hex).
# The saved environment is a full copy of U-Boot's (including bootcmd and the
# transient "silent", which keeps the console quiet from the environment load
# on): an updater that replaces U-Boot must rewrite it, keeping the rsos_*
# variables. Updates never touch U-Boot itself (not A/B).
#
# Optional settings, also in the U-Boot environment (fw_setenv):
#   rsos_overlays   e.g. "emmc sata": /boot/overlays/<name>.dtbo to apply
#                   "cpu-lowvolt" (a test that may freeze the board) is
#                   one-shot: applied with rsos_lowvolt=trial, which
#                   rsos-boot-ok clears when that boot is confirmed (or given
#                   back); a boot that finds "trial" still set skips it and
#                   sets rsos_lowvolt=failed (skipped until
#                   "fw_setenv rsos_lowvolt")
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
l_maxrounds=4
if test -n "${rsos_maxrounds}"; then
	l_maxrounds=${rsos_maxrounds}
fi
if test -z "${rsos_rounds}"; then
	setenv rsos_rounds 0
fi

l_fallback=0
l_off=0
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
	if test "${rsos_ok}" = "0"; then
		# An update that never got confirmed: never boot it again by
		# falling back (the updater's next write of the slot clears it).
		echo "rsos: updated slot ${rsos_slot} was never confirmed: marking it bad"
		setenv rsos_bad ${rsos_slot}
	fi
	setexpr rsos_rounds ${rsos_rounds} + 1
	if test ${rsos_rounds} -ge ${l_maxrounds}; then
		echo "rsos: ${rsos_rounds} slot failures without a confirmed boot"
		l_off=1
	elif test "${rsos_bad}" = "${l_other}"; then
		echo "rsos: slot ${rsos_slot} failed too often, slot ${l_other} is marked bad: staying on ${rsos_slot}"
	elif test -e mmc 0:${l_opart} /boot/zImage; then
		if test "${rsos_fallback}" != "${l_other}" || test "${rsos_good}" = "${l_other}"; then
			echo "rsos: slot ${rsos_slot} failed too often, falling back to slot ${l_other}"
			setenv rsos_fallback ${rsos_slot}
			setenv rsos_slot ${l_other}
		else
			echo "rsos: slot ${rsos_slot} fails too, and slot ${l_other} failed before: staying on ${rsos_slot}"
		fi
	else
		echo "rsos: slot ${l_other} has no kernel: staying on ${rsos_slot}"
	fi
	# This boot is the first counted boot of the slot now selected.
	setenv rsos_ok 1
	setenv rsos_tries 0
	setenv rsos_fails 1
fi

# The failure budget is spent: power off rather than loop with the panel
# powered and no picture. The next power-on starts a fresh budget, on the
# last confirmed slot unless it is marked bad. ("reset" in case the
# power-off returns: never a U-Boot prompt.)
if test ${l_off} = 1; then
	if test -n "${rsos_good}" && test "${rsos_good}" != "${rsos_bad}"; then
		setenv rsos_slot ${rsos_good}
	fi
	setenv rsos_rounds 0
	setenv rsos_ok 1
	setenv rsos_tries 0
	setenv rsos_fails 0
	setenv rsos_fallback
	setenv rsos_p
	setenv silent
	echo "rsos: no slot starts, powering off (next power-on: slot ${rsos_slot})"
	saveenv
	poweroff
	reset
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

# cpu-lowvolt: one boot at a time until a boot with it is confirmed (a
# freeze at 1.0 V must not come back after every watchdog reset).
l_lowvolt=0
if test -n "${rsos_overlays}"; then
	for l_ov in ${rsos_overlays}; do
		if test "${l_ov}" = "cpu-lowvolt"; then
			l_lowvolt=1
		fi
	done
fi
if test ${l_lowvolt} = 1; then
	if test "${rsos_lowvolt}" = "trial"; then
		echo "rsos: the last boot with overlay cpu-lowvolt was not confirmed: skipping it (fw_setenv rsos_lowvolt to try again)"
		setenv rsos_lowvolt failed
		l_lowvolt=0
		l_save=1
	elif test "${rsos_lowvolt}" = "failed"; then
		echo "rsos: overlay cpu-lowvolt skipped (rsos_lowvolt=failed)"
		l_lowvolt=0
	else
		setenv rsos_lowvolt trial
		l_save=1
	fi
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
				if test "${l_ov}" = "cpu-lowvolt" && test ${l_lowvolt} = 0; then
					true
				elif load mmc 0:${l_part} ${fdtoverlay_addr_r} /boot/overlays/${l_ov}.dtbo; then
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
# slot at once when it has a kernel and is not marked bad (one round of the
# failure budget: two unloadable slots cannot ping-pong for ever), else power
# off. (The slot that failed before, rsos_fallback, is tried too: it may
# well boot, and powering off at every power-on would be worse.) A trial slot
# that cannot be loaded is marked bad.
setenv silent
echo "rsos: cannot boot slot ${rsos_slot} (partition ${l_part})"
if test "${rsos_ok}" = "0"; then
	setenv rsos_bad ${rsos_slot}
fi
setexpr rsos_rounds ${rsos_rounds} + 1
setenv rsos_ok 1
setenv rsos_tries 0
setenv rsos_fails 0
setenv rsos_p
if test ${rsos_rounds} -lt ${l_maxrounds} && test "${rsos_bad}" != "${l_other}" && test -e mmc 0:${l_opart} /boot/zImage; then
	echo "rsos: switching to slot ${l_other}"
	setenv rsos_fallback ${rsos_slot}
	setenv rsos_slot ${l_other}
	setenv silent 1
	saveenv
	reset
fi
# The next power-on starts a fresh budget, on the last confirmed slot.
if test -n "${rsos_good}" && test "${rsos_good}" != "${rsos_bad}"; then
	setenv rsos_slot ${rsos_good}
fi
setenv rsos_rounds 0
setenv rsos_fallback
echo "rsos: no slot can be booted, powering off (next power-on: slot ${rsos_slot})"
saveenv
poweroff
reset
