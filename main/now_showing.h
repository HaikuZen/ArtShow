#pragma once
/* the Now Showing page: the slideshow's picture on show, its title and artist
 * from the file name ("Artist - Title.jpg"), and Wikipedia's summary of the
 * painting, or else of its artist. A "?" button top right of the slideshow
 * opens it over the slideshow (paused meanwhile); the same button, then an X,
 * closes it */
#include <stdbool.h>
#include "lvgl.h"

/* build the page (hidden), its button and its lookup task; call holding the
 * LVGL lock, after the slideshow and before the menu's other pages */
void now_showing_create(lv_obj_t *parent);
/* the menu shows the slideshow, or another page (in the LVGL task): the
 * button is only on the slideshow, and another page closes this one */
void now_showing_slideshow(bool shown);
/* open or close it, as its button would (the demo tour); on the slideshow, in the LVGL task */
void now_showing_open(bool on);
/* the wall label over the slideshow: the picture just put on show's artist
   and title, faded in and out (g_settings.captions). Any task */
void now_showing_caption(void);
