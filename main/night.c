/* night.c - night mode: a 1 s LVGL timer compares the time with the night
 * (set hours, or today's sunset to sunrise from astro.c, worked out once a
 * day) and how long the screen has gone untouched (LVGL keeps that). */
#include "night.h"
#include "settings.h"
#include "astro.h"
#include "weather.h"
#include "time_sync.h"
#include "board.h"
#include "brightness_ui.h"
#include "slideshow.h"
#include "music.h"
#include "lvgl.h"
#include "esp_log.h"
#include <math.h>

#define WAKE_MS (2 * 60 * 1000)   /* lit this long after a touch */

static const char *TAG = "night";

static bool s_dark, s_fresh = true;   /* fresh: apply the state even if unchanged */
static time_t s_day_start, s_rise, s_set;
static double s_lat = NAN, s_lon = NAN;

static time_t midnight(time_t t, int add_days) {
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_mday += add_days;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

bool night_sun_times(time_t *rise, time_t *set) {
    double lat, lon;
    if (!time_synced() || !weather_place(&lat, &lon, NULL, 0)) return false;
    time_t now = time(NULL), day = midnight(now, 0);
    if (day != s_day_start || lat != s_lat || lon != s_lon) {   /* once a day: ~0.3 s */
        astro_day_t a;
        astro_day(lat, lon, day, midnight(now, 1), now, &a);
        s_day_start = day;
        s_lat = lat;
        s_lon = lon;
        s_rise = a.sunrise;
        s_set = a.sunset;
    }
    *rise = s_rise;
    *set = s_set;
    return s_rise && s_set;
}

static bool is_night(void) {
    if (!time_synced()) return false;
    time_t now = time(NULL);
    switch (g_settings.night_mode) {
    case NIGHT_HOURS: {
        struct tm tm;
        localtime_r(&now, &tm);
        int m = tm.tm_hour * 60 + tm.tm_min, f = g_settings.night_from, t = g_settings.night_to;
        return f < t ? m >= f && m < t : f > t ? m >= f || m < t : false;   /* 23:00-07:00 crosses midnight */
    }
    case NIGHT_SUN: {
        time_t rise, set;
        return night_sun_times(&rise, &set) && (now >= set || now < rise);
    }
    default:
        return false;
    }
}

static void check_cb(lv_timer_t *t) {
    bool dark = is_night() && lv_display_get_inactive_time(NULL) > WAKE_MS;
    if (dark == s_dark && !s_fresh) return;
    s_dark = dark;
    s_fresh = false;
    int level = dark ? g_settings.night_level : brightness_ui_level();
    board_set_brightness(level);
    slideshow_sleep(dark && level == 0);   /* nobody sees it: no reading, no decoding */
    music_quiet(dark);                     /* nor hears it: the Generative page's music stops at night */
    ESP_LOGI(TAG, "%s: backlight %d %%", dark ? "night" : "day", level);
}

void night_refresh(void) {
    s_fresh = true;
    check_cb(NULL);
}

void night_start(void) {
    lv_timer_create(check_cb, 1000, NULL);
    s_fresh = false;   /* the brightness is set already: day until the first check says otherwise */
}
