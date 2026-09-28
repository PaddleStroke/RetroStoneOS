# frontend/batch2.mk - the checks of the batch 2 features (docs/integration-todo.md
# "Batch 2"): included by the Makefile, `make check-b2` (part of `make check`).
#
#   check-b2-unit      tests/test_b2.c: the new systems (the real core .ini files: names,
#                      default cores, carousel order), letter groups, search match,
#                      gamedb.tsv columns, sort orders, hidden games
#   check-rumble       tests/test_rumble.c: input_rumble() on a uinput pad with FF_RUMBLE
#                      (SKIP without /dev/uinput and evdev)
#   check-smb          tests/smb-test.sh: the rsos-smb helper with stub commands
#   check-b2-ui        tests/b2-ui.sh: letter jump, search, Search all games, settings,
#                      the Windows share next to the network transfer (preview tool)
#   check-b2-frontend  tests/b2-frontend.sh: play time, the game switcher's relaunch,
#                      Resume on boot, per-game settings, Hide, Delete (the real main loop)
#
# (The host side, in check-host: rsos-launch-test runs the switcher protocol, the
# "playtime"/"setting" lines, fast-forward with clock pacing and the screenshot PNG;
# rsos-host-test the fast-forward DRC model and the play-time clock.)

B2_CHECK_DIR = $(BUILDDIR)/b2-check
B2_TEST      = $(BUILDDIR)/test_b2
RUMBLE_TEST  = $(BUILDDIR)/test_rumble
# the core metadata of the image (not in a Buildroot copy of frontend/ alone)
B2_CORE_INIS = $(filter-out %.bench.ini,$(sort $(wildcard ../buildroot-external/package/libretro-*/*.ini)))

$(B2_TEST): $(BUILDDIR)/tests/test_b2.o $(UI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(UI_LDLIBS)

$(RUMBLE_TEST): $(BUILDDIR)/tests/test_rumble.o $(UI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(UI_LDLIBS)

check-b2: check-b2-unit check-rumble check-smb check-b2-ui check-b2-frontend

check-b2-unit: $(B2_TEST)
	rm -rf $(B2_CHECK_DIR)/unit && mkdir -p $(B2_CHECK_DIR)/unit/cores
	$(if $(B2_CORE_INIS),cp $(B2_CORE_INIS) $(B2_CHECK_DIR)/unit/cores/ && \
		$(B2_TEST) $(B2_CHECK_DIR)/unit $(B2_CHECK_DIR)/unit/cores,@echo "check-b2-unit: no core .ini files here: skipped")

# uinput and evdev are modules on most hosts (the test says SKIP without them)
check-rumble: $(RUMBLE_TEST)
	-@modprobe uinput 2>/dev/null; modprobe evdev 2>/dev/null; true
	rm -rf $(B2_CHECK_DIR)/rumble && mkdir -p $(B2_CHECK_DIR)/rumble
	$(RUMBLE_TEST) $(B2_CHECK_DIR)/rumble

check-smb:
	sh tests/smb-test.sh $(B2_CHECK_DIR)/smb

check-b2-ui: $(UIPREVIEW)
	sh tests/b2-ui.sh $(UIPREVIEW) $(B2_CHECK_DIR)/ui

check-b2-frontend: $(FRONTEND) $(HOST_TESTCORE) $(CATALOGS)
	sh tests/b2-frontend.sh $(FRONTEND) $(abspath $(HOST_TESTCORE)) $(B2_CHECK_DIR)/frontend \
		$(abspath $(BUILDDIR)/locale) $(BOARD_INI_RS2)

.PHONY: check-b2 check-b2-unit check-rumble check-smb check-b2-ui check-b2-frontend

-include $(BUILDDIR)/tests/test_b2.d $(BUILDDIR)/tests/test_rumble.d
