################################################################################
#
# libretro-snes9x2005
#
################################################################################

# libretro/snes9x2005 master, 2026-07-22
LIBRETRO_SNES9X2005_VERSION = deb49d80d1836e3e737480a326e31a54c46c04ae
LIBRETRO_SNES9X2005_SITE = $(call github,libretro,snes9x2005,$(LIBRETRO_SNES9X2005_VERSION))
LIBRETRO_SNES9X2005_LICENSE = Snes9x non-commercial license, MIT (libretro glue)
LIBRETRO_SNES9X2005_LICENSE_FILES = copyright

# platform=armv-hardfloat: -marm -mfloat-abi=hard -DARM, -O2. The core has no
# NEON code; "armv-neon-hardfloat" would only append -mfpu=neon and so
# downgrade the wrapper's -mfpu=neon-vfpv4 to VFPv3. The "armv" branch
# hard-codes CC = gcc, hence CC/CXX on the command line. USE_BLARGG_APU=0
# (default) keeps the fast APU; =1 builds "snes9x2005_plus" (more accurate
# sound, slower).
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): armv-hardfloat on 32-bit ARM, unix elsewhere.
LIBRETRO_SNES9X2005_PLATFORM = \
	$(call rsos-libretro-select,arm=armv-hardfloat default=unix)

LIBRETRO_SNES9X2005_MAKE_OPTS = \
	platform=$(LIBRETRO_SNES9X2005_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_SNES9X2005_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_SNES9X2005_MAKE_OPTS)
endef

define LIBRETRO_SNES9X2005_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/snes9x2005_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/snes9x2005_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_SNES9X2005_PKGDIR)/snes9x2005.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/snes9x2005.ini
endef

$(eval $(generic-package))
