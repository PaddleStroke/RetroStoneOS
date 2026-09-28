# frontend/transfer.mk - ROM/BIOS/save transfer: USB stick import, web
# upload server, name responder, QR encoder (src/transfer/).
#
# Standalone (host, from frontend/):
#   make -f transfer.mk                         # objects + test + host tool
#   make -f transfer.mk TR_BUILD=~/rsos/transfer-build transfer-check
#   make -f transfer.mk transfer-web-test       # curl tests against the host server
#   make -f transfer.mk transfer-arm-check      # cross-compile + link for the A20
#
# From the main Makefile:
#   include transfer.mk
#   frontend objects += $(TR_OBJS); CFLAGS += $(TR_INCLUDES); LDLIBS += $(TR_LDLIBS)
# TR_OBJS contains the library objects and the embedded web page, compiled
# with $(TR_CC) (default $(CC)). No external libraries: libc + pthreads.
# The page (src/transfer/web/index.html) is turned into a C array with od +
# sed, so no xxd is needed on the build host.

TR_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
TR_BUILD ?= $(TR_DIR)/build-transfer
TR_SRC := $(TR_DIR)/src/transfer

TR_SRCS := \
	$(TR_SRC)/tr_util.c \
	$(TR_SRC)/sysmap.c \
	$(TR_SRC)/usb.c \
	$(TR_SRC)/import.c \
	$(TR_SRC)/backup.c \
	$(TR_SRC)/webshare.c \
	$(TR_SRC)/netnames.c \
	$(TR_SRC)/qr.c

TR_WEB := $(TR_SRC)/web/index.html
TR_GEN := $(TR_BUILD)/gen/web_index.c

TR_INCLUDES := -I$(TR_SRC)
TR_WARN := -Wall -Wextra -Wshadow -Wformat=2 -Wformat-security -Werror=format-security -Wstrict-prototypes \
	-Wmissing-prototypes -Wno-missing-field-initializers
# 64-bit off_t/time_t, as the target build gets them from Buildroot's
# CPPFLAGS (the standalone and cross checks too; tr_util.c asserts it).
TR_CFLAGS ?= -std=gnu11 $(TR_WARN) -O2 -g -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 $(TR_INCLUDES)
TR_LDLIBS := -lpthread
# Link flags: $(LDFLAGS) for TR_CC, $(ARM_LDFLAGS) for the cross check.
ARM_LDFLAGS ?=

TR_CC ?= $(CC)
ARM_CC ?= arm-linux-gnueabihf-gcc
ARM_FLAGS ?= -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard

TR_OBJS := $(patsubst $(TR_SRC)/%.c,$(TR_BUILD)/obj/%.o,$(TR_SRCS)) $(TR_BUILD)/obj/web_index.o
TR_TEST_OBJ := $(TR_BUILD)/obj/tests/test_transfer.o
TR_TOOL_OBJ := $(TR_BUILD)/obj/tests/webshare_host.o
TR_ARM_OBJS := $(patsubst $(TR_SRC)/%.c,$(TR_BUILD)/arm/%.o,$(TR_SRCS)) $(TR_BUILD)/arm/web_index.o

.PHONY: transfer-all transfer-check transfer-web-test transfer-arm-check transfer-install transfer-clean

transfer-all: $(TR_BUILD)/test_transfer $(TR_BUILD)/rsos-webshare-host

$(TR_GEN): $(TR_WEB)
	@mkdir -p $(dir $@)
	{ printf '/* generated from src/transfer/web/index.html by transfer.mk */\n'; \
	  printf 'const unsigned char webshare_index_html[] = {\n'; \
	  od -An -v -tx1 $< | sed -e 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'; \
	  printf '};\nconst unsigned int webshare_index_html_len = sizeof(webshare_index_html);\n'; \
	} > $@.tmp && mv $@.tmp $@

# --- host
$(TR_BUILD)/obj/%.o: $(TR_SRC)/%.c
	@mkdir -p $(dir $@)
	$(TR_CC) $(TR_CFLAGS) -MMD -MP -c -o $@ $<

$(TR_BUILD)/obj/web_index.o: $(TR_GEN)
	@mkdir -p $(dir $@)
	$(TR_CC) -std=gnu11 -O2 -c -o $@ $<

$(TR_BUILD)/test_transfer: $(TR_OBJS) $(TR_TEST_OBJ)
	$(TR_CC) $(LDFLAGS) -o $@ $^ $(TR_LDLIBS)

$(TR_BUILD)/rsos-webshare-host: $(TR_OBJS) $(TR_TOOL_OBJ)
	$(TR_CC) $(LDFLAGS) -o $@ $^ $(TR_LDLIBS)

transfer-check: $(TR_BUILD)/test_transfer
	$(TR_BUILD)/test_transfer

transfer-web-test: $(TR_BUILD)/rsos-webshare-host
	sh $(TR_SRC)/tests/webshare_test.sh $(TR_BUILD)/rsos-webshare-host

# --- ARM (A20) cross check: every object plus the test program, linked
$(TR_BUILD)/arm/%.o: $(TR_SRC)/%.c
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_FLAGS) $(TR_CFLAGS) -MMD -MP -c -o $@ $<

$(TR_BUILD)/arm/web_index.o: $(TR_GEN)
	@mkdir -p $(dir $@)
	$(ARM_CC) $(ARM_FLAGS) -std=gnu11 -O2 -c -o $@ $<

transfer-arm-check: $(TR_BUILD)/arm/test_transfer $(TR_BUILD)/arm/rsos-webshare-host

$(TR_BUILD)/arm/test_transfer: $(TR_ARM_OBJS) $(TR_BUILD)/arm/tests/test_transfer.o
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) -o $@ $^ $(TR_LDLIBS)

$(TR_BUILD)/arm/rsos-webshare-host: $(TR_ARM_OBJS) $(TR_BUILD)/arm/tests/webshare_host.o
	$(ARM_CC) $(ARM_FLAGS) $(ARM_LDFLAGS) -o $@ $^ $(TR_LDLIBS)

# README.txt for the root of the data partition: the build copies it into the
# seed partition image, and data-partition restores it when it is missing.
TR_DATA_README := $(TR_SRC)/data-README.txt

transfer-install:
	install -d $(DESTDIR)/usr/share/rsos
	install -m 0644 $(TR_DATA_README) $(DESTDIR)/usr/share/rsos/data-README.txt

transfer-clean:
	rm -rf $(TR_BUILD)

-include $(TR_OBJS:.o=.d) $(TR_TEST_OBJ:.o=.d) $(TR_TOOL_OBJ:.o=.d) $(TR_ARM_OBJS:.o=.d)
