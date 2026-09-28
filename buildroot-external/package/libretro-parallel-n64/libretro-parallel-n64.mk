################################################################################
#
# libretro-parallel-n64
#
################################################################################

# libretro/parallel-n64 master, 2026-09-19
LIBRETRO_PARALLEL_N64_VERSION = 6e4c44c51885c8dc16e46d68464c517e6fca6712
LIBRETRO_PARALLEL_N64_SITE = $(call github,libretro,parallel-n64,$(LIBRETRO_PARALLEL_N64_VERSION))
LIBRETRO_PARALLEL_N64_LICENSE = GPL-2.0 (mupen64plus core), GPL-2.0+ (video plugins)
LIBRETRO_PARALLEL_N64_LICENSE_FILES = mupen64plus-core/LICENSES
LIBRETRO_PARALLEL_N64_DEPENDENCIES = libgles $(if $(BR2_x86_64),host-nasm)

# Same recipe as RetrOrangePi's lr-parallel-n64 (the build that ran on this
# device), with the generic "unix" platform so nothing Raspberry Pi specific
# (/opt/vc) is pulled in:
# - GLES=1 GL_LIB=-lGLESv2: GLES2 build (-DHAVE_OPENGLES2, glsym_es2.c, HW
#   render context RETRO_HW_CONTEXT_OPENGLES2). The Makefile drops GLideN64
#   for GLES2; glide64, gln64 and rice remain, plus angrylion (software).
# - ARCH=arm WITH_DYNAREC=arm: ari64 new_dynarec, ARM backend
#   (NEW_DYNAREC=3). ARCH must be given: the Makefile defaults it to
#   "uname -m", i.e. the build host.
# - HAVE_NEON=1 and CPUFLAGS: NEON code paths (-D__NEON_OPT) and the ARM
#   options RetroPie/RetrOrangePi use (ARM_ASM, ARM_FIX, NO_ASM for the x86
#   asm, NOSSE). -mcpu/-mfpu come from the Buildroot toolchain wrapper.
# - -Ofast (Makefile default for unix).
# 0001-libretro-count-per-op-and-frameskip-core-options.patch (applied by
# Buildroot): the parallel-n64-CountPerOp and parallel-n64-frameskip core
# options (docs/cores.md, "N64 performance"), and the "auto" GFX plugin
# value no longer swallowing the gln64 choice at startup.
# -z noexecstack: keep GNU_STACK non-executable whatever the dynarec asm
# says (glibc >= 2.41 refuses to dlopen() libraries that need an executable
# stack).
LIBRETRO_PARALLEL_N64_MAKE_ENV = \
	$(TARGET_MAKE_ENV) \
	$(TARGET_CONFIGURE_OPTS) \
	LDFLAGS="$(TARGET_LDFLAGS) -Wl,-z,noexecstack"

# ARCH, dynarec and CPU flags per architecture (package/libretro-common.mk,
# docs/cores.md "Architectures"). platform=unix everywhere (rpi4_64 is
# the Vulkan-only build); ARCH is always given (the Makefile would take
# "uname -m" of the build host). GLES2 everywhere.
#   RetroStone2: ARM dynarec (NEW_DYNAREC=3), NEON, the ARM CPUFLAGS
#   aarch64: arm64 dynarec (new_dynarec/arm64), no ARM32 CPUFLAGS
#            (-D__arm__ would select ARM32 code)
#   x86_64: x86_64 dynarec (+ Hacktarux dispatch, nasm)
ifeq ($(BR2_arm),y)
LIBRETRO_PARALLEL_N64_ARCH_OPTS = \
	ARCH=arm \
	WITH_DYNAREC=arm \
	HAVE_NEON=1 \
	GLES=1 \
	GL_LIB=-lGLESv2 \
	CPUFLAGS="-DNO_ASM -DARM -D__arm__ -DARM_ASM -D__NEON_OPT -DNOSSE -DARM_FIX"
else ifeq ($(BR2_aarch64),y)
LIBRETRO_PARALLEL_N64_ARCH_OPTS = \
	ARCH=aarch64 \
	WITH_DYNAREC=aarch64 \
	HAVE_NEON=0 \
	GLES=1 \
	GL_LIB=-lGLESv2 \
	CPUFLAGS=
else
LIBRETRO_PARALLEL_N64_ARCH_OPTS = \
	ARCH=x86_64 \
	WITH_DYNAREC=x86_64 \
	HAVE_NEON=0 \
	GLES=1 \
	GL_LIB=-lGLESv2 \
	CPUFLAGS= \
	NASM=$(HOST_DIR)/bin/nasm
endif

LIBRETRO_PARALLEL_N64_MAKE_OPTS = \
	platform=unix \
	$(LIBRETRO_PARALLEL_N64_ARCH_OPTS) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_PARALLEL_N64_BUILD_CMDS
	$(LIBRETRO_PARALLEL_N64_MAKE_ENV) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_PARALLEL_N64_MAKE_OPTS)
endef

define LIBRETRO_PARALLEL_N64_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/parallel_n64_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/parallel_n64_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_PARALLEL_N64_PKGDIR)/parallel_n64.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/parallel_n64.ini
	$(INSTALL) -D -m 0644 $(LIBRETRO_PARALLEL_N64_PKGDIR)/gles2n64rom.conf \
		$(TARGET_DIR)/usr/share/rsos/cores/parallel_n64/gles2n64rom.conf
endef

$(eval $(generic-package))
