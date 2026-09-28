/*
 * font8x8.h - tiny 8x8 bitmap font for printable ASCII.
 * Public domain, see font8x8.c.
 */
#ifndef RSOS_FONT8X8_H
#define RSOS_FONT8X8_H

#include <stdint.h>

#define FONT8X8_FIRST 0x20
#define FONT8X8_LAST  0x7e

/* font8x8_ascii[c - FONT8X8_FIRST][row], bit 7 = leftmost pixel. */
extern const uint8_t font8x8_ascii[95][8];

static inline const uint8_t *font8x8_glyph(unsigned char c)
{
	if (c < FONT8X8_FIRST || c > FONT8X8_LAST)
		c = '?';
	return font8x8_ascii[c - FONT8X8_FIRST];
}

#endif
