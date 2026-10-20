#pragma once
/* the change from one picture to the next, drawn into the screen buffer on show:
 * each time one of a few reveals (an iris, an ink drop, a brushstroke, mosaic
 * tiles, sand, blinds, a clock hand, a dissolve), picked at random. All of it
 * runs in the LVGL task (a timer); call everything here holding the LVGL lock */
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/* view shows out, a screen-sized RGB565 buffer; false: no memory */
bool transition_init(lv_obj_t *view, uint16_t *out);
/* from what out shows now to next (screen-sized RGB565). next must stay
 * untouched until the transition is over: transition_finish() makes sure */
void transition_start(const uint16_t *next);
/* jump to the end of a running transition (out = next); nothing if none runs */
void transition_finish(void);
