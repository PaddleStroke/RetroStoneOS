################################################################################
#
# rsos-frontend
#
################################################################################

# Also the OS version of builds without BR2_RETROSTONE_VERSION (CI sets that
# from the tag). Raise it right after each release tag (docs/ci.md,
# "Making a release"): development builds report <this>-dev and must sort
# after the release they follow.
RSOS_FRONTEND_VERSION = 0.1.1
RSOS_FRONTEND_SITE = $(BR2_EXTERNAL_RETROSTONE_PATH)/../frontend
RSOS_FRONTEND_SITE_METHOD = local

# libdrm (display), alsa-lib (game audio). EGL/GLES2/GBM: headers only, the
# HW-render path dlopen()s the libraries (docs/host-design.md §13).
RSOS_FRONTEND_DEPENDENCIES = libdrm alsa-lib host-pkgconf \
	$(if $(BR2_PACKAGE_HAS_LIBEGL),libegl) \
	$(if $(BR2_PACKAGE_HAS_LIBGLES),libgles) \
	$(if $(BR2_PACKAGE_HAS_LIBGBM),libgbm)
# rsos-update (docs/updates.md): zstd payloads, HTTPS through mbedTLS; the
# CA bundle (ca-certificates) is only needed at run time
RSOS_FRONTEND_DEPENDENCIES += zstd mbedtls

# Licences (make legal-info). The project is MIT (LICENSE at the top of the
# repository, copied into the package's source tree after the rsync, since a
# local package only sees frontend/). Bundled with the frontend and installed
# on the device: the DejaVu (Bitstream Vera + Arev) and Roboto (Apache-2.0)
# fonts, miniz (MIT), nanosvg (Zlib), stb (public domain or MIT, licence in
# the headers), libretro.h (MIT, in the header), SDL GameControllerDB (Zlib)
# and the themes: rsos-dark/rsos-light and gbz35/gbz35-dark (CC BY-NC-SA art;
# third_party/README.md, themes/*/LICENSE.txt).
RSOS_FRONTEND_LICENSE = MIT, Bitstream-Vera (DejaVu fonts), Apache-2.0 (Roboto fonts), \
	MIT (miniz), Zlib (nanosvg), Unlicense or MIT (stb), Zlib (SDL GameControllerDB), \
	BSD-2-Clause or CC0-1.0 (Monocypher), \
	CC-BY-NC-SA-3.0 (gbz35 themes), CC-BY-NC-SA-4.0 (rsos themes art), \
	OFL-1.1 (Noto Sans CJK subsets)
RSOS_FRONTEND_LICENSE_FILES = LICENSE \
	third_party/fonts/LICENSE-DejaVu.txt \
	third_party/fonts/LICENSE-NotoSansCJK.txt \
	third_party/fonts/LICENSE-Roboto.txt \
	third_party/miniz/LICENSE \
	third_party/nanosvg/LICENSE.txt \
	third_party/sdl-gamecontrollerdb/LICENSE \
	third_party/monocypher/LICENCE.md \
	themes/gbz35/LICENSE.txt \
	themes/gbz35-dark/LICENSE.txt \
	themes/rsos-dark/LICENSE.txt \
	themes/rsos-light/LICENSE.txt

define RSOS_FRONTEND_COPY_PROJECT_LICENSE
	$(INSTALL) -m 0644 $(BR2_EXTERNAL_RETROSTONE_PATH)/../LICENSE $(@D)/LICENSE
endef
RSOS_FRONTEND_POST_RSYNC_HOOKS += RSOS_FRONTEND_COPY_PROJECT_LICENSE

# Shown in Settings > System information (/etc/rsos-version). The date is the
# build date, or the day of SOURCE_DATE_EPOCH when it is set in the
# environment or on the command line, or BR2_REPRODUCIBLE is on (then
# Buildroot sets it: its own release date unless one is given).
ifneq ($(BR2_REPRODUCIBLE)$(filter environment% command%,$(origin SOURCE_DATE_EPOCH)),)
RSOS_FRONTEND_BUILD_DATE := $(shell date -u -d @$(SOURCE_DATE_EPOCH) +%Y-%m-%d)
else
RSOS_FRONTEND_BUILD_DATE := $(shell date +%Y-%m-%d)
endif
ifneq ($(BR2_REPRODUCIBLE)$(filter environment% command%,$(origin SOURCE_DATE_EPOCH)),)
RSOS_FRONTEND_BUILD_TIME := $(SOURCE_DATE_EPOCH)
else
RSOS_FRONTEND_BUILD_TIME := $(shell date +%s)
endif
# The OS version (docs/updates.md): BR2_RETROSTONE_VERSION (CI: the release
# tag), else this package's version; "-dev" for a development build.
RSOS_FRONTEND_OS_VERSION = $(or $(call qstrip,$(BR2_RETROSTONE_VERSION)),$(RSOS_FRONTEND_VERSION))$(if $(BR2_RETROSTONE_RELEASE),,-dev)
RSOS_FRONTEND_VERSION_STRING = RetroStoneOS $(RSOS_FRONTEND_OS_VERSION) ($(RSOS_FRONTEND_BUILD_DATE))
# The board id of update packages: the defconfig name without _defconfig
# and _release, "_" -> "-" (retrostone2, rpi4-64), as the image names of CI
# (scripts/ci/board-info.sh).
RSOS_FRONTEND_BOARD_ID = $(subst _,-,$(patsubst %_release,%,$(patsubst %_defconfig,%,$(notdir $(call qstrip,$(BR2_DEFCONFIG))))))

# Objects go to $(@D)/build: the rsync'ed source never mixes with host
# builds (those default to ~/rsos/frontend-build).
RSOS_FRONTEND_MAKE_OPTS = \
	$(TARGET_CONFIGURE_OPTS) \
	PKG_CONFIG="$(PKG_CONFIG_HOST_BINARY)" \
	BUILDDIR=$(@D)/build \
	UPDATE_TLS=1 \
	UPD_BUILD_EPOCH=$(RSOS_FRONTEND_BUILD_TIME) \
	$(if $(call qstrip,$(BR2_PACKAGE_RSOS_FRONTEND_SPLASH_BG)),SPLASH_BG=$(call qstrip,$(BR2_PACKAGE_RSOS_FRONTEND_SPLASH_BG)))

define RSOS_FRONTEND_BUILD_CMDS
	rm -rf $(@D)/build
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(RSOS_FRONTEND_MAKE_OPTS) all
endef

define RSOS_FRONTEND_INSTALL_TARGET_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) $(RSOS_FRONTEND_MAKE_OPTS) \
		DESTDIR=$(TARGET_DIR) PREFIX=/usr \
		RSOS_VERSION_STRING="$(RSOS_FRONTEND_VERSION_STRING)" \
		RSOS_OS_VERSION="$(RSOS_FRONTEND_OS_VERSION)" \
		RSOS_OS_VARIANT="$(if $(BR2_RETROSTONE_RELEASE),release,dev)" \
		RSOS_OS_BUILD_TIME="$(RSOS_FRONTEND_BUILD_TIME)" \
		RSOS_OS_BUILD_DATE="$(RSOS_FRONTEND_BUILD_DATE)" \
		RSOS_BOARD_ID="$(RSOS_FRONTEND_BOARD_ID)" install
endef

$(eval $(generic-package))
