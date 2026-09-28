################################################################################
#
# rsos-freedoom
#
################################################################################

# Freedoom 0.13.0 (2024-01-30), the latest release: a free IWAD for Doom
# engines, BSD-3-Clause. Pre-installed for libretro-prboom (docs/homebrew.md).
RSOS_FREEDOOM_VERSION = 0.13.0
RSOS_FREEDOOM_SOURCE = freedoom-$(RSOS_FREEDOOM_VERSION).zip
RSOS_FREEDOOM_SITE = https://github.com/freedoom/freedoom/releases/download/v$(RSOS_FREEDOOM_VERSION)
RSOS_FREEDOOM_LICENSE = BSD-3-Clause
RSOS_FREEDOOM_LICENSE_FILES = COPYING.txt
# Data only: nothing is compiled.
RSOS_FREEDOOM_ADD_TOOLCHAIN_DEPENDENCY = NO

define RSOS_FREEDOOM_EXTRACT_CMDS
	$(UNZIP) -d $(@D) $(RSOS_FREEDOOM_DL_DIR)/$(RSOS_FREEDOOM_SOURCE)
	mv $(@D)/freedoom-$(RSOS_FREEDOOM_VERSION)/* $(@D)/
	rmdir $(@D)/freedoom-$(RSOS_FREEDOOM_VERSION)
endef

RSOS_FREEDOOM_WADS = \
	$(if $(BR2_PACKAGE_RSOS_FREEDOOM_PHASE1),freedoom1.wad) \
	$(if $(BR2_PACKAGE_RSOS_FREEDOOM_PHASE2),freedoom2.wad)

RSOS_FREEDOOM_TARGET_ROMS = $(TARGET_DIR)/usr/share/rsos/homebrew/doom
RSOS_FREEDOOM_TARGET_LICENSES = $(TARGET_DIR)/usr/share/rsos/licenses/freedoom

# rsos-seed-homebrew copies /usr/share/rsos/homebrew/<system>/ to
# /data/roms/<system>/ on the first boot (never overwriting) and merges
# gamelist.xml. The gamelist lists both phases; the frontend drops an entry
# whose file is missing.
define RSOS_FREEDOOM_INSTALL_TARGET_CMDS
	rm -rf $(RSOS_FREEDOOM_TARGET_ROMS) $(RSOS_FREEDOOM_TARGET_LICENSES)
	$(INSTALL) -D -m 0644 $(RSOS_FREEDOOM_PKGDIR)/doom.xml \
		$(RSOS_FREEDOOM_TARGET_ROMS)/gamelist.xml
	$(foreach w,$(RSOS_FREEDOOM_WADS),
		$(INSTALL) -D -m 0644 $(@D)/$(w) $(RSOS_FREEDOOM_TARGET_ROMS)/$(w)
	)
	$(INSTALL) -D -m 0644 $(@D)/COPYING.txt \
		$(RSOS_FREEDOOM_TARGET_LICENSES)/COPYING.txt
	$(INSTALL) -D -m 0644 $(@D)/CREDITS.txt \
		$(RSOS_FREEDOOM_TARGET_LICENSES)/CREDITS.txt
	$(INSTALL) -D -m 0644 $(@D)/CREDITS-MUSIC.txt \
		$(RSOS_FREEDOOM_TARGET_LICENSES)/CREDITS-MUSIC.txt
endef

$(eval $(generic-package))
