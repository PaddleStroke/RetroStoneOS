################################################################################
#
# rsos-vc-games
#
################################################################################

# The RetroStone VC games (docs/vc-games.md): each game is a libretro core
# built from the RetroStone VC source (https://github.com/PaddleStroke/RetroStoneVC):
# the SDK and tools (MIT) plus the game (games/<game>/: code MIT, art, music,
# sound, levels and design CC BY-NC-SA 4.0). The package syncs a local
# checkout (BR2_PACKAGE_RSOS_VC_GAMES_SOURCE_DIR, by default ../RetroStoneVC
# next to the RetroStoneOS checkout; CI checks out the public repository,
# docs/ci.md). A developer can also point RSOS_VC_GAMES_OVERRIDE_SRCDIR at
# another tree in local.mk.
RSOS_VC_GAMES_VERSION = local
RSOS_VC_GAMES_SITE = $(call qstrip,$(BR2_PACKAGE_RSOS_VC_GAMES_SOURCE_DIR))
RSOS_VC_GAMES_SITE_METHOD = local
RSOS_VC_GAMES_LICENSE = \
	MIT (RetroStone VC SDK, tools and game code), \
	CC-BY-NC-SA-4.0 (RetroStone VC games: art, music, sound, levels, design), \
	MIT (libxmp-lite), MIT or Public Domain (stb), MIT (libretro.h)
RSOS_VC_GAMES_LICENSE_FILES = LICENSE-MIT LICENSE-CC-BY-NC-SA-4.0.txt THIRD_PARTY.md \
	games/bombermole/LICENSE games/leadysquid/LICENSE \
	games/duckparade/LICENSE \
	games/blueberrytumble/LICENSE \
	games/beaverrush/LICENSE \
	games/pancaketower/LICENSE \
	games/pogomamie/LICENSE
# Not copied into the build directory: the developer's own builds, the
# deliverables, the image agent's art inbox (tens of MB, not used by the
# build: the games build with their committed art sets, e.g. Bomber Mole's
# games/bombermole/art-ai/) and the art previews.
RSOS_VC_GAMES_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = \
	--exclude /build --exclude /dist --exclude /docs/art-preview \
	--exclude '/games/*/art/incoming' --exclude __pycache__ --exclude '*.srm'

# "make legal-info" saves the source. Buildroot's own archive of a local
# package copies the whole checkout, a developer's build/ and dist/ (host
# binaries, the SDL2 package) included: it is replaced by one made with the
# exclusions above, i.e. exactly what the build used.
RSOS_VC_GAMES_LEGAL_TMP = $(BUILD_DIR)/rsos-vc-games-legal-info
define RSOS_VC_GAMES_SAVE_SOURCE
	rm -rf $(RSOS_VC_GAMES_LEGAL_TMP)
	mkdir -p $(RSOS_VC_GAMES_LEGAL_TMP)/$(RSOS_VC_GAMES_BASENAME_RAW)
	rsync -a --chmod=u=rwX,go=rX $(RSOS_VC_GAMES_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS) $(RSYNC_VCS_EXCLUSIONS) \
		$(call qstrip,$(RSOS_VC_GAMES_OVERRIDE_SRCDIR))/ \
		$(RSOS_VC_GAMES_LEGAL_TMP)/$(RSOS_VC_GAMES_BASENAME_RAW)/
	tar -C $(RSOS_VC_GAMES_LEGAL_TMP) --sort=name --owner=0 --group=0 --numeric-owner \
		-cf - $(RSOS_VC_GAMES_BASENAME_RAW) | gzip -9n \
		> $(RSOS_VC_GAMES_REDIST_SOURCES_DIR)/$(RSOS_VC_GAMES_BASENAME_RAW).tar.gz
	rm -rf $(RSOS_VC_GAMES_LEGAL_TMP)
endef
RSOS_VC_GAMES_POST_LEGAL_INFO_HOOKS += RSOS_VC_GAMES_SAVE_SOURCE

RSOS_VC_GAMES_LIST = \
	$(if $(BR2_PACKAGE_RSOS_VC_GAMES_BOMBERMOLE),bombermole) \
	$(if $(BR2_PACKAGE_RSOS_VC_GAMES_LEADYSQUID),leadysquid) \
	$(if $(BR2_PACKAGE_RSOS_VC_GAMES_DUCKPARADE),duckparade) \
	$(if $(BR2_PACKAGE_RSOS_VC_GAMES_BLUEBERRYTUMBLE),blueberrytumble) \
	$(if $(BR2_PACKAGE_RSOS_VC_GAMES_BEAVERRUSH),beaverrush) \
	$(if $(BR2_PACKAGE_RSOS_VC_GAMES_PANCAKETOWER),pancaketower) \
	$(if $(BR2_PACKAGE_RSOS_VC_GAMES_POGOMAMIE),pogomamie)
# The menu entry name (the game's name: the saves are named after it) and
# the picture of the game list (the title screen, 640x480).
RSOS_VC_GAMES_NAME_bombermole = Bomber Mole
RSOS_VC_GAMES_SHOT_bombermole = docs/screenshots/title.png
RSOS_VC_GAMES_NAME_leadysquid = Leady Squid
RSOS_VC_GAMES_SHOT_leadysquid = games/leadysquid/docs/screenshots/title.png
RSOS_VC_GAMES_NAME_duckparade = Duck Parade
RSOS_VC_GAMES_SHOT_duckparade = games/duckparade/docs/screenshots/title.png
RSOS_VC_GAMES_NAME_blueberrytumble = Blueberry Tumble
RSOS_VC_GAMES_SHOT_blueberrytumble = games/blueberrytumble/docs/screenshots/title.png
RSOS_VC_GAMES_NAME_beaverrush = Beaver Rush
RSOS_VC_GAMES_SHOT_beaverrush = games/beaverrush/docs/screenshots/title.png
RSOS_VC_GAMES_NAME_pancaketower = Pancake Tower
RSOS_VC_GAMES_SHOT_pancaketower = games/pancaketower/docs/screenshots/title.png
RSOS_VC_GAMES_NAME_pogomamie = Pogo Mamie
RSOS_VC_GAMES_SHOT_pogomamie = games/pogomamie/docs/screenshots/title.png

# The asset step (games/<game>/tools/build_assets.py) needs Pillow, which
# Buildroot has no host package for: the build host's own python3
# (python3-pil; install-deps.sh installs it for CI). Not the python3 of
# $(HOST_DIR), which the toolchain PATH would find first.
RSOS_VC_GAMES_PYTHON ?= /usr/bin/python3

# The source's commit, for the .ini files and the licence folder ("-dirty":
# local changes), taken from the original tree (the copy has no .git) by
# rsos-vc-commit: git, else the HEAD ref files (a worktree made on Windows,
# which WSL git cannot open), or RSOS_VC_COMMIT (make RSOS_VC_COMMIT=<sha>).
define RSOS_VC_GAMES_RECORD_COMMIT
	RSOS_VC_COMMIT='$(RSOS_VC_COMMIT)' sh $(RSOS_VC_GAMES_PKGDIR)/rsos-vc-commit \
		'$(call qstrip,$(SRCDIR))' > $(@D)/.rsos-vc-commit
	@echo "rsos-vc-games: RetroStone VC $$(cat $(@D)/.rsos-vc-commit) from $(call qstrip,$(SRCDIR))"
endef
RSOS_VC_GAMES_POST_RSYNC_HOOKS += RSOS_VC_GAMES_RECORD_COMMIT

# The RetroStone VC Makefile's "armhf" target is its cross build: the core
# for any target here, with Buildroot's compiler and flags (the wrapper adds
# the CPU flags: Cortex-A7 NEON on the RetroStone2, the right ARMv8 CPU on the
# 64-bit boards). ARM_FLAGS replaces its own Cortex-A7 default; the output is
# build/armhf/<game>_libretro.so whatever the architecture.
# No ART or CHAR_SIZE: the RetroStone VC defaults (Bomber Mole: the AI art of
# games/bombermole/art-ai/, 24-px characters, like the owner's Windows build).
RSOS_VC_GAMES_MAKE_OPTS = \
	ARM_CC="$(TARGET_CC)" \
	ARM_AR="$(TARGET_AR)" \
	ARM_FLAGS="$(TARGET_CFLAGS)" \
	PYTHON=$(RSOS_VC_GAMES_PYTHON)

define RSOS_VC_GAMES_BUILD_CMDS
	@$(RSOS_VC_GAMES_PYTHON) -c 'import PIL' 2>/dev/null || { \
		echo "rsos-vc-games: $(RSOS_VC_GAMES_PYTHON) with Pillow is needed (apt install python3-pil)" >&2; \
		exit 1; }
	$(foreach g,$(RSOS_VC_GAMES_LIST),\
		$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) GAME=$(g) $(RSOS_VC_GAMES_MAKE_OPTS) \
			build/armhf/$(g)_libretro.so$(sep))
endef

RSOS_VC_GAMES_TARGET_GAMES = $(TARGET_DIR)/usr/share/rsos/games/retrostone
RSOS_VC_GAMES_TARGET_LICENSES = $(TARGET_DIR)/usr/share/rsos/licenses/retrostone-vc

# One game: the core, its .ini (with the commit), the menu entry (a stub:
# its extension picks the core, its name is the save name), the title screen.
define RSOS_VC_GAMES_INSTALL_GAME
	$(INSTALL) -D -m 0755 $(@D)/build/armhf/$(1)_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/$(1)_libretro.so
	mkdir -p $(TARGET_DIR)/usr/share/rsos/cores
	sed "s/^commit = .*/commit = $$(cat $(@D)/.rsos-vc-commit)/" $(RSOS_VC_GAMES_PKGDIR)/$(1).ini \
		> $(TARGET_DIR)/usr/share/rsos/cores/$(1).ini
	printf '%s\n' "RetroStone VC: $(RSOS_VC_GAMES_NAME_$(1)). The game is inside" \
		"/usr/lib/libretro/$(1)_libretro.so; this file is only its menu entry." \
		> "$(RSOS_VC_GAMES_TARGET_GAMES)/$(RSOS_VC_GAMES_NAME_$(1)).$(1)"
	$(INSTALL) -m 0644 $(@D)/$(RSOS_VC_GAMES_SHOT_$(1)) \
		"$(RSOS_VC_GAMES_TARGET_GAMES)/media/images/$(RSOS_VC_GAMES_NAME_$(1)).png"
	$(INSTALL) -D -m 0644 $(@D)/games/$(1)/LICENSE $(RSOS_VC_GAMES_TARGET_LICENSES)/$(1)/LICENSE
endef

# gamelist.xml lists all seven games; the menu drops an entry without its file
# (a game turned off in menuconfig).
define RSOS_VC_GAMES_INSTALL_TARGET_CMDS
	rm -rf $(RSOS_VC_GAMES_TARGET_GAMES) $(RSOS_VC_GAMES_TARGET_LICENSES)
	mkdir -p $(RSOS_VC_GAMES_TARGET_GAMES)/media/images $(RSOS_VC_GAMES_TARGET_LICENSES)
	$(INSTALL) -m 0644 $(RSOS_VC_GAMES_PKGDIR)/gamelist.xml $(RSOS_VC_GAMES_TARGET_GAMES)/gamelist.xml
	$(foreach g,$(RSOS_VC_GAMES_LIST),$(call RSOS_VC_GAMES_INSTALL_GAME,$(g))$(sep))
	$(INSTALL) -m 0644 $(@D)/LICENSE-MIT $(@D)/LICENSE-CC-BY-NC-SA-4.0.txt $(@D)/THIRD_PARTY.md \
		$(RSOS_VC_GAMES_TARGET_LICENSES)/
	cp $(@D)/.rsos-vc-commit $(RSOS_VC_GAMES_TARGET_LICENSES)/COMMIT
endef

$(eval $(generic-package))
