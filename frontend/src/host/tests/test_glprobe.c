/*
 * test_glprobe.c - the GL probe through the real dynamic linker: this program
 * is linked like the frontend (--dynamic-list=src/host/glprobe.syms), loads a
 * "core" (glcore.c) RTLD_LOCAL whose NEEDED libGLESv2.so.2 is a fake with
 * known call times (fakegles.c), and checks that the core's direct GL calls
 * and the get_proc_address path both went through the timed wrappers and
 * still reached the "driver".
 *
 *   rsos-glprobe-test BUILDDIR/fakegl/libglcore.so
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include "../glprobe.h"

static int fails;

#define CHECK(cond, ...)                              \
	do {                                          \
		bool ok_ = (cond);                    \
		printf("%s: ", ok_ ? "ok  " : "FAIL"); \
		printf(__VA_ARGS__);                  \
		printf("\n");                         \
		if (!ok_)                             \
			fails++;                      \
	} while (0)

typedef void (*proc_t)(void);

static proc_t gpa(const char *sym)
{
	return (proc_t)glprobe_wrap(sym);
}

int main(int argc, char **argv)
{
	struct glprobe_stats a, b, d;
	int (*frame)(proc_t (*)(const char *));
	int (*calls)(void);
	void *core, *gl;

	if (argc < 2) {
		fprintf(stderr, "usage: %s libglcore.so\n", argv[0]);
		return 2;
	}
	core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	CHECK(core != NULL, "core loaded RTLD_LOCAL (%s)", core ? "ok" : dlerror());
	if (!core)
		return 1;
	*(void **)&frame = dlsym(core, "glcore_frame");
	gl = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_NOLOAD);
	*(void **)&calls = gl ? dlsym(gl, "fakegl_calls") : NULL;
	CHECK(frame && calls, "core and fake libGLESv2 symbols");
	if (!frame || !calls)
		return 1;

	/* 1. not bound: the wrappers find the loaded libGLESv2.so.2 themselves */
	glprobe_get(&a);
	frame(NULL);
	glprobe_get(&b);
	glprobe_diff(&a, &b, &d);
	CHECK(glprobe_seen(), "the core's direct calls went through the probe");
	CHECK(d.compiles == 2 && d.compile_us >= 110000, "2 compiles, %.1f ms (>= 110)", d.compile_us / 1000.0);
	CHECK(d.links == 1 && d.link_us >= 4000, "1 link, %.1f ms", d.link_us / 1000.0);
	CHECK(d.draws == 3 && d.slow_draws == 1 && d.slow_draw_us >= 2500, "3 draws, 1 slow (%.1f ms)",
	      d.slow_draw_us / 1000.0);
	CHECK(d.tex == 1, "1 texture upload");
	CHECK(calls() == 7, "every call reached the driver (%d of 7)", calls());

	/* 2. bound (as hwrender does), and the get_proc_address path */
	glprobe_bind(gl);
	CHECK(glprobe_wrap("glCompileShader") != NULL && glprobe_wrap("glGetString") == NULL,
	      "get_proc_address: wrappers for probed symbols only");
	glprobe_get(&a);
	CHECK(frame(gpa) == 1, "compile through the resolved pointer");
	glprobe_get(&b);
	glprobe_diff(&a, &b, &d);
	CHECK(d.compiles == 3 && d.slow_draws == 0 && d.draws == 3, "second frame: 3 compiles, no slow draw");
	CHECK(calls() == 15, "every call reached the driver (%d of 15)", calls());
	printf("%s\n", fails ? "GLPROBE TEST: FAIL" : "GLPROBE TEST: PASS");
	return fails ? 1 : 0;
}
