/*
 * content.h - content (ROM) preparation for retro_load_game():
 *  - .zip for a core that does not list "zip" in valid_extensions and is
 *    not an arcade core (block_extract): the first member whose extension
 *    the core accepts is extracted with miniz into tmpfs (/tmp/rsos);
 *    arcade cores (mame2003_plus, fbneo) always get the zip as is;
 *  - need_fullpath = false (incl. SET_CONTENT_INFO_OVERRIDE): the file is
 *    read into memory and passed as data (the path is passed too);
 *  - GET_GAME_INFO_EXT is answered from the same data.
 * Save naming follows RetroArch: the content basename without extension,
 * and for an extracted archive the name of the file inside it.
 */
#ifndef RSOS_HOST_CONTENT_H
#define RSOS_HOST_CONTENT_H

#include <stdbool.h>
#include <stddef.h>

#include "../../third_party/libretro/libretro.h"

/* Step 1 (after retro_get_system_info): extraction, sets H.content_path
 * and H.game. Returns 0, or -errno with a message for the UI. */
int content_prepare(char *err, size_t n);
/* Step 2 (after retro_init, right before retro_load_game): loads the data
 * into memory if the core wants it, fills gi. */
int content_load(struct retro_game_info *gi, char *err, size_t n);
/* GET_GAME_INFO_EXT. */
bool content_game_info_ext(const struct retro_game_info_ext **ext);
/* Frees the data, removes extracted files. */
void content_cleanup(void);

#endif
