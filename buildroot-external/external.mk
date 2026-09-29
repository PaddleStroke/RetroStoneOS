################################################################################
#
# RetroStoneOS BR2_EXTERNAL glue
#
################################################################################

# Packages written in package/<name>/<name>.mk (rsos-frontend, libretro-*).
# Remember to also add their Config.in to ../Config.in (kconfig cannot glob).
# package/libretro-common.mk first: the architecture helpers the libretro-*
# packages use (rsos-libretro-select).
include $(BR2_EXTERNAL_RETROSTONE_PATH)/package/libretro-common.mk
include $(sort $(wildcard $(BR2_EXTERNAL_RETROSTONE_PATH)/package/*/*.mk))

################################################################################
# Out-of-tree device tree and overlays
#
# Since Linux 6.5 the ARM device trees live in vendor subdirectories
# (arch/arm/boot/dts/allwinner/), and since 6.12 a .dtb must be listed in the
# Makefile of its directory. BR2_LINUX_KERNEL_CUSTOM_DTS_PATH (deprecated)
# copies to arch/arm/boot/dts/ and cannot handle that, and
# BR2_LINUX_KERNEL_CUSTOM_DTS_DIR needs an "allwinner/" subdirectory in our
# tree. So the defconfig uses BR2_LINUX_KERNEL_INTREE_DTS_NAME =
# "allwinner/sun7i-a20-retrostone2" and this hook drops our .dts/.dtsi (and
# overlays/*.dtso) into the kernel tree and registers them in its Makefile,
# right before every kernel build. The kernel then builds them with its own
# dtc, include paths and warnings (and -@, BR2_LINUX_KERNEL_DTB_OVERLAY_SUPPORT).
# After editing the DTS, run "make linux-rebuild".
################################################################################
RSOS_DTS_DIR = $(call qstrip,$(BR2_RETROSTONE_DTS_DIR))
# Only for a board with its own device tree in this tree (the RetroStone2,
# the RetroStone1): other boards leave BR2_RETROSTONE_DTS_DIR empty
# (docs/porting.md). The board's .dts is the first name of
# BR2_LINUX_KERNEL_INTREE_DTS_NAME; its SoC prefix picks the kernel Makefile
# symbol (sun7i- -> CONFIG_MACH_SUN7I, sun8i- -> CONFIG_MACH_SUN8I).
ifneq ($(RSOS_DTS_DIR),)
RSOS_KERNEL_DTS_DIR = $(LINUX_DIR)/arch/arm/boot/dts/allwinner
RSOS_DTS_MAIN = $(notdir $(firstword $(call qstrip,$(BR2_LINUX_KERNEL_INTREE_DTS_NAME))))
RSOS_DTS_MACH = $(if $(filter sun8i-%,$(RSOS_DTS_MAIN)),CONFIG_MACH_SUN8I,CONFIG_MACH_SUN7I)

define RSOS_LINUX_INSTALL_DTS
	@if [ ! -f "$(RSOS_DTS_DIR)/$(RSOS_DTS_MAIN).dts" ]; then \
		echo "RetroStoneOS: $(RSOS_DTS_DIR)/$(RSOS_DTS_MAIN).dts is missing" >&2; \
		exit 1; \
	fi
	for f in $(RSOS_DTS_DIR)/*.dts $(RSOS_DTS_DIR)/*.dtsi \
		 $(RSOS_DTS_DIR)/overlays/*.dtso; do \
		[ -f "$$f" ] || continue; \
		cp -f "$$f" $(RSOS_KERNEL_DTS_DIR)/ || exit 1; \
		case "$$f" in \
		*.dts) out=$$(basename "$$f" .dts).dtb ;; \
		*.dtso) out=$$(basename "$$f" .dtso).dtbo ;; \
		*) continue ;; \
		esac; \
		grep -q "+= $$out\$$" $(RSOS_KERNEL_DTS_DIR)/Makefile || \
			printf 'dtb-$$(%s) += %s\n' "$(RSOS_DTS_MACH)" "$$out" \
				>> $(RSOS_KERNEL_DTS_DIR)/Makefile || exit 1; \
	done
endef
LINUX_PRE_BUILD_HOOKS += RSOS_LINUX_INSTALL_DTS

# Install the overlays as /boot/overlays/<name>.dtbo, where <name> is the
# .dtso basename without its "retrostone<N>-" prefix (retrostone2-sata.dtso ->
# sata.dtbo), which is what rsos_overlays in the U-Boot environment refers to.
define RSOS_LINUX_INSTALL_OVERLAYS
	rm -rf $(TARGET_DIR)/boot/overlays
	for f in $(RSOS_DTS_DIR)/overlays/*.dtso; do \
		[ -f "$$f" ] || continue; \
		n=$$(basename "$$f" .dtso); \
		o=$${n#retrostone2-}; o=$${o#retrostone1-}; \
		$(INSTALL) -D -m 0644 $(RSOS_KERNEL_DTS_DIR)/$$n.dtbo \
			$(TARGET_DIR)/boot/overlays/$$o.dtbo || exit 1; \
	done
endef
LINUX_POST_INSTALL_TARGET_HOOKS += RSOS_LINUX_INSTALL_OVERLAYS
endif

################################################################################
# Bluetooth firmware for the AP6210 (BCM20710) and AP6212 (BCM43430A1).
# Not in linux-firmware; taken from the Armbian firmware repository, which the
# armbian-firmware package already downloads (none of its own options needed).
# btbcm looks for brcm/<chip>.hcd; see post-build.sh for the aliases.
# TODO(hw): check in dmesg which .hcd name btbcm actually requests.
################################################################################
define RSOS_ARMBIAN_FIRMWARE_INSTALL_BT
	$(INSTALL) -D -m 0644 $(ARMBIAN_FIRMWARE_DIR)/ap6210/bcm20710a1.hcd \
		$(TARGET_DIR)/lib/firmware/brcm/BCM20710A1.hcd
	$(INSTALL) -D -m 0644 $(ARMBIAN_FIRMWARE_DIR)/brcm/BCM43430A1.hcd \
		$(TARGET_DIR)/lib/firmware/brcm/BCM43430A1.hcd
endef
ARMBIAN_FIRMWARE_POST_INSTALL_TARGET_HOOKS += RSOS_ARMBIAN_FIRMWARE_INSTALL_BT

################################################################################
# The on-device licence notice: /usr/share/rsos/licenses.txt lists every
# package of the image with its version and licence (its <pkg>_LICENSE, as
# legal-info reports it), plus the toolchain's runtime libraries. Written at
# target-finalize from what make already knows (no legal-info run, no
# download); the boot never reads it (docs/build.md, "Licences").
################################################################################
RSOS_LICENSES_TXT = $(TARGET_DIR)/usr/share/rsos/licenses.txt
# the image's packages: no host tools, no virtual or toolchain meta packages
RSOS_LICENSE_PKGS = $(sort $(foreach p,$(filter-out host-% toolchain toolchain-external toolchain-external-%,$(PACKAGES)),\
	$(if $(filter YES,$($(call UPPERCASE,$(p))_IS_VIRTUAL)),,$(p))))
rsos_shq = '$(subst ','\'',$(1))'
# no declared licence: Buildroot's own scripts and skeleton files (no
# version, no source) are Buildroot's GPL-2.0+
rsos_lic = $(if $(filter-out unknown,$($(call UPPERCASE,$(1))_LICENSE)),$($(call UPPERCASE,$(1))_LICENSE),$(if \
	$($(call UPPERCASE,$(1))_VERSION),not declared (see the legal-info archive),GPL-2.0+ (files of Buildroot)))

define RSOS_WRITE_LICENSES_TXT
	mkdir -p $(dir $(RSOS_LICENSES_TXT))
	{ \
	printf '%s\n' 'The software in RetroStoneOS and its licences' '' \
		'Each line: package version: licence (SPDX identifiers).' \
		'The licence texts and the complete source code of these packages are' \
		'published with every release (its legal-info archive):' \
		'https://github.com/PaddleStroke/RetroStoneOS/releases' ''; \
	$(if $(BR2_TOOLCHAIN_USES_GLIBC),printf '%s\n' 'C library (glibc, from the toolchain): LGPL-2.1+';) \
	$(if $(BR2_TOOLCHAIN_USES_MUSL),printf '%s\n' 'C library (musl, from the toolchain): MIT';) \
	$(if $(BR2_TOOLCHAIN_USES_UCLIBC),printf '%s\n' 'C library (uClibc-ng, from the toolchain): LGPL-2.1+';) \
	printf '%s\n' 'GCC runtime libraries (from the toolchain): GPL-3.0+ with the GCC Runtime Library Exception'; \
	$(foreach p,$(RSOS_LICENSE_PKGS),printf '%s %s: %s\n' $(call rsos_shq,$(p)) \
		$(call rsos_shq,$(or $($(call UPPERCASE,$(p))_VERSION),-)) \
		$(call rsos_shq,$(call rsos_lic,$(p)));) \
	} > $(RSOS_LICENSES_TXT)
	chmod 0644 $(RSOS_LICENSES_TXT)
endef
TARGET_FINALIZE_HOOKS += RSOS_WRITE_LICENSES_TXT
