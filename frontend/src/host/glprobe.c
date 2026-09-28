/*
 * glprobe.c - see glprobe.h. No GL headers here on purpose: the wrappers
 * below *define* the GL entry points (the executable exports them), with the
 * same integer ABI as <GLES2/gl2.h> (GLenum/GLuint = unsigned int,
 * GLint/GLsizei = int).
 */
#include "glprobe.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { F_COMPILE, F_LINK, F_DRAWA, F_DRAWE, F_TEX, F_TEXSUB, F_N };

static const char *const names[F_N] = {
	"glCompileShader", "glLinkProgram", "glDrawArrays", "glDrawElements", "glTexImage2D", "glTexSubImage2D",
};
static void *real[F_N];
static void *lib;
static struct glprobe_stats S;
static int seen, missing_logged;

/* The exported wrappers (prototypes for -Wmissing-prototypes builds). */
void glCompileShader(unsigned int shader);
void glLinkProgram(unsigned int program);
void glDrawArrays(unsigned int mode, int first, int count);
void glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices);
void glTexImage2D(unsigned int target, int level, int internalformat, int width, int height, int border,
		  unsigned int format, unsigned int type, const void *pixels);
void glTexSubImage2D(unsigned int target, int level, int xoffset, int yoffset, int width, int height,
		     unsigned int format, unsigned int type, const void *pixels);

static int64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void add(uint64_t *v, uint64_t d)
{
	__atomic_fetch_add(v, d, __ATOMIC_RELAXED);
}

void glprobe_bind(void *gles_lib)
{
	__atomic_store_n(&lib, gles_lib, __ATOMIC_RELEASE);
}

static void *resolve(int i)
{
	void *p = __atomic_load_n(&real[i], __ATOMIC_ACQUIRE);
	void *l;

	if (p)
		return p;
	l = __atomic_load_n(&lib, __ATOMIC_ACQUIRE);
	if (!l) {
		/* The core's own NEEDED libGLESv2 (a reference we keep: the
		 * process exits with it mapped anyway). */
		l = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_NOLOAD);
		if (l)
			__atomic_store_n(&lib, l, __ATOMIC_RELEASE);
	}
	if (l)
		p = dlsym(l, names[i]);
	if (!p)
		p = dlsym(RTLD_NEXT, names[i]);
	if (p)
		__atomic_store_n(&real[i], p, __ATOMIC_RELEASE);
	else if (!__atomic_exchange_n(&missing_logged, 1, __ATOMIC_RELAXED))
		fprintf(stderr, "glprobe: %s not found in libGLESv2: call dropped\n", names[i]);
	return p;
}

#define ENTER(i, fn_t, fn)                          \
	fn_t fn = (fn_t)resolve(i);                   \
	int64_t t0;                                   \
	if (!fn)                                      \
		return;                               \
	if (!seen)                                    \
		__atomic_store_n(&seen, 1, __ATOMIC_RELAXED); \
	t0 = now_us()

void glCompileShader(unsigned int shader)
{
	typedef void (*f_t)(unsigned int);
	ENTER(F_COMPILE, f_t, f);
	f(shader);
	add(&S.compiles, 1);
	add(&S.compile_us, (uint64_t)(now_us() - t0));
}

void glLinkProgram(unsigned int program)
{
	typedef void (*f_t)(unsigned int);
	ENTER(F_LINK, f_t, f);
	f(program);
	add(&S.links, 1);
	add(&S.link_us, (uint64_t)(now_us() - t0));
}

static void draw_done(int64_t t0)
{
	int64_t d = now_us() - t0;

	add(&S.draws, 1);
	if (d >= GLPROBE_SLOW_DRAW_US) {
		add(&S.slow_draws, 1);
		add(&S.slow_draw_us, (uint64_t)d);
	}
}

void glDrawArrays(unsigned int mode, int first, int count)
{
	typedef void (*f_t)(unsigned int, int, int);
	ENTER(F_DRAWA, f_t, f);
	f(mode, first, count);
	draw_done(t0);
}

void glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices)
{
	typedef void (*f_t)(unsigned int, int, unsigned int, const void *);
	ENTER(F_DRAWE, f_t, f);
	f(mode, count, type, indices);
	draw_done(t0);
}

void glTexImage2D(unsigned int target, int level, int internalformat, int width, int height, int border,
		  unsigned int format, unsigned int type, const void *pixels)
{
	typedef void (*f_t)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
	ENTER(F_TEX, f_t, f);
	f(target, level, internalformat, width, height, border, format, type, pixels);
	add(&S.tex, 1);
	add(&S.tex_us, (uint64_t)(now_us() - t0));
}

void glTexSubImage2D(unsigned int target, int level, int xoffset, int yoffset, int width, int height,
		     unsigned int format, unsigned int type, const void *pixels)
{
	typedef void (*f_t)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void *);
	ENTER(F_TEXSUB, f_t, f);
	f(target, level, xoffset, yoffset, width, height, format, type, pixels);
	add(&S.tex, 1);
	add(&S.tex_us, (uint64_t)(now_us() - t0));
}

void *glprobe_wrap(const char *sym)
{
	static void *const wrappers[F_N] = {
		(void *)glCompileShader, (void *)glLinkProgram, (void *)glDrawArrays,
		(void *)glDrawElements, (void *)glTexImage2D, (void *)glTexSubImage2D,
	};

	if (!sym)
		return NULL;
	for (int i = 0; i < F_N; i++)
		if (!strcmp(sym, names[i]))
			return resolve(i) ? wrappers[i] : NULL;
	return NULL;
}

void glprobe_get(struct glprobe_stats *s)
{
	s->compiles = __atomic_load_n(&S.compiles, __ATOMIC_RELAXED);
	s->compile_us = __atomic_load_n(&S.compile_us, __ATOMIC_RELAXED);
	s->links = __atomic_load_n(&S.links, __ATOMIC_RELAXED);
	s->link_us = __atomic_load_n(&S.link_us, __ATOMIC_RELAXED);
	s->draws = __atomic_load_n(&S.draws, __ATOMIC_RELAXED);
	s->slow_draws = __atomic_load_n(&S.slow_draws, __ATOMIC_RELAXED);
	s->slow_draw_us = __atomic_load_n(&S.slow_draw_us, __ATOMIC_RELAXED);
	s->tex = __atomic_load_n(&S.tex, __ATOMIC_RELAXED);
	s->tex_us = __atomic_load_n(&S.tex_us, __ATOMIC_RELAXED);
}

void glprobe_diff(const struct glprobe_stats *a, const struct glprobe_stats *b, struct glprobe_stats *d)
{
	d->compiles = b->compiles - a->compiles;
	d->compile_us = b->compile_us - a->compile_us;
	d->links = b->links - a->links;
	d->link_us = b->link_us - a->link_us;
	d->draws = b->draws - a->draws;
	d->slow_draws = b->slow_draws - a->slow_draws;
	d->slow_draw_us = b->slow_draw_us - a->slow_draw_us;
	d->tex = b->tex - a->tex;
	d->tex_us = b->tex_us - a->tex_us;
}

bool glprobe_seen(void)
{
	return __atomic_load_n(&seen, __ATOMIC_RELAXED) != 0;
}
