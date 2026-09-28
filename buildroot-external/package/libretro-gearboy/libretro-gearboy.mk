################################################################################
#
# libretro-gearboy
#
################################################################################

# drhelius/Gearboy master, 2026-09-25. The libretro core lives in
# platforms/libretro/ of the emulator's own repository; no submodules.
LIBRETRO_GEARBOY_VERSION = 1f2ef68a5a43f2b6cdaa4fdf1b462c772d5d31d3
LIBRETRO_GEARBOY_SITE = $(call github,drhelius,Gearboy,$(LIBRETRO_GEARBOY_VERSION))
LIBRETRO_GEARBOY_LICENSE = GPL-3.0
LIBRETRO_GEARBOY_LICENSE_FILES = LICENSE

# platform=rpi2 (the Makefile turns it into "rpi2 unix"): -O3 -DNDEBUG,
# RGB565 video. The Makefile adds its -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4
# -mfloat-abi=hard to CFLAGS only, but the sources are C++; the same flags
# come from the Buildroot toolchain wrapper anyway. Plain C++, no asm.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
LIBRETRO_GEARBOY_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_GEARBOY_MAKE_OPTS = \
	platform=$(LIBRETRO_GEARBOY_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_GEARBOY_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D)/platforms/libretro -f Makefile \
		$(LIBRETRO_GEARBOY_MAKE_OPTS)
endef

define LIBRETRO_GEARBOY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/platforms/libretro/gearboy_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/gearboy_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_GEARBOY_PKGDIR)/gearboy.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/gearboy.ini
endef

$(eval $(generic-package))
