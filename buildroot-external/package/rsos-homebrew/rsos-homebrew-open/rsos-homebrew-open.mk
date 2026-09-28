################################################################################
#
# rsos-homebrew-open
#
################################################################################

# Open-licence supplement (docs/homebrew.md). Each game is an
# _EXTRA_DOWNLOADS entry (full URL, hashed in rsos-homebrew-open.hash); a GPL
# game also lists its source archive there, so that "make legal-info" collects
# the source. No main archive.
# TODO: the list is empty until the owner has reviewed the candidates.
RSOS_HOMEBREW_OPEN_SOURCE =
RSOS_HOMEBREW_OPEN_EXTRA_DOWNLOADS =
RSOS_HOMEBREW_OPEN_LICENSE = various open licences (see docs/homebrew.md)
RSOS_HOMEBREW_OPEN_DEPENDENCIES = rsos-homebrew
RSOS_HOMEBREW_OPEN_ADD_TOOLCHAIN_DEPENDENCY = NO

$(eval $(generic-package))
