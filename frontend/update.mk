# frontend/update.mk - the system updater (src/update, docs/updates.md):
#   rsos-update     the device program (the menu runs it as a helper; UART CLI)
#   rsos-mkupdate   the build machine tool that makes and signs .rsu packages
#   test_update     unit and integration tests (make check-update)
#
# Dependencies: libzstd (payload decompression), mbedTLS for HTTPS
# (UPDATE_TLS=1; auto-detected on a host build, set by the Buildroot
# package), the vendored Monocypher (Ed25519) and our SHA-256. rsos-frontend
# itself links none of them: it runs rsos-update as a separate process, so
# the menu's start-up loads no extra library (boot time) and a TLS or JSON
# parsing bug cannot take the menu down.
#
# From the main Makefile: include update.mk; $(UPDATE_BIN) in the binaries;
# check-update in check; update-install from install. Standalone (from
# frontend/): make -f update.mk BUILDDIR=~/rsos/update-build check-update

UPD_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
BUILDDIR ?= $(if $(HOME),$(HOME)/rsos/frontend-build,/tmp/rsos-frontend-build)
UPD_BUILD = $(BUILDDIR)/update
PKG_CONFIG ?= pkg-config
CC_FOR_BUILD ?= cc
CFLAGS_FOR_BUILD ?= -O2

UPD_SRCS := \
	$(UPD_DIR)/src/update/sha256.c \
	$(UPD_DIR)/src/update/rsu.c \
	$(UPD_DIR)/src/update/json.c \
	$(UPD_DIR)/src/update/http.c \
	$(UPD_DIR)/src/update/update.c \
	$(UPD_DIR)/third_party/monocypher/monocypher.c \
	$(UPD_DIR)/third_party/monocypher/monocypher-ed25519.c
UPD_MKUPDATE_SRCS := \
	$(UPD_DIR)/src/tools/rsos-mkupdate.c \
	$(UPD_DIR)/src/update/sha256.c \
	$(UPD_DIR)/src/update/rsu.c \
	$(UPD_DIR)/third_party/monocypher/monocypher.c \
	$(UPD_DIR)/third_party/monocypher/monocypher-ed25519.c

# HTTPS: mbedTLS when its headers are there (host), UPDATE_TLS=1 forces it
UPDATE_TLS ?= $(shell printf '\043include <mbedtls/ssl.h>\n' | $(CC) $(CPPFLAGS) -E - > /dev/null 2>&1 && echo 1 || echo 0)
UPD_TLS_DEFS = $(if $(filter 1,$(UPDATE_TLS)),-DRSOS_UPDATE_TLS)
UPD_TLS_LIBS = $(if $(filter 1,$(UPDATE_TLS)),-lmbedtls -lmbedx509 -lmbedcrypto)
UPD_ZSTD_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libzstd 2>/dev/null)
UPD_ZSTD_LIBS ?= $(shell $(PKG_CONFIG) --libs libzstd 2>/dev/null || echo -lzstd)
# The clock can never be before the build (TLS needs a set clock).
UPD_BUILD_EPOCH ?= $(or $(SOURCE_DATE_EPOCH),$(shell date +%s))

UPD_WARN = -Wall -Wextra -Wshadow -Wformat=2 -Wformat-security -Werror=format-security -Wstrict-prototypes \
	-Wmissing-prototypes -Wno-missing-field-initializers
UPD_INCLUDES = -I$(UPD_DIR)/src -I$(UPD_DIR)/src/update -I$(UPD_DIR)/third_party/monocypher
UPD_CFLAGS = $(CPPFLAGS) $(CFLAGS) -std=gnu11 $(UPD_WARN) -g -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
	$(UPD_INCLUDES) $(UPD_ZSTD_CFLAGS) $(UPD_TLS_DEFS) -DRSOS_BUILD_EPOCH=$(UPD_BUILD_EPOCH)L
# Monocypher as it is upstream: its own style, not our extra warnings
UPD_MC_CFLAGS = $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -g $(UPD_INCLUDES)

UPD_OBJS := $(patsubst $(UPD_DIR)/%.c,$(UPD_BUILD)/%.o,$(UPD_SRCS))
UPD_BIN = $(BUILDDIR)/rsos-update
UPD_TEST = $(BUILDDIR)/test_update
UPD_MKUPDATE = $(BUILDDIR)/build-tools/rsos-mkupdate
UPD_LIBS = $(UPD_ZSTD_LIBS) $(UPD_TLS_LIBS) -lpthread

# The public key of the release signing key (docs/updates.md, "Keys"):
# installed as /usr/share/rsos/update.pub. RSOS_UPDATE_PUBKEY=<file> puts
# another one in an image (a fork's own key).
RSOS_UPDATE_PUBKEY ?= $(UPD_DIR)/assets/update.pub

.PHONY: update-all check-update update-install mkupdate

update-all: $(UPD_BIN)

$(UPD_BUILD)/third_party/monocypher/%.o: $(UPD_DIR)/third_party/monocypher/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UPD_MC_CFLAGS) -MMD -MP -c -o $@ $<

$(UPD_BUILD)/%.o: $(UPD_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UPD_CFLAGS) -MMD -MP -c -o $@ $<

$(UPD_BIN): $(UPD_OBJS) $(UPD_BUILD)/src/update/rsos-update.o
	$(CC) $(LDFLAGS) -o $@ $^ $(UPD_LIBS)

$(UPD_TEST): $(UPD_OBJS) $(UPD_BUILD)/src/update/tests/test_update.o
	$(CC) $(LDFLAGS) -o $@ $^ $(UPD_LIBS)

# Build machine (CC_FOR_BUILD): no zstd, no TLS.
$(UPD_MKUPDATE): $(UPD_MKUPDATE_SRCS) $(wildcard $(UPD_DIR)/src/update/*.h)
	@mkdir -p $(dir $@)
	$(CC_FOR_BUILD) $(CFLAGS_FOR_BUILD) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE $(UPD_INCLUDES) $(LDFLAGS_FOR_BUILD) \
		-o $@ $(UPD_MKUPDATE_SRCS)

mkupdate: $(UPD_MKUPDATE)

check-update: $(UPD_TEST) $(UPD_BIN) $(UPD_MKUPDATE)
	$(UPD_TEST) $(abspath $(BUILDDIR))/update-check
	sh $(UPD_DIR)/src/update/tests/cli_test.sh $(UPD_BIN) $(UPD_MKUPDATE) $(abspath $(BUILDDIR))/update-cli

# The menu screens (ui/update_ui.c) with the preview tool and a fake helper
# (tests/fake-rsos-update.sh): check, notes, install, restart dialog; up to
# date; a failed install; a board without A/B; "Updated to" after a
# restart; the USB dialog's "Install system update". Only from the main
# Makefile (it needs the UI build).
ifneq ($(UI_BUILD),)
UPD_UI_DIR = $(abspath $(BUILDDIR))/update-ui-check
UPD_FAKE = $(UPD_UI_DIR)/fake-rsos-update
UPD_PREVIEW = RSOS_UPDATE_SYNC=1 FAKE_UPDATE_LOG=$(UPD_UI_DIR)/calls timeout 120 $(UI_BUILD)/rsos-uipreview \
	--root $(UPD_UI_DIR) --quiet --update-helper $(UPD_FAKE)
# Settings > System update (the item after System information)
UPD_OPEN = start down down down down down down down down expect:menu:Settings|System_update a wait:100 \
	expect:menu:System_update|Installed_version down expect:|Check_for_updates
check-update: check-update-ui
check-update-ui: $(UI_BUILD)/rsos-uipreview
	rm -rf $(UPD_UI_DIR) && mkdir -p $(UPD_UI_DIR)/data/roms $(UPD_UI_DIR)/data/rsos
	cp $(UPD_DIR)/tests/fake-rsos-update.sh $(UPD_FAKE) && chmod +x $(UPD_FAKE)
	@echo "check-update-ui: check, the notes, install, restart later"
	FAKE_UPDATE=found $(UPD_PREVIEW) --keys "$(UPD_OPEN) a wait:300 expect:update:0.2.0|installable|net \
		down down a wait:300 expect:dialog:RetroStoneOS_0.2.0_is_installed expect:|RESTART_NOW right a \
		wait:300 expect:toast=The_new_version"
	grep -q -- '--machine check$$' $(UPD_UI_DIR)/calls
	grep -q -- '--machine apply --url https://github.com/PaddleStroke/RetroStoneOS/releases/download/v0.2.0/retrostoneos-0.2.0-retrostone2.rsu --name retrostoneos-0.2.0-retrostone2.rsu --size 88080384$$' $(UPD_UI_DIR)/calls
	@echo "check-update-ui: up to date; a failed install (battery); a board without A/B"
	FAKE_UPDATE=uptodate $(UPD_PREVIEW) --keys "$(UPD_OPEN) a wait:300 expect:toast=RetroStoneOS_is_up_to_date \
		expect:menu:System_update|Check_for_updates b b"
	FAKE_UPDATE=fail $(UPD_PREVIEW) --keys "$(UPD_OPEN) a wait:300 expect:update:0.2.0 a wait:300 \
		expect:dialog:The_battery_is_too_low b"
	FAKE_UPDATE=noab $(UPD_PREVIEW) --keys "$(UPD_OPEN) a wait:300 expect:update:0.2.0|noab|net a wait:100 \
		expect:dialog:This_console_is_updated_by_flashing b b"
	@echo "check-update-ui: after the restart: \"Updated to RetroStoneOS 0.2.0\" once"
	printf 'version=0.2.0\nslot=b\nannounced=0\n' > $(UPD_UI_DIR)/data/rsos/update-state.ini
	FAKE_UPDATE_BOOT=updated $(UPD_PREVIEW) --keys "wait:500 expect:dialog:Updated_to_RetroStoneOS_0.2.0. a"
	FAKE_UPDATE_BOOT=none $(UPD_PREVIEW) --keys "wait:500 expect:carousel"
	@echo "check-update-ui: a USB drive with a package: \"Install system update\" in its dialog"
	rm -rf /tmp/rsos-upd-stick && mkdir -p /tmp/rsos-upd-stick/RetroStoneOS
	printf 'x' > /tmp/rsos-upd-stick/RetroStoneOS/retrostoneos-0.2.0-retrostone2.rsu
	RSOS_FAKE_USB_MP=/tmp/rsos-upd-stick FAKE_UPDATE=usb $(UPD_PREVIEW) --fake-transfer --keys "usb wait:300 \
		expect:dialog:USB_drive down right expect:|INSTALL_UPDATE a wait:300 expect:update:0.2.0|installable|file b b"
	@echo "check-update-ui: a development build on the USB drive of a release console: refused, said so (code variant)"
	RSOS_FAKE_USB_MP=/tmp/rsos-upd-stick FAKE_UPDATE=variant $(UPD_PREVIEW) --fake-transfer --keys "usb wait:300 \
		expect:dialog:USB_drive down right expect:|INSTALL_UPDATE a wait:300 \
		expect:dialog:RetroStoneOS_0.2.0_was_found,_but:_This_update_is_a_development_build_and_cannot_be_installed a b"
	grep -q -- '--machine check --local-only --dir /tmp/rsos-upd-stick$$' $(UPD_UI_DIR)/calls
	RSOS_FAKE_USB_MP=/tmp/rsos-upd-stick-none $(UPD_PREVIEW) --fake-transfer --keys "usb wait:300 \
		expect:dialog:USB_drive down expect:|BACK_UP_SAVES right expect:|NOTHING b"
	rm -rf /tmp/rsos-upd-stick
	@echo "check-update-ui: OK"
endif

# /usr/bin/rsos-update, the public key, and /etc/rsos/version.env (what the
# updater and the package builder read: the version, the variant, the build
# time and the board id; the Buildroot package passes RSOS_OS_* and
# RSOS_BOARD_ID).
RSOS_OS_VERSION ?= dev
RSOS_OS_VARIANT ?= dev
RSOS_BOARD_ID ?=
RSOS_OS_BUILD_TIME ?= $(UPD_BUILD_EPOCH)
RSOS_OS_BUILD_DATE ?= $(shell date -u -d @$(RSOS_OS_BUILD_TIME) +%Y-%m-%d 2>/dev/null)
update-install: $(UPD_BIN)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(DATADIR) $(DESTDIR)$(SYSCONFDIR)/rsos
	install -m 0755 $(UPD_BIN) $(DESTDIR)$(BINDIR)/rsos-update
	install -m 0644 $(RSOS_UPDATE_PUBKEY) $(DESTDIR)$(DATADIR)/update.pub
	printf "# Generated by the rsos-frontend package (frontend/update.mk).\nRSOS_VERSION='%s'\nRSOS_VARIANT='%s'\nRSOS_BUILD_TIME='%s'\nRSOS_BUILD_DATE='%s'\nRSOS_BOARD_ID='%s'\n" \
		'$(RSOS_OS_VERSION)' '$(RSOS_OS_VARIANT)' '$(RSOS_OS_BUILD_TIME)' '$(RSOS_OS_BUILD_DATE)' \
		'$(RSOS_BOARD_ID)' > $(DESTDIR)$(SYSCONFDIR)/rsos/version.env
	chmod 0644 $(DESTDIR)$(SYSCONFDIR)/rsos/version.env

# ARM (A20) cross check of the device program, like the other modules'
# (needs the armhf zstd/mbedTLS development files: frontend-cross.sh builds
# it within "make all" with the target libraries).

-include $(UPD_OBJS:.o=.d) $(UPD_BUILD)/src/update/rsos-update.d $(UPD_BUILD)/src/update/tests/test_update.d
