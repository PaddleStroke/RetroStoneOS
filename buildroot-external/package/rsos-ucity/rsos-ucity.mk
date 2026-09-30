################################################################################
#
# rsos-ucity
#
################################################################################

# uCity 1.2 (Antonio Nino Diaz), a city builder for the Game Boy Color,
# pre-installed in roms/gbc/ (docs/homebrew.md). The ROM is the upstream
# release file, unchanged; the main download is the source of the same tag,
# so that "make legal-info" publishes it with the ROM (GPL-3.0+). Nothing is
# built: the ROM is the release file of that tag (building it would need the
# RGBDS assembler).
RSOS_UCITY_VERSION = 1.2
RSOS_UCITY_SITE = $(call github,AntonioND,ucity,v$(RSOS_UCITY_VERSION))
RSOS_UCITY_EXTRA_DOWNLOADS = \
	https://github.com/AntonioND/ucity/releases/download/v$(RSOS_UCITY_VERSION)/ucity.gbc
RSOS_UCITY_LICENSE = GPL-3.0+ (code), BSD-2-Clause (GBT Player), \
	CC-BY-SA-4.0 (graphics and music)
# readme.rst: the licence terms of each part (the CC BY-SA 4.0 media, the
# GPL headers); gbt_player.asm: the BSD-2-Clause text of GBT Player.
RSOS_UCITY_LICENSE_FILES = gpl-3.0.txt readme.rst source/engine/gbt_player.asm
# Data only: nothing is compiled.
RSOS_UCITY_ADD_TOOLCHAIN_DEPENDENCY = NO

RSOS_UCITY_TARGET_ROMS = $(TARGET_DIR)/usr/share/rsos/homebrew/gbc
RSOS_UCITY_TARGET_LICENSES = $(TARGET_DIR)/usr/share/rsos/licenses/ucity

# rsos-seed-homebrew (rsos-homebrew) copies /usr/share/rsos/homebrew/gbc/ to
# /data/roms/gbc/ on the first boot and merges gamelist.xml. The licence
# texts and the NOTICE (credits, source offer) are LICENSES/homebrew/ucity/.
define RSOS_UCITY_INSTALL_TARGET_CMDS
	rm -rf $(RSOS_UCITY_TARGET_ROMS) $(RSOS_UCITY_TARGET_LICENSES)
	$(INSTALL) -D -m 0644 $(RSOS_UCITY_DL_DIR)/ucity.gbc \
		$(RSOS_UCITY_TARGET_ROMS)/ucity.gbc
	$(INSTALL) -D -m 0644 $(RSOS_UCITY_PKGDIR)/gamelist.xml \
		$(RSOS_UCITY_TARGET_ROMS)/gamelist.xml
	mkdir -p $(RSOS_UCITY_TARGET_LICENSES)
	$(INSTALL) -m 0644 $(BR2_EXTERNAL_RETROSTONE_PATH)/../LICENSES/homebrew/ucity/* \
		$(RSOS_UCITY_TARGET_LICENSES)/
endef

$(eval $(generic-package))
