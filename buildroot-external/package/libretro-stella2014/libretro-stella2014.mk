################################################################################
#
# libretro-stella2014
#
################################################################################

# libretro/stella2014-libretro master, 2026-09-04 (Stella 3.9.3 with the
# libretro port; kept by libretro as the light Atari 2600 core).
LIBRETRO_STELLA2014_VERSION = 7d1361e407e63f29e52892655069e5fb4096e691
LIBRETRO_STELLA2014_SITE = $(call github,libretro,stella2014-libretro,$(LIBRETRO_STELLA2014_VERSION))
LIBRETRO_STELLA2014_LICENSE = GPL-2.0
LIBRETRO_STELLA2014_LICENSE_FILES = stella/license.txt

# platform=rpi2: -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
# -fomit-frame-pointer, -O2. Plain C++, no asm. RGB565 video unless the
# "stella2014_color_depth" option asks for 24 bit.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
LIBRETRO_STELLA2014_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 default=unix)

LIBRETRO_STELLA2014_MAKE_OPTS = \
	platform=$(LIBRETRO_STELLA2014_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_STELLA2014_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_STELLA2014_MAKE_OPTS)
endef

define LIBRETRO_STELLA2014_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/stella2014_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/stella2014_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_STELLA2014_PKGDIR)/stella2014.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/stella2014.ini
endef

$(eval $(generic-package))
