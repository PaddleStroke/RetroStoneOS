################################################################################
#
# libretro-scummvm
#
################################################################################

# libretro/scummvm (ScummVM master as synced by libretro, with the libretro
# backend in backends/platform/libretro), 2026-09-19.
LIBRETRO_SCUMMVM_VERSION = fcbce3ae815269dacdc309092bc92ccc6d3e13bb
LIBRETRO_SCUMMVM_SITE = $(call github,libretro,scummvm,$(LIBRETRO_SCUMMVM_VERSION))
LIBRETRO_SCUMMVM_LICENSE = GPL-3.0+ (ScummVM), LGPL-2.1+, BSD, MIT, zlib, FTL and others (bundled libraries, see LICENSES/)
LIBRETRO_SCUMMVM_LICENSE_FILES = \
	COPYING \
	COPYRIGHT \
	LICENSES/COPYING.BSD \
	LICENSES/COPYING.LGPL \
	LICENSES/COPYING.MIT \
	LICENSES/COPYING.ISC \
	LICENSES/COPYING.LUA \
	LICENSES/COPYING.OFL

# The backend's Makefile clones two repositories at build time
# (scripts/configure_submodules.sh): libretro-deps (zlib, libpng, libjpeg,
# FLAC, libvorbis/Tremor, libmad, faad2, freetype, fribidi, libmpeg2,
# giflib, theora) and libretro-common, at the commits pinned in
# backends/platform/libretro/dependencies.mk. Buildroot downloads the same
# commits instead, they are unpacked into deps/ after the extraction, and
# DEPS_SUBMODULES= turns the build-time clone off.
LIBRETRO_SCUMMVM_DEPS_COMMIT = bab7d258c451c0e7cba4b6a79f1b062c13efff38
LIBRETRO_SCUMMVM_COMMON_COMMIT = 879c8d507b0b52e77e27d759239c2b5df1e26dfd
LIBRETRO_SCUMMVM_EXTRA_DOWNLOADS = \
	https://github.com/libretro/libretro-deps/archive/$(LIBRETRO_SCUMMVM_DEPS_COMMIT).tar.gz \
	https://github.com/libretro/libretro-common/archive/$(LIBRETRO_SCUMMVM_COMMON_COMMIT).tar.gz

LIBRETRO_SCUMMVM_BACKEND = $(@D)/backends/platform/libretro

define LIBRETRO_SCUMMVM_EXTRACT_DEPS
	mkdir -p $(LIBRETRO_SCUMMVM_BACKEND)/deps/libretro-deps \
		$(LIBRETRO_SCUMMVM_BACKEND)/deps/libretro-common
	$(call suitable-extractor,$(LIBRETRO_SCUMMVM_DEPS_COMMIT).tar.gz) \
		$(LIBRETRO_SCUMMVM_DL_DIR)/$(LIBRETRO_SCUMMVM_DEPS_COMMIT).tar.gz | \
		$(TAR) --strip-components=1 -C $(LIBRETRO_SCUMMVM_BACKEND)/deps/libretro-deps \
			$(TAR_OPTIONS) -
	$(call suitable-extractor,$(LIBRETRO_SCUMMVM_COMMON_COMMIT).tar.gz) \
		$(LIBRETRO_SCUMMVM_DL_DIR)/$(LIBRETRO_SCUMMVM_COMMON_COMMIT).tar.gz | \
		$(TAR) --strip-components=1 -C $(LIBRETRO_SCUMMVM_BACKEND)/deps/libretro-common \
			$(TAR_OPTIONS) -
endef
LIBRETRO_SCUMMVM_POST_EXTRACT_HOOKS += LIBRETRO_SCUMMVM_EXTRACT_DEPS

# Build choices (docs/cores.md, "ScummVM"):
# - platform=unix everywhere (-O3, C++11). The Makefile's ARM branches are
#   the Raspberry Pi ones (they force -mcpu and GLES2) and "armv7", which is
#   built at -O1 to dodge a GCC 15.1 bug; the toolchain wrapper gives the
#   CPU flags. HAVE_NEON=1 on ARM (-DSCUMMVM_NEON: the NEON blitters).
# - BUILD_64BIT=1 on 64-bit targets: with an explicit platform the Makefile
#   only guesses it from the platform name (SIZEOF_SIZE_T, SCUMM_64BITS).
# - LITE=1: the upstream list of 31 engines for small devices
#   (lite_engines.list: SCUMM v0-v8 incl. HE, AGI, AGOS, SCI/SCI32, Kyra/
#   EOB/LOL, Broken Sword 1-2, Sky, Queen, Lure, Tinsel, Saga, Gob, ...).
#   The full build has ~120 engines, many of them 3D or high-resolution
#   games this device cannot run, and is several times bigger.
# - FORCE_OPENGLNONE=1: software rendering only (the host gives GLES2 to
#   the N64 cores only).
# - USE_MT32EMU=0 (needs MT-32 ROMs, too heavy here), USE_FLUIDSYNTH=0
#   (needs a SoundFont, heavy): MIDI music uses the AdLib/OPL emulation.
#   USE_IMGUI=0 (debugger UI), no cloud/network features.
# - All the libraries come from libretro-deps and are linked in statically
#   (USE_SYSTEM_* unset): the .so needs only libc/libm/libstdc++.
LIBRETRO_SCUMMVM_MAKE_OPTS = \
	platform=unix \
	BUILD_64BIT=$(call rsos-libretro-select,aarch64=1 x86_64=1 default=0) \
	HAVE_NEON=$(call rsos-libretro-select,arm=1 aarch64=1 default=0) \
	DEPS_SUBMODULES= \
	LITE=1 \
	FORCE_OPENGLNONE=1 \
	USE_MT32EMU=0 \
	USE_FLUIDSYNTH=0 \
	USE_IMGUI=0 \
	USE_CLOUD=0 \
	CC="$(TARGET_CC)" \
	CXX="$(TARGET_CXX)" \
	LD="$(TARGET_CXX)" \
	AR="$(TARGET_AR) cru" \
	RANLIB="$(TARGET_RANLIB)"

define LIBRETRO_SCUMMVM_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) \
		$(MAKE) -C $(LIBRETRO_SCUMMVM_BACKEND) $(LIBRETRO_SCUMMVM_MAKE_OPTS) core
endef

# Data files the core looks for in <system dir>/scummvm/extra and
# <system dir>/scummvm/theme (what upstream's scummvm.zip holds, trimmed to
# the engines built): the engine data of the lite engines, the core data
# files, the AGI predictive dictionary, the virtual keyboard and two GUI
# themes. The frontend copies the tree into the system directory
# (system_tree in scummvm.ini); fonts.dat (6 MB, only used with FreeType
# GUI fonts / translated menus) and the launcher icons are left out.
LIBRETRO_SCUMMVM_EXTRA_FILES = \
	dists/engine-data/achievements.dat \
	dists/engine-data/classicmacfonts.dat \
	dists/engine-data/encoding.dat \
	dists/engine-data/helpdialog.zip \
	dists/engine-data/macgui.dat \
	dists/engine-data/drascula.dat \
	dists/engine-data/kyra.dat \
	dists/engine-data/lure.dat \
	dists/engine-data/mort.dat \
	dists/engine-data/queen.tbl \
	dists/engine-data/sky.cpt \
	dists/engine-data/teenagent.dat \
	dists/pred.dic \
	backends/vkeybd/packs/vkeybd_default.zip
LIBRETRO_SCUMMVM_THEME_FILES = \
	gui/themes/scummremastered.zip \
	gui/themes/scummclassic.zip
LIBRETRO_SCUMMVM_DATA = $(TARGET_DIR)/usr/share/rsos/cores/scummvm/scummvm

define LIBRETRO_SCUMMVM_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(LIBRETRO_SCUMMVM_BACKEND)/scummvm_libretro.so \
		$(TARGET_DIR)/usr/lib/libretro/scummvm_libretro.so
	$(INSTALL) -D -m 0644 $(LIBRETRO_SCUMMVM_PKGDIR)/scummvm.ini \
		$(TARGET_DIR)/usr/share/rsos/cores/scummvm.ini
	rm -rf $(LIBRETRO_SCUMMVM_DATA)
	$(foreach f,$(LIBRETRO_SCUMMVM_EXTRA_FILES),
		$(INSTALL) -D -m 0644 $(@D)/$(f) $(LIBRETRO_SCUMMVM_DATA)/extra/$(notdir $(f))
	)
	$(foreach f,$(LIBRETRO_SCUMMVM_THEME_FILES),
		$(INSTALL) -D -m 0644 $(@D)/$(f) $(LIBRETRO_SCUMMVM_DATA)/theme/$(notdir $(f))
	)
endef

$(eval $(generic-package))
