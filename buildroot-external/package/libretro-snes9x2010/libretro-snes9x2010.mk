################################################################################
#
# libretro-snes9x2010
#
################################################################################

# libretro/snes9x2010 master, 2026-09-21
LIBRETRO_SNES9X2010_VERSION = fe690dd321fa5a46b5234a2bde089d2518c62b0e
LIBRETRO_SNES9X2010_SITE = $(call github,libretro,snes9x2010,$(LIBRETRO_SNES9X2010_VERSION))
LIBRETRO_SNES9X2010_LICENSE = Snes9x non-commercial license
LIBRETRO_SNES9X2010_LICENSE_FILES = LICENSE.txt

# platform=rpi2: -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
# -ffast-math -fomit-frame-pointer; the NEON tile renderer in src/tile.c is
# enabled by __ARM_NEON. That branch does not link libm, which breaks the link
# (ceilf, --no-undefined), so LIBM=-lm is forced.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
LIBRETRO_SNES9X2010_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_SNES9X2010_MAKE_OPTS = \
	platform=$(LIBRETRO_SNES9X2010_PLATFORM) \
	LIBM=-lm \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_SNES9X2010_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile.libretro $(LIBRETRO_SNES9X2010_MAKE_OPTS)
endef

define LIBRETRO_SNES9X2010_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/snes9x2010_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/snes9x2010_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_SNES9X2010_PKGDIR)/snes9x2010.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/snes9x2010.ini
endef

$(eval $(generic-package))
