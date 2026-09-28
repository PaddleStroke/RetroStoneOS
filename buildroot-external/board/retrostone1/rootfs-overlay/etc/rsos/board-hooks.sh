# RetroStone1 board hooks, sourced by /usr/libexec/rsos/board.sh (from rcS).
# Only function definitions: nothing runs when this file is read.

# rcS, in the background once the menu is up.
# Audio: the volume wheel and the speaker/headphone switch are analog, so the
# H3 codec's line out is set to a fixed high level, stereo, with the DAC
# routed to the output mixers. The card number depends on the probe order
# (codec vs HDMI), so it is looked up by its id.
# TODO(hw): tune the level (0..31) so the wheel covers a useful range.
rsos_board_late_audio() {
	[ -x /usr/bin/amixer ] || return 0
	for id in /proc/asound/card*/id; do
		[ -f "$id" ] || continue
		case $(cat "$id") in
		*[Cc]odec*)
			c=${id%/id}
			c=${c##*card}
			amixer -q -c "$c" cset name='Line Out Playback Volume' 28 2>/dev/null
			amixer -q -c "$c" cset name='Line Out Playback Switch' on,on 2>/dev/null
			amixer -q -c "$c" cset name='Line Out Source Playback Route' Stereo 2>/dev/null
			amixer -q -c "$c" cset name='DAC Playback Switch' on,on 2>/dev/null
			amixer -q -c "$c" cset name='DAC Playback Volume' 63 2>/dev/null
			return 0
			;;
		esac
	done
}
