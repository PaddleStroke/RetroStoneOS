################################################################################
#
# libretro-gearcoleco
#
################################################################################

# drhelius/Gearcoleco master, 2026-09-16. The libretro core lives in
# platforms/libretro/ of the emulator's own repository; no submodules.
LIBRETRO_GEARCOLECO_VERSION = 8d32d242c6ba2104d75c17bb79773275387d4c37
LIBRETRO_GEARCOLECO_SITE = $(call github,drhelius,Gearcoleco,$(LIBRETRO_GEARCOLECO_VERSION))
LIBRETRO_GEARCOLECO_LICENSE = GPL-3.0
LIBRETRO_GEARCOLECO_LICENSE_FILES = LICENSE

# platform=rpi2 (the Makefile turns it into "rpi2 unix"): -O3 -DNDEBUG,
# RGB565 video. The Makefile adds its -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4
# -mfloat-abi=hard to CFLAGS only, but the sources are C++; the same flags
# come from the Buildroot toolchain wrapper anyway. Plain C++, no asm.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
LIBRETRO_GEARCOLECO_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_GEARCOLECO_MAKE_OPTS = \
	platform=$(LIBRETRO_GEARCOLECO_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_GEARCOLECO_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D)/platforms/libretro -f Makefile \
		$(LIBRETRO_GEARCOLECO_MAKE_OPTS)
endef

define LIBRETRO_GEARCOLECO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/platforms/libretro/gearcoleco_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/gearcoleco_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_GEARCOLECO_PKGDIR)/gearcoleco.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/gearcoleco.ini
endef

$(eval $(generic-package))
