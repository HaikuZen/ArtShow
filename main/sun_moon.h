#pragma once
/* the Sun and Moon page: today's sunrise, sunset, day length and golden
 * hours, the sun's path; the moon's phase (drawn), rise and set, the next
 * full and new moon; the next solstice or equinox. Computed here (astro.c),
 * for the weather's place (weather_place()) */
#include <stdbool.h>
#include "lvgl.h"

/* build the page, hidden; call holding the LVGL lock */
lv_obj_t *sun_moon_create(lv_obj_t *parent);
/* the menu shows or hides it (in the LVGL task) */
void sun_moon_shown(bool shown);
