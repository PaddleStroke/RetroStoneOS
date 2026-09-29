/*
 * host_png.c - PNG thumbnails for save states: stb_image_write (encode)
 * and stb_image (decode, PNG only), both compiled here as *static*
 * copies (STB_IMAGE_WRITE_STATIC / STB_IMAGE_STATIC) so they never clash
 * with the UI's copy in gfx/third_party_impl.c when both are linked into
 * one binary. Only the two host_png_* functions are exported.
 */
#include "host_png.h"

#include <stdlib.h>
#include <string.h>

#include "hutil.h"

/* Static stb builds declare prototypes of compiled-out functions: GCC
 * reports those at the end of the translation unit, after any pop. */
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wshadow"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../third_party/stb/stb_image_write.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
/* thumbnails and screenshots are a few hundred pixels: a crafted PNG on the
 * card (12000000x1, stb's default limit is 2^24) is refused, not decoded */
#define STBI_MAX_DIMENSIONS HOST_PNG_MAX_DIM
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../../third_party/stb/stb_image.h"

#pragma GCC diagnostic pop

struct membuf {
	unsigned char *p;
	size_t n, cap;
	bool fail;
};

static void mem_write(void *ctx, void *data, int size)
{
	struct membuf *m = ctx;

	if (m->fail || size <= 0)
		return;
	if (m->n + (size_t)size > m->cap) {
		size_t nc = (m->cap ? m->cap * 2 : 16384) + (size_t)size;
		unsigned char *np = realloc(m->p, nc);

		if (!np) {
			m->fail = true;
			return;
		}
		m->p = np;
		m->cap = nc;
	}
	memcpy(m->p + m->n, data, (size_t)size);
	m->n += (size_t)size;
}

void *host_png_encode(const uint8_t *rgb, int w, int h, size_t *size)
{
	struct membuf m = { 0 };

	if (!stbi_write_png_to_func(mem_write, &m, w, h, 3, rgb, w * 3) || m.fail) {
		free(m.p);
		return NULL;
	}
	*size = m.n;
	return m.p;
}

uint8_t *host_png_read_rgb(const char *path, int *w, int *h)
{
	size_t size;
	long long fsize = hfile_size(path);
	void *data;
	int comp;
	uint8_t *px;

	if (fsize <= 0 || fsize > HOST_PNG_MAX_FILE)
		return NULL;
	data = hread_file(path, &size);
	if (!data)
		return NULL;
	if (size > HOST_PNG_MAX_FILE) {   /* it grew since the stat */
		free(data);
		return NULL;
	}
	px = stbi_load_from_memory(data, (int)size, w, h, &comp, 3);
	free(data);
	return px;
}

void host_png_free(void *p)
{
	stbi_image_free(p);
}
