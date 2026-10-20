#pragma once
/* the Settings page: the slideshow (seconds per picture, the transition,
 * shuffle, fill, which folders play), night mode, and about the board */
#include <stdbool.h>
#include "lvgl.h"

/* build the page, hidden; call holding the LVGL lock */
lv_obj_t *settings_page_create(lv_obj_t *parent);
/* the menu shows or hides it (in the LVGL task) */
void settings_page_shown(bool shown);
