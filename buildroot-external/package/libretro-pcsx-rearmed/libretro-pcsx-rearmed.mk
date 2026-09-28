################################################################################
#
# libretro-pcsx-rearmed
#
################################################################################

# libretro/pcsx_rearmed master, 2026-09-24. The only submodule
# (frontend/libpicofe) is not used by the libretro build, so the GitHub
# tarball is enough.
LIBRETRO_PCSX_REARMED_VERSION = ff81ed17a15241d2f3730cdd7585b38e172532ca
LIBRETRO_PCSX_REARMED_SITE = $(call github,libretro,pcsx_rearmed,$(LIBRETRO_PCSX_REARMED_VERSION))
LIBRETRO_PCSX_REARMED_LICENSE = GPL-2.0+
LIBRETRO_PCSX_REARMED_LICENSE_FILES = COPYING

# platform=rpi2: -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard,
# -O3, ARCH=arm, DYNAREC=ari64 (new_dynarec, ARM backend; the default
# "lightrec" is not used on ARM32), BUILTIN_GPU=neon (psx_gpu NEON asm
# renderer), HAVE_NEON_ASM=1 (NEON GTE, NEON GPU asm, NEON colour conversion).
# Makefile defaults kept on purpose for the dual-core A20: NDRC_THREAD=1
# (recompile on a second thread), USE_ASYNC_GPU/SPU/CDROM=1.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
# (rpi2: ARCH=arm, ari64 dynarec, NEON asm GPU; rpi3_64/rpi4_64: ARCH=arm64,
# ari64 arm64 dynarec, the SIMD C GPU; unix on x86_64: lightrec.)
LIBRETRO_PCSX_REARMED_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_PCSX_REARMED_MAKE_OPTS = \
	platform=$(LIBRETRO_PCSX_REARMED_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_PCSX_REARMED_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile.libretro $(LIBRETRO_PCSX_REARMED_MAKE_OPTS)
endef

define LIBRETRO_PCSX_REARMED_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/pcsx_rearmed_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/pcsx_rearmed_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_PCSX_REARMED_PKGDIR)/pcsx_rearmed.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/pcsx_rearmed.ini
endef

$(eval $(generic-package))
