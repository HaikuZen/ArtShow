#pragma once
/* the artwork of the day page: the painting, and its title, artist, date,
 * medium, size and Wikipedia's summary beside it */
#include "lvgl.h"
#include "artwork.h"

/* build the page, hidden; call holding the LVGL lock */
lv_obj_t *artwork_page_create(lv_obj_t *parent);
/* the artwork task's callback (artwork_cb_t): takes the LVGL lock itself */
void artwork_page_update(const artwork_t *a, const char *status);
