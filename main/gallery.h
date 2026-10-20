#pragma once
/* the Gallery page: the index's pictures as thumbnails, 15 a page, by
 * artist; all, favourites, an artist's, or the ones that can't be shown.
 * A tap shows the picture on the slideshow; the star makes it a favourite */
#include <stdbool.h>
#include "lvgl.h"

/* build the page, hidden, and its worker task; call holding the LVGL lock */
lv_obj_t *gallery_create(lv_obj_t *parent);
/* the menu shows or hides it (in the LVGL task) */
void gallery_shown(bool shown);
