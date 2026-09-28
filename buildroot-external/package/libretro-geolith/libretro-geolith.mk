################################################################################
#
# libretro-geolith
#
################################################################################

# libretro/geolith-libretro master, 2026-09-14 (libretro port of
# gitlab.com/jgemu/geolith). No submodules: dependencies are vendored in deps/.
# Geolith BSD-3-Clause; Musashi 68000 core MIT-style; Z80 core MIT; ymfm
# (YM2610) BSD-3-Clause; speex resampler BSD-3-Clause; miniz MIT; zstd BSD;
# LZMA SDK public domain; libretro-common MIT.
LIBRETRO_GEOLITH_VERSION = 194024931935eff2092e36fc4f8e53e62ed11097
LIBRETRO_GEOLITH_SITE = $(call github,libretro,geolith-libretro,$(LIBRETRO_GEOLITH_VERSION))
LIBRETRO_GEOLITH_LICENSE = BSD-3-Clause, MIT (Musashi, Z80, miniz), BSD-3-Clause (ymfm, speex, zstd), Public Domain (LZMA SDK)
LIBRETRO_GEOLITH_LICENSE_FILES = LICENSE src/z80/LICENSE deps/speex/COPYING deps/miniz/LICENSE deps/zstd/LICENSE

# The Makefile links -lz (libchdr's zlib codec), so zlib is a dependency.
LIBRETRO_GEOLITH_DEPENDENCIES = zlib

# libretro/Makefile, platform=unix: -O2 -flto -fsigned-char, XRGB8888 video
# (the only format the core offers). The CPU flags (-marm -mcpu=cortex-a7
# -mfpu=neon-vfpv4 -mfloat-abi=hard) come from the Buildroot toolchain
# wrapper. Plain C, no asm: Musashi and the Z80 core are interpreters. No
# threads.
LIBRETRO_GEOLITH_MAKE_OPTS = \
	platform=unix \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_GEOLITH_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D)/libretro -f Makefile $(LIBRETRO_GEOLITH_MAKE_OPTS)
endef

define LIBRETRO_GEOLITH_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libretro/geolith_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/geolith_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_GEOLITH_PKGDIR)/geolith.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/geolith.ini
endef

$(eval $(generic-package))
