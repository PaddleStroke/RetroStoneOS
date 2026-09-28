/*
 * glprobe.h - counts the GL calls that can stall a frame: shader compiles
 * and links (Mesa's GLSL front end), draws that take long (lima compiles the
 * shader variant for the current state on the first draw that uses it), and
 * texture uploads. docs/host-design.md §13.4.
 *
 * How the calls are seen: the game process exports glCompileShader,
 * glLinkProgram, glDrawArrays, glDrawElements, glTexImage2D and
 * glTexSubImage2D from its own dynamic symbol table (frontend/glprobe.syms,
 * --dynamic-list). A core that links libGLESv2 directly (parallel-n64 does:
 * NEEDED libGLESv2.so.2) gets these wrappers, because the dynamic linker
 * searches the executable before the core's own dependencies; a core that
 * resolves them through the hw render get_proc_address gets them from
 * glprobe_wrap(). Each wrapper times the call and forwards it to Mesa.
 * Cost: two clock reads (vDSO) and a few atomic adds per call.
 */
#ifndef RSOS_HOST_GLPROBE_H
#define RSOS_HOST_GLPROBE_H

#include <stdbool.h>
#include <stdint.h>

/* A draw call slower than this is counted as a stall (variant compile). */
#define GLPROBE_SLOW_DRAW_US 2000

struct glprobe_stats {
	uint64_t compiles, compile_us;       /* glCompileShader */
	uint64_t links, link_us;             /* glLinkProgram */
	uint64_t draws;                      /* every glDrawArrays/glDrawElements */
	uint64_t slow_draws, slow_draw_us;   /* the ones over GLPROBE_SLOW_DRAW_US */
	uint64_t tex, tex_us;                /* glTexImage2D + glTexSubImage2D */
};

/* The library the real entry points come from (hwrender's libGLESv2 handle).
 * Without it, the already loaded libGLESv2.so.2 is used (RTLD_NOLOAD). */
void glprobe_bind(void *gles_lib);
/* get_proc_address hook: our wrapper for a probed symbol, else NULL. */
void *glprobe_wrap(const char *sym);
/* Cumulative counters (all threads). */
void glprobe_get(struct glprobe_stats *s);
/* b - a */
void glprobe_diff(const struct glprobe_stats *a, const struct glprobe_stats *b, struct glprobe_stats *d);
/* True once a probed call went through a wrapper (the interposition works). */
bool glprobe_seen(void);

#endif
