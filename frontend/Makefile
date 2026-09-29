# RetroStoneOS frontend
#
# Honours CC, CFLAGS, CPPFLAGS, LDFLAGS, PKG_CONFIG, DESTDIR, PREFIX.
# Buildroot passes $(TARGET_CONFIGURE_OPTS), which sets all of them, and
# BUILDDIR=$(@D)/build.
#
#   make                 rsos-frontend + the tools (rsos-kmstest, rsos-display-selftest,
#                        rsos-run, rsos-bootreason, rsos-clock)
#   make check           build and run every module's tests (host), plus a headless
#                        run of the real rsos-frontend main loop (check-frontend)
#   make check-asan      the same under ASan + UBSan + LeakSanitizer ($(BUILDDIR)-asan)
#   make uipreview       the UI preview tool (host only)
#   make check-syntax    compile-only check of the display layer (cross compiler without libdrm)
#   make install DESTDIR=...
#
# Objects go to $(BUILDDIR), outside the source tree by default (in WSL:
# ~/rsos/frontend-build, on ext4; never build under /mnt/c).
#
# Every module fragment is included: ui.mk (src/{ui,gfx,theme,input}),
# host.mk (src/host, src/audio), transfer.mk (src/transfer) and power.mk
# (src/power). All objects get the caller's CPPFLAGS/CFLAGS (Buildroot's
# -D_FILE_OFFSET_BITS=64 / -D_TIME_BITS=64 must be the same everywhere:
# time_t and off_t cross the module boundaries).

CC         ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS     ?= -O2
PREFIX     ?= /usr
BINDIR     ?= $(PREFIX)/bin
DATADIR    ?= $(PREFIX)/share/rsos
SYSCONFDIR ?= /etc
BUILDDIR   ?= $(if $(HOME),$(HOME)/rsos/frontend-build,/tmp/rsos-frontend-build)
# /etc/rsos-version (shown in System information); the package sets it.
RSOS_VERSION_STRING ?= RetroStoneOS (development build)

# libdrm flags: pkg-config when available, plain defaults otherwise.
DRM_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libdrm 2>/dev/null || echo -I$(SYSROOT)/usr/include/libdrm)
DRM_LIBS   ?= $(shell $(PKG_CONFIG) --libs libdrm 2>/dev/null || echo -ldrm)

# Always on, independent of the caller's CFLAGS. A non-literal format
# string is an error everywhere (review F-L24).
RSOS_FORMAT_WARN = -Wformat -Wformat-security -Werror=format-security
# 64-bit off_t in every module, also when CPPFLAGS does not say so (the
# cross checks): off_t crosses the module boundaries.
RSOS_LFS = -D_FILE_OFFSET_BITS=64
RSOS_CFLAGS = -std=c11 -Wall -Wextra $(RSOS_FORMAT_WARN) $(RSOS_LFS) -D_GNU_SOURCE -Isrc $(DRM_CFLAGS)

# ------------------------------------------------------------ display layer
LIB_SRCS  = src/display.c src/uevent.c src/font8x8.c
LIB_OBJS  = $(LIB_SRCS:src/%.c=$(BUILDDIR)/%.o)
LIB       = $(BUILDDIR)/librsos-display.a

KMSTEST_OBJS = $(BUILDDIR)/tools/kmstest.o
KMSTEST      = $(BUILDDIR)/rsos-kmstest

# The self-test #includes display.c to reach its static helpers.
SELFTEST_OBJS = $(BUILDDIR)/tests/display_selftest.o $(BUILDDIR)/uevent.o
SELFTEST      = $(BUILDDIR)/rsos-display-selftest

# The display layer against a KMS mock (host only: libdrm replaced by mocks,
# display-design.md 8.5 panel safety).
KMS_TEST_OBJS = $(BUILDDIR)/tests/test_display_kms.o $(BUILDDIR)/uevent.o
KMS_TEST      = $(BUILDDIR)/test-display-kms

# ------------------------------------------------------- module fragments
# The fragments' "host" objects are simply "this compiler" (power.md §11).
HOST_CC = $(CC)
TR_CC   = $(CC)
UI_BUILD    = $(BUILDDIR)/ui
POWER_BUILD = $(BUILDDIR)/power
TR_BUILD    = $(BUILDDIR)/transfer
# Module flags (their warnings and optimisation, after the caller's flags).
UI_CFLAGS    = $(CPPFLAGS) $(CFLAGS) -std=gnu11 $(UI_WARN) $(UI_OPT) -g -D_GNU_SOURCE $(RSOS_LFS) $(UI_INCLUDES)
POWER_CFLAGS = $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra $(RSOS_FORMAT_WARN) -O2 -g -D_GNU_SOURCE \
	$(RSOS_LFS) $(POWER_INCLUDES)
TR_CFLAGS    = $(CPPFLAGS) $(CFLAGS) -std=gnu11 $(TR_WARN) -O2 -g -D_GNU_SOURCE $(RSOS_LFS) $(TR_INCLUDES)
# rsos-run links the input layer, the translations and the TrueType text
# (in-game menu, toasts) from the UI build (no second copy).
HOST_INPUT_EXTRA_OBJS = $(UI_BUILD)/host/input/input.o $(UI_BUILD)/host/ui/util.o \
	$(UI_BUILD)/host/i18n/i18n.o $(UI_BUILD)/host/gfx/font.o $(UI_BUILD)/host/gfx/gfx.o \
	$(UI_BUILD)/host/gfx/third_party_impl.o

all: frontend-all

include ui.mk
include host.mk
include transfer.mk
include power.mk
# the system updater: rsos-update (a separate program), rsos-mkupdate, check-update
include update.mk

# ---------------------------------------------------------------- boot logo
# mksplash runs on the build machine (CC_FOR_BUILD: Buildroot passes its
# host compiler) and turns the logo into splash.rle, a 640x480 RLE frame
# (src/splash.h, docs/bringup.md). SPLASH_BG is the background colour
# (RRGGBB); previews of the candidates: `make splash-previews`.
CC_FOR_BUILD     ?= cc
CFLAGS_FOR_BUILD ?= -O2
SPLASH_LOGO  = assets/splash/retrostone2-logo.png
SPLASH_BG   ?= 2a2a35
SPLASH_ARGS ?=
MKSPLASH     = $(BUILDDIR)/build-tools/mksplash
SPLASH       = $(BUILDDIR)/splash.rle
SPLASH_CFG   = $(BUILDDIR)/splash.cfg
SPLASH_PREVIEW_DIR ?= ../docs/ui-previews
SPLASH_PREVIEWS = black:000000 near-black:16161c dark-slate:2a2a35 rsos-dark:16191e \
	light-grey:e6e6ea white:ffffff

$(MKSPLASH): src/tools/mksplash.c src/splash.c src/splash.h
	@mkdir -p $(dir $@)
	$(CC_FOR_BUILD) $(CFLAGS_FOR_BUILD) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE $(LDFLAGS_FOR_BUILD) \
		-o $@ src/tools/mksplash.c src/splash.c -lm

# Rebuilt when the colour or the options change.
$(SPLASH_CFG): FORCE
	@mkdir -p $(dir $@)
	@echo '$(SPLASH_BG) $(SPLASH_ARGS)' | cmp -s - $@ || echo '$(SPLASH_BG) $(SPLASH_ARGS)' > $@

$(SPLASH): $(MKSPLASH) $(SPLASH_LOGO) $(SPLASH_CFG)
	$(MKSPLASH) -q --bg $(SPLASH_BG) $(SPLASH_ARGS) -o $@ $(SPLASH_LOGO)

splash-previews: $(MKSPLASH)
	@for p in $(SPLASH_PREVIEWS); do \
		echo "  $(SPLASH_PREVIEW_DIR)/splash-$${p%%:*}.png (#$${p#*:})"; \
		$(MKSPLASH) -q --bg $${p#*:} $(SPLASH_ARGS) --png $(SPLASH_PREVIEW_DIR)/splash-$${p%%:*}.png $(SPLASH_LOGO) || exit 1; \
	done

FORCE:

# ------------------------------------------------------------ translations
# docs/translating.md. rsos-i18n (build machine) extracts the marked strings
# (make pot), compiles po/<lang>.po into $(BUILDDIR)/locale/<lang>.cat
# (installed to $(DATADIR)/locale) and checks them (check-i18n).
RSOS_I18N   = $(BUILDDIR)/build-tools/rsos-i18n
PO_FILES    = $(sort $(wildcard po/*.po))
CATALOGS    = $(PO_FILES:po/%.po=$(BUILDDIR)/locale/%.cat)
# Every source that can show text (tests and tools excluded).
I18N_SOURCES = $(sort $(wildcard src/*.c src/ui/*.c src/gfx/*.c src/host/*.c src/i18n/*.c src/power/*.c \
	src/transfer/*.c))
# The fonts of the text renderer's fallback chain (gfx/font.c): every
# character of every translation must be in one of them.
I18N_FONTS  = third_party/fonts/DejaVuSans.ttf $(sort $(wildcard third_party/fonts/RSOS-CJK-*.otf))

$(RSOS_I18N): src/tools/rsos-i18n.c src/i18n/i18n.c src/i18n/i18n.h src/i18n/i18n_cat.h
	@mkdir -p $(dir $@)
	$(CC_FOR_BUILD) $(CFLAGS_FOR_BUILD) -std=gnu11 -Wall -Wextra -D_GNU_SOURCE $(LDFLAGS_FOR_BUILD) \
		-o $@ src/tools/rsos-i18n.c src/i18n/i18n.c -lpthread

$(BUILDDIR)/locale/%.cat: po/%.po $(RSOS_I18N)
	@mkdir -p $(dir $@)
	$(RSOS_I18N) compile -o $@ $<

catalogs: $(CATALOGS)

# Regenerates po/rsos.pot from the sources (commit it with the change).
pot: $(RSOS_I18N)
	$(RSOS_I18N) extract -o po/rsos.pot $(I18N_SOURCES)

# Merges po/rsos.pot into every po/<lang>.po (needs gettext's msgmerge).
po-update:
	@command -v msgmerge >/dev/null || { echo "po-update: msgmerge (gettext) is needed"; exit 1; }
	for p in $(PO_FILES); do msgmerge --quiet --update --backup=none --no-location --no-wrap $$p po/rsos.pot || exit 1; done

# The CJK fallback fonts (third_party/fonts/RSOS-CJK-{JP,SC,KR}.otf):
# subsets of Noto Sans CJK with the characters of ja/zh_CN/ko.po and common
# ones (po/mkcjkfont.py). Run after changing those translations, commit the
# result. Needs python3-fonttools and the Noto Sans CJK collection.
NOTO_CJK_TTC ?= /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc
cjk-fonts: $(RSOS_I18N)
	@mkdir -p $(BUILDDIR)/cjk
	for l in ja zh_CN ko; do $(RSOS_I18N) chars po/$$l.po > $(BUILDDIR)/cjk/$$l.txt || exit 1; done
	python3 po/mkcjkfont.py $(NOTO_CJK_TTC) third_party/fonts $(BUILDDIR)/cjk/ja.txt \
		$(BUILDDIR)/cjk/zh_CN.txt $(BUILDDIR)/cjk/ko.txt

# Translation statistics (no failure on missing entries).
po-stats: $(RSOS_I18N)
	$(RSOS_I18N) check --pot po/rsos.pot $(PO_FILES)

# Every marked string is in po/rsos.pot, every .po compiles without a
# format error, every translated character is in the fonts, and the unit
# test (catalogs, plurals, the glyph fallback) passes.
I18N_TEST = $(BUILDDIR)/test_i18n
I18N_CHECK_DIR = $(BUILDDIR)/i18n-check

$(I18N_TEST): $(BUILDDIR)/tests/test_i18n.o $(UI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(UI_LDLIBS)

I18N_UI_DIR = $(I18N_CHECK_DIR)/ui
I18N_PREVIEW = $(UIPREVIEW) --root $(I18N_UI_DIR) --quiet --locale $(abspath $(BUILDDIR)/locale)
check-i18n: $(RSOS_I18N) $(I18N_TEST) $(CATALOGS) $(UIPREVIEW)
	rm -rf $(I18N_CHECK_DIR) && mkdir -p $(I18N_CHECK_DIR)/cat
	$(RSOS_I18N) extract -o $(I18N_CHECK_DIR)/fresh.pot $(I18N_SOURCES)
	$(RSOS_I18N) check-pot $(I18N_CHECK_DIR)/fresh.pot po/rsos.pot
	$(RSOS_I18N) check --pot po/rsos.pot $(PO_FILES)
	$(RSOS_I18N) fontcheck $(I18N_FONTS) -- $(PO_FILES)
	@echo "check-i18n: the compiler drops the two bad entries of the test catalog (2 warnings expected)"
	$(RSOS_I18N) compile -o $(I18N_CHECK_DIR)/cat/da.cat tests/i18n-test.po
	$(I18N_TEST) $(I18N_CHECK_DIR) $(abspath $(BUILDDIR)/locale) $(abspath third_party/fonts)
	@echo "check-i18n: first boot: the language picker before the menu, previewed as the cursor moves; the choice is saved"
	mkdir -p $(I18N_UI_DIR)/data/roms $(I18N_UI_DIR)/data/rsos
	$(I18N_PREVIEW) --language-prompt --keys "wait:300 expect:language:Choose_your_language|en|English \
		b expect:language:Choose_your_language|en down down expect:language:Choisis_ta_langue|fr| a wait:300 \
		expect:carousel start expect:menu:Paramètres|Affichage b"
	grep -q '^language *= *fr$$' $(I18N_UI_DIR)/data/rsos/settings.ini
	$(I18N_PREVIEW) --language-prompt --keys "wait:300 expect:carousel start expect:menu:Paramètres b"
	@echo "check-i18n: Settings > Language, live: the menus come back in the new language, saved"
	$(I18N_PREVIEW) --keys "start up expect:menu:Paramètres|Langue_(Language) a \
		expect:menu:Langue_(Language)|Français up up up expect:|Deutsch a wait:300 \
		expect:menu:Sprache_(Language)|Deutsch b expect:menu:Einstellungen|Sprache b expect:carousel \
		lang:en start expect:menu:Settings|Display b"
	grep -q '^language *= *en$$' $(I18N_UI_DIR)/data/rsos/settings.ini
	@echo "check-i18n: OK"

# ------------------------------------------------------------ rsos-frontend
FRONTEND      = $(BUILDDIR)/rsos-frontend
FRONTEND_OBJS = $(BUILDDIR)/main.o $(BUILDDIR)/splash.o $(UI_OBJS) $(POWER_OBJS) $(TR_OBJS) $(HOST_OBJS)
FRONTEND_LIBS = $(DRM_LIBS) $(HOST_LIBS) $(TR_LDLIBS) $(UI_LDLIBS) $(POWER_LDLIBS)
UIPREVIEW     = $(UI_BUILD)/rsos-uipreview

BINS = $(FRONTEND) $(KMSTEST) $(SELFTEST) $(RSOS_RUN) $(POWER_TOOLS) $(SPLASH) $(CATALOGS) $(UPD_BIN)

frontend-all: $(BINS)

$(BUILDDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(RSOS_CFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/tests/%.o: tests/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(RSOS_CFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(FRONTEND): $(FRONTEND_OBJS) $(LIB) src/host/glprobe.syms
	$(CC) $(LDFLAGS) $(HOST_EXPORT_LDFLAGS) -o $@ $(FRONTEND_OBJS) $(LIB) $(FRONTEND_LIBS)

$(KMSTEST): $(KMSTEST_OBJS) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(DRM_LIBS)

$(SELFTEST): $(SELFTEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(DRM_LIBS)

$(KMS_TEST): $(KMS_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(DRM_LIBS)

uipreview: $(UIPREVIEW)

# ------------------------------------------------------------------ checks
# Every module's tests. Paths are used as they are (BUILDDIR may be absolute).
check: check-display check-host check-power check-transfer check-ui check-ui-assets check-loader check-splash \
	check-board check-i18n check-frontend check-update check-b2 check-robust

check-display: $(SELFTEST) $(KMS_TEST)
	$(SELFTEST)
	$(KMS_TEST)

check-host: $(HOST_TEST) $(HOST_TESTCORE) $(HOST_LAUNCH_TEST) $(RSOS_RUN) $(HOST_GLPROBE_TEST)
	$(HOST_TEST)
	$(HOST_GLPROBE_TEST) $(HOST_GLCORE)
	rm -rf $(BUILDDIR)/launch-test && $(HOST_LAUNCH_TEST) $(RSOS_RUN) $(HOST_TESTCORE) $(BUILDDIR)/launch-test

check-power: $(POWER_TEST)
	$(POWER_TEST)

check-transfer: $(TR_BUILD)/test_transfer
	$(TR_BUILD)/test_transfer

# The UI has no unit-test binary: a scripted tour of the preview tool over an
# empty library (every menu, both themes, both sizes) must run clean.
UI_CHECK_DIR = $(BUILDDIR)/ui-check
check-ui: $(UIPREVIEW)
	rm -rf $(UI_CHECK_DIR) && mkdir -p $(UI_CHECK_DIR)/data/roms $(UI_CHECK_DIR)/data/rsos
	$(UIPREVIEW) --root $(UI_CHECK_DIR) --quiet --fake-transfer \
		--keys "start down a b down a b b usb wait:500 b size:854x480 theme:rsos-light start shot:$(UI_CHECK_DIR)/settings.png b" \
		--out $(UI_CHECK_DIR)/final.png
	test -s $(UI_CHECK_DIR)/settings.png && test -s $(UI_CHECK_DIR)/final.png
	@echo "check-ui: USB drive plugged after a long idle (the device's poll() sleep): the dialog shows"
	$(UIPREVIEW) --root $(UI_CHECK_DIR) --quiet --fake-transfer \
		--keys "idle:9000 usb wait:300 expect:dialog:USB_drive_\"GAMES\"_connected expect:|IMPORT_GAMES \
			b expect:carousel usb wait:300 unplug wait:300 expect:carousel \
			idle:9000 unplug wait:300 expect:toast=USB_drive_removed wait:3000 expect:toast=USB_drive_removed"
	printf 'usb_import_prompt=0\n' > $(UI_CHECK_DIR)/data/rsos/settings.ini
	$(UIPREVIEW) --root $(UI_CHECK_DIR) --quiet --fake-transfer \
		--keys "idle:9000 usb wait:300 expect:carousel_toast=USB_drive_GAMES_connected"
	rm -f $(UI_CHECK_DIR)/data/rsos/settings.ini
	@echo "check-ui: import: library picker, the four duplicate answers; one library: no picker"
	$(UIPREVIEW) --root $(UI_CHECK_DIR) --quiet --fake-transfer \
		--keys "usb wait:300 a wait:1000 expect:menu:Import_from_GAMES|All a wait:1000 expect:|Copy \
			a wait:700 expect:dialog:This_game expect:|SKIP a wait:700 right expect:|REPLACE a wait:700 \
			expect:dialog:This_save down expect:|SKIP_ALL a wait:700 down right expect:|REPLACE_ALL a \
			wait:3000 expect:dialog:Imported right a wait:300 expect:carousel \
			usbtrees:1 usb wait:300 a wait:1500 expect:menu:Import_from_GAMES|Copy b" > $(UI_CHECK_DIR)/usb.out
	grep -q 'ANSWER 1 skip$$' $(UI_CHECK_DIR)/usb.out && grep -q 'ANSWER 2 replace$$' $(UI_CHECK_DIR)/usb.out
	grep -q 'ANSWER 3 skip-all$$' $(UI_CHECK_DIR)/usb.out && grep -q 'ANSWER 4 replace-all$$' $(UI_CHECK_DIR)/usb.out
	grep -q '^PLAN 3 roots' $(UI_CHECK_DIR)/usb.out && grep -q '^PLAN 1 root' $(UI_CHECK_DIR)/usb.out
	@echo "check-ui: export games and back up saves: read-write only around the copy"
	$(UIPREVIEW) --root $(UI_CHECK_DIR) --quiet --fake-transfer \
		--keys "usbfs:vfat usb wait:300 right expect:|EXPORT_GAMES a wait:1000 expect:menu:Export_games_to_GAMES|Export \
			a wait:7000 expect:dialog:Copied right a usb wait:300 down expect:|BACK_UP_SAVES a wait:1000 \
			expect:menu:Back_up_saves_to_GAMES|Back_up a wait:7000 expect:dialog:Copied right a" > $(UI_CHECK_DIR)/usb.out
	grep -q '^BACKUP export bios$$' $(UI_CHECK_DIR)/usb.out && grep -q '^BACKUP saves$$' $(UI_CHECK_DIR)/usb.out
	test $$(grep -c '^REMOUNT /media/usb0 rw$$' $(UI_CHECK_DIR)/usb.out) = 2
	test $$(grep -c '^REMOUNT /media/usb0 ro$$' $(UI_CHECK_DIR)/usb.out) = 2
	@echo "check-ui: LCD refresh rate: 60 Hz on trial reverts by itself after 15 s; KEEP saves it; 78 Hz at once"
	rm -f $(UI_CHECK_DIR)/data/rsos/settings.ini
	$(UIPREVIEW) --root $(UI_CHECK_DIR) --quiet \
		--keys "start a down expect:menu:Display|LCD_refresh_rate right wait:300 \
			expect:dialog:The_screen_now_refreshes_at_60_Hz._Keep_this_setting?_Reverting_in_1 expect:|REVERT \
			wait:9000 expect:dialog:The_screen wait:6000 expect:menu:Display|LCD_refresh_rate \
			right wait:300 b wait:300 expect:menu:Display" > $(UI_CHECK_DIR)/lcd.out
	test "$$(grep '^SETTING lcd_refresh=' $(UI_CHECK_DIR)/lcd.out | tr '\n' ' ')" = \
		"SETTING lcd_refresh=60 SETTING lcd_refresh=78 SETTING lcd_refresh=60 SETTING lcd_refresh=78 "
	! grep -qs 'lcd_refresh' $(UI_CHECK_DIR)/data/rsos/settings.ini
	$(UIPREVIEW) --root $(UI_CHECK_DIR) --quiet \
		--keys "start a down right wait:300 left expect:|KEEP a wait:300 expect:menu:Display|LCD_refresh_rate \
			wait:16000 expect:menu:Display" > $(UI_CHECK_DIR)/lcd.out
	test "$$(grep '^SETTING lcd_refresh=' $(UI_CHECK_DIR)/lcd.out | tr '\n' ' ')" = "SETTING lcd_refresh=60 "
	grep -q '^lcd_refresh *= *60$$' $(UI_CHECK_DIR)/data/rsos/settings.ini
	$(UIPREVIEW) --root $(UI_CHECK_DIR) --quiet --keys "start a down left wait:300 expect:menu:Display" \
		> $(UI_CHECK_DIR)/lcd.out
	test "$$(grep '^SETTING lcd_refresh=' $(UI_CHECK_DIR)/lcd.out | tr '\n' ' ')" = "SETTING lcd_refresh=78 "
	grep -q '^lcd_refresh *= *78$$' $(UI_CHECK_DIR)/data/rsos/settings.ini
	rm -f $(UI_CHECK_DIR)/data/rsos/settings.ini $(UI_CHECK_DIR)/data/rsos/settings.ini.bak
	@echo "check-ui: the on-screen keyboard lives through timer ticks (review F-C1: it ran the dialog's"
	@echo "          countdown code on its own, smaller struct; check-asan catches the overflow)"
	$(UIPREVIEW) --root $(UI_CHECK_DIR) --quiet \
		--keys "start down down a expect:menu:Network|WiFi down a wait:1500 expect:osk:WiFi_network_name| \
			a wait:1500 expect:osk:WiFi_network_name|q x wait:300 expect:osk:WiFi_network_name|q_ \
			select wait:300 expect:menu:Network|WiFi_network down a wait:1500 expect:osk:WiFi_password| \
			select wait:300 expect:menu:Network|WiFi_password"
	@echo "check-ui: game options act on their game after Favorite re-sorts the list (review F-H4)"
	rm -rf $(UI_CHECK_DIR)-games && mkdir -p $(UI_CHECK_DIR)-games/data/roms/nes \
		$(UI_CHECK_DIR)-games/data/rsos $(UI_CHECK_DIR)-games/usr/share/rsos/cores
	printf '[core]\nid = c_nes\nsystems = nes\nextensions = nes\n' > \
		$(UI_CHECK_DIR)-games/usr/share/rsos/cores/c_nes.ini
	for g in A B C; do printf 'NES' > "$(UI_CHECK_DIR)-games/data/roms/nes/$$g Game.nes"; done
	$(UIPREVIEW) --root $(UI_CHECK_DIR)-games --quiet \
		--keys "a wait:500 expect:list down down select wait:300 expect:menu:C_Game|Favorite a wait:300 \
			expect:menu:C_Game|Favorite b b"
	grep -q 'C Game.nes	1' $(UI_CHECK_DIR)-games/data/rsos/gamedb.tsv
	$(UIPREVIEW) --root $(UI_CHECK_DIR)-games --quiet \
		--keys "a wait:500 expect:list select wait:300 expect:menu:C_Game|Favorite a wait:300 b b"
	! grep -q 'C Game\|B Game\|A Game' $(UI_CHECK_DIR)-games/data/rsos/gamedb.tsv
	rm -f $(UI_CHECK_DIR)-games/data/rsos/gamedb.tsv*
	$(UIPREVIEW) --root $(UI_CHECK_DIR)-games --quiet \
		--keys "a wait:500 expect:list down down select wait:300 expect:menu:C_Game|Favorite a wait:300 \
			a wait:300 expect:menu:C_Game|Favorite b b"
	! grep -q 'Game.nes	1' $(UI_CHECK_DIR)-games/data/rsos/gamedb.tsv

# The asset worker (src/ui/prefetch.c, docs/ui-design.md §4.2), with the
# preview's per-frame accounting of the UI thread's work (costcheck:on: a
# frame that decodes an image, composites a backdrop, reads a cache file or
# parses a theme on the UI thread fails the run):
#  1. once everything is prefetched, first visits of every system, a list, a
#     game next to the first, the neighbour's list: no such frame (gbz35-dark:
#     per-system photos and SVG logos; rsos-dark: text logos and SVG art);
#  2. an empty cache and a slow worker (RSOS_PREFETCH_DELAY_MS): a move is
#     drawn at once with a placeholder (no late frame: costmax), then the
#     backdrop and logos arrive; the same while the key repeats.
UI_ASSETS_DIR = $(BUILDDIR)/ui-assets
UI_ASSETS_SYSTEMS = nes snes gb gba megadrive mastersystem gamegear pcengine
UI_ASSETS_PV = $(UIPREVIEW) --root $(UI_ASSETS_DIR) --quiet
check-ui-assets: $(UIPREVIEW)
	rm -rf $(UI_ASSETS_DIR) && mkdir -p $(UI_ASSETS_DIR)/data/rsos $(UI_ASSETS_DIR)/usr/share/rsos/cores
	printf '[core]\nid = c_all\nsystems = %s\nextensions = rom\n' "$$(echo $(UI_ASSETS_SYSTEMS) | sed 's/ /, /g')" \
		> $(UI_ASSETS_DIR)/usr/share/rsos/cores/c_all.ini
	for s in $(UI_ASSETS_SYSTEMS); do mkdir -p $(UI_ASSETS_DIR)/data/roms/$$s && \
		printf 'x' > "$(UI_ASSETS_DIR)/data/roms/$$s/A Game.rom" && printf 'x' > "$(UI_ASSETS_DIR)/data/roms/$$s/B Game.rom" || exit 1; done
	mkdir -p $(UI_ASSETS_DIR)/data/roms/snes/media
	cp themes/gbz35-dark/snes/background.png $(UI_ASSETS_DIR)/data/roms/snes/media/a.png
	cp themes/gbz35-dark/nes/background.png $(UI_ASSETS_DIR)/data/roms/snes/media/b.png
	printf '<?xml version="1.0"?>\n<gameList>\n<game><path>./A Game.rom</path><name>A Game</name><desc>Box art and a marquee.</desc><image>./media/a.png</image><marquee>./media/b.png</marquee></game>\n<game><path>./B Game.rom</path><name>B Game</name><image>./media/b.png</image><marquee>./media/a.png</marquee></game>\n</gameList>\n' \
		> $(UI_ASSETS_DIR)/data/roms/snes/gamelist.xml
	for t in gbz35-dark rsos-dark; do \
		echo "check-ui-assets: $$t: prefetch complete: first visits do no decode, composite, cache read or theme parse on the UI thread"; \
		rm -rf $(UI_ASSETS_DIR)/data/rsos/cache; \
		printf 'language = en\nlast_system = snes\n' > $(UI_ASSETS_DIR)/data/rsos/settings.ini; \
		$(UI_ASSETS_PV) --theme $$t --keys "bgwait:all expect-assets:worker=idle costcheck:on \
			right right right right right right right right right left left left left left left left left left \
			expect:carousel expect-assets:sys=snes_backdrop=ready_logos=ready bgwait:all a wait:500 expect:list:snes \
			bgwait:all down wait:500 expect:game=B_Game bgwait:all right wait:500 expect:list: b costcheck:off" \
			|| exit 1; \
	done
	@echo "check-ui-assets: empty cache, slow worker: the move is drawn at once (placeholder), then the backdrop"
	rm -rf $(UI_ASSETS_DIR)/data/rsos/cache
	printf 'language = en\nlast_system = snes\n' > $(UI_ASSETS_DIR)/data/rsos/settings.ini
	RSOS_PREFETCH_DELAY_MS=3000 $(UI_ASSETS_PV) --theme gbz35-dark --keys "costcheck:on bgsync:off tap:right \
		expect-assets:backdrop=pending costmax:100000 costcheck:off bgwait wait:300 \
		expect-assets:backdrop=ready_logos=ready"
	rm -rf $(UI_ASSETS_DIR)/data/rsos/cache
	RSOS_PREFETCH_DELAY_MS=150 $(UI_ASSETS_PV) --theme rsos-dark --keys "costcheck:on bgsync:off hold:right:1200 \
		costmax:100000 costcheck:off bgwait wait:300 expect-assets:backdrop=ready_logos=ready"

# The carousel snapshot and the background game list loader (src/ui/loader.c):
# a 34-folder library, first boot, snapshot boot (menu before the lists),
# library changed while off, full load comparison, on-demand opening.
LOADER_TEST = $(BUILDDIR)/test_loader
LOADER_CHECK_DIR = $(BUILDDIR)/loader-check

$(LOADER_TEST): $(BUILDDIR)/tests/test_loader.o $(UI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(UI_LDLIBS)

check-loader: $(LOADER_TEST)
	rm -rf $(LOADER_CHECK_DIR)
	$(LOADER_TEST) $(LOADER_CHECK_DIR) $(abspath third_party) $(abspath themes)

# Broken or odd files (a theme, a gamelist, a font from a USB stick) do not
# freeze or crash the menu: SVG sizes, include loops, XML text and nesting,
# no usable font, a looping catalog chain (tests/test_robust.c).
ROBUST_TEST = $(BUILDDIR)/test_robust
ROBUST_CHECK_DIR = $(BUILDDIR)/robust-check

$(ROBUST_TEST): $(BUILDDIR)/tests/test_robust.o $(UI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(UI_LDLIBS)

check-robust: $(ROBUST_TEST)
	rm -rf $(ROBUST_CHECK_DIR)
	$(ROBUST_TEST) $(ROBUST_CHECK_DIR) $(abspath third_party/fonts)

# The board profile (src/board.h, docs/porting.md): the RetroStone2's
# board.ini gives the constants the code had before, and the module
# configurations equal their defaults; the power module does the same on a
# fake sysfs with and without it. Any other board.ini in the tree must parse.
# (Host only: the board files are outside frontend/, which Buildroot copies.)
BOARD_TEST = $(BUILDDIR)/test_board
BOARD_CHECK_DIR = $(BUILDDIR)/board-check
BOARD_INI_RS2 = ../buildroot-external/board/retrostone2/rootfs-overlay/etc/rsos/board.ini
BOARD_INI_OTHERS = $(filter-out $(BOARD_INI_RS2),$(wildcard ../buildroot-external/board/*/rootfs-overlay/etc/rsos/board.ini))

$(BOARD_TEST): $(BUILDDIR)/tests/test_board.o $(BUILDDIR)/board.o $(POWER_OBJS) $(UI_OBJS) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(DRM_LIBS) $(UI_LDLIBS) $(POWER_LDLIBS)

check-board: $(BOARD_TEST)
	rm -rf $(BOARD_CHECK_DIR)
	$(BOARD_TEST) $(BOARD_CHECK_DIR) $(BOARD_INI_RS2) $(BOARD_INI_OTHERS)

# The boot logo: mksplash decodes its own output again (round trip) and
# times the decoder; the file must stay small; `rsos-frontend --splash
# --message` draws it (the first-boot screen) into a PNG.
SPLASH_CHECK_DIR = $(BUILDDIR)/splash-check
check-splash: $(MKSPLASH) $(SPLASH) $(FRONTEND)
	rm -rf $(SPLASH_CHECK_DIR) && mkdir -p $(SPLASH_CHECK_DIR)
	$(MKSPLASH) -q --bg $(SPLASH_BG) $(SPLASH_ARGS) --bench 2000 -o $(SPLASH_CHECK_DIR)/splash.rle $(SPLASH_LOGO)
	cmp $(SPLASH) $(SPLASH_CHECK_DIR)/splash.rle
	test $$(wc -c < $(SPLASH)) -lt 32768
	@echo "check-splash: splash.rle is $$(wc -c < $(SPLASH)) bytes"
	$(FRONTEND) --splash --headless --res $(abspath third_party) --splash-file $(SPLASH) \
		--message "Preparing the SD card..." --shot $(SPLASH_CHECK_DIR)/first-boot.png
	test -s $(SPLASH_CHECK_DIR)/first-boot.png
	@# SIGTERM ends it within a second (data-partition stops it on the way to
	@# the menu, and gives up on it after a few seconds)
	$(FRONTEND) --splash --headless --res $(abspath third_party) --splash-file $(SPLASH) \
		--message "Preparing the SD card..." > $(SPLASH_CHECK_DIR)/term.log 2>&1 & pid=$$!; \
	i=0; while ! grep -q 'logo shown' $(SPLASH_CHECK_DIR)/term.log && [ $$i -lt 50 ]; do sleep 0.1; i=$$((i + 1)); done; \
	kill $$pid; (sleep 1; kill -9 $$pid 2> /dev/null) & w=$$!; wait $$pid; rc=$$?; kill $$w 2> /dev/null; \
	[ $$rc -eq 0 ] || { echo "check-splash: not ended 1 s after SIGTERM (status $$rc)"; exit 1; }; \
	grep -q 'splash: signal 15, closing the display' $(SPLASH_CHECK_DIR)/term.log && \
		grep -q 'splash: display closed' $(SPLASH_CHECK_DIR)/term.log
	@echo "check-splash: SIGTERM ends the splash at once"

# The real rsos-frontend main loop, headless: an in-memory surface, a fake
# /sys, /dev and /data under FE_CHECK_DIR, scripted buttons (each one also
# reaches the power module first, as the built-in keys do on the device).
# It loads the menu, launches a game (a real fork + exec of `rsos-frontend
# --run --headless` with the host's test core), comes back, switches to a
# fake HDMI output, then powers off through the power module. Step 1b is the
# regression for "after the screen comes back, no button works": idle
# screen-off, a button wakes it (swallowed), the next button must act; then
# a short power-key press powers off.
FE_CHECK_DIR = $(BUILDDIR)/frontend-check
FE_LOG = $(FE_CHECK_DIR)/run/rsos/frontend.log
FE_GAME_LOG = $(FE_CHECK_DIR)/run/rsos/game.log
FE_RESUME = $(FE_CHECK_DIR)/data/rsos/resume.ini
FE_SETTINGS = $(FE_CHECK_DIR)/data/rsos/settings.ini
FE_STATE = "$(FE_CHECK_DIR)/data/states/nes/Smoke Test.state.auto"
FE_WATCHDOG = $(FE_CHECK_DIR)/dev/watchdog
FE_RUN = $(FRONTEND) --headless --root $(FE_CHECK_DIR) --res $(abspath third_party) \
	--themes $(abspath themes) --locale $(abspath $(BUILDDIR)/locale) --splash-file $(SPLASH) --run-ms 20000 \
	--watchdog $(FE_WATCHDOG)
FE_SCRIPT = wait:300 shot:$(FE_CHECK_DIR)/carousel.png a wait:500 shot:$(FE_CHECK_DIR)/list.png \
	a wait:1500 shot:$(FE_CHECK_DIR)/after-game.png hdmi wait:300 shot:$(FE_CHECK_DIR)/hdmi.png \
	lcd wait:200 poweroff
FE_WAKE_SCRIPT = wait:1500 shot:$(FE_CHECK_DIR)/idle-before.png idleoff:2 wait:2600 \
	a wait:300 shot:$(FE_CHECK_DIR)/idle-woke.png start wait:300 shot:$(FE_CHECK_DIR)/idle-next.png \
	b wait:300 pek wait:500
check-frontend: $(FRONTEND) $(HOST_TESTCORE) $(SPLASH)
	@# the USB checks mount under $(FE_CHECK_DIR)/media: transfer_usb_init
	@# refuses a mount base of 58 characters or more (usb.c, mount_base[58])
	@test $$(printf '%s' '$(FE_CHECK_DIR)/media' | wc -c) -le 57 || \
		{ echo "check-frontend: BUILDDIR is too long for the USB checks: $(FE_CHECK_DIR)/media is over 57 characters"; exit 1; }
	rm -rf $(FE_CHECK_DIR)
	mkdir -p $(FE_CHECK_DIR)/data/roms/nes $(FE_CHECK_DIR)/data/rsos $(FE_CHECK_DIR)/data/bios \
		$(FE_CHECK_DIR)/usr/share/rsos/cores $(FE_CHECK_DIR)/dev/input $(FE_CHECK_DIR)/run/rsos \
		$(FE_CHECK_DIR)/tmp $(FE_CHECK_DIR)/media $(FE_CHECK_DIR)/sys/block \
		$(FE_CHECK_DIR)/sys/class/power_supply/axp20x-battery $(FE_CHECK_DIR)/sys/class/power_supply/axp20x-ac
	mkdir -p $(FE_CHECK_DIR)/etc/rsos && cp $(BOARD_INI_RS2) $(FE_CHECK_DIR)/etc/rsos/board.ini
	# a language is set: no first-boot language picker (check-i18n tests it)
	printf 'language = en\n' > $(FE_SETTINGS)
	printf 'NES\032smoke' > "$(FE_CHECK_DIR)/data/roms/nes/Smoke Test.nes"
	printf '[core]\nid = fceumm\ndisplay_name = Test core\nlibrary = %s\nsystems = nes\nextensions = nes\n' \
		"$(abspath $(HOST_TESTCORE))" > $(FE_CHECK_DIR)/usr/share/rsos/cores/fceumm.ini
	cd $(FE_CHECK_DIR)/sys/class/power_supply/axp20x-battery && printf '1\n' > present && \
		printf 'Discharging\n' > status && printf '80\n' > capacity && printf '3900000\n' > voltage_now && \
		printf '300000\n' > current_now && printf 'Battery\n' > type
	cd $(FE_CHECK_DIR)/sys/class/power_supply/axp20x-ac && printf '0\n' > online && printf 'Mains\n' > type
	@echo "check-frontend: 1. menu, game, HDMI, power-off"
	printf 'key\n' > $(FE_CHECK_DIR)/run/rsos/bootreason
	$(FE_RUN) --script "$(FE_SCRIPT)"
	test -s $(FE_CHECK_DIR)/carousel.png && test -s $(FE_CHECK_DIR)/after-game.png && test -s $(FE_CHECK_DIR)/hdmi.png
	grep -q 'splash: [0-9]* bytes, 640x480, shown' $(FE_LOG)
	grep -q 'game ended: status 0, exit 0' $(FE_LOG)
	grep -q 'Smoke Test' $(FE_CHECK_DIR)/data/rsos/gamedb.tsv
	grep -q 'output hotplug: HDMI' $(FE_LOG)
	grep -q 'power-off requested: exiting' $(FE_LOG)
	grep -q 'exit 0 after' $(FE_LOG)
	grep -Eq '^[0-9]+ 0$$' $(FE_CHECK_DIR)/run/rsos/battery
	grep -q '^=== \[.*\] launch: .*Smoke Test.nes' $(FE_CHECK_DIR)/run/rsos/game.log
	grep -q 'no carousel snapshot' $(FE_LOG)
	test -s $(FE_CHECK_DIR)/data/rsos/cache/systems.idx
	@echo "check-frontend: 1. the watchdog: kept alive (menu and game), magic close at the power-off, never in the game process"
	grep -q 'watchdog: .*/dev/watchdog open' $(FE_LOG)
	test $$(tr -cd k < $(FE_WATCHDOG) | wc -c) -ge 2
	test "$$(tail -c 1 $(FE_WATCHDOG))" = V
	! grep -q 'watchdog' $(FE_GAME_LOG)
	@echo "check-frontend: 1a. snapshot boot: the menu before the lists (loader slowed down), then the same state as a full load"
	rm -f $(FE_LOG)
	$(FE_RUN) --script "wait:300 term"
	grep -q 'carousel updated' $(FE_LOG)
	rm -f $(FE_LOG)
	RSOS_LOADER_DELAY_MS=400 $(FE_RUN) --script "shot:$(FE_CHECK_DIR)/snapshot-menu.png wait:1500 term"
	grep -q 'menu from the snapshot' $(FE_LOG)
	grep -q 'menu up .* (game lists still loading)' $(FE_LOG)
	grep -q 'game lists complete.*carousel unchanged, 0 lists replaced' $(FE_LOG)
	grep -o 'digest [0-9a-f]*' $(FE_LOG) > $(FE_CHECK_DIR)/digest-snapshot
	rm -f $(FE_LOG) $(FE_CHECK_DIR)/data/rsos/cache/systems.idx
	$(FE_RUN) --script "shot:$(FE_CHECK_DIR)/full-load-menu.png wait:300 term"
	grep -q 'no carousel snapshot' $(FE_LOG)
	grep -o 'digest [0-9a-f]*' $(FE_LOG) | cmp - $(FE_CHECK_DIR)/digest-snapshot
	cmp $(FE_CHECK_DIR)/full-load-menu.png $(FE_CHECK_DIR)/snapshot-menu.png
	@echo "check-frontend: 1b. idle screen-off, wake, the next button works; power key short press"
	rm -f $(FE_LOG) $(FE_CHECK_DIR)/idle-*.png
	$(FE_RUN) --script "$(FE_WAKE_SCRIPT)"
	grep -q 'screen off (headless)' $(FE_LOG)
	grep -q 'screen on (headless)' $(FE_LOG)
	cmp $(FE_CHECK_DIR)/idle-before.png $(FE_CHECK_DIR)/idle-woke.png
	! cmp -s $(FE_CHECK_DIR)/idle-before.png $(FE_CHECK_DIR)/idle-next.png
	grep -q 'power key short press: powering off' $(FE_LOG)
	grep -q 'power-off requested: exiting' $(FE_LOG)
	grep -q 'exit 0 after' $(FE_LOG)
	@echo "check-frontend: 2. the game crashes: message, then SIGTERM"
	rm -f $(FE_LOG)
	rm -rf $(FE_CHECK_DIR)/data/states   # step 1's auto state would ask "Resume?" first
	RSOS_TESTCORE=segv $(FE_RUN) --script "a wait:300 a wait:1000 shot:$(FE_CHECK_DIR)/crash.png term"
	grep -q 'crashed' $(FE_LOG)
	grep -q 'signal 15: exiting' $(FE_LOG)
	grep -q 'exit 0 after' $(FE_LOG)
	@echo "check-frontend: 3. charge mode, charger already unplugged: power-off"
	rm -f $(FE_LOG)
	printf 'charger\n' > $(FE_CHECK_DIR)/run/rsos/bootreason
	$(FE_RUN) --script "wait:300 shot:$(FE_CHECK_DIR)/charge.png"
	grep -q 'charge mode' $(FE_LOG)
	! grep -q 'splash: [0-9]* bytes' $(FE_LOG)
	grep -q 'power-off requested: exiting' $(FE_LOG)
	test -s $(FE_CHECK_DIR)/crash.png && test -s $(FE_CHECK_DIR)/charge.png
	@echo "check-frontend: 3a. charge mode, then the power key: the menu, then rsos-boot-ok and the boot notes"
	rm -f $(FE_LOG)
	printf '1\n' > $(FE_CHECK_DIR)/sys/class/power_supply/axp20x-ac/online
	printf 'Charging\n' > $(FE_CHECK_DIR)/sys/class/power_supply/axp20x-battery/status
	printf 'b\n' > $(FE_CHECK_DIR)/run/rsos/boot-fallback
	$(FE_RUN) --script "wait:2200 pek wait:1500 expect:dialog:The_last_update_didn't_start b wait:300 term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	grep -q 'boot reason: charger' $(FE_LOG)
	test $$(grep -n 'charge mode: booting normally' $(FE_LOG) | cut -d: -f1) -lt \
		$$(grep -n 'would run .*rsos-boot-ok' $(FE_LOG) | cut -d: -f1)
	grep -q 'boot note: fallback from slot b' $(FE_LOG)
	rm -f $(FE_CHECK_DIR)/run/rsos/boot-fallback
	printf '0\n' > $(FE_CHECK_DIR)/sys/class/power_supply/axp20x-ac/online
	printf 'Discharging\n' > $(FE_CHECK_DIR)/sys/class/power_supply/axp20x-battery/status
	@echo "check-frontend: 3b. the data partition was checked at boot: a calm toast"
	rm -f $(FE_LOG)
	printf 'key\n' > $(FE_CHECK_DIR)/run/rsos/bootreason
	printf '1\n' > $(FE_CHECK_DIR)/run/rsos/data-fsck
	$(FE_RUN) --script "wait:600 expect:toast=The_SD_card_was_checked term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	rm -f $(FE_CHECK_DIR)/run/rsos/data-fsck
	@echo "check-frontend: 4. a USB stick already plugged at boot: its dialog comes once the menu is up (bug: never shown)"
	rm -f $(FE_LOG)
	printf 'key\n' > $(FE_CHECK_DIR)/run/rsos/bootreason
	sh tests/fake-usb-stick.sh $(FE_CHECK_DIR) sda STICK
	mkdir -p $(FE_CHECK_DIR)/sticks/sda1/roms/nes $(FE_CHECK_DIR)/data/saves/nes
	printf 'NES\032plugged' > "$(FE_CHECK_DIR)/sticks/sda1/roms/nes/Plugged Game.nes"
	printf 'NES\032other!' > "$(FE_CHECK_DIR)/sticks/sda1/roms/nes/Smoke Test.nes"
	printf 'SRAM' > "$(FE_CHECK_DIR)/data/saves/nes/Plugged Game.srm"
	ln -s ../devices/platform/soc/1c14000.usb/usb1/1-1/host0/sda $(FE_CHECK_DIR)/sys/block/sda
	$(FE_RUN) --script "wait:2500 expect:dialog:USB_drive_\"STICK\"_connected shot:$(FE_CHECK_DIR)/usb-boot.png b \
		wait:300 expect:carousel term"
	grep -q 'usb: sda1 (vfat, "STICK", 30.0 GB) mounted read-only' $(FE_LOG)
	grep -q 'usb: STICK (.*/media/usb0): dialog shown' $(FE_LOG)
	grep -q 'usb: dialog answer for .*usb0: nothing' $(FE_LOG)
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	@echo "check-frontend: 4a. hot-plugged after the menu is up: import (a duplicate: replace), export games, back up saves, unplug"
	rm -f $(FE_LOG) $(FE_CHECK_DIR)/sys/block/sda
	$(FE_RUN) --run-ms 45000 --script "wait:1500 plug:sda wait:1500 expect:dialog:USB_drive_\"STICK\" \
		shot:$(FE_CHECK_DIR)/usb-dialog.png expect:|IMPORT_GAMES a wait:2000 expect:menu:Import_from_STICK|Copy \
		a wait:1500 expect:dialog:This_game_is_already right expect:|REPLACE a wait:1500 \
		expect:dialog:Imported_1,_already_there_0,_skipped_0,_replaced_1 right a wait:1000 \
		unplug:sda wait:1500 plug:sda wait:1500 expect:dialog:USB_drive right expect:|EXPORT_GAMES a wait:2000 \
		expect:menu:Export_games_to_STICK|Export a wait:2000 expect:dialog:Copied_2_files right a wait:500 \
		unplug:sda wait:1500 plug:sda wait:1500 down expect:|BACK_UP_SAVES a wait:2000 \
		expect:menu:Back_up_saves_to_STICK|Back_up a wait:2000 expect:dialog:Copied right a wait:500 \
		unplug:sda wait:1500 plug:sda wait:1500 expect:dialog:USB_drive unplug:sda wait:1500 expect:carousel \
		poweroff"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	cmp "$(FE_CHECK_DIR)/sticks/sda1/roms/nes/Plugged Game.nes" "$(FE_CHECK_DIR)/data/roms/nes/Plugged Game.nes"
	cmp "$(FE_CHECK_DIR)/sticks/sda1/roms/nes/Smoke Test.nes" "$(FE_CHECK_DIR)/data/roms/nes/Smoke Test.nes"
	cmp "$(FE_CHECK_DIR)/data/roms/nes/Plugged Game.nes" "$(FE_CHECK_DIR)/sticks/sda1/RetroStone2/roms/nes/Plugged Game.nes"
	cmp "$(FE_CHECK_DIR)/data/saves/nes/Plugged Game.srm" "$(FE_CHECK_DIR)/sticks/sda1/RetroStone2/saves/nes/Plugged Game.srm"
	grep -q '^games_updated=[1-9]' $(FE_CHECK_DIR)/sticks/sda1/RetroStone2/.rsos-backup
	grep -q 'import done: imported 1, already there 0, skipped 0, replaced 1' $(FE_LOG)
	grep -q 'export done: copied 2 files' $(FE_LOG)
	grep -q 'saves backup done: copied [1-9]' $(FE_LOG)
	test $$(grep -c 'remount .*usb0 read-write' $(FE_LOG)) = 2
	test $$(grep -c 'remount .*usb0 read-only' $(FE_LOG)) = 2
	grep -q 'usb: STICK (.*usb0) removed' $(FE_LOG)
	test -s $(FE_CHECK_DIR)/run/rsos/shutdown-start && grep -q '^poweroff$$' $(FE_CHECK_DIR)/run/rsos/shutdown-mode
	grep -q 'exit 0 after .*state flushed' $(FE_LOG)
	@echo "check-frontend: 5. power-off during a game: the auto state and resume.ini are written"
	rm -f $(FE_LOG) $(FE_GAME_LOG) $(FE_RESUME) "$(FE_CHECK_DIR)/data/roms/nes/Plugged Game.nes"
	rm -rf $(FE_CHECK_DIR)/data/states
	RSOS_TESTCORE=slow $(FE_RUN) --script "wait:300 a wait:500 a game:wait:700 game:poweroff"
	grep -q 'auto state written at exit' $(FE_LOG)
	grep -q 'resume: recorded Smoke Test' $(FE_LOG)
	grep -q 'auto state saved (power-off)' $(FE_GAME_LOG)
	@echo "check-frontend: 5 (fast shutdown): the child exits right after its saves, the menu does not take the display back, init is signalled after resume.ini, then the exit"
	grep -q 'power-off: saves on the card, exiting without the teardown' $(FE_GAME_LOG)
	grep -q 'game ended for a power-off: the display is not taken back' $(FE_LOG)
	test -s $(FE_CHECK_DIR)/run/rsos/shutdown-signal
	test $$(grep -n 'resume: recorded' $(FE_LOG) | cut -d: -f1) -lt $$(grep -n 'powering off (' $(FE_LOG) | cut -d: -f1)
	test $$(grep -n 'powering off (' $(FE_LOG) | cut -d: -f1) -lt \
		$$(grep -n 'exit 0 after .*state flushed' $(FE_LOG) | cut -d: -f1)
	! grep -q 'frame: no buffer' $(FE_LOG)
	grep -q '^rom = .*/data/roms/nes/Smoke Test.nes$$' $(FE_RESUME)
	grep -q '^reason = user$$' $(FE_RESUME) && grep -q '^core = fceumm$$' $(FE_RESUME)
	test -s $(FE_STATE) && test -s $(FE_STATE).png
	grep -q 'power-off requested: exiting' $(FE_LOG)
	cp $(FE_RESUME) $(FE_CHECK_DIR)/resume.saved
	@echo "check-frontend: 5a. next boot: \"Resume Smoke Test?\" -> Resume launches with --load-state auto"
	rm -f $(FE_LOG) $(FE_GAME_LOG)
	$(FE_RUN) --script "wait:400 expect:dialog:Resume_Smoke_Test?|RESUME shot:$(FE_CHECK_DIR)/resume-boot.png \
		a wait:1500 expect:carousel term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	grep -q 'resume: offering Smoke Test' $(FE_LOG)
	grep -q 'launch Smoke Test .*, resuming (boot offer)' $(FE_LOG)
	grep -q 'load state .*Smoke Test.state.auto (.*): ok' $(FE_GAME_LOG)
	test ! -e $(FE_RESUME)
	@echo "check-frontend: 5b. Start fresh (and B): the menu, the auto state kept, the offer gone"
	cp $(FE_CHECK_DIR)/resume.saved $(FE_RESUME)
	rm -f $(FE_LOG) $(FE_GAME_LOG)
	$(FE_RUN) --script "wait:400 expect:dialog:Resume_Smoke_Test? right expect:|START_FRESH a wait:300 \
		expect:carousel term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	grep -q 'resume: start fresh (auto state kept)' $(FE_LOG)
	! grep -q 'launch Smoke Test' $(FE_LOG)
	test -s $(FE_STATE) && test ! -e $(FE_RESUME)
	cp $(FE_CHECK_DIR)/resume.saved $(FE_RESUME)
	rm -f $(FE_LOG)
	$(FE_RUN) --script "wait:400 expect:dialog:Resume_Smoke_Test? b wait:300 expect:carousel term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	test -s $(FE_STATE) && test ! -e $(FE_RESUME)
	@echo "check-frontend: 5c. the ROM is gone: no dialog, the file dropped"
	sed 's#^rom = .*#rom = /nonexistent/Gone.nes#' $(FE_CHECK_DIR)/resume.saved > $(FE_RESUME)
	rm -f $(FE_LOG)
	$(FE_RUN) --script "wait:400 expect:carousel term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	grep -q 'resume: Smoke Test (/nonexistent/Gone.nes): the game file is gone, offer dropped' $(FE_LOG)
	! grep -q 'resume: offering' $(FE_LOG)
	test ! -e $(FE_RESUME)
	@echo "check-frontend: 5d. the game's own prompt, resume_mode ask / always / never"
	rm -f $(FE_LOG) $(FE_GAME_LOG)
	$(FE_RUN) --script "wait:300 a wait:500 a wait:300 expect:dialog:Resume_where_you_left_off?|RESUME \
		shot:$(FE_CHECK_DIR)/resume-prompt.png b wait:300 expect:list term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	! grep -q 'launch Smoke Test' $(FE_LOG)
	sed -i '/^resume_mode/d;/^autosave_exit/d' $(FE_SETTINGS) && printf 'resume_mode = always\n' >> $(FE_SETTINGS)
	rm -f $(FE_LOG) $(FE_GAME_LOG)
	$(FE_RUN) --script "wait:300 a wait:500 a wait:1500 expect:list term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	grep -q 'resume_mode always: resuming' $(FE_LOG) && grep -q 'launch Smoke Test .*, resuming$$' $(FE_LOG)
	grep -q 'load state .*Smoke Test.state.auto (.*): ok' $(FE_GAME_LOG)
	sed -i '/^resume_mode/d' $(FE_SETTINGS) && printf 'resume_mode = never\n' >> $(FE_SETTINGS)
	rm -f $(FE_LOG) $(FE_GAME_LOG)
	$(FE_RUN) --script "wait:300 a wait:500 a wait:1500 expect:list term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	grep -q 'resume_mode never: "Smoke Test" starts fresh' $(FE_LOG)
	! grep -q 'load state' $(FE_GAME_LOG)
	@echo "check-frontend: 5e. Settings > Games > Auto-save on exit: off = no auto state at exit, on = written"
	rm -f $(FE_LOG) $(FE_GAME_LOG) $(FE_STATE)
	$(FE_RUN) --script "wait:300 start down down down down down expect:menu:Settings|Games a wait:200 \
		expect:menu:Games|Auto-save_on_exit shot:$(FE_CHECK_DIR)/settings-games.png a wait:200 b b \
		wait:200 a wait:500 a wait:1500 expect:list term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	grep -q '^autosave_exit *= *0$$' $(FE_SETTINGS)
	grep -q 'auto state not written (Settings > Games > Auto-save on exit is off)' $(FE_GAME_LOG)
	test ! -e $(FE_STATE)
	sed -i '/^autosave_exit/d' $(FE_SETTINGS) && printf 'autosave_exit = 1\n' >> $(FE_SETTINGS)
	rm -f $(FE_LOG) $(FE_GAME_LOG)
	$(FE_RUN) --script "wait:300 a wait:500 a wait:1500 expect:list term"
	grep -q 'auto state written at exit' $(FE_LOG) && grep -q 'auto state saved (exit)' $(FE_GAME_LOG)
	test -s $(FE_STATE)
	@# the test game runs 120 frames of 20 ms: the power-off (1 s after the start) comes before its end
	@echo "check-frontend: 5f. idle power-off during a game: the notice in the game, then the auto state and resume.ini (reason idle) before init is signalled"
	rm -f $(FE_LOG) $(FE_GAME_LOG) $(FE_RESUME) $(FE_STATE) $(FE_CHECK_DIR)/run/rsos/shutdown-signal
	RSOS_TESTCORE=slow $(FE_RUN) --script "wait:300 a wait:500 a game:wait:100 game:idlepoweroff:1"
	grep -q 'idle power-off: notice (10 s)' $(FE_LOG)
	grep -q 'toast: Powering off in 10 s' $(FE_GAME_LOG)
	grep -q 'auto state saved (power-off)' $(FE_GAME_LOG)
	grep -q 'resume: recorded Smoke Test' $(FE_LOG) && grep -q '^reason = idle$$' $(FE_RESUME)
	test $$(grep -n 'resume: recorded' $(FE_LOG) | cut -d: -f1) -lt $$(grep -n 'powering off (idle)' $(FE_LOG) | cut -d: -f1)
	test -s $(FE_STATE) && test -s $(FE_CHECK_DIR)/run/rsos/shutdown-signal
	grep -q 'power-off requested: exiting' $(FE_LOG)
	rm -f $(FE_RESUME)
	@echo "check-frontend: 5g. idle power-off in the menu: the 10 s notice, then the power-off"
	rm -f $(FE_LOG) $(FE_GAME_LOG)
	$(FE_RUN) --script "wait:600 idlepoweroff:3 wait:6000"
	grep -q 'ui: toast (warning): Powering off in 10 s' $(FE_LOG)
	grep -q 'powering off (idle)' $(FE_LOG) && grep -q 'power-off requested: exiting' $(FE_LOG)
	@echo "check-frontend: 6. LCD refresh 60 Hz: applied live, kept, the game child runs with it; the next boot too"
	rm -f $(FE_LOG) $(FE_GAME_LOG)
	$(FE_RUN) --script "wait:300 start a down expect:menu:Display|LCD_refresh_rate right wait:500 \
		expect:dialog:The_screen_now_refreshes_at_60_Hz. left a wait:300 b b wait:300 a wait:500 a wait:1500 \
		expect:list term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	grep -q 'setting lcd_refresh = 60' $(FE_LOG) && grep -q 'lcd refresh: 60 Hz (headless)' $(FE_LOG)
	grep -q 'output reprobe: Unknown-1(fake) 640x480@60.000 Hz' $(FE_LOG)
	grep -q '^lcd_refresh *= *60$$' $(FE_SETTINGS)
	grep -q -- '--lcd-refresh 60\|lcd refresh: 60 Hz (25.2 MHz user mode)' $(FE_GAME_LOG)
	rm -f $(FE_LOG) $(FE_GAME_LOG)
	$(FE_RUN) --script "wait:300 term"
	grep -q 'output init: Unknown-1(fake) 640x480@60.000 Hz' $(FE_LOG)
	sed -i '/^lcd_refresh/d' $(FE_SETTINGS)
	@echo "check-frontend: 7. board profile: without board.ini (generic) and with a board without a built-in screen"
	rm -f $(FE_LOG) $(FE_CHECK_DIR)/etc/rsos/board.ini
	$(FE_RUN) --script "wait:300 start a expect:menu:Display|Brightness down expect:menu:Display|HDMI_resolution term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	grep -q 'board Generic (.*/etc/rsos/board.ini: not found, generic defaults)' $(FE_LOG)
	printf 'name = Test HDMI box\ninternal_display = none\nbattery_supply = none\n' > $(FE_CHECK_DIR)/etc/rsos/board.ini
	rm -f $(FE_LOG) $(FE_GAME_LOG)
	$(FE_RUN) --script "wait:300 start a expect:menu:Display|HDMI_resolution b b wait:300 a wait:500 a wait:1500 \
		expect:list term"
	! grep -q 'EXPECT FAILED' $(FE_LOG)
	grep -q 'board Test HDMI box (' $(FE_LOG) && grep -q 'board Test HDMI box (' $(FE_GAME_LOG)
	cp $(BOARD_INI_RS2) $(FE_CHECK_DIR)/etc/rsos/board.ini
	@echo "check-frontend: OK"

# The whole `check` under AddressSanitizer + UndefinedBehaviorSanitizer (a
# host build in its own directory). Any report fails the run: UBSan does
# not recover, ASan halts, LeakSanitizer is on. handle_segv=0 & co: the
# process-model tests crash the test core on purpose and expect a real
# SIGSEGV, not ASan's report and exit(1).
ASAN_BUILDDIR ?= $(BUILDDIR)-asan
ASAN_CFLAGS   ?= -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
ASAN_LDFLAGS  ?= -fsanitize=address,undefined
# Reports also go to files (log_path): one from a process whose exit status
# no check looks at (a game child, a helper) still fails the run.
ASAN_REPORTS   = $(abspath $(ASAN_BUILDDIR))/sanitizer-report
ASAN_ENV       = ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1:handle_segv=0:handle_sigbus=0:handle_sigfpe=0:handle_sigill=0:log_path=$(ASAN_REPORTS) \
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:log_path=$(ASAN_REPORTS)
check-asan:
	@mkdir -p $(ASAN_BUILDDIR) && rm -f $(ASAN_REPORTS)*
	$(ASAN_ENV) $(MAKE) BUILDDIR=$(ASAN_BUILDDIR) CFLAGS="$(ASAN_CFLAGS)" LDFLAGS="$(ASAN_LDFLAGS)" check || \
		{ cat $(ASAN_REPORTS)* 2>/dev/null; exit 1; }
	@if ls $(ASAN_REPORTS)* >/dev/null 2>&1; then cat $(ASAN_REPORTS)*; echo "check-asan: sanitizer reports"; exit 1; fi
	@echo "check-asan: OK (ASan + UBSan + LeakSanitizer, every check, no report)"

check-syntax:
	@for f in $(LIB_SRCS) src/tools/kmstest.c tests/display_selftest.c; do \
		echo "  CHECK $$f"; \
		$(CC) $(CPPFLAGS) $(RSOS_CFLAGS) $(CFLAGS) -fsyntax-only $$f || exit 1; \
	done

# ----------------------------------------------------------------- install
REMAPS   = $(wildcard src/input/remaps/*.ini)
FONTS    = $(wildcard third_party/fonts/*.ttf) $(wildcard third_party/fonts/*.otf) $(wildcard third_party/fonts/LICENSE*)
WEB      = $(wildcard src/transfer/web/*)
COREOPTS = $(wildcard src/host/coreopts/*.ini)

install: $(BINS) update-install
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(DATADIR)/fonts $(DESTDIR)$(DATADIR)/themes \
		$(DESTDIR)$(DATADIR)/remaps $(DESTDIR)$(DATADIR)/coreopts $(DESTDIR)$(DATADIR)/web \
		$(DESTDIR)$(SYSCONFDIR) $(DESTDIR)$(DATADIR)/locale
	install -m 0755 $(FRONTEND) $(DESTDIR)$(BINDIR)/rsos-frontend
	install -m 0755 $(KMSTEST) $(DESTDIR)$(BINDIR)/rsos-kmstest
	install -m 0755 $(SELFTEST) $(DESTDIR)$(BINDIR)/rsos-display-selftest
	install -m 0755 $(RSOS_RUN) $(DESTDIR)$(BINDIR)/rsos-run
	install -m 0755 $(POWER_BUILD)/rsos-bootreason $(DESTDIR)$(BINDIR)/rsos-bootreason
	install -m 0755 $(POWER_BUILD)/rsos-clock $(DESTDIR)$(BINDIR)/rsos-clock
	install -m 0755 src/transfer/rsos-smb $(DESTDIR)$(BINDIR)/rsos-smb
	install -m 0644 $(FONTS) $(DESTDIR)$(DATADIR)/fonts/
	cp -R themes/. $(DESTDIR)$(DATADIR)/themes/
	chmod -R u+rwX,go+rX,go-w $(DESTDIR)$(DATADIR)/themes
	$(if $(REMAPS),install -m 0644 $(REMAPS) $(DESTDIR)$(DATADIR)/remaps/)
	$(if $(COREOPTS),install -m 0644 $(COREOPTS) $(DESTDIR)$(DATADIR)/coreopts/)
	$(if $(WEB),install -m 0644 $(WEB) $(DESTDIR)$(DATADIR)/web/)
	install -m 0644 third_party/sdl-gamecontrollerdb/gamecontrollerdb.txt $(DESTDIR)$(DATADIR)/gamecontrollerdb.txt
	install -m 0644 src/transfer/data-README.txt $(DESTDIR)$(DATADIR)/data-README.txt
	install -m 0644 $(SPLASH) $(DESTDIR)$(DATADIR)/splash.rle
	$(if $(CATALOGS),install -m 0644 $(CATALOGS) $(DESTDIR)$(DATADIR)/locale/)
	printf '%s\n' "$(RSOS_VERSION_STRING)" > $(DESTDIR)$(SYSCONFDIR)/rsos-version
	chmod 0644 $(DESTDIR)$(SYSCONFDIR)/rsos-version

clean:
	rm -rf $(BUILDDIR)

.PHONY: all frontend-all install clean check check-display check-host check-power check-transfer \
	check-ui check-ui-assets check-loader check-splash check-board check-frontend check-syntax check-asan uipreview \
	splash-previews FORCE catalogs pot po-update po-stats check-i18n cjk-fonts check-robust

# the batch 2 checks (check-b2), after every variable they use
include batch2.mk

-include $(LIB_OBJS:.o=.d) $(KMSTEST_OBJS:.o=.d) $(SELFTEST_OBJS:.o=.d) $(KMS_TEST_OBJS:.o=.d) $(BUILDDIR)/main.d \
	$(BUILDDIR)/tests/test_loader.d $(BUILDDIR)/tests/test_board.d $(BUILDDIR)/tests/test_i18n.d \
	$(BUILDDIR)/tests/test_robust.d
