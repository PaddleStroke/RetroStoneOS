################################################################################
#
# libretro-beetle-pce-fast
#
################################################################################

# libretro/beetle-pce-fast-libretro master, 2026-09-25. No submodules: the
# CHD dependencies (libchdr, zlib, LZMA SDK, zstd) are vendored in deps/.
LIBRETRO_BEETLE_PCE_FAST_VERSION = 1c693c630121366f0b819d785762f1ce5b3a68d0
LIBRETRO_BEETLE_PCE_FAST_SITE = $(call github,libretro,beetle-pce-fast-libretro,$(LIBRETRO_BEETLE_PCE_FAST_VERSION))
LIBRETRO_BEETLE_PCE_FAST_LICENSE = GPL-2.0+ (Mednafen), BSD-3-Clause (libchdr), Zlib (zlib), BSD-3-Clause or GPL-2.0 (zstd), Public Domain (LZMA SDK)
LIBRETRO_BEETLE_PCE_FAST_LICENSE_FILES = \
	COPYING \
	deps/libchdr/LICENSE.txt \
	deps/zstd/LICENSE \
	deps/lzma-19.00/LICENSE

# platform=rpi2 (the Makefile turns it into "rpi2 unix"): -DARM -marm
# -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fomit-frame-pointer
# -ffast-math, -O2, RGB565 video, CHD support (HAVE_CHD=1). Plain C, no asm.
# CACHE_CD stays 0: CD images are streamed from the SD card, not copied to RAM.
#
# Checked with -D_TIME_BITS=64 too: the vendored zlib is only used through
# inflate (no gz* files), so the zlib/_LARGEFILE64_SOURCE clash that
# mupen64plus-next works around does not happen here.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
LIBRETRO_BEETLE_PCE_FAST_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_BEETLE_PCE_FAST_MAKE_OPTS = \
	platform=$(LIBRETRO_BEETLE_PCE_FAST_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_BEETLE_PCE_FAST_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_BEETLE_PCE_FAST_MAKE_OPTS)
endef

define LIBRETRO_BEETLE_PCE_FAST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mednafen_pce_fast_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mednafen_pce_fast_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_BEETLE_PCE_FAST_PKGDIR)/mednafen_pce_fast.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/mednafen_pce_fast.ini
endef

$(eval $(generic-package))
