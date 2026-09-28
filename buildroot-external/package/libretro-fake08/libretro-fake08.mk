################################################################################
#
# libretro-fake08
#
################################################################################

# jtothebell/fake-08 master, 2026-06-13. Git with submodules: libs/z8lua (the
# PICO-8 flavoured Lua 5.2) is a submodule, missing from the GitHub tarball.
LIBRETRO_FAKE08_VERSION = 814991a2571ad3970e386cef48f3b148aa1c27b9
LIBRETRO_FAKE08_SITE = https://github.com/jtothebell/fake-08.git
LIBRETRO_FAKE08_SITE_METHOD = git
LIBRETRO_FAKE08_GIT_SUBMODULES = YES
LIBRETRO_FAKE08_LICENSE = MIT (fake-08, z8lua, miniz), Zlib (lodepng), WTFPL (zepto8 parts)
# z8lua has no licence file: its MIT notice (Lua 5.2) is at the end of lua.h.
LIBRETRO_FAKE08_LICENSE_FILES = \
	LICENSE.MD \
	libs/z8lua/lua.h \
	libs/miniz/LICENSE \
	libs/lodepng/LICENSE

# platform=unix everywhere (-O2, C++17). The Makefile's only ARM Linux
# platform is "miyoomini", which hard-codes the arm-linux-gnueabihf-*
# compilers; its CPU flags are what the toolchain wrapper gives anyway.
# Plain C/C++, no asm. Carts are read from memory (need_fullpath = false).
LIBRETRO_FAKE08_PLATFORM = $(call rsos-libretro-select,default=unix)

LIBRETRO_FAKE08_MAKE_OPTS = \
	platform=$(LIBRETRO_FAKE08_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_FAKE08_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D)/platform/libretro $(LIBRETRO_FAKE08_MAKE_OPTS)
endef

define LIBRETRO_FAKE08_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/platform/libretro/fake08_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/fake08_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_FAKE08_PKGDIR)/fake08.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/fake08.ini
endef

$(eval $(generic-package))
