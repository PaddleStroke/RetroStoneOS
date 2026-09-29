################################################################################
#
# libretro-nxengine
#
################################################################################

# libretro/nxengine-libretro master, 2026-08-22. No submodules. The tree
# also carries the freeware Cave Story data (datafiles/).
LIBRETRO_NXENGINE_VERSION = fd1c0686f8b4c0aea9b5addbc077e3ad7da23bb7
LIBRETRO_NXENGINE_SITE = $(call github,libretro,nxengine-libretro,$(LIBRETRO_NXENGINE_VERSION))
LIBRETRO_NXENGINE_LICENSE = GPL-3.0 (NXEngine)
LIBRETRO_NXENGINE_LICENSE_FILES = nxengine/LICENSE
ifeq ($(BR2_PACKAGE_LIBRETRO_NXENGINE_CAVESTORY),y)
LIBRETRO_NXENGINE_LICENSE += , Freeware (Cave Story data, Studio Pixel; translation Aeon Genesis)
LIBRETRO_NXENGINE_LICENSE_FILES += datafiles/Readme.txt
endif

# make legal-info: the source offer covers the GPL engine, not the freeware
# game data that the upstream tarball also carries (datafiles/; the images
# only install it with BR2_PACKAGE_LIBRETRO_NXENGINE_CAVESTORY, off in every
# defconfig). Buildroot saves either the whole tarball or nothing
# (REDISTRIBUTE = NO), so it saves nothing and this hook saves the same tree
# without datafiles/ in its place: legal-info/sources/libretro-nxengine-
# <version>/. The licence files are saved as usual.
LIBRETRO_NXENGINE_REDISTRIBUTE = NO
LIBRETRO_NXENGINE_LEGAL_TMP = $(BUILD_DIR)/libretro-nxengine-legal-info
define LIBRETRO_NXENGINE_SAVE_ENGINE_SOURCE
	rm -rf $(LIBRETRO_NXENGINE_LEGAL_TMP) $(LIBRETRO_NXENGINE_REDIST_SOURCES_DIR)
	mkdir -p $(LIBRETRO_NXENGINE_LEGAL_TMP) $(LIBRETRO_NXENGINE_REDIST_SOURCES_DIR)
	gzip -dc $(LIBRETRO_NXENGINE_DL_DIR)/$(LIBRETRO_NXENGINE_SOURCE) | \
		tar -x -C $(LIBRETRO_NXENGINE_LEGAL_TMP) --exclude='*/datafiles' --exclude='*/datafiles/*'
	if find $(LIBRETRO_NXENGINE_LEGAL_TMP) -path '*/datafiles*' | grep -q .; then \
		echo "libretro-nxengine: datafiles/ left in the legal-info source" >&2; exit 1; fi
	tar -C $(LIBRETRO_NXENGINE_LEGAL_TMP) --sort=name --owner=0 --group=0 --numeric-owner \
		-cf - . | gzip -9n > $(LIBRETRO_NXENGINE_REDIST_SOURCES_DIR)/libretro-nxengine-$(LIBRETRO_NXENGINE_VERSION)-engine.tar.gz
	printf '%s\n' \
		"The NXEngine libretro core (GPL-3.0): $(LIBRETRO_NXENGINE_SOURCE) from" \
		"$(LIBRETRO_NXENGINE_SITE)," \
		"without its datafiles/ folder (the freeware Cave Story game data, which" \
		"is not GPL software and is not part of this source offer)." \
		> $(LIBRETRO_NXENGINE_REDIST_SOURCES_DIR)/README
	rm -rf $(LIBRETRO_NXENGINE_LEGAL_TMP)
endef
LIBRETRO_NXENGINE_POST_LEGAL_INFO_HOOKS += LIBRETRO_NXENGINE_SAVE_ENGINE_SOURCE

# platform=unix everywhere: the Makefile has no ARM Linux platform and the
# toolchain wrapper gives the CPU flags. Plain C++, no asm.
LIBRETRO_NXENGINE_PLATFORM = $(call rsos-libretro-select,default=unix)

LIBRETRO_NXENGINE_MAKE_OPTS = \
	platform=$(LIBRETRO_NXENGINE_PLATFORM) \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	AR="$(TARGET_AR)"

define LIBRETRO_NXENGINE_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(@D) -f Makefile $(LIBRETRO_NXENGINE_MAKE_OPTS)
endef

# The game: Doukutsu.exe (the core extracts music and graphics from it) and
# data/, plus the readme (credits), to /usr/share/rsos/homebrew/cavestory/,
# copied to /data/roms/cavestory/ by rsos-seed-homebrew on the first boot.
LIBRETRO_NXENGINE_GAME_DIR = $(TARGET_DIR)/usr/share/rsos/homebrew/cavestory
ifeq ($(BR2_PACKAGE_LIBRETRO_NXENGINE_CAVESTORY),y)
define LIBRETRO_NXENGINE_INSTALL_CAVESTORY
	rm -rf $(LIBRETRO_NXENGINE_GAME_DIR)
	mkdir -p $(LIBRETRO_NXENGINE_GAME_DIR)
	cp -R $(@D)/datafiles/data $(LIBRETRO_NXENGINE_GAME_DIR)/
	$(INSTALL) -m 0644 $(@D)/datafiles/Doukutsu.exe \
		$(@D)/datafiles/Readme.txt $(LIBRETRO_NXENGINE_GAME_DIR)/
	$(INSTALL) -m 0644 $(LIBRETRO_NXENGINE_PKGDIR)/cavestory.xml \
		$(LIBRETRO_NXENGINE_GAME_DIR)/gamelist.xml
	chmod -R u=rwX,go=rX $(LIBRETRO_NXENGINE_GAME_DIR)
endef
endif

define LIBRETRO_NXENGINE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/nxengine_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/nxengine_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_NXENGINE_PKGDIR)/nxengine.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/nxengine.ini
	$(LIBRETRO_NXENGINE_INSTALL_CAVESTORY)
endef

$(eval $(generic-package))
