################################################################################
#
# libretro-mame2003-plus
#
################################################################################

# libretro/mame2003-plus-libretro master, 2026-09-24
LIBRETRO_MAME2003_PLUS_VERSION = 546424f3cd46674e996867ac26bd1060011739df
LIBRETRO_MAME2003_PLUS_SITE = $(call github,libretro,mame2003-plus-libretro,$(LIBRETRO_MAME2003_PLUS_VERSION))
LIBRETRO_MAME2003_PLUS_LICENSE = MAME non-commercial license
LIBRETRO_MAME2003_PLUS_LICENSE_FILES = LICENSE.md

# platform=rpi2: -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
# -fomit-frame-pointer -ffast-math, -O2, ARM=1 / HAVE_ARMv6=1. Full driver
# list (INCLUDE_DRV=all).
#
# USE_CYCLONE=1 / USE_DRZ80=1 add the Cyclone (68000) and DrZ80 (Z80) ARM asm
# CPU cores. Upstream only turns them on for Vita and Miyoo, but the core
# option mame2003-plus_cyclone_mode defaults to "default", which swaps them
# in only for the drivers on its built-in compatibility list and keeps the C
# cores (Musashi, MAME Z80) everywhere else. Big speed-up for CPS-1/2 and
# other 68000/Z80 boards on the Cortex-A7.
#
# The asm objects have no .note.GNU-stack section, so the linker would mark
# the .so as needing an executable stack, and glibc >= 2.41 refuses to
# dlopen() such objects. They do not need one: force -z noexecstack.
LIBRETRO_MAME2003_PLUS_MAKE_ENV = \
	$(TARGET_MAKE_ENV) \
	$(TARGET_CONFIGURE_OPTS) \
	LDFLAGS="$(TARGET_LDFLAGS) -Wl,-z,noexecstack"

# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"). Cyclone (68000) and DrZ80 are ARM32 assembly: 32-bit
# ARM only; elsewhere the C cores (the x86 MIPS3 DRC is 32-bit x86 only).
LIBRETRO_MAME2003_PLUS_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)
LIBRETRO_MAME2003_PLUS_ARCH_OPTS = \
	$(if $(BR2_arm),USE_CYCLONE=1 USE_DRZ80=1)

LIBRETRO_MAME2003_PLUS_MAKE_OPTS = \
	platform=$(LIBRETRO_MAME2003_PLUS_PLATFORM) \
	$(LIBRETRO_MAME2003_PLUS_ARCH_OPTS) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_MAME2003_PLUS_BUILD_CMDS
	$(LIBRETRO_MAME2003_PLUS_MAKE_ENV) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_MAME2003_PLUS_MAKE_OPTS)
endef

define LIBRETRO_MAME2003_PLUS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mame2003_plus_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mame2003_plus_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_MAME2003_PLUS_PKGDIR)/mame2003_plus.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/mame2003_plus.ini
endef

$(eval $(generic-package))
