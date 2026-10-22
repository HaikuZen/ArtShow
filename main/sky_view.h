#pragma once
/* the painted sky (sky_paint.c) for the pages that show it, the Sun and Moon
 * page and the Weather page: one 1024 x 600 canvas in PSRAM and one painter
 * task, shared, as one page is shown at a time. A picture is ~0.4 s on the
 * board: painted in place, off the LVGL task (a minute's change is a few
 * pixels, so a half-painted one never shows) */
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "lvgl.h"
#include "sky_paint.h"

#define SKY_W 1024
#define SKY_H 600

/* a full-screen image of the canvas, first child of parent; call holding
 * the LVGL lock (the first call makes the canvas and the task) */
lv_obj_t *sky_view_image(lv_obj_t *parent);
/* paint it: base gives the place, the day, the sun's path and the weather;
 * the sun's and the moon's places now are worked out here (day: a place and
 * a clock; else a night without a moon). A small moon at (mx, my), radius
 * mr, if mr > 0 (the Sun and Moon page's caption). Any task */
void sky_view_paint(const sky_t *base, double lat, double lon, time_t now, bool day, float mx, float my, float mr);
/* the hills' seed for a place: the same hills on every page */
uint32_t sky_view_place(double lat, double lon);
