################################################################################
#
# libretro-prboom
#
################################################################################

# libretro/libretro-prboom master, 2026-09-15. No submodules; prboom.wad is
# generated into src/prboom_wad_data.h in the tree and compiled into the core.
LIBRETRO_PRBOOM_VERSION = d20300de2d32e5b8e8b0a0f15b1e1a889583d248
LIBRETRO_PRBOOM_SITE = $(call github,libretro,libretro-prboom,$(LIBRETRO_PRBOOM_VERSION))
LIBRETRO_PRBOOM_LICENSE = GPL-2.0+
LIBRETRO_PRBOOM_LICENSE_FILES = COPYING

# platform=unix everywhere: the Makefile's ARM branches ("armv-neon-...")
# only add -marm/-mfpu, which the toolchain wrapper already gives, and drop
# the version script. The NEON column/span drawers are selected by
# __ARM_NEON, which the wrapper's -mfpu=neon-vfpv4 (Cortex-A7) and aarch64
# define. Plain C, no asm.
LIBRETRO_PRBOOM_PLATFORM = $(call rsos-libretro-select,default=unix)

LIBRETRO_PRBOOM_MAKE_OPTS = \
	platform=$(LIBRETRO_PRBOOM_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_PRBOOM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_PRBOOM_MAKE_OPTS)
endef

define LIBRETRO_PRBOOM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/prboom_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/prboom_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_PRBOOM_PKGDIR)/prboom.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/prboom.ini
endef

$(eval $(generic-package))
