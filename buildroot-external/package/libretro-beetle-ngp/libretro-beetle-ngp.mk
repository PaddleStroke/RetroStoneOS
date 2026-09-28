################################################################################
#
# libretro-beetle-ngp
#
################################################################################

# libretro/beetle-ngp-libretro master, 2026-06-14 (Mednafen NGP, based on
# NeoPop).
LIBRETRO_BEETLE_NGP_VERSION = a50d5ac288a81f2104ddf43195a4efdd15c72227
LIBRETRO_BEETLE_NGP_SITE = $(call github,libretro,beetle-ngp-libretro,$(LIBRETRO_BEETLE_NGP_VERSION))
LIBRETRO_BEETLE_NGP_LICENSE = GPL-2.0+
LIBRETRO_BEETLE_NGP_LICENSE_FILES = COPYING

# platform=rpi2 (the Makefile turns it into "rpi2 unix"): -marm
# -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard, -O2, RGB565 video
# (NEED_BPP=16). No asm; the TLCS-900h CPU is an interpreter.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
LIBRETRO_BEETLE_NGP_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_BEETLE_NGP_MAKE_OPTS = \
	platform=$(LIBRETRO_BEETLE_NGP_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_BEETLE_NGP_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_BEETLE_NGP_MAKE_OPTS)
endef

define LIBRETRO_BEETLE_NGP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mednafen_ngp_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mednafen_ngp_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_BEETLE_NGP_PKGDIR)/mednafen_ngp.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/mednafen_ngp.ini
endef

$(eval $(generic-package))
