################################################################################
#
# libretro-common.mk: the target, as the libretro core makefiles see it
#
# Included by ../external.mk before the packages. Each libretro-*.mk picks
# its "platform=" value (and its dynarec options) per architecture with
# rsos-libretro-select, so one tree builds for:
#   arm-a7       ARMv7 Cortex-A7, hard-float + NEON: the RetroStone2
#                (Allwinner A20). The values are the ones used before this
#                helper existed, e.g. platform=rpi2 (docs/cores.md).
#   aarch64-a72  Raspberry Pi 4 (Cortex-A72), 64-bit
#   aarch64-a53  Raspberry Pi 3 (Cortex-A53), 64-bit
#   aarch64      any other 64-bit ARM (Pi 5 Cortex-A76, ...)
#   x86_64       a PC or VM test build
# See docs/cores.md, "Architectures", for the per-core table.
#
# Notes that apply to every core:
# - Buildroot's toolchain wrapper adds -mcpu/-march for BR2_cortex_* only
#   when the command line has none: a core platform that sets -mcpu/-mtune
#   (rpi2, rpi3_64, rpi4_64) must match the CPU, hence the per-CPU keys.
#   "platform=unix" leaves the CPU flags to the wrapper.
# - Buildroot does "unexport ARCH", and many core makefiles guess the
#   architecture with "uname -m" of the build host (x86_64): the dynarec
#   cores are given ARCH (or their arm64 platform) explicitly.
#
################################################################################

ifeq ($(BR2_arm),y)
RSOS_LIBRETRO_ARCH = arm
else ifeq ($(BR2_aarch64),y)
RSOS_LIBRETRO_ARCH = aarch64
else ifeq ($(BR2_x86_64),y)
RSOS_LIBRETRO_ARCH = x86_64
else
RSOS_LIBRETRO_ARCH = unknown
endif

RSOS_LIBRETRO_CPU = \
	$(if $(BR2_cortex_a7),a7,$(if $(BR2_cortex_a53),a53,$(if $(BR2_cortex_a72),a72,$(if $(BR2_cortex_a76),a76,generic))))

# Keys tried in order: <arch>-<cpu>, <arch>, default.
RSOS_LIBRETRO_KEYS = $(RSOS_LIBRETRO_ARCH)-$(strip $(RSOS_LIBRETRO_CPU)) $(RSOS_LIBRETRO_ARCH) default

# $(call rsos-libretro-select,KEY=VALUE KEY=VALUE ...): the value of the
# first key of RSOS_LIBRETRO_KEYS found in the list, e.g.
#   $(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 default=unix)
# gives rpi2 on the RetroStone2, rpi4_64 on a Pi 4 and unix on x86_64.
# Values cannot contain spaces (use a per-architecture variable for option
# lists, see libretro-parallel-n64.mk).
rsos-libretro-select = \
	$(firstword $(foreach k,$(RSOS_LIBRETRO_KEYS),$(patsubst $(k)=%,%,$(filter $(k)=%,$(1)))))
