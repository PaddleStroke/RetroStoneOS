#!/bin/sh
# Super Mario 64 (sm64-port, built from your own ROM by ports/sm64/build-sm64.sh).
# Saves (sm64_save_file.bin) and settings (sm64config.txt) stay in sm64/.
cd sm64 || exit 1
if [ -x ./sm64.us ]; then
	exec ./sm64.us
fi
exec /lib/ld-linux-armhf.so.3 ./sm64.us
