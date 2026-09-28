# frontend/ui.mk - the RetroStoneOS UI: renderer (src/gfx), ES theme engine
# (src/theme), screens (src/ui), input layer (src/input) and the host
# preview tool rsos-uipreview (src/tools/uipreview.c).
#
# Standalone:
#   make -f ui.mk                         # host build of rsos-uipreview
#   make -f ui.mk UI_BUILD=~/rsos/ui-build
#   make -f ui.mk ui-arm-check            # cross-compile + link with arm-linux-gnueabihf-gcc
#
# From the main Makefile:
#   include ui.mk
#   frontend objects += $(UI_OBJS); CFLAGS += $(UI_INCLUDES); LDLIBS += $(UI_LDLIBS)
# UI_OBJS contains everything except the preview tool. No libdrm needed.

UI_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
UI_BUILD ?= $(UI_DIR)/build-ui

UI_SRCS := \
	$(UI_DIR)/src/ui/util.c \
	$(UI_DIR)/src/ui/xml.c \
	$(UI_DIR)/src/ui/settings.c \
	$(UI_DIR)/src/ui/systems.c \
	$(UI_DIR)/src/ui/games.c \
	$(UI_DIR)/src/ui/fswarm.c \
	$(UI_DIR)/src/ui/loader.c \
	$(UI_DIR)/src/ui/prefetch.c \
	$(UI_DIR)/src/ui/hw.c \
	$(UI_DIR)/src/ui/widgets.c \
	$(UI_DIR)/src/ui/view_system.c \
	$(UI_DIR)/src/ui/view_gamelist.c \
	$(UI_DIR)/src/ui/menu.c \
	$(UI_DIR)/src/ui/screens.c \
	$(UI_DIR)/src/ui/transfer_ui.c \
	$(UI_DIR)/src/ui/update_ui.c \
	$(UI_DIR)/src/ui/ui.c \
	$(UI_DIR)/src/gfx/gfx.c \
	$(UI_DIR)/src/gfx/font.c \
	$(UI_DIR)/src/gfx/image.c \
	$(UI_DIR)/src/gfx/third_party_impl.c \
	$(UI_DIR)/src/theme/theme.c \
	$(UI_DIR)/src/input/input.c \
	$(UI_DIR)/src/i18n/i18n.c

# qr.c (transfer module, a pure function) draws the web share QR code in the preview
UI_TOOL_SRCS := $(UI_DIR)/src/tools/uipreview.c $(UI_DIR)/src/transfer/qr.c

UI_INCLUDES := -I$(UI_DIR)/src
UI_WARN := -Wall -Wextra -Wformat -Wformat-security -Werror=format-security
# Link flags: $(LDFLAGS) for HOST_CC, $(ARM_LDFLAGS) for the cross check.
ARM_LDFLAGS ?=
# -ftree-vectorize: the blend/fill loops in gfx.c are written for GCC's
# vectorizer (NEON on the A20).
UI_OPT := -O2 -ftree-vectorize
UI_CFLAGS ?= -std=gnu11 $(UI_WARN) $(UI_OPT) -g -D_GNU_SOURCE $(UI_INCLUDES)
UI_LDLIBS := -lm -lpthread

HOST_CC ?= gcc
ARM_CC ?= arm-linux-gnueabihf-gcc
ARM_FLAGS ?= -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard

UI_OBJS := $(patsubst $(UI_DIR)/src/%.c,$(UI_BUILD)/host/%.o,$(UI_SRCS))
UI_TOOL_OBJS := $(patsubst $(UI_DIR)/src/%.c,$(UI_BUILD)/host/%.o,$(UI_TOOL_SRCS))
UI_ARM_OBJS := $(patsubst $(UI_DIR)/src/%.c,$(UI_BUILD)/arm/%.o,$(UI_SRCS) $(UI_TOOL_SRCS))

.PHONY: ui-all ui-clean ui-arm-check ui-asan rsos-uipreview

ui-all: rsos-uipreview

rsos-uipreview: $(UI_BUILD)/rsos-uipreview

$(UI_BUILD)/rsos-uipreview: $(UI_OBJS) $(UI_TOOL_OBJS)
	$(HOST_CC) $(LDFLAGS) -o $@ $^ $(UI_LDLIBS)

$(UI_BUILD)/host/%.o: $(UI_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(UI_CFLAGS) -MMD -MP -c -o $@ $<

# Cross check: every UI object plus the preview tool, compiled and linked
# for the A20 (proves the code builds and links for the target).
ui-arm-check: $(UI_BUILD)/arm/rsos-uipreview

$(UI_BUILD)/arm/rsos-uipreview: $(UI_ARM_OBJS)
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) -o $@ $^ $(UI_LDLIBS)

$(UI_BUILD)/arm/%.o: $(UI_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_FLAGS) $(UI_CFLAGS) -MMD -MP -c -o $@ $<

# Host build with AddressSanitizer + UBSan (for testing the preview scripts).
UI_ASAN_OBJS := $(patsubst $(UI_DIR)/src/%.c,$(UI_BUILD)/asan/%.o,$(UI_SRCS) $(UI_TOOL_SRCS))
ui-asan: $(UI_BUILD)/asan/rsos-uipreview

$(UI_BUILD)/asan/rsos-uipreview: $(UI_ASAN_OBJS)
	$(HOST_CC) $(LDFLAGS) -fsanitize=address,undefined -o $@ $^ $(UI_LDLIBS)

$(UI_BUILD)/asan/%.o: $(UI_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(UI_CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -MMD -MP -c -o $@ $<

ui-clean:
	rm -rf $(UI_BUILD)

-include $(UI_OBJS:.o=.d) $(UI_TOOL_OBJS:.o=.d) $(UI_ARM_OBJS:.o=.d) $(UI_ASAN_OBJS:.o=.d)
