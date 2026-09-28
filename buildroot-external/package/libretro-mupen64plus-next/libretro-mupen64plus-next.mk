################################################################################
#
# libretro-mupen64plus-next
#
################################################################################

# libretro/mupen64plus-libretro-nx master, 2026-09-12. GLideN64 and the other
# sub-projects are vendored (git subrepo), so the GitHub tarball is complete.
LIBRETRO_MUPEN64PLUS_NEXT_VERSION = 6752836de8b224febfd5708444755b77712ac939
LIBRETRO_MUPEN64PLUS_NEXT_SITE = $(call github,libretro,mupen64plus-libretro-nx,$(LIBRETRO_MUPEN64PLUS_NEXT_VERSION))
LIBRETRO_MUPEN64PLUS_NEXT_LICENSE = GPL-2.0
LIBRETRO_MUPEN64PLUS_NEXT_LICENSE_FILES = LICENSE
LIBRETRO_MUPEN64PLUS_NEXT_DEPENDENCIES = libgles libegl $(if $(BR2_x86_64),host-nasm)

# platform=rpi2-mesa: the Raspberry Pi 2 + Mesa recipe, which is our case
# (Cortex-A7 + a Mesa GLES2 driver): GLES=1 (-DEGL -DHAVE_OPENGLES2 -DGLES2,
# GLideN64 GLES2 path), GL_LIB=-lGLESv2, EGL_LIB=-lEGL, -mcpu=cortex-a7
# -mfpu=neon-vfpv4 -mfloat-abi=hard, WITH_DYNAREC=arm (ari64), HAVE_NEON=1
# (GLideN64 NEON 3D math and gSP), -ldl. "mesa" keeps the VideoCore blob
# (/opt/vc, brcmGLESv2) out. LLE stays off (no angrylion/paraLLEl: too slow,
# or Vulkan).
# ARCH=arm: the Makefile otherwise takes "uname -m" of the build host.
# -z noexecstack: see libretro-parallel-n64.mk.
# -U_LARGEFILE64_SOURCE: the bundled zlib (custom/dependencies/libzlib/
# gzguts.h) undefines _FILE_OFFSET_BITS when _LARGEFILE64_SOURCE is set,
# which is a hard #error with 64-bit time_t (BR2_TIME_BITS_64, or any
# toolchain defaulting to _TIME_BITS=64). _FILE_OFFSET_BITS=64 alone still
# gives 64-bit off_t.
LIBRETRO_MUPEN64PLUS_NEXT_MAKE_ENV = \
	$(TARGET_MAKE_ENV) \
	$(TARGET_CONFIGURE_OPTS) \
	CFLAGS="$(TARGET_CFLAGS) -U_LARGEFILE64_SOURCE" \
	LDFLAGS="$(TARGET_LDFLAGS) -Wl,-z,noexecstack"

# platform=, ARCH (= the dynarec) and GL per architecture
# (package/libretro-common.mk, docs/cores.md "Architectures"). The
# frontend's HW render is GLES 2.0, so GLES2 everywhere:
#   RetroStone2: rpi2-mesa ARCH=arm (GLES2, Mesa, ARM dynarec, NEON)
#   aarch64: unix ARCH=aarch64 FORCE_GLES=1 (GLES2, arm64 dynarec; not
#            rpi4_64, which is GLES3). ARCH must be exactly aarch64.
#   x86_64: unix ARCH=x86_64 FORCE_GLES=1 (x64 dynarec, assembled with nasm)
LIBRETRO_MUPEN64PLUS_NEXT_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2-mesa default=unix)
LIBRETRO_MUPEN64PLUS_NEXT_ARCH_OPTS = \
	$(if $(BR2_arm),ARCH=arm) \
	$(if $(BR2_aarch64),ARCH=aarch64 FORCE_GLES=1) \
	$(if $(BR2_x86_64),ARCH=x86_64 FORCE_GLES=1 NASM=$(HOST_DIR)/bin/nasm)

LIBRETRO_MUPEN64PLUS_NEXT_MAKE_OPTS = \
	platform=$(LIBRETRO_MUPEN64PLUS_NEXT_PLATFORM) \
	$(strip $(LIBRETRO_MUPEN64PLUS_NEXT_ARCH_OPTS)) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_MUPEN64PLUS_NEXT_BUILD_CMDS
	$(LIBRETRO_MUPEN64PLUS_NEXT_MAKE_ENV) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_MUPEN64PLUS_NEXT_MAKE_OPTS)
endef

define LIBRETRO_MUPEN64PLUS_NEXT_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/mupen64plus_next_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/mupen64plus_next_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_MUPEN64PLUS_NEXT_PKGDIR)/mupen64plus_next.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/mupen64plus_next.ini
endef

$(eval $(generic-package))
