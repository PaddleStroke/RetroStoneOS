/*
 * playtime.h - play time accounting of the game process (pure, unit-tested):
 * the wall time while the game runs, without the in-game menu, the game
 * switcher, a pause (player 1 controller gone) or a sleep. The game process
 * reports the total as "playtime <seconds>" status lines (periodically and at
 * exit); the menu adds it to the game's play time in gamedb.tsv
 * (docs/host-design.md §10.3).
 */
#ifndef RSOS_HOST_PLAYTIME_H
#define RSOS_HOST_PLAYTIME_H

#include <stdbool.h>
#include <stdint.h>

struct playtime {
	int64_t total_ms;        /* closed segments */
	int64_t since_ms;        /* start of the running segment */
	int paused;              /* nesting: menu inside a pause... */
	bool started;
};

void pt_start(struct playtime *p, int64_t now_ms);
/* Pauses nest: the clock runs again when every pause has ended. */
void pt_pause(struct playtime *p, int64_t now_ms);
void pt_resume(struct playtime *p, int64_t now_ms);
int64_t pt_total_ms(const struct playtime *p, int64_t now_ms);
bool pt_running(const struct playtime *p);

#endif
