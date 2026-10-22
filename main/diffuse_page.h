#pragma once
/* Diffuse: a button on the slideshow (a drop, left of "?") opens the picture
 * on show in a view over it, where diffuse.c's effects run on it, a slice at
 * a time, as you watch; Save puts the result on the card ("Diffused") and
 * into the slideshow. The slideshow waits meanwhile */
#include <stdbool.h>
#include "lvgl.h"

/* build the view (hidden) and its button; call holding the LVGL lock, after
 * now_showing_create */
void diffuse_page_create(lv_obj_t *parent);
/* the menu shows the slideshow, or another page (in the LVGL task): the
 * button is only on the slideshow, and another page closes the view */
void diffuse_page_slideshow(bool shown);
