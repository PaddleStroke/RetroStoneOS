################################################################################
#
# libretro-beetle-supergrafx
#
################################################################################

# libretro/beetle-supergrafx-libretro master, 2026-04-20. No submodules: the
# CHD dependencies (libchdr, zlib, LZMA SDK) are vendored in deps/.
LIBRETRO_BEETLE_SUPERGRAFX_VERSION = 3c6fcd3deded54ebecd69408f108407ac03d11b5
LIBRETRO_BEETLE_SUPERGRAFX_SITE = $(call github,libretro,beetle-supergrafx-libretro,$(LIBRETRO_BEETLE_SUPERGRAFX_VERSION))
LIBRETRO_BEETLE_SUPERGRAFX_LICENSE = GPL-2.0+ (Mednafen), BSD-3-Clause (libchdr, Tremor), Zlib (zlib), Public Domain (LZMA SDK)
LIBRETRO_BEETLE_SUPERGRAFX_LICENSE_FILES = \
	COPYING \
	deps/libchdr/LICENSE.txt \
	deps/lzma-19.00/LICENSE \
	mednafen/tremor/COPYING

# Same Makefile layout as beetle-pce-fast: platform=rpi2 (turned into
# "rpi2 unix") gives -DARM -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4
# -mfloat-abi=hard -fomit-frame-pointer -ffast-math, -O2, RGB565, CHD.
# rpi4_64 / rpi3_64 on the Pi 4 / Pi 3, unix elsewhere. Plain C/C++, no asm.
LIBRETRO_BEETLE_SUPERGRAFX_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_BEETLE_SUPERGRAFX_MAKE_OPTS = \
	platform=$(LIBRETRO_BEETLE_SUPERGRAFX_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_BEETLE_SUPERGRAFX_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_BEETLE_SUPERGRAFX_MAKE_OPTS)
endef

define LIBRETRO_BEETLE_SUPERGRAFX_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mednafen_supergrafx_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mednafen_supergrafx_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_BEETLE_SUPERGRAFX_PKGDIR)/mednafen_supergrafx.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/mednafen_supergrafx.ini
endef

$(eval $(generic-package))
