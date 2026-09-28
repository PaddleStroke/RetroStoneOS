# frontend/power.mk - RetroStoneOS power management (src/power): the module
# the frontend links (battery, warnings, critical save + power-off, power
# key, fake sleep, idle dimming, governors, thermal, charge mode, clock),
# the init helpers rsos-bootreason and rsos-clock, and the unit tests.
# Design: docs/power.md.
#
# Standalone:
#   make -f power.mk                       # host build: tools + unit tests
#   make -f power.mk power-check           # build and run the unit tests
#   make -f power.mk power-arm-check       # cross-compile + link for the A20
#   make -f power.mk POWER_BUILD=~/rsos/power-build power-check
#
# From the main Makefile:
#   include power.mk
#   frontend objects += $(POWER_OBJS); CFLAGS += $(POWER_INCLUDES);
#   LDLIBS += $(POWER_LDLIBS); install $(POWER_TOOLS) as /usr/bin/rsos-bootreason
#   and /usr/bin/rsos-clock (target: power-install DESTDIR=...).
#   For a target build set HOST_CC to the target compiler (Buildroot:
#   HOST_CC="$(TARGET_CC)"): the "host" objects are simply "this compiler".
# POWER_OBJS does not contain src/uevent.c, which power.c uses: the frontend
# already links it (librsos-display.a). The standalone targets add it.
# Only pclock.c gets $(POWER_DEFS) (the build time), so objects never disagree.

POWER_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
POWER_BUILD ?= $(POWER_DIR)/build-power

POWER_SRCS := \
	$(POWER_DIR)/src/power/power.c \
	$(POWER_DIR)/src/power/battery.c \
	$(POWER_DIR)/src/power/pclock.c \
	$(POWER_DIR)/src/power/bootreason.c \
	$(POWER_DIR)/src/power/psys.c
POWER_EXTRA_SRCS := $(POWER_DIR)/src/uevent.c
POWER_BOOTREASON_SRCS := $(POWER_DIR)/src/power/rsos-bootreason.c \
	$(POWER_DIR)/src/power/bootreason.c $(POWER_DIR)/src/power/psys.c
POWER_CLOCK_SRCS := $(POWER_DIR)/src/power/rsos-clock.c \
	$(POWER_DIR)/src/power/pclock.c $(POWER_DIR)/src/power/psys.c
POWER_TEST_SRCS := $(POWER_DIR)/src/power/tests/power_test.c

# The clock can never be earlier than the build (rsos-clock restore floor).
# Reproducible builds set SOURCE_DATE_EPOCH.
POWER_BUILD_EPOCH ?= $(or $(SOURCE_DATE_EPOCH),$(shell date +%s))
POWER_DEFS := -DRSOS_BUILD_EPOCH=$(POWER_BUILD_EPOCH)L
POWER_INCLUDES := -I$(POWER_DIR)/src
POWER_CFLAGS ?= -std=gnu11 -Wall -Wextra -Wformat -Wformat-security -Werror=format-security -O2 -g \
	-D_GNU_SOURCE $(POWER_INCLUDES)
# Link flags: $(LDFLAGS) for this compiler (HOST_CC), $(ARM_LDFLAGS) for the
# cross check (the caller's LDFLAGS are for the other compiler).
ARM_LDFLAGS ?=
POWER_LDLIBS := -lm

HOST_CC ?= gcc
ARM_CC ?= arm-linux-gnueabihf-gcc
ARM_FLAGS ?= -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard

p_obj = $(patsubst $(POWER_DIR)/src/%.c,$(POWER_BUILD)/$(1)/%.o,$(2))

POWER_OBJS := $(call p_obj,host,$(POWER_SRCS))
POWER_TOOLS := $(POWER_BUILD)/rsos-bootreason $(POWER_BUILD)/rsos-clock
POWER_TEST := $(POWER_BUILD)/power-test
$(POWER_BUILD)/host/power/pclock.o $(POWER_BUILD)/arm/power/pclock.o: POWER_OBJ_DEFS = $(POWER_DEFS)

POWER_ARM_BINS := $(POWER_BUILD)/arm/rsos-bootreason $(POWER_BUILD)/arm/rsos-clock \
	$(POWER_BUILD)/arm/power-test

.PHONY: power-all power-check power-arm-check power-install power-clean

power-all: $(POWER_TOOLS) $(POWER_TEST)

$(POWER_BUILD)/rsos-bootreason: $(call p_obj,host,$(POWER_BOOTREASON_SRCS))
	$(HOST_CC) $(LDFLAGS) -o $@ $^

$(POWER_BUILD)/rsos-clock: $(call p_obj,host,$(POWER_CLOCK_SRCS))
	$(HOST_CC) $(LDFLAGS) -o $@ $^

$(POWER_TEST): $(call p_obj,host,$(POWER_SRCS) $(POWER_EXTRA_SRCS) $(POWER_TEST_SRCS))
	$(HOST_CC) $(LDFLAGS) -o $@ $^ $(POWER_LDLIBS)

$(POWER_BUILD)/host/%.o: $(POWER_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(POWER_CFLAGS) $(POWER_OBJ_DEFS) -MMD -MP -c -o $@ $<

power-check: $(POWER_TEST)
	$(POWER_TEST)

# Cross check: the module, both tools and the test binary, compiled and
# linked for the A20.
power-arm-check: $(POWER_ARM_BINS)

$(POWER_BUILD)/arm/rsos-bootreason: $(call p_obj,arm,$(POWER_BOOTREASON_SRCS))
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) -o $@ $^

$(POWER_BUILD)/arm/rsos-clock: $(call p_obj,arm,$(POWER_CLOCK_SRCS))
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) -o $@ $^

$(POWER_BUILD)/arm/power-test: $(call p_obj,arm,$(POWER_SRCS) $(POWER_EXTRA_SRCS) $(POWER_TEST_SRCS))
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) -o $@ $^ $(POWER_LDLIBS)

$(POWER_BUILD)/arm/%.o: $(POWER_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_FLAGS) $(POWER_CFLAGS) $(POWER_OBJ_DEFS) -MMD -MP -c -o $@ $<

power-install: $(POWER_TOOLS)
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(POWER_BUILD)/rsos-bootreason $(DESTDIR)/usr/bin/rsos-bootreason
	install -m 0755 $(POWER_BUILD)/rsos-clock $(DESTDIR)/usr/bin/rsos-clock

power-clean:
	rm -rf $(POWER_BUILD)

-include $(shell find $(POWER_BUILD) -name '*.d' 2>/dev/null)
