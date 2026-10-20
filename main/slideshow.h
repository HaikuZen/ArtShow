#pragma once
/* the picture slideshow: the JPEGs on the microSD card, full screen, one after
 * the other. Tap the left or right third of the screen for the previous or
 * next picture */
#include <stdbool.h>
#include "lvgl.h"

/* bigger picture files are skipped: each is read whole into PSRAM */
#define SLIDESHOW_MAX_JPEG (12 * 1024 * 1024)

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
/* walk the card again and bring the index (index.c) up to date; the
 * slideshow's task does it, between pictures; a new list starts from its
 * first picture */
void slideshow_rescan(void);
/* a picture just put on the card (Artwork of the Day): into the index and
 * the list, without a rescan */
void slideshow_added(const char *path);
/* show this picture next, at once (the Gallery), and go on from there */
void slideshow_jump(const char *path);
/* the thumbnails made while paused (the Gallery's): off while another page
 * needs the CPU and PSRAM (the Generative page) */
void slideshow_background(bool on);
/* hold the pictures while the screen is off for the night (night.c) */
void slideshow_sleep(bool sleep);
/* the file of the picture on show (its full path); false: none yet */
bool slideshow_now(char *path, size_t n);
/* the folders with pictures under the card's top ("" for the top itself) and
 * how many each holds, up to max; 0 until the card is listed */
int slideshow_folders(char names[][64], int *counts, int max);
int slideshow_count(void);   /* pictures on the card */
/* what the slideshow shows: 1024x600 RGB565, rows of 1024 pixels; NULL before
 * it starts. Read it in the LVGL task, after transition_finish() */
const uint16_t *slideshow_screen(void);
