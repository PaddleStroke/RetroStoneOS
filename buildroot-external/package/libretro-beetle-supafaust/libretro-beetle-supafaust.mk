################################################################################
#
# libretro-beetle-supafaust
#
################################################################################

# libretro/supafaust master, 2026-08-23. Mednafen's snes_faust SNES emulator
# with a multithreaded PPU, written for dual/quad-core ARM Cortex-A7/A9/A53
# Linux devices. GPL-2.0+ (Mednafen file headers). No submodules.
LIBRETRO_BEETLE_SUPAFAUST_VERSION = 642d1d1b6684aa7e306a02a89885f3f5456a5157
LIBRETRO_BEETLE_SUPAFAUST_SITE = $(call github,libretro,supafaust,$(LIBRETRO_BEETLE_SUPAFAUST_VERSION))
LIBRETRO_BEETLE_SUPAFAUST_LICENSE = GPL-2.0+
LIBRETRO_BEETLE_SUPAFAUST_LICENSE_FILES = COPYING

# platform=unix: -O2, -pthread, -fwrapv -fsigned-char -std=c++11. The CPU
# flags (-marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard) come from
# the Buildroot toolchain wrapper. No asm.
#
# The upstream "classic_armv7_a7" platform (SNES Classic, quad A7) is not
# used: it links libstdc++/libgcc statically and builds with -Ofast plus
# whole-program LTO. TODO(hw): if the core is short of speed on the A20, try
# -O3 / LTO here before anything else.
#
# PPU render thread: always on. retro_load_game() forces
# snes_faust.renderer=mt whatever the "supafaust_renderer" option says, so
# no build flag is involved. Thread placement is a core option (see
# beetle_supafaust.ini): emulation on CPU1 (the frontend thread that calls
# retro_run() gets its affinity changed), PPU renderer on CPU0.
#
# -DHAVE_SEM_CLOCKWAIT: the emulation and PPU threads hand frames over with
# 1 ms timed semaphore waits. Without this define (which Mednafen's
# configure would set, but the libretro Makefile does not), they are built
# on sem_timedwait() against CLOCK_REALTIME, and the build warns "Using
# realtime-clock-based sem_timedwait()". The RetroStone2 has no RTC backup:
# its wall clock jumps when the saved time or NTP sets it, which can stall
# or spin those waits. glibc >= 2.30 has sem_clockwait(), so wait on
# CLOCK_MONOTONIC instead.
LIBRETRO_BEETLE_SUPAFAUST_MAKE_ENV = \
	$(TARGET_MAKE_ENV) \
	$(TARGET_CONFIGURE_OPTS) \
	CXXFLAGS="$(TARGET_CXXFLAGS) -DHAVE_SEM_CLOCKWAIT"

LIBRETRO_BEETLE_SUPAFAUST_MAKE_OPTS = \
	platform=unix \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_BEETLE_SUPAFAUST_BUILD_CMDS
	$(LIBRETRO_BEETLE_SUPAFAUST_MAKE_ENV) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_BEETLE_SUPAFAUST_MAKE_OPTS)
endef

define LIBRETRO_BEETLE_SUPAFAUST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mednafen_supafaust_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mednafen_supafaust_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_BEETLE_SUPAFAUST_PKGDIR)/mednafen_supafaust.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/mednafen_supafaust.ini
endef

$(eval $(generic-package))
