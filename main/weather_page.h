#pragma once
/* the weather page: the weather now, and the next days as cards */
#include "lvgl.h"
#include "weather.h"

/* build the page, hidden; call holding the LVGL lock */
lv_obj_t *weather_page_create(lv_obj_t *parent);
/* the weather task's callback (weather_cb_t): takes the LVGL lock itself */
void weather_page_update(const weather_t *w, const char *status);
