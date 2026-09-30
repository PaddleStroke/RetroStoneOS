################################################################################
#
# rsos-homebrew
#
################################################################################

# The first-boot seeding of the bundled games (docs/homebrew.md): the games
# themselves are other packages (rsos-ucity, rsos-freedoom, ...) that install
# to /usr/share/rsos/homebrew/<system>/. Only a shell script of this tree: no
# download, nothing compiled.
RSOS_HOMEBREW_LICENSE = MIT
RSOS_HOMEBREW_ADD_TOOLCHAIN_DEPENDENCY = NO

define RSOS_HOMEBREW_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(RSOS_HOMEBREW_PKGDIR)/rsos-seed-homebrew \
		$(TARGET_DIR)/usr/bin/rsos-seed-homebrew
endef

$(eval $(generic-package))

# The optional open-licence supplement lives in a subdirectory so that it can
# have its own package name; external.mk only includes package/*/*.mk.
include $(RSOS_HOMEBREW_PKGDIR)/rsos-homebrew-open/rsos-homebrew-open.mk
