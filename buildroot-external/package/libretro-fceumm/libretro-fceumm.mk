################################################################################
#
# libretro-fceumm
#
################################################################################

# libretro/libretro-fceumm master, 2026-08-22
LIBRETRO_FCEUMM_VERSION = 236ccdfc911e84c60fea6b9d0699c2d440a8de14
LIBRETRO_FCEUMM_SITE = $(call github,libretro,libretro-fceumm,$(LIBRETRO_FCEUMM_VERSION))
LIBRETRO_FCEUMM_LICENSE = GPL-2.0+
LIBRETRO_FCEUMM_LICENSE_FILES = Copying

# platform=armv-neon-hardfloat: shared object with -marm -mfloat-abi=hard -DARM,
# -O2, RGB565 video (the "unix" platform would force XRGB8888). FCEUmm has no
# asm or NEON code; -mcpu=cortex-a7 -mfpu=neon-vfpv4 come from the Buildroot
# toolchain wrapper. CFLAGS/LDFLAGS are passed in the environment (via
# TARGET_CONFIGURE_OPTS) so that the Makefile's own "CFLAGS +=" still apply;
# CC/AR go on the command line because some Makefile branches hard-code them.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"). unix and rpi*_64 switch the core to XRGB8888:
# WANT_32BPP=0 keeps RGB565 as on ARMv7.
LIBRETRO_FCEUMM_PLATFORM = \
	$(call rsos-libretro-select,arm=armv-neon-hardfloat aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_FCEUMM_MAKE_OPTS = \
	platform=$(LIBRETRO_FCEUMM_PLATFORM)$(if $(BR2_arm),, WANT_32BPP=0) \
	CC="$(TARGET_CC)" \
	AR="$(TARGET_AR)"

define LIBRETRO_FCEUMM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile.libretro $(LIBRETRO_FCEUMM_MAKE_OPTS)
endef

define LIBRETRO_FCEUMM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fceumm_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/fceumm_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_FCEUMM_PKGDIR)/fceumm.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/fceumm.ini
endef

$(eval $(generic-package))
