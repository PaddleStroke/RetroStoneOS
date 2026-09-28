################################################################################
#
# libretro-gambatte
#
################################################################################

# libretro/gambatte-libretro master, 2026-08-21
LIBRETRO_GAMBATTE_VERSION = d9d6cd06382d1ced30de34d56d3609452323dab1
LIBRETRO_GAMBATTE_SITE = $(call github,libretro,gambatte-libretro,$(LIBRETRO_GAMBATTE_VERSION))
LIBRETRO_GAMBATTE_LICENSE = GPL-2.0
LIBRETRO_GAMBATTE_LICENSE_FILES = COPYING

# platform=rpi2 (the Makefile turns it into "rpi2 unix"): -marm
# -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -fomit-frame-pointer, -O2,
# RGB565 video. Plain C++, no asm. HAVE_NETWORK=0 drops the link-cable-over-TCP
# code (sockets, not wanted on the handheld).
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
LIBRETRO_GAMBATTE_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_GAMBATTE_MAKE_OPTS = \
	platform=$(LIBRETRO_GAMBATTE_PLATFORM) \
	HAVE_NETWORK=0 \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_GAMBATTE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile.libretro $(LIBRETRO_GAMBATTE_MAKE_OPTS)
endef

define LIBRETRO_GAMBATTE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/gambatte_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/gambatte_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_GAMBATTE_PKGDIR)/gambatte.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/gambatte.ini
endef

$(eval $(generic-package))
