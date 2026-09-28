################################################################################
#
# rsos-homebrew
#
################################################################################

# The owner's approved set of homebrew games (docs/homebrew.md). They are
# shipped with the authors' permission for the RetroStoneOS images only, so they are
# not in git: the package syncs them from a local directory (by default
# homebrew/license ok/ next to buildroot-external/). The path has a space in
# it, which make cannot keep in a word, so it is escaped for the shell.
RSOS_HOMEBREW_SITE = $(subst $(space),\$(space),$(call qstrip,$(BR2_PACKAGE_RSOS_HOMEBREW_SOURCE_DIR)))
RSOS_HOMEBREW_SITE_METHOD = local
RSOS_HOMEBREW_LICENSE = \
	Proprietary (permission to 8BCraft to ship them in RetroStoneOS images), \
	GPL-3.0+ (ucity code), BSD-2-Clause (ucity GBT Player), \
	CC-BY-SA-4.0 (ucity media)
RSOS_HOMEBREW_LICENSE_FILES = \
	rsos-licenses/PERMISSION-8BCraft-RetroStone2.txt \
	rsos-licenses/ucity/NOTICE.txt \
	rsos-licenses/ucity/GPL-3.0.txt \
	rsos-licenses/ucity/GBT-Player-BSD-2-Clause.txt \
	rsos-licenses/ucity/CC-BY-SA-4.0.txt
# The ROMs may only be shipped inside RetroStoneOS images: keep them out
# of "make legal-info" (the ucity source offer is in its NOTICE.txt).
RSOS_HOMEBREW_REDISTRIBUTE = NO
# Data files and a shell script: nothing is compiled.
RSOS_HOMEBREW_ADD_TOOLCHAIN_DEPENDENCY = NO

RSOS_HOMEBREW_LICENSES_DIR = $(BR2_EXTERNAL_RETROSTONE_PATH)/../LICENSES/homebrew
RSOS_HOMEBREW_TARGET_ROMS = $(TARGET_DIR)/usr/share/rsos/homebrew
RSOS_HOMEBREW_TARGET_LICENSES = $(TARGET_DIR)/usr/share/rsos/licenses/homebrew

# Licence texts from LICENSES/homebrew/ (the open/ subdirectory belongs to
# rsos-homebrew-open) and the authors' readme files, gathered in the build
# directory so that legal-info finds them too.
define RSOS_HOMEBREW_GATHER_LICENSES
	rm -rf $(@D)/rsos-licenses
	mkdir -p $(@D)/rsos-licenses
	cp -R $(RSOS_HOMEBREW_LICENSES_DIR)/PERMISSION-8BCraft-RetroStone2.txt \
		$(RSOS_HOMEBREW_LICENSES_DIR)/ucity $(@D)/rsos-licenses/
	cp $(@D)/ReadMe.txt "$(@D)/rsos-licenses/Run to Databay - ReadMe.txt"
	unzip -p $(@D)/Espionage.zip "Espionage/Read Me.txt" \
		> "$(@D)/rsos-licenses/Espionage - Read Me.txt"
endef
RSOS_HOMEBREW_POST_RSYNC_HOOKS += RSOS_HOMEBREW_GATHER_LICENSES

# 1. Every file in rsos-homebrew.hash must be present and match.
# 2. Every source in games.txt must be one of those hashed files.
# 3. Install the games, one gamelist.xml per system, the licences and the
#    first-boot seeding script.
define RSOS_HOMEBREW_INSTALL_TARGET_CMDS
	cd $(@D) && sed -n 's/^sha256  *\([0-9a-f]\{64\}\)  *\(.*[^ ]\) *$$/\1  \2/p' \
		$(RSOS_HOMEBREW_PKGDIR)/rsos-homebrew.hash | sha256sum -c --quiet -
	sed -n 's/^sha256  *[0-9a-f]\{64\}  *\(.*[^ ]\) *$$/\1/p' \
		$(RSOS_HOMEBREW_PKGDIR)/rsos-homebrew.hash > $(@D)/.rsos-hashed
	grep -v -e '^#' -e '^ *$$' $(RSOS_HOMEBREW_PKGDIR)/games.txt | \
		cut -d '|' -f 1 | sort -u > $(@D)/.rsos-systems
	for sys in $$(cat $(@D)/.rsos-systems); do \
		rm -rf $(RSOS_HOMEBREW_TARGET_ROMS)/$$sys; \
		$(INSTALL) -D -m 0644 $(RSOS_HOMEBREW_PKGDIR)/gamelist/$$sys.xml \
			$(RSOS_HOMEBREW_TARGET_ROMS)/$$sys/gamelist.xml || exit 1; \
	done
	grep -v -e '^#' -e '^ *$$' $(RSOS_HOMEBREW_PKGDIR)/games.txt | \
	while IFS='|' read -r sys dst src; do \
		if ! grep -qxF "$$src" $(@D)/.rsos-hashed; then \
			echo "rsos-homebrew: $$src is not in rsos-homebrew.hash" >&2; \
			exit 1; \
		fi; \
		$(INSTALL) -D -m 0644 "$(@D)/$$src" \
			"$(RSOS_HOMEBREW_TARGET_ROMS)/$$sys/$$dst" || exit 1; \
	done
	rm -rf $(RSOS_HOMEBREW_TARGET_LICENSES)
	mkdir -p $(RSOS_HOMEBREW_TARGET_LICENSES)
	cp -R $(@D)/rsos-licenses/. $(RSOS_HOMEBREW_TARGET_LICENSES)/
	chmod -R u=rwX,go=rX $(RSOS_HOMEBREW_TARGET_LICENSES)
	$(INSTALL) -D -m 0755 $(RSOS_HOMEBREW_PKGDIR)/rsos-seed-homebrew \
		$(TARGET_DIR)/usr/bin/rsos-seed-homebrew
endef

$(eval $(generic-package))

# The optional open-licence supplement lives in a subdirectory so that it can
# have its own package name; external.mk only includes package/*/*.mk.
include $(RSOS_HOMEBREW_PKGDIR)/rsos-homebrew-open/rsos-homebrew-open.mk
