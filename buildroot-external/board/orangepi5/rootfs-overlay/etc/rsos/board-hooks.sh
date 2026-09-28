# RetroStoneOS board hooks: Orange Pi 5 (sourced by
# /usr/libexec/rsos/board.sh; definitions only, nothing runs here).

# rcS, background, once the menu is up: load the Panthor GPU driver. It is a
# module because it loads its CSF firmware from /lib/firmware when it
# probes, which a built-in driver would do before the root filesystem is
# mounted. Only the N64 cores (GLES 2.0) need the GPU, and the game process
# sets EGL up when a game starts, long after this. (There is no dedicated
# "late modules" hook; the audio one runs first after the menu is up.)
rsos_board_late_audio() {
	modprobe -q panthor
}
