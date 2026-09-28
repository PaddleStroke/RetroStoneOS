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
# Only for a board with its own device tree in this tree (the RetroStone2):
# other boards leave BR2_RETROSTONE_DTS_DIR empty (docs/porting.md).
ifneq ($(RSOS_DTS_DIR),)
RSOS_KERNEL_DTS_DIR = $(LINUX_DIR)/arch/arm/boot/dts/allwinner

define RSOS_LINUX_INSTALL_DTS
	@if [ ! -f "$(RSOS_DTS_DIR)/sun7i-a20-retrostone2.dts" ]; then \
		echo "RetroStoneOS: $(RSOS_DTS_DIR)/sun7i-a20-retrostone2.dts is missing" >&2; \
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
			printf 'dtb-$$(CONFIG_MACH_SUN7I) += %s\n' "$$out" \
				>> $(RSOS_KERNEL_DTS_DIR)/Makefile || exit 1; \
	done
endef
LINUX_PRE_BUILD_HOOKS += RSOS_LINUX_INSTALL_DTS

# Install the overlays as /boot/overlays/<name>.dtbo, where <name> is the
# .dtso basename without its "retrostone2-" prefix (retrostone2-sata.dtso ->
# sata.dtbo), which is what rsos_overlays in the U-Boot environment refers to.
define RSOS_LINUX_INSTALL_OVERLAYS
	rm -rf $(TARGET_DIR)/boot/overlays
	for f in $(RSOS_DTS_DIR)/overlays/*.dtso; do \
		[ -f "$$f" ] || continue; \
		n=$$(basename "$$f" .dtso); \
		$(INSTALL) -D -m 0644 $(RSOS_KERNEL_DTS_DIR)/$$n.dtbo \
			$(TARGET_DIR)/boot/overlays/$${n#retrostone2-}.dtbo || exit 1; \
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
