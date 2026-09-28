/*
 * fakegles.c - a stand-in libGLESv2.so.2 for the GL probe test: the probed
 * entry points take a known time (a "compile" 60 ms, a link 5 ms, the first
 * draw 3 ms like a lima variant compile, the next ones nothing).
 */
#include <time.h>

void glCompileShader(unsigned int shader);
void glLinkProgram(unsigned int program);
void glDrawArrays(unsigned int mode, int first, int count);
void glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices);
void glTexImage2D(unsigned int target, int level, int internalformat, int width, int height, int border,
		  unsigned int format, unsigned int type, const void *pixels);
void glTexSubImage2D(unsigned int target, int level, int xoffset, int yoffset, int width, int height,
		     unsigned int format, unsigned int type, const void *pixels);
int fakegl_calls(void);

static int calls, draws;

static void busy_ms(int ms)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)ms * 1000000L };

	nanosleep(&ts, NULL);
}

void glCompileShader(unsigned int shader)
{
	(void)shader;
	calls++;
	busy_ms(60);
}

void glLinkProgram(unsigned int program)
{
	(void)program;
	calls++;
	busy_ms(5);
}

void glDrawArrays(unsigned int mode, int first, int count)
{
	(void)mode;
	(void)first;
	(void)count;
	calls++;
	if (!draws++)
		busy_ms(3);
}

void glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices)
{
	(void)mode;
	(void)count;
	(void)type;
	(void)indices;
	calls++;
}

void glTexImage2D(unsigned int target, int level, int internalformat, int width, int height, int border,
		  unsigned int format, unsigned int type, const void *pixels)
{
	(void)target, (void)level, (void)internalformat, (void)width, (void)height, (void)border;
	(void)format, (void)type, (void)pixels;
	calls++;
}

void glTexSubImage2D(unsigned int target, int level, int xoffset, int yoffset, int width, int height,
		     unsigned int format, unsigned int type, const void *pixels)
{
	(void)target, (void)level, (void)xoffset, (void)yoffset, (void)width, (void)height;
	(void)format, (void)type, (void)pixels;
	calls++;
}

/* How many GL calls reached the "driver" (the wrappers must forward all). */
int fakegl_calls(void)
{
	return calls;
}
