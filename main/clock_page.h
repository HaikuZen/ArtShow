#pragma once
/* the clock page: an analog clock with the digital time and date, and the
 * current month's calendar. The time comes from NTP (time_sync.c) */
#include <stdbool.h>
#include "lvgl.h"

/* build the page, hidden; call holding the LVGL lock */
lv_obj_t *clock_page_create(lv_obj_t *parent);
/* the menu shows or hides it (in the LVGL task): the clock ticks only when shown */
void clock_page_shown(bool shown);
