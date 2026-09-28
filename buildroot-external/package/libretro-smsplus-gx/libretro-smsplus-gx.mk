################################################################################
#
# libretro-smsplus-gx
#
################################################################################

# libretro/smsplus-gx master, 2026-09-04. SMS Plus GX (Charles MacDonald,
# Eke-Eke, gameblabla): GPL-2.0+. It descends from the GPL releases of SMS
# Plus (<= 1.3), not from the later non-commercial ones. The Z80 core is
# MAME's, BSD-3-Clause; the YM2413 core is GPL-2.0+; the NTSC filter is
# blargg's LGPL-2.1+ sms_ntsc (docs/contributors.txt, file headers).
LIBRETRO_SMSPLUS_GX_VERSION = 3844b46caa926b6494987b97da63092818c4ddef
LIBRETRO_SMSPLUS_GX_SITE = $(call github,libretro,smsplus-gx,$(LIBRETRO_SMSPLUS_GX_VERSION))
LIBRETRO_SMSPLUS_GX_LICENSE = GPL-2.0+, BSD-3-Clause (Z80 core), LGPL-2.1+ (sms_ntsc)
LIBRETRO_SMSPLUS_GX_LICENSE_FILES = docs/license

# Makefile.libretro, platform=rpi2 (turned into "rpi2 unix"): -DARM -marm
# -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fomit-frame-pointer
# -ffast-math -DLSB_FIRST, -O2, RGB565 video. Plain C, no asm.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
LIBRETRO_SMSPLUS_GX_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 default=unix)

LIBRETRO_SMSPLUS_GX_MAKE_OPTS = \
	platform=$(LIBRETRO_SMSPLUS_GX_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_SMSPLUS_GX_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile.libretro $(LIBRETRO_SMSPLUS_GX_MAKE_OPTS)
endef

define LIBRETRO_SMSPLUS_GX_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/smsplus_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/smsplus_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_SMSPLUS_GX_PKGDIR)/smsplus.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/smsplus.ini
endef

$(eval $(generic-package))
