################################################################################
#
# libretro-dosbox-pure
#
################################################################################

# schellingb/dosbox-pure main, 2026-09-15 (libretro/dosbox-pure mirrors it).
# No submodules, no dependencies: one C++ tree.
LIBRETRO_DOSBOX_PURE_VERSION = 73e03aa145e0549ed4d5a20f8e65532714da33f5
LIBRETRO_DOSBOX_PURE_SITE = $(call github,schellingb,dosbox-pure,$(LIBRETRO_DOSBOX_PURE_VERSION))
LIBRETRO_DOSBOX_PURE_LICENSE = GPL-2.0+
LIBRETRO_DOSBOX_PURE_LICENSE_FILES = LICENSE

# The Makefile has no "platform" for Linux: it builds the generic .so with
# its own flags (CFLAGS := -O2 ..., so Buildroot's CFLAGS are not used; the
# toolchain wrapper still adds the CPU/ABI flags). The CPU is only guessed
# from "uname -m" of the build host, so MAKE_CPUFLAGS is given per
# architecture instead:
# - arm (Cortex-A7): nothing; include/config.h turns on the DOSBox dynrec
#   with the ARMV7LE backend (C_DYNREC, C_UNALIGNED_MEMORY) from __arm__ and
#   __ARM_ARCH >= 7. No -ffast-math (upstream only uses it for the Pi 4).
# - aarch64: dynrec ARMV8LE; -DPAGESIZE=4096 is what upstream passes on an
#   aarch64 host (the dynrec code cache alignment).
# - x86_64: the dynamic_x86 core, nothing to add.
# STRIP comes from TARGET_CONFIGURE_OPTS (the Makefile strips the .so).
LIBRETRO_DOSBOX_PURE_CPUFLAGS = \
	$(call rsos-libretro-select,aarch64=-DPAGESIZE=4096 default=)

LIBRETRO_DOSBOX_PURE_MAKE_OPTS = \
	MAKE_CPUFLAGS="$(LIBRETRO_DOSBOX_PURE_CPUFLAGS)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_DOSBOX_PURE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_DOSBOX_PURE_MAKE_OPTS)
endef

define LIBRETRO_DOSBOX_PURE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/dosbox_pure_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/dosbox_pure_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_DOSBOX_PURE_PKGDIR)/dosbox_pure.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/dosbox_pure.ini
endef

$(eval $(generic-package))
