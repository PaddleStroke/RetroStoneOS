################################################################################
#
# libretro-fuse
#
################################################################################

# libretro/fuse-libretro master, 2026-09-26. No submodules: fuse,
# libspectrum, zlib and bzip2 are in the tree.
LIBRETRO_FUSE_VERSION = e997e2bc32c888348f862f69f2c53babfedf7791
LIBRETRO_FUSE_SITE = $(call github,libretro,fuse-libretro,$(LIBRETRO_FUSE_VERSION))
LIBRETRO_FUSE_LICENSE = GPL-3.0 (libretro port), GPL-2.0+ (Fuse, libspectrum), bzip2-1.0.6 (bzip2), Zlib (zlib), Amstrad ROM distribution permission (Spectrum ROMs)
LIBRETRO_FUSE_LICENSE_FILES = \
	LICENSE \
	fuse/COPYING \
	libspectrum/COPYING \
	bzip2/LICENSE \
	fuse/roms/README.copyright

# platform=rpi2: -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
# -fomit-frame-pointer -ffast-math -DARM, -O3. rpi4_64 / rpi3_64 on the
# Pi 4 / Pi 3, unix elsewhere. Plain C, no asm.
LIBRETRO_FUSE_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_FUSE_MAKE_OPTS = \
	platform=$(LIBRETRO_FUSE_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_FUSE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile.libretro $(LIBRETRO_FUSE_MAKE_OPTS)
endef

define LIBRETRO_FUSE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/fuse_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/fuse_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_FUSE_PKGDIR)/fuse.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/fuse.ini
endef

$(eval $(generic-package))
