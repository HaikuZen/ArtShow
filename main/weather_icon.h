#pragma once
/* a weather icon drawn with LVGL's shapes (no image files): sun, moon, clouds,
 * rain, snow, fog, lightning. Any size, the same for every provider */
#include <stdbool.h>
#include "lvgl.h"
#include "weather.h"

/* a size x size icon; call holding the LVGL lock */
lv_obj_t *weather_icon_create(lv_obj_t *parent, int32_t size);
void weather_icon_set(lv_obj_t *icon, wx_kind_t kind, bool night);
