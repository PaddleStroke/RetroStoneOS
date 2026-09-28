/*
 * host_png.h - PNG encode (thumbnails, frame dumps) and decode (slot
 * picker) for the host. See host_png.c.
 */
#ifndef RSOS_HOST_PNG_H
#define RSOS_HOST_PNG_H

#include <stddef.h>
#include <stdint.h>

/* RGB24 -> PNG in memory (malloc; free()). NULL on error. */
void *host_png_encode(const uint8_t *rgb, int w, int h, size_t *size);
/* PNG file -> RGB24 (free with host_png_free). NULL on error. */
uint8_t *host_png_read_rgb(const char *path, int *w, int *h);
void host_png_free(void *p);

#endif
