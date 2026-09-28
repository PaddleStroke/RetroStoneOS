################################################################################
#
# libretro-pokemini
#
################################################################################

# libretro/PokeMini master, 2026-07-31. No submodules.
LIBRETRO_POKEMINI_VERSION = 132111b76343559860532a1ccc094f93f1ed5650
LIBRETRO_POKEMINI_SITE = $(call github,libretro,PokeMini,$(LIBRETRO_POKEMINI_VERSION))
LIBRETRO_POKEMINI_LICENSE = GPL-3.0+
LIBRETRO_POKEMINI_LICENSE_FILES = LICENSE

# platform=rpi2: -DARM -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4
# -mfloat-abi=hard -funsafe-math-optimizations -fomit-frame-pointer
# -ffast-math. Its "rpi4" platform is the 64-bit Cortex-A72 one; rpi3_64 on
# a Pi 3, unix elsewhere. Plain C, no asm.
LIBRETRO_POKEMINI_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_POKEMINI_MAKE_OPTS = \
	platform=$(LIBRETRO_POKEMINI_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_POKEMINI_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile.libretro $(LIBRETRO_POKEMINI_MAKE_OPTS)
endef

define LIBRETRO_POKEMINI_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pokemini_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/pokemini_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_POKEMINI_PKGDIR)/pokemini.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/pokemini.ini
endef

$(eval $(generic-package))
