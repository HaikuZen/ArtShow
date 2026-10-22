#pragma once
/* the weather page: the weather now, painted as the place's sky (a woodblock
 * print, like the Sun and Moon page), and the next hours and days */
#include <stdbool.h>
#include "lvgl.h"
#include "weather.h"

/* build the page, hidden; call holding the LVGL lock */
lv_obj_t *weather_page_create(lv_obj_t *parent);
/* the menu shows or hides it (in the LVGL task): the sky is painted again
 * each minute while shown */
void weather_page_shown(bool shown);
/* the weather task's callback (weather_cb_t): takes the LVGL lock itself */
void weather_page_update(const weather_t *w, const char *status);
