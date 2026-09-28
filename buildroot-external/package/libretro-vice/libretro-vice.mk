################################################################################
#
# libretro-vice
#
################################################################################

# libretro/vice-libretro master, 2026-09-22 (VICE 3.x). No submodules.
LIBRETRO_VICE_VERSION = 9d7983826ea792f6cce7fdfe6c09488129c6f886
LIBRETRO_VICE_SITE = $(call github,libretro,vice-libretro,$(LIBRETRO_VICE_VERSION))
# vice/README states that the embedded ROMs (vice/include/embedded) are
# "Copyright (C) by Commodore Business Machines", with no redistribution
# grant: docs/cores.md, "Licences (third batch)".
LIBRETRO_VICE_LICENSE = GPL-2.0+ (VICE, reSID, libretro port), Commodore copyright (embedded C64/1541 ROMs)
LIBRETRO_VICE_LICENSE_FILES = COPYING vice/COPYING vice/README

# EMUTYPE=x64: the fast C64 emulator (line-based VIC-II), the one that runs
# at full speed on Cortex-A7-class CPUs; x64sc (cycle-exact) is heavier and
# is not built. platform=unix everywhere: the Makefile's only ARM Linux
# branch is "rpi4" (64-bit, adds -DARM -DALIGN_DWORD for alignment-strict
# CPUs, which ARMv7/ARMv8 Linux are not). -O3, CPU flags from the wrapper.
# Plain C/C++ (reSID), no asm.
LIBRETRO_VICE_PLATFORM = $(call rsos-libretro-select,default=unix)

LIBRETRO_VICE_MAKE_OPTS = \
	platform=$(LIBRETRO_VICE_PLATFORM) \
	EMUTYPE=x64 \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_VICE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_VICE_MAKE_OPTS)
endef

define LIBRETRO_VICE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/vice_x64_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/vice_x64_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_VICE_PKGDIR)/vice_x64.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/vice_x64.ini
endef

$(eval $(generic-package))
