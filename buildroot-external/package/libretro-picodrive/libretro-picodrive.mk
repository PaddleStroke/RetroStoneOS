################################################################################
#
# libretro-picodrive
#
################################################################################

# libretro/picodrive master, 2026-09-04. Git, not a GitHub tarball: the build
# needs the submodules (cpu/cyclone, pico/sound/emu2413, pico/cd/libchdr,
# platform/common/dr_libs, platform/libpicofe).
LIBRETRO_PICODRIVE_VERSION = ab021146b70eef7ec0ac2afe06a94e9b4c16ef74
LIBRETRO_PICODRIVE_SITE = https://github.com/libretro/picodrive.git
LIBRETRO_PICODRIVE_SITE_METHOD = git
LIBRETRO_PICODRIVE_GIT_SUBMODULES = YES
LIBRETRO_PICODRIVE_LICENSE = PicoDrive non-commercial license
LIBRETRO_PICODRIVE_LICENSE_FILES = COPYING

# platform=rpi2: -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
# -ffast-math, -O3. With ARCH=arm the main Makefile enables every ARM path:
# use_cyclone, use_drz80, use_sh2drc, use_svpdrc and all asm_* renderers /
# memory handlers / YM2612 / mixer.
#
# Cyclone.s is generated at build time by a host tool (cpu/cyclone/cyclone_gen):
# the recipe exports CC=$(CYCLONE_CC) to that sub-make. A CC given on the
# command line would be inherited by the sub-make and override it, so the
# target CC/CXX are passed in the environment only (Makefile.libretro uses
# "CC ?= gcc"), and CYCLONE_CC/CYCLONE_CXX point at the host compiler.
#
# Cyclone.s and DrZ80 have no .note.GNU-stack section, so the linker would
# mark the .so as needing an executable stack, and glibc >= 2.41 refuses to
# dlopen() such objects. They do not need one: force -z noexecstack.
#
# -U_LARGEFILE64_SOURCE: with Buildroot's -D_LARGEFILE64_SOURCE
# -D_FILE_OFFSET_BITS=64, dr_mp3.h calls fopen64(), which the libretro VFS
# macros (fopen -> rfopen) do not remap, and assigns its FILE * to an RFILE *.
# GCC >= 14 rejects that (-Wincompatible-pointer-types is an error), and it
# would bypass the VFS at run time. _FILE_OFFSET_BITS=64 alone still gives
# 64-bit off_t.
LIBRETRO_PICODRIVE_MAKE_ENV = \
	$(TARGET_MAKE_ENV) \
	$(TARGET_CONFIGURE_OPTS) \
	CFLAGS="$(TARGET_CFLAGS) -U_LARGEFILE64_SOURCE" \
	LDFLAGS="$(TARGET_LDFLAGS) -Wl,-z,noexecstack"

# platform= and ARCH per architecture (package/libretro-common.mk,
# docs/cores.md "Architectures"): rpi2 + ARCH=arm (Cyclone, DrZ80, SH2
# and SVP DRCs, ARM asm) on the RetroStone2; FAME + CZ80 + the SH2 DRC
# (arm64 or x86 emitter) elsewhere.
LIBRETRO_PICODRIVE_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64=aarch64 default=unix)
LIBRETRO_PICODRIVE_ARCH = \
	$(call rsos-libretro-select,arm=arm aarch64=aarch64 default=x86_64)

LIBRETRO_PICODRIVE_MAKE_OPTS = \
	platform=$(LIBRETRO_PICODRIVE_PLATFORM) \
	ARCH=$(LIBRETRO_PICODRIVE_ARCH) \
	CYCLONE_CC="$(HOSTCC)" \
	CYCLONE_CXX="$(HOSTCXX)"

define LIBRETRO_PICODRIVE_BUILD_CMDS
	$(LIBRETRO_PICODRIVE_MAKE_ENV) \
		$(MAKE) -C $(@D) -f Makefile.libretro $(LIBRETRO_PICODRIVE_MAKE_OPTS)
endef

define LIBRETRO_PICODRIVE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/picodrive_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/picodrive_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_PICODRIVE_PKGDIR)/picodrive.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/picodrive.ini
endef

$(eval $(generic-package))
