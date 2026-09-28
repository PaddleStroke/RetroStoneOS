################################################################################
#
# libretro-prosystem
#
################################################################################

# libretro/prosystem-libretro master, 2026-08-22
LIBRETRO_PROSYSTEM_VERSION = 8a88014287c7a01cd568067e5a557d0a2b2a051f
LIBRETRO_PROSYSTEM_SITE = $(call github,libretro,prosystem-libretro,$(LIBRETRO_PROSYSTEM_VERSION))
LIBRETRO_PROSYSTEM_LICENSE = GPL-2.0+ (ProSystem), Zlib (CoreTone/bupboop)
LIBRETRO_PROSYSTEM_LICENSE_FILES = License.txt bupboop/License.txt

# platform=rpi2: -DARM -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
# -fomit-frame-pointer -ffast-math -fsigned-char, -O2. Plain C, no asm. RGB565
# video unless the "prosystem_color_depth" option asks for 24 bit.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
LIBRETRO_PROSYSTEM_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_PROSYSTEM_MAKE_OPTS = \
	platform=$(LIBRETRO_PROSYSTEM_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_PROSYSTEM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_PROSYSTEM_MAKE_OPTS)
endef

define LIBRETRO_PROSYSTEM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/prosystem_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/prosystem_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_PROSYSTEM_PKGDIR)/prosystem.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/prosystem.ini
endef

$(eval $(generic-package))
