/*
 * run.c - rsos-run: runs one libretro game with display, audio and input
 * (or --headless for tests). The same entry point is reachable as
 * `rsos-frontend --run ...` (host_main); see docs/host-design.md.
 *
 *   rsos-run --core /usr/lib/libretro/fceumm_libretro.so \
 *            --rom /data/roms/nes/game.nes [--system nes] [--frames N] [--headless]
 */
#include "../host/host.h"

int main(int argc, char **argv)
{
	return host_main(argc, argv);
}
