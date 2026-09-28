/*
 * glcore.c - a stand-in for a core linked against libGLESv2 (as parallel-n64
 * is: NEEDED libGLESv2.so.2): it calls the GL entry points directly, and once
 * through a get_proc_address it is handed (the libretro hw render path).
 */
typedef void (*glcore_proc_t)(void);
typedef glcore_proc_t (*glcore_gpa_t)(const char *sym);

void glCompileShader(unsigned int shader);
void glLinkProgram(unsigned int program);
void glDrawArrays(unsigned int mode, int first, int count);
void glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices);
void glTexImage2D(unsigned int target, int level, int internalformat, int width, int height, int border,
		  unsigned int format, unsigned int type, const void *pixels);
int glcore_frame(glcore_gpa_t gpa);

/* One "frame": a new combiner (compile x2, link), three draws, an upload,
 * and a compile through the resolved pointer. */
int glcore_frame(glcore_gpa_t gpa)
{
	void (*compile)(unsigned int) = gpa ? (void (*)(unsigned int))gpa("glCompileShader") : 0;

	glCompileShader(1);
	glCompileShader(2);
	glLinkProgram(3);
	glDrawArrays(4, 0, 3);
	glDrawArrays(4, 0, 3);
	glDrawElements(4, 3, 0x1403, 0);
	glTexImage2D(0x0de1, 0, 0x1908, 1, 1, 0, 0x1908, 0x1401, 0);
	if (compile)
		compile(4);
	return compile ? 1 : 0;
}
