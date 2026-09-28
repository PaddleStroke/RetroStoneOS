################################################################################
#
# libretro-gpsp
#
################################################################################

# libretro/gpsp master, 2026-09-19
LIBRETRO_GPSP_VERSION = 5819380c2ffb0900219d700a382ee68c464ebb99
LIBRETRO_GPSP_SITE = $(call github,libretro,gpsp,$(LIBRETRO_GPSP_VERSION))
LIBRETRO_GPSP_LICENSE = GPL-2.0+
LIBRETRO_GPSP_LICENSE_FILES = COPYING

# platform=rpi2: -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
# -ffast-math -fomit-frame-pointer, -O3, CPU_ARCH=arm (-DARM_ARCH),
# HAVE_DYNAREC=1 (ARM recompiler, arm/arm_stub.S) and MMAP_JIT_CACHE=1 (JIT
# cache in an mmap()ed RWX area). The Makefile resets LDFLAGS itself.
# platform= and dynarec per architecture (package/libretro-common.mk,
# docs/cores.md "Architectures"): rpi2 (ARM dynarec) on the RetroStone2,
# arm64 (arm64 dynarec) on aarch64, unix + the x86 dynarec on x86_64.
# Never platform=unix on aarch64: the Makefile reads "uname -a" of the
# build host.
LIBRETRO_GPSP_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64=arm64 default=unix)
LIBRETRO_GPSP_ARCH_OPTS = \
	$(if $(BR2_x86_64), HAVE_DYNAREC=1 CPU_ARCH=x86_32 MMAP_JIT_CACHE=1)

LIBRETRO_GPSP_MAKE_OPTS = \
	platform=$(LIBRETRO_GPSP_PLATFORM)$(LIBRETRO_GPSP_ARCH_OPTS) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_GPSP_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_GPSP_MAKE_OPTS)
endef

define LIBRETRO_GPSP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/gpsp_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/gpsp_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_GPSP_PKGDIR)/gpsp.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/gpsp.ini
endef

$(eval $(generic-package))
