#pragma once
/* the Album Wall page: the user's favourite Deezer albums (albums.c):
 * Spotlight (one cover large, its details and tracks, another every 30 s or
 * on a tap) and Wall (18 covers a page; a tap spotlights one); Sync */
#include <stdbool.h>
#include "lvgl.h"

/* build the page, hidden, and its worker; call holding the LVGL lock */
lv_obj_t *album_wall_create(lv_obj_t *parent);
/* the menu shows or hides it (in the LVGL task) */
void album_wall_shown(bool shown);
/* Spotlight or Wall, as its tabs would (the demo tour); in the LVGL task */
void album_wall_mode(bool wall);
