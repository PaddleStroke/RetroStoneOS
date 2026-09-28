/*
 * third_party_impl.c - the one translation unit that compiles the vendored
 * single-header libraries (see frontend/third_party/README.md).
 * Their own warnings are silenced here so the rest of the tree builds with
 * -Wall -Wextra and zero warnings.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

/* stb_image: PNG, JPEG, GIF (first frame), BMP, TGA. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
/* No decode past 4096 x 4096 (64 MB): a huge picture in a scraped library
 * is refused, never an out-of-memory abort of the menu (review F-L21). */
#define STBI_MAX_DIMENSIONS 4096
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define STBI_NEON
#endif
#include "../../third_party/stb/stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"

#define NANOSVG_IMPLEMENTATION
#include "../../third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../../third_party/nanosvg/nanosvgrast.h"

#pragma GCC diagnostic pop
