################################################################################
#
# libretro-fbneo
#
################################################################################

# libretro/FBNeo master, 2026-09-23
LIBRETRO_FBNEO_VERSION = aceeebed9e7edc8a28652365a064baee9a16e274
LIBRETRO_FBNEO_SITE = $(call github,libretro,FBNeo,$(LIBRETRO_FBNEO_VERSION))
LIBRETRO_FBNEO_LICENSE = FBNeo non-commercial license
LIBRETRO_FBNEO_LICENSE_FILES = src/license.txt

# platform=rpi2 (the Makefile turns it into "rpi2 unix"): -marm
# -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard, -O3, HAVE_NEON=1,
# USE_CYCLONE=1 (pregenerated Cyclone ARM 68000 core, src/cpu/cyclone).
# Full driver set (no SUBSET).
#
# Cyclone.S has no .note.GNU-stack section, so the linker would mark the .so
# as needing an executable stack, and glibc >= 2.41 refuses to dlopen() such
# objects. It does not need one: force -z noexecstack.
LIBRETRO_FBNEO_MAKE_ENV = \
	$(TARGET_MAKE_ENV) \
	$(TARGET_CONFIGURE_OPTS) \
	LDFLAGS="$(TARGET_LDFLAGS) -Wl,-z,noexecstack"

# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
# (rpi2 also turns on HAVE_NEON and USE_CYCLONE, ARM32 only; the rpi*_64
# platforms and unix leave them off. No x86_64 DRC.)
LIBRETRO_FBNEO_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_FBNEO_MAKE_OPTS = \
	platform=$(LIBRETRO_FBNEO_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_FBNEO_BUILD_CMDS
	$(LIBRETRO_FBNEO_MAKE_ENV) \
		$(MAKE) -C $(@D)/src/burner/libretro -f Makefile \
		$(LIBRETRO_FBNEO_MAKE_OPTS)
endef

define LIBRETRO_FBNEO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/src/burner/libretro/fbneo_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/fbneo_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_FBNEO_PKGDIR)/fbneo.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/fbneo.ini
endef

$(eval $(generic-package))
