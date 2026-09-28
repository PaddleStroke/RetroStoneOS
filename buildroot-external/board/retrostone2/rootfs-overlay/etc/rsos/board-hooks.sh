# RetroStone2 board hooks, sourced by /usr/libexec/rsos/board.sh (from rcS).
# Only function definitions: nothing runs when this file is read.

# rcS, in the background once the menu is up.
# Audio: the volume wheel and the speaker/headphone switch are analog, so
# the codec output is simply set to a fixed high level with the DAC routed
# to the power amplifier (A20 sun4i-codec, ALSA card 0).
# TODO(hw): tune the level (0..63) so the wheel covers a useful range.
rsos_board_late_audio() {
	if [ -x /usr/bin/amixer ]; then
		amixer -q -c 0 cset name='Power Amplifier Volume' 56 2>/dev/null
		amixer -q -c 0 cset name='Power Amplifier DAC Playback Switch' on 2>/dev/null
		amixer -q -c 0 cset name='Power Amplifier Mute Switch' on 2>/dev/null
	fi
}

# rcS, in the background, after the USB storage modules.
# SATA ("Pro" units, sata overlay): libata/ahci_sunxi are modules; the
# platform device only exists when the overlay enabled the controller.
rsos_board_late_storage() {
	[ -d /sys/bus/platform/devices/1c18000.sata ] && modprobe -q ahci_sunxi
}
