#pragma once
/* the backlight control: a button in the bottom left corner opens a slider next
 * to it, which closes again after a few seconds untouched. The level is kept in
 * NVS (namespace "artshow"). Call holding the LVGL lock, after nvs_flash_init */
#include "lvgl.h"

void brightness_ui_create(lv_obj_t *parent);
/* the level set on the slider, % (night mode goes back to it) */
int brightness_ui_level(void);
