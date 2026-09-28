################################################################################
#
# libretro-bluemsx
#
################################################################################

# libretro/blueMSX-libretro master, 2026-08-23. license.txt: the blueMSX code
# itself is under a zlib-style licence ("any purpose, including commercial
# applications"); some sound chips and the R800 disassembler come from openMSX
# and fMSX-SDL (GPL-2.0+); TinyXML and minizip are zlib; the V9938 command
# engine is "free for any MSX emulator, commercial or not". fMSX-derived code
# is gone (fMSX itself is non-commercial and is not packaged).
LIBRETRO_BLUEMSX_VERSION = e3086eb5d36d77fa11704cf53dc176686e70127d
LIBRETRO_BLUEMSX_SITE = $(call github,libretro,blueMSX-libretro,$(LIBRETRO_BLUEMSX_VERSION))
LIBRETRO_BLUEMSX_LICENSE = Zlib (blueMSX, TinyXML, minizip), GPL-2.0+ (openMSX/fMSX-SDL sound chips), BSD-2-Clause (C-BIOS)
LIBRETRO_BLUEMSX_LICENSE_FILES = license.txt

# Makefile.libretro, platform=rpi2: -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4
# -mfloat-abi=hard -fomit-frame-pointer -ffast-math -DARM, -O2, RGB565 video,
# -DSINGLE_THREADED. No asm (HAVE_NEON has no effect in this core).
# Makefile.libretro starts with "CFLAGS := -std=gnu89" / "CXXFLAGS := ...",
# which discards Buildroot's CFLAGS from the environment (LFS defines, -O2
# -g0); the toolchain wrapper still adds the CPU/ABI flags, and the Makefile
# its own -O2.
# platform= per architecture (package/libretro-common.mk, docs/cores.md
# "Architectures"): rpi2 on the RetroStone2 (Cortex-A7).
LIBRETRO_BLUEMSX_PLATFORM = \
	$(call rsos-libretro-select,arm-a7=rpi2 aarch64-a72=rpi4_64 aarch64-a53=rpi3_64 default=unix)

LIBRETRO_BLUEMSX_MAKE_OPTS = \
	platform=$(LIBRETRO_BLUEMSX_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_BLUEMSX_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile.libretro $(LIBRETRO_BLUEMSX_MAKE_OPTS)
endef

# blueMSX reads <system dir>/Machines/<machine>/config.ini and
# <system dir>/Databases/*.xml. The upstream system/bluemsx/ tree also carries
# the original (copyrighted) MSX system ROMs in "Machines/Shared Roms": those
# are NOT installed. Only the databases and the three C-BIOS machines are
# (C-BIOS: free replacement MSX BIOS, BSD-2-Clause, see its cbios.txt).
# The core's default "Auto" machine for cartridges, disks and tapes is
# "MSX2+", so Machines/MSX2+/config.ini (and MSX, MSX2) are installed as
# copies of the C-BIOS configs, whose ROM paths point at the C-BIOS folders:
# cartridges then run out of the box. A user who copies the full blueMSX
# Machines folder (with real BIOS ROMs) into the system directory replaces
# them. The frontend copies this tree into the system directory, see
# "system_tree" in bluemsx.ini.
LIBRETRO_BLUEMSX_SYSDIR = $(TARGET_DIR)/usr/share/rsos/cores/bluemsx

define LIBRETRO_BLUEMSX_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bluemsx_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/bluemsx_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_BLUEMSX_PKGDIR)/bluemsx.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/bluemsx.ini
	rm -rf $(LIBRETRO_BLUEMSX_SYSDIR)
	mkdir -p $(LIBRETRO_BLUEMSX_SYSDIR)/Databases $(LIBRETRO_BLUEMSX_SYSDIR)/Machines
	$(INSTALL) -m 0644 $(@D)/system/bluemsx/Databases/*.xml \
		$(LIBRETRO_BLUEMSX_SYSDIR)/Databases/
	for m in "MSX - C-BIOS" "MSX2 - C-BIOS" "MSX2+ - C-BIOS"; do \
		mkdir -p "$(LIBRETRO_BLUEMSX_SYSDIR)/Machines/$$m" && \
		$(INSTALL) -m 0644 "$(@D)/system/bluemsx/Machines/$$m"/* \
			"$(LIBRETRO_BLUEMSX_SYSDIR)/Machines/$$m/" || exit 1; \
	done
	$(INSTALL) -D -m 0644 "$(@D)/system/bluemsx/Machines/MSX - C-BIOS/config.ini" \
		"$(LIBRETRO_BLUEMSX_SYSDIR)/Machines/MSX/config.ini"
	$(INSTALL) -D -m 0644 "$(@D)/system/bluemsx/Machines/MSX2 - C-BIOS/config.ini" \
		"$(LIBRETRO_BLUEMSX_SYSDIR)/Machines/MSX2/config.ini"
	$(INSTALL) -D -m 0644 "$(@D)/system/bluemsx/Machines/MSX2+ - C-BIOS/config.ini" \
		"$(LIBRETRO_BLUEMSX_SYSDIR)/Machines/MSX2+/config.ini"
endef

$(eval $(generic-package))
