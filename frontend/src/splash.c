/*
 * splash.c - boot logo decoder (see splash.h for the format). No
 * dependencies: also compiled into the build-time tool mksplash, which
 * checks its own output with it.
 */
#include "splash.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned rd16(const uint8_t *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8;
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int splash_load_mem(const void *buf, size_t size, struct splash *s)
{
	const uint8_t *p = buf;
	unsigned n;

	memset(s, 0, sizeof(*s));
	if (size < SPLASH_HEADER || memcmp(p, SPLASH_MAGIC, 4) || rd16(p + 4) != SPLASH_VERSION)
		return -EINVAL;
	n = rd16(p + 10);
	s->width = (int)rd16(p + 6);
	s->height = (int)rd16(p + 8);
	if (!n || n > 256 || s->width <= 0 || s->height <= 0 || size < SPLASH_HEADER + 4u * n)
		return -EINVAL;
	s->bg = rd32(p + 12) & 0xffffffu;
	s->data = malloc(size);
	if (!s->data)
		return -ENOMEM;
	memcpy(s->data, buf, size);
	s->size = size;
	return 0;
}

int splash_load(const char *path, struct splash *s)
{
	struct stat st;
	uint8_t *buf;
	ssize_t r;
	size_t got = 0;
	int fd, ret;

	memset(s, 0, sizeof(*s));
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > (16 << 20)) {
		close(fd);
		return -EINVAL;
	}
	buf = malloc((size_t)st.st_size);
	if (!buf) {
		close(fd);
		return -ENOMEM;
	}
	while (got < (size_t)st.st_size && (r = read(fd, buf + got, (size_t)st.st_size - got)) > 0)
		got += (size_t)r;
	close(fd);
	ret = got == (size_t)st.st_size ? splash_load_mem(buf, got, s) : -EIO;
	free(buf);
	return ret;
}

void splash_free(struct splash *s)
{
	free(s->data);
	memset(s, 0, sizeof(*s));
}

static void fill(uint32_t *d, uint32_t c, int n)
{
	for (int i = 0; i < n; i++)
		d[i] = c;
}

int splash_draw(const struct splash *s, uint32_t *dst, int dw, int dh, int stride_px)
{
	const uint8_t *p, *end;
	const uint8_t *pal;
	unsigned npal;
	uint32_t *line, *prev = NULL;
	int w, h, ox, oy, x0, x1, y;
	int ret = 0;

	if (!s->data || !dst || dw <= 0 || dh <= 0 || stride_px < dw)
		return -EINVAL;
	w = s->width;
	h = s->height;
	npal = rd16(s->data + 10);
	pal = s->data + SPLASH_HEADER;
	p = pal + 4 * npal;
	end = s->data + s->size;
	ox = (dw - w) / 2;
	oy = (dh - h) / 2;
	x0 = ox < 0 ? -ox : 0;                   /* visible part of a frame row */
	x1 = ox + w > dw ? dw - ox : w;
	line = malloc((size_t)w * 4);            /* for rows that are not drawn in place */
	if (!line)
		return -ENOMEM;

	/* rows above and below the frame */
	for (y = 0; y < oy && y < dh; y++)
		fill(dst + (size_t)y * stride_px, s->bg, dw);
	for (y = oy + h < 0 ? 0 : oy + h; y < dh; y++)
		fill(dst + (size_t)y * stride_px, s->bg, dw);

	for (y = 0; y < h; y++) {
		int dy = oy + y;
		int visible = dy >= 0 && dy < dh;
		uint32_t *row = dst + (size_t)(visible ? dy : 0) * stride_px;
		uint32_t *t = visible && ox >= 0 && ox + w <= dw ? row + ox : line;
		uint8_t op;

		if (p >= end) {
			ret = -EINVAL;
			break;
		}
		op = *p++;
		if (op == 0 || op == 2) {
			if (!prev) {
				ret = -EINVAL;
				break;
			}
			if (t != prev)
				memcpy(t, prev, (size_t)w * 4);
		}
		if (op == 1 || op == 2) {
			unsigned n, rec = op == 1 ? 2 : 4;
			int x = 0;

			if (end - p < 2) {
				ret = -EINVAL;
				break;
			}
			n = rd16(p);
			p += 2;
			if ((size_t)(end - p) < rec * n) {
				ret = -EINVAL;
				break;
			}
			for (unsigned i = 0; i < n; i++, p += rec) {
				int len;
				unsigned idx;

				if (op == 2) {
					x = (int)rd16(p);
					len = p[2];
					idx = p[3];
				} else {
					len = p[0];
					idx = p[1];
				}
				if (idx >= npal || len > w - x) {
					ret = -EINVAL;
					break;
				}
				fill(t + x, rd32(pal + 4 * idx) & 0xffffffu, len);
				x += len;
			}
			if (ret == 0 && op == 1 && x != w)
				ret = -EINVAL;
			if (ret)
				break;
		} else if (op != 0) {
			ret = -EINVAL;
			break;
		}
		if (visible) {
			if (ox > 0) {                    /* side margins */
				fill(row, s->bg, ox);
				fill(row + ox + w, s->bg, dw - ox - w);
			}
			if (t == line && x1 > x0)
				memcpy(row + (ox > 0 ? ox : 0), line + x0, (size_t)(x1 - x0) * 4);
		}
		prev = t;
	}
	free(line);
	return ret;
}
