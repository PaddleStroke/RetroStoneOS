# RetroStoneOS libretro host: make fragment.
#
# Included by frontend/Makefile (`include host.mk`), or used standalone:
#     make -f host.mk                 rsos-run + rsos-host-test
#     make -f host.mk host-check      run the unit tests (resampler, DRC, options, saves)
#     make -f host.mk host-check-syntax   compile-only (cross compiler, no target libs)
#     make -f host.mk host-install DESTDIR=...
#
# Provides:
#   HOST_OBJS      the host (src/host, src/audio) + miniz, for the main binary
#   HOST_LIBS      what they link against (libdrm is the display layer's)
#   RSOS_RUN       $(BUILDDIR)/rsos-run
#   host-all host-check host-check-syntax host-install host-clean
#
# The main binary gets the `--run` child mode by calling host_main() (see
# docs/host-design.md, "Integration"). The input backend is the real input
# layer when src/input/input.c exists, otherwise a temporary evdev reader.

HOST_MK_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

# ---------------------------------------------------------------- standalone
ifeq ($(origin LIB_SRCS),undefined)
HOST_STANDALONE := 1
CC         ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS     ?= -O2
PREFIX     ?= /usr
BINDIR     ?= $(PREFIX)/bin
BUILDDIR   ?= build
DRM_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libdrm 2>/dev/null || echo -I$(SYSROOT)/usr/include/libdrm)
DRM_LIBS   ?= $(shell $(PKG_CONFIG) --libs libdrm 2>/dev/null || echo -ldrm)
RSOS_CFLAGS = -std=c11 -Wall -Wextra -Wformat -Wformat-security -Werror=format-security -D_GNU_SOURCE -Isrc \
	$(DRM_CFLAGS)
HOST_DISPLAY_SRCS = src/display.c src/uevent.c src/font8x8.c
HOST_DISPLAY_OBJS = $(HOST_DISPLAY_SRCS:src/%.c=$(BUILDDIR)/%.o)

host-default: host-all

$(BUILDDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(RSOS_CFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<
else
HOST_DISPLAY_OBJS = $(LIB)
endif

ALSA_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags alsa 2>/dev/null)
ALSA_LIBS   ?= $(shell $(PKG_CONFIG) --libs alsa 2>/dev/null || echo -lasound)

# ------------------------------------------------------------------ sources
HOST_INPUT_BACKEND ?= $(if $(wildcard src/input/input.c),rsos,evdev)
# The translations and the TrueType text (in-game menu, toasts), with either
# input backend.
HOST_TEXT_SRCS = src/ui/util.c src/i18n/i18n.c src/gfx/font.c src/gfx/gfx.c src/gfx/third_party_impl.c
ifeq ($(HOST_INPUT_BACKEND),rsos)
HOST_INPUT_SRC = src/host/host_input_rsos.c
# The input layer's own objects. When included by the main Makefile they are
# part of the frontend already: set HOST_INPUT_EXTRA_OBJS to them there.
HOST_INPUT_EXTRA_SRCS ?= $(wildcard src/input/*.c) $(HOST_TEXT_SRCS)
else
HOST_INPUT_SRC = src/host/host_input_evdev.c
HOST_INPUT_EXTRA_SRCS ?= $(HOST_TEXT_SRCS)
endif
HOST_INPUT_EXTRA_OBJS ?= $(HOST_INPUT_EXTRA_SRCS:src/%.c=$(BUILDDIR)/%.o)
# launch.c (UI side) translates its messages: the i18n object of the build.
HOST_I18N_OBJ ?= $(firstword $(filter %/i18n/i18n.o,$(HOST_INPUT_EXTRA_OBJS)) $(BUILDDIR)/i18n/i18n.o)

HOST_SRCS = \
	src/host/host.c src/host/core.c src/host/cli.c src/host/launch.c \
	src/host/options.c src/host/saves.c src/host/content.c src/host/coreinfo.c \
	src/host/pacing.c src/host/menu.c src/host/osd.c src/host/draw.c \
	src/host/hwrender.c src/host/sys.c src/host/ini.c src/host/md5.c \
	src/host/hutil.c src/host/host_png.c src/host/batt_overlay.c src/host/perf.c \
	src/host/bench.c src/host/bench_run.c src/host/glprobe.c src/host/playtime.c \
	src/host/switcher.c $(HOST_INPUT_SRC) \
	src/audio/audio.c src/audio/resampler.c src/board.c
HOST_OBJS = $(HOST_SRCS:src/%.c=$(BUILDDIR)/%.o) $(BUILDDIR)/third_party/miniz.o
HOST_LIBS = $(ALSA_LIBS) -lpthread -ldl -lm
# The GL probe (src/host/glprobe.c, docs/host-design.md §13.4): the game
# process exports the probed GL entry points, so a core linked against
# libGLESv2 (parallel-n64) calls the timed wrappers first. Link the frontend
# and rsos-run with it.
HOST_EXPORT_LDFLAGS = -Wl,--dynamic-list=$(HOST_MK_DIR)/src/host/glprobe.syms

RSOS_RUN       = $(BUILDDIR)/rsos-run
HOST_TEST      = $(BUILDDIR)/rsos-host-test
HOST_TEST_OBJS = $(BUILDDIR)/host/tests/test_host.o $(BUILDDIR)/host/pacing.o \
	$(BUILDDIR)/audio/resampler.o $(BUILDDIR)/host/options.o $(BUILDDIR)/host/ini.o \
	$(BUILDDIR)/host/hutil.o $(BUILDDIR)/host/md5.o $(BUILDDIR)/host/coreinfo.o \
	$(BUILDDIR)/host/batt_overlay.o $(BUILDDIR)/font8x8.o $(BUILDDIR)/third_party/miniz.o \
	$(BUILDDIR)/host/perf.o $(BUILDDIR)/host/bench.o $(BUILDDIR)/host/playtime.o \
	$(BUILDDIR)/host/draw.o
# The player-assignment tests use the input layer's pure functions; the BIOS
# check (coreinfo.o) translates its messages.
HOST_TEST_OBJS += $(HOST_INPUT_EXTRA_OBJS)
ifeq ($(HOST_INPUT_BACKEND),rsos)
$(BUILDDIR)/host/tests/test_host.o: RSOS_CFLAGS += -DHOST_TEST_INPUT_PORTS
endif

# ALSA headers only where needed; miniz is third-party code (no warnings).
$(BUILDDIR)/audio/audio.o: RSOS_CFLAGS += $(ALSA_CFLAGS)
$(BUILDDIR)/host/osd.o: RSOS_CFLAGS += $(ALSA_CFLAGS)
$(BUILDDIR)/host/core.o: RSOS_CFLAGS += $(ALSA_CFLAGS)
$(BUILDDIR)/host/host.o: RSOS_CFLAGS += $(ALSA_CFLAGS)
$(BUILDDIR)/host/menu.o: RSOS_CFLAGS += $(ALSA_CFLAGS)
$(BUILDDIR)/host/switcher.o: RSOS_CFLAGS += $(ALSA_CFLAGS)

$(BUILDDIR)/third_party/miniz.o: third_party/miniz/miniz.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -w -D_GNU_SOURCE -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES \
		-c -o $@ $<

$(RSOS_RUN): $(BUILDDIR)/tools/run.o $(BUILDDIR)/splash.o $(HOST_OBJS) $(HOST_INPUT_EXTRA_OBJS) $(HOST_DISPLAY_OBJS)
	$(CC) $(LDFLAGS) $(HOST_EXPORT_LDFLAGS) -o $@ $^ $(DRM_LIBS) $(HOST_LIBS)

$(HOST_TEST): $(HOST_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm -lpthread

# Process-model test: a tiny libretro core that can crash or hang, and a
# driver that runs it through host_launch() + rsos-run --headless.
HOST_TESTCORE     = $(BUILDDIR)/rsos-testcore_libretro.so
HOST_LAUNCH_TEST  = $(BUILDDIR)/rsos-launch-test
HOST_LAUNCH_OBJS  = $(BUILDDIR)/host/tests/test_launch.o $(BUILDDIR)/host/launch.o \
	$(BUILDDIR)/host/sys.o $(BUILDDIR)/host/hutil.o $(BUILDDIR)/board.o $(HOST_I18N_OBJ) \
	$(BUILDDIR)/host/host_png.o

$(HOST_TESTCORE): src/host/tests/testcore.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -D_GNU_SOURCE -fPIC -shared $(LDFLAGS) -o $@ $< -lm

$(HOST_LAUNCH_TEST): $(HOST_LAUNCH_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lpthread

# GL probe test: a fake libGLESv2.so.2, a "core" linked against it, and a
# driver linked like the frontend (exported wrappers).
HOST_FAKEGL       = $(BUILDDIR)/fakegl/libGLESv2.so.2
HOST_GLCORE       = $(BUILDDIR)/fakegl/libglcore.so
HOST_GLPROBE_TEST = $(BUILDDIR)/rsos-glprobe-test

$(HOST_FAKEGL): src/host/tests/fakegles.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -D_GNU_SOURCE -fPIC -shared $(LDFLAGS) \
		-Wl,-soname,libGLESv2.so.2 -o $@ $<

$(HOST_GLCORE): src/host/tests/glcore.c $(HOST_FAKEGL)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -fPIC -shared $(LDFLAGS) -o $@ $< $(HOST_FAKEGL) \
		-Wl,-rpath,'$$ORIGIN'

$(HOST_GLPROBE_TEST): $(BUILDDIR)/host/tests/test_glprobe.o $(BUILDDIR)/host/glprobe.o $(HOST_GLCORE)
	$(CC) $(LDFLAGS) $(HOST_EXPORT_LDFLAGS) -o $@ $(BUILDDIR)/host/tests/test_glprobe.o \
		$(BUILDDIR)/host/glprobe.o -ldl

host-all: $(RSOS_RUN) $(HOST_TEST) $(HOST_TESTCORE) $(HOST_LAUNCH_TEST) $(HOST_GLPROBE_TEST)

# Objects only (cross-compile check without the target libraries).
host-objs: $(HOST_OBJS) $(BUILDDIR)/tools/run.o

host-check: host-all
	$(HOST_TEST)
	$(HOST_GLPROBE_TEST) $(HOST_GLCORE)
	rm -rf $(BUILDDIR)/launch-test && $(HOST_LAUNCH_TEST) $(RSOS_RUN) $(HOST_TESTCORE) $(BUILDDIR)/launch-test

# Compile-only (e.g. arm-linux-gnueabihf-gcc without target libdrm/alsa):
# pass the host's header dirs with CPPFLAGS if needed.
host-check-syntax:
	@for f in $(HOST_SRCS) src/tools/run.c src/host/tests/test_host.c; do \
		echo "  CHECK $$f"; \
		$(CC) $(CPPFLAGS) $(RSOS_CFLAGS) $(ALSA_CFLAGS) $(CFLAGS) -fsyntax-only $$f || exit 1; \
	done

HOST_COREOPTS = $(wildcard src/host/coreopts/*.ini)

host-install: $(RSOS_RUN)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)/usr/share/rsos/coreopts
	install -m 0755 $(RSOS_RUN) $(DESTDIR)$(BINDIR)/rsos-run
	$(if $(HOST_COREOPTS),install -m 0644 $(HOST_COREOPTS) $(DESTDIR)/usr/share/rsos/coreopts/)

host-clean:
	rm -rf $(BUILDDIR)/host $(BUILDDIR)/audio $(BUILDDIR)/third_party/miniz.o \
		$(BUILDDIR)/tools/run.o $(RSOS_RUN) $(HOST_TEST) $(HOST_TESTCORE) $(HOST_LAUNCH_TEST)

.PHONY: host-default host-all host-objs host-check host-check-syntax host-install host-clean

-include $(HOST_OBJS:.o=.d) $(HOST_TEST_OBJS:.o=.d) $(HOST_LAUNCH_OBJS:.o=.d) $(BUILDDIR)/tools/run.d
ifdef HOST_STANDALONE
-include $(HOST_DISPLAY_OBJS:.o=.d)
endif
