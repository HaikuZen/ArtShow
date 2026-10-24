#pragma once
/* the slideshow's radio button (top right) and its status; LVGL task */
#include "lvgl.h"
#include <stdbool.h>

void radio_ui_create(lv_obj_t *parent);
/* the slideshow on screen or not: the button with it, the radio too */
void radio_ui_slideshow(bool shown);
/* another page with the radio (it plays on, if it's on), shown or hidden */
void radio_ui_here(bool here);
/* night: the radio off, as if its button were tapped (only a tap turns it on again) */
void radio_ui_off(void);
