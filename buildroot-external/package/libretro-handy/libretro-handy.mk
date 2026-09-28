################################################################################
#
# libretro-handy
#
################################################################################

# libretro/libretro-handy master, 2026-04-20. There is no top-level licence
# file: the Handy emulator (lynx/) is zlib-licensed (lynx/license.txt, repeated
# in every source header). The bundled Blip_Buffer is LGPL-2.1+ and
# Stereo_Buffer GPL-2.0+ (their file headers), the libretro glue is MIT-style.
LIBRETRO_HANDY_VERSION = bc55d462f0b2d6b073ea93dc552ebd73cec60fd1
LIBRETRO_HANDY_SITE = $(call github,libretro,libretro-handy,$(LIBRETRO_HANDY_VERSION))
LIBRETRO_HANDY_LICENSE = Zlib (Handy), LGPL-2.1+ (Blip_Buffer), GPL-2.0+ (Stereo_Buffer)
LIBRETRO_HANDY_LICENSE_FILES = lynx/license.txt

# The Makefile has no Raspberry Pi / ARM platform: the default "unix" platform
# gives -O2 -fomit-frame-pointer, RGB565 video (XRGB8888 compiled in, used only
# if the "handy_gfx_colors" option asks for 24 bit). The CPU flags
# (-marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard) come from the
# Buildroot toolchain wrapper. Plain C++, no asm.
LIBRETRO_HANDY_MAKE_OPTS = \
	platform=unix \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_HANDY_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_HANDY_MAKE_OPTS)
endef

define LIBRETRO_HANDY_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/handy_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/handy_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_HANDY_PKGDIR)/handy.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/handy.ini
endef

$(eval $(generic-package))
