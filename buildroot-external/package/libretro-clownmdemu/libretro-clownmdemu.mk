################################################################################
#
# libretro-clownmdemu
#
################################################################################

# Clownacy/clownmdemu-libretro master, 2026-09-25. Git, not a GitHub tarball:
# the emulator itself is in nested submodules (common = the shared frontend
# code, common/core = the Mega Drive core, with clown68000 and clownz80,
# common/clowncd = the CD reader with libchdr, and libretro-common).
LIBRETRO_CLOWNMDEMU_VERSION = 0f23a1696581f6969cfc78cc763af492c4bb3fd6
LIBRETRO_CLOWNMDEMU_SITE = https://github.com/Clownacy/clownmdemu-libretro.git
LIBRETRO_CLOWNMDEMU_SITE_METHOD = git
LIBRETRO_CLOWNMDEMU_GIT_SUBMODULES = YES
# The emulator, 68000 and Z80 cores and the libretro frontend are
# AGPL-3.0-or-later. clowncd, clowncommon and clownresampler are ISC-style
# ("Permission to use, copy, modify, and/or distribute ... for any purpose").
# libchdr BSD-3-Clause, LZMA SDK public domain, libretro-common MIT.
LIBRETRO_CLOWNMDEMU_LICENSE = AGPL-3.0+, ISC (clowncd, clowncommon, clownresampler), BSD-3-Clause (libchdr), Public Domain (LZMA SDK)
LIBRETRO_CLOWNMDEMU_LICENSE_FILES = \
	LICENCE.txt \
	common/clowncd/LICENCE.txt \
	common/clowncd/libraries/clownresampler/LICENCE.txt \
	common/clowncd/libraries/chd/libchdr/LICENSE.txt

# platform=unix: one translation unit (unity.c), plain C89, -O2 -DNDEBUG. The
# Makefile has no ARM platform worth using (only the SNES Classic one, which
# links libgcc statically and uses -Ofast/LTO); the CPU flags
# (-marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard) come from the
# Buildroot toolchain wrapper. No asm, no dynarec: the 68000s and Z80 are
# interpreters. No threads.
#
# The "unix" branch links without the repository's link.T, so all ~650
# internal symbols of the unity build would be exported from the .so. Pass
# the version script (it keeps only retro_*) through LDFLAGS, which the
# Makefile appends to ("LDFLAGS += $(LIBM)").
LIBRETRO_CLOWNMDEMU_MAKE_ENV = \
	$(TARGET_MAKE_ENV) \
	$(TARGET_CONFIGURE_OPTS) \
	LDFLAGS="$(TARGET_LDFLAGS) -Wl,--version-script=link.T"

LIBRETRO_CLOWNMDEMU_MAKE_OPTS = \
	platform=unix \
	CC="$(TARGET_CC)" \
	AR="$(TARGET_AR)"

define LIBRETRO_CLOWNMDEMU_BUILD_CMDS
	$(LIBRETRO_CLOWNMDEMU_MAKE_ENV) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_CLOWNMDEMU_MAKE_OPTS)
endef

define LIBRETRO_CLOWNMDEMU_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/clownmdemu_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/clownmdemu_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_CLOWNMDEMU_PKGDIR)/clownmdemu.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/clownmdemu.ini
endef

$(eval $(generic-package))
