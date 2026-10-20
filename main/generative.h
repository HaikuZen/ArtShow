#pragma once
/* the Generative page: pieces that paint themselves (gen_art.c): a flow field,
 * a strange attractor, Hitomezashi stitching, in turn, each in the colours of
 * the slideshow's picture; Next, and Save (into the slideshow) */
#include <stdbool.h>
#include "lvgl.h"

/* build the page, hidden, and its task; call holding the LVGL lock */
lv_obj_t *generative_create(lv_obj_t *parent);
/* the menu shows or hides it (in the LVGL task): it paints only when shown */
void generative_shown(bool shown);
