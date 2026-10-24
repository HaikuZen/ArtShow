#pragma once
/* the slideshow's radio button (top right) and its status; LVGL task */
#include "lvgl.h"
#include <stdbool.h>

void radio_ui_create(lv_obj_t *parent);
/* the slideshow on screen or not: the button with it, the radio too */
void radio_ui_slideshow(bool shown);
