#pragma once
/* the demo tour: with g_settings.demo on, the app shows itself, page after
 * page, as a scripted loop (to film it). A touch pauses it: it starts again
 * from the top after 2 minutes untouched. Never in the dark of night mode */
#include <stdbool.h>

/* start the tour's timer (in the LVGL task); call holding the LVGL lock,
 * after menu_create */
void demo_start(void);
