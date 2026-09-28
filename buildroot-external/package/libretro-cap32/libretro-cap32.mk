################################################################################
#
# libretro-cap32
#
################################################################################

# libretro/libretro-cap32 master, 2026-09-27. No submodules.
LIBRETRO_CAP32_VERSION = 1abeaac1589156bc1c968015337595cce4d853d6
LIBRETRO_CAP32_SITE = $(call github,libretro,libretro-cap32,$(LIBRETRO_CAP32_VERSION))
LIBRETRO_CAP32_LICENSE = GPL-2.0+ (Caprice32), Amstrad ROM distribution permission (CPC ROMs)
LIBRETRO_CAP32_LICENSE_FILES = cap32/COPYING.txt

# Why cap32 and not crocods (MIT, lighter): both are full speed on a 1 GHz
# A7 (a 4 MHz Z80), cap32 is more accurate, supports the 6128+ and its
# cartridges, has the better on-screen keyboard and is the core the other
# handheld firmwares ship (docs/cores.md).
#
# platform=rpi2 on the RetroStone2: the Makefile's "rpi" branch adds
# -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -marm -fomit-frame-pointer
# -ffast-math -falign-*=1, -O3. Its rpi3 branch is 32-bit only (-mfpu), so
# unix (-O3, CPU flags from the wrapper) everywhere else. Plain C, no asm.
LIBRETRO_CAP32_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 default=unix)

LIBRETRO_CAP32_MAKE_OPTS = \
	platform=$(LIBRETRO_CAP32_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_CAP32_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_CAP32_MAKE_OPTS)
endef

define LIBRETRO_CAP32_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/cap32_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/cap32_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_CAP32_PKGDIR)/cap32.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/cap32.ini
endef

$(eval $(generic-package))
