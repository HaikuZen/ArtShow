#pragma once
/* the hamburger menu, top left: a list of pages, one shown at a time */
#include <stdbool.h>
#include "lvgl.h"

typedef struct {
    const char *label;                 /* in the menu (may start with an LV_SYMBOL) */
    lv_obj_t *page;                    /* shown over the screen; NULL: the screen itself (the slideshow) */
    void (*shown)(bool shown);         /* told when it's shown or hidden, in the LVGL task; may be NULL */
    uint32_t color;                    /* its icon's colour in the menu, 0xRRGGBB (the page's own); 0: grey */
} menu_page_t;

/* the button and its list; pages[0] is shown first. Call holding the LVGL lock,
 * after everything else on parent: the button stays on top */
void menu_create(lv_obj_t *parent, const menu_page_t *pages, int n);
/* show page k, as if picked in the menu (in the LVGL task) */
void menu_select(int k);
