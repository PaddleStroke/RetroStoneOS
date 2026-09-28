################################################################################
#
# libretro-beetle-wswan
#
################################################################################

# libretro/beetle-wswan-libretro master, 2026-07-31 (Mednafen WonderSwan,
# based on Cygne). The NEC V30MZ core (mednafen/wswan/v30mz.c, Bryan McPhail)
# may be used "for purposes both commercial and noncommercial" with a credit
# in the documentation, which the on-device licence screen must include.
LIBRETRO_BEETLE_WSWAN_VERSION = 4b01295838ea89e3f1355bbe4cb5cf98aa6108cd
LIBRETRO_BEETLE_WSWAN_SITE = $(call github,libretro,beetle-wswan-libretro,$(LIBRETRO_BEETLE_WSWAN_VERSION))
LIBRETRO_BEETLE_WSWAN_LICENSE = GPL-2.0+, V30MZ credit licence (v30mz.c)
LIBRETRO_BEETLE_WSWAN_LICENSE_FILES = COPYING

# platform=rpi2 (the Makefile turns it into "rpi2 unix"): -marm
# -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard, -O2, RGB565 video
# (NEED_BPP=16; XRGB8888 only with the "wswan_gfx_colors" 24-bit option).
# Plain C, no asm.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
LIBRETRO_BEETLE_WSWAN_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_BEETLE_WSWAN_MAKE_OPTS = \
	platform=$(LIBRETRO_BEETLE_WSWAN_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_BEETLE_WSWAN_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_BEETLE_WSWAN_MAKE_OPTS)
endef

define LIBRETRO_BEETLE_WSWAN_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mednafen_wswan_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mednafen_wswan_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_BEETLE_WSWAN_PKGDIR)/mednafen_wswan.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/mednafen_wswan.ini
endef

$(eval $(generic-package))
