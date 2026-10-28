#pragma once
/* the screen at night: between sunset and sunrise (the Sun and Moon page's
 * times), or between set hours (the Settings page), the backlight goes to the
 * night level once the screen has been left alone for 2 minutes; a touch
 * brings it back. With the night level at 0 the backlight is off and the
 * slideshow sleeps */
#include <stdbool.h>
#include <time.h>

/* start the check (a timer in the LVGL task); call holding the LVGL lock */
void night_start(void);
/* the settings changed: apply them now, not at the next change of state */
void night_refresh(void);
/* the screen dimmed (or off) for the night now */
bool night_dark(void);
/* today's sunrise and sunset, as night mode uses them; false: no place yet,
 * no clock, or no rise or set today */
bool night_sun_times(time_t *rise, time_t *set);
