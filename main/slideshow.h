#pragma once
/* the picture slideshow: the JPEGs on the microSD card, full screen, one after
 * the other. Tap the left or right third of the screen for the previous or
 * next picture */
#include <stdbool.h>
#include "lvgl.h"

/* mount the card; call once, BEFORE wifi_start(). The card shares the SDMMC
 * host with esp_hosted's link to the C6, and a card init (its clock changes)
 * while that link runs breaks it: esp_hosted restarts the board. So no card
 * now means none until a restart. false: no card */
bool slideshow_mount(void);
/* build the screen and start the slideshow's task; call holding the LVGL lock */
void slideshow_start(lv_obj_t *parent);
/* hold the next picture back while another page covers the slideshow (the
 * next one still loads); resumed, it shows at once */
void slideshow_pause(bool pause);
/* the file of the picture on show (its full path); false: none yet */
bool slideshow_now(char *path, size_t n);
/* what the slideshow shows: 1024x600 RGB565, rows of 1024 pixels; NULL before
 * it starts. Read it in the LVGL task, after transition_finish() */
const uint16_t *slideshow_screen(void);
