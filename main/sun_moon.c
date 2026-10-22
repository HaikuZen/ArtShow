/* sun_moon.c - the page: the sky as it is now, painted (sky_paint.c) like a
 * woodblock print, full screen, by a task, with the day's numbers on it. The day's
 * numbers (two astro_day and the sun's path: ~0.6 s of software doubles,
 * the P4's FPU is single precision) are worked out when it's shown on a new
 * day or for a new place; each minute meanwhile the sun and the moon move
 * and the picture is painted again. The text: the times of sunrise, noon
 * and sunset on the horizon where they happen; the title in a cartouche, as
 * a print's; the rest a caption on the near hill */
#include "sun_moon.h"
#include "astro.h"
#include "sky_paint.h"
#include "weather.h"
#include "time_sync.h"
#include "fonts/art_fonts.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define W           1024
#define H           600
#define PAPER       0xF2E8D2
#define PAPER_TEXT  0xF4ECD8
#define INK         0x2A2420
#define SEAL        0xA8322A    /* the cartouche's border: a seal's red */
#define MINOR       0xC8BCA8
#define GOLDEN      0xFFB45A
#define MOON_X      546         /* the caption's moon */
#define MOON_Y      540

static const char *TAG = "sunmoon";

static lv_obj_t *s_page, *s_img, *s_sub, *s_note;
static lv_obj_t *s_rise, *s_noon, *s_set;
static lv_obj_t *s_length, *s_change, *s_golden, *s_season, *s_phase, *s_moon_rs, *s_full, *s_new;
static lv_obj_t *s_body;   /* the text that needs a place and a clock */
static lv_image_dsc_t s_dsc;
static uint16_t *s_px;
static lv_timer_t *s_tick;

static astro_day_t s_day;
static time_t s_day_start, s_day_end;   /* the day worked out; 0: none */
static double s_lat = NAN, s_lon = NAN;
static sky_t s_sky;            /* the day's part (the sun's path, the place): the LVGL task's */

/* the painter: a task, not the LVGL task (a picture is ~0.25 s on the
   board). It paints in place: a minute later the picture differs by a few
   pixels, so a half-painted one never shows */
static TaskHandle_t s_painter;
static SemaphoreHandle_t s_job_lock;
static struct {
    sky_t sky;
    double lat, lon;
    time_t now;
    bool day;              /* a place and a clock: else a night without a moon */
} s_job;

/* ---- the numbers ---- */

static const char *phase_name(double e) {
    if (e < 7 || e > 353) return "New Moon";
    if (fabs(e - 90) < 7) return "First Quarter";
    if (fabs(e - 180) < 7) return "Full Moon";
    if (fabs(e - 270) < 7) return "Last Quarter";
    return e < 90 ? "Waxing Crescent" : e < 180 ? "Waxing Gibbous" : e < 270 ? "Waning Gibbous" : "Waning Crescent";
}

static void hm(char *buf, size_t n, time_t t) {
    if (!t) {
        snprintf(buf, n, "\xE2\x80\x94");   /* an em dash: not today */
        return;
    }
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%H:%M", &tm);
}

/* "Sun 25 Oct, 19:12" */
static void when(char *buf, size_t n, time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%a %e %b, %H:%M", &tm);
}

static time_t midnight(time_t t, int add_days) {
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_mday += add_days;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/* a time on the horizon, under where it happens (t's sun azimuth) */
static void on_horizon(lv_obj_t *l, time_t t, int row) {
    lv_obj_set_hidden(l, !t);
    if (!t) return;
    double alt, az;
    float x, y;
    astro_sun_pos(s_lat, s_lon, t, &alt, &az);
    sky_xy(&s_sky, az, 0, W, H, &x, &y);
    lv_obj_update_layout(l);
    int lw = lv_obj_get_width(l);
    int lx = (int)x - lw / 2;
    if (lx < 84) lx = 84;   /* clear of the corners */
    if (lx > W - 84 - lw) lx = W - 84 - lw;
    lv_obj_set_pos(l, lx, sky_horizon(H) + 34 + row * 36);
}

/* the whole day: once a day, or for a new place */
static void work_out_day(time_t now) {
    int64_t t0 = esp_timer_get_time();
    s_day_start = midnight(now, 0);
    s_day_end = midnight(now, 1);
    astro_day(s_lat, s_lon, s_day_start, s_day_end, now, &s_day);
    for (int k = 0; k < SKY_TRACK; k++) {
        double alt, az;
        astro_sun_pos(s_lat, s_lon, s_day_start + (s_day_end - s_day_start) * k / (SKY_TRACK - 1), &alt, &az);
        s_sky.track_alt[k] = alt;
        s_sky.track_az[k] = az;
    }
    s_sky.ntrack = SKY_TRACK;
    s_sky.lat = s_lat;
    s_sky.place = (uint32_t)lround(s_lat * 100) * 40000u + (uint32_t)lround(s_lon * 100 + 20000);
    s_sky.day = (uint32_t)(s_day_start / 86400);
    astro_day_t y;   /* yesterday, for the day length's change */
    astro_day(s_lat, s_lon, midnight(now, -1), s_day_start, now, &y);
    ESP_LOGI(TAG, "worked out in %d ms", (int)((esp_timer_get_time() - t0) / 1000));

    char a[32], b[32], c[32];
    hm(a, sizeof a, s_day.sunrise);
    lv_label_set_text_fmt(s_rise, "sunrise  %s", a);
    hm(a, sizeof a, s_day.noon);
    lv_label_set_text_fmt(s_noon, "noon  %s  \xC2\xB7  %d\xC2\xB0 high", a, (int)lround(s_day.noon_alt));
    hm(a, sizeof a, s_day.sunset);
    lv_label_set_text_fmt(s_set, "sunset  %s", a);
    /* the noon one a row lower: the rise and set ones may come near it */
    on_horizon(s_rise, s_day.sunrise, 0);
    on_horizon(s_noon, s_day.noon && s_day.noon_alt > 0 ? s_day.noon : 0, 1);
    on_horizon(s_set, s_day.sunset, 0);

    if (s_day.sunrise && s_day.sunset && s_day.sunset > s_day.sunrise) {
        long len = s_day.sunset - s_day.sunrise, d = len - (y.sunset && y.sunrise ? y.sunset - y.sunrise : len);
        long ad = labs(d);
        if (ad >= 60) snprintf(c, sizeof c, "%ld min %ld s", ad / 60, ad % 60);
        else snprintf(c, sizeof c, "%ld s", ad);
        lv_label_set_text_fmt(s_length, "Day length %ld h %ld min", len / 3600, len % 3600 / 60);
        lv_label_set_text_fmt(s_change, "%s %s than yesterday", c, d < 0 ? "shorter" : "longer");
    } else {
        lv_label_set_text(s_length, s_day.up_all_day ? "The sun doesn't set today" : "The sun doesn't rise today");
        lv_label_set_text(s_change, "");
    }
    if (s_day.golden_am[0] || s_day.golden_pm[1]) {
        hm(a, sizeof a, s_day.golden_am[0]);
        hm(b, sizeof b, s_day.golden_am[1]);
        char d[16], e[16];
        hm(d, sizeof d, s_day.golden_pm[0]);
        hm(e, sizeof e, s_day.golden_pm[1]);
        lv_label_set_text_fmt(s_golden, "Golden hours  %s\xE2\x80\x93%s  and  %s\xE2\x80\x93%s", a, b, d, e);
    } else {
        lv_label_set_text(s_golden, "No golden hour today");
    }

    hm(a, sizeof a, s_day.moonrise);
    hm(b, sizeof b, s_day.moonset);
    lv_label_set_text_fmt(s_moon_rs, "Moonrise %s  \xC2\xB7  Moonset %s", a, b);
    when(a, sizeof a, s_day.next_full);
    when(b, sizeof b, s_day.next_new);
    lv_label_set_text_fmt(s_full, "Full moon  %s", a);
    lv_label_set_text_fmt(s_new, "New moon  %s", b);

    static const char *north[] = { "Spring equinox", "Summer solstice", "Autumn equinox", "Winter solstice" };
    static const char *south[] = { "Autumn equinox", "Winter solstice", "Spring equinox", "Summer solstice" };
    long days = lround((double)(midnight(s_day.next_season, 0) - s_day_start) / 86400);
    when(a, sizeof a, s_day.next_season);
    lv_label_set_text_fmt(s_season, "%s  %s, in %ld day%s", (s_lat >= 0 ? north : south)[s_day.season], a, days,
                          days == 1 ? "" : "s");
}

static void painter(void *arg) {
    static sky_t sky;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_px) continue;
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
        sky = s_job.sky;
        double lat = s_job.lat, lon = s_job.lon;
        time_t now = s_job.now;
        bool day = s_job.day;
        xSemaphoreGive(s_job_lock);
        int64_t t0 = esp_timer_get_time();
        if (day) {
            double alt, az, lit;
            astro_sun_pos(lat, lon, now, &alt, &az);
            sky.sun_alt = alt;
            sky.sun_az = az;
            astro_moon_pos(lat, lon, now, &alt, &az);
            sky.moon_alt = alt;
            sky.moon_az = az;
            sky.moon_elong = astro_moon_phase(now, &lit);
            sky.moon_lit = lit;
        } else {
            memset(&sky, 0, sizeof sky);
            sky.sun_alt = -30;
            sky.moon_alt = -90;
            sky.lat = 45;
        }
        sky_paint(&sky, s_px, W, H);
        if (day) sky_paint_moon(&sky, s_px, W, H, MOON_X, MOON_Y, 22);   /* the caption's: up or not */
        ESP_LOGD(TAG, "painted in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
        lvgl_port_lock(0);
        lv_obj_invalidate(s_img);
        lvgl_port_unlock();
    }
}

/* the picture, now: the painter's job */
static void paint(time_t now) {
    if (!s_painter) return;
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_job.sky = s_sky;
    s_job.lat = s_lat;
    s_job.lon = s_lon;
    s_job.now = now;
    s_job.day = s_day_start != 0;
    xSemaphoreGive(s_job_lock);
    xTaskNotifyGive(s_painter);
}

/* each minute: the sky; the whole day if it's a new one, or the place moved */
static void update(void) {
    double lat, lon;
    char place[64];
    bool have = weather_place(&lat, &lon, place, sizeof place);
    const char *why = !have ? "The sun and moon need your place: it comes with the weather (a provider in "
                              "config.json), or set \"location\" there to \"lat,lon\""
                    : !time_synced() ? "Waiting for the clock" : NULL;
    lv_obj_set_hidden(s_note, !why);
    lv_obj_set_hidden(s_body, why);
    time_t now = time(NULL);
    if (why) {
        lv_label_set_text(s_note, why);
        lv_label_set_text(s_sub, "");
        s_day_start = 0;
        paint(now);
        return;
    }
    if (lat != s_lat || lon != s_lon || now >= s_day_end || now < s_day_start) {
        s_lat = lat;
        s_lon = lon;
        work_out_day(now);
    }
    struct tm tm;
    localtime_r(&now, &tm);
    char date[48];
    strftime(date, sizeof date, "%A %e %B %Y", &tm);
    lv_label_set_text_fmt(s_sub, "%s  \xC2\xB7  %s", place[0] ? place : "Here", date);
    double lit;
    double e = astro_moon_phase(now, &lit);
    lv_label_set_text_fmt(s_phase, "%s, %d%% lit", phase_name(e), (int)lround(lit * 100));
    paint(now);
}

static void tick_cb(lv_timer_t *t) { update(); }

void sun_moon_shown(bool shown) {
    if (shown) {
        update();
        lv_timer_resume(s_tick);
    } else {
        lv_timer_pause(s_tick);
    }
}

/* ---- the page ---- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t x, int32_t y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    lv_obj_set_pos(l, x, y);
    return l;
}

/* a caption line: kept to its column and to one line (dots if too long:
   LVGL only shortens a label of a fixed height, else it wraps over the next) */
static lv_obj_t *line(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t x, int32_t y, int32_t w) {
    lv_obj_t *l = label(parent, font, color, x, y);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    return l;
}

/* a time on the horizon: a small dark tab, readable on any sky */
static lv_obj_t *tab(lv_obj_t *parent) {
    lv_obj_t *l = label(parent, &art_font_16, PAPER_TEXT, 0, 0);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_40, 0);
    lv_obj_set_style_radius(l, 4, 0);
    lv_obj_set_style_pad_hor(l, 8, 0);
    lv_obj_set_style_pad_ver(l, 3, 0);
    return l;
}

lv_obj_t *sun_moon_create(lv_obj_t *parent) {
    lv_obj_t *page = s_page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(page, false);

    /* the picture: an lv_image on a canvas in PSRAM (64-byte aligned for the PPA) */
    s_px = heap_caps_aligned_alloc(64, W * H * 2, MALLOC_CAP_SPIRAM);
    s_img = lv_image_create(page);
    if (s_px) {
        s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_dsc.header.w = W;
        s_dsc.header.h = H;
        s_dsc.header.stride = W * 2;
        s_dsc.data = (const uint8_t *)s_px;
        s_dsc.data_size = W * H * 2;
        lv_image_set_src(s_img, &s_dsc);
    } else {
        ESP_LOGE(TAG, "no memory for the picture");
    }

    /* the cartouche, top right, as on a print */
    lv_obj_t *cart = lv_obj_create(page);
    lv_obj_remove_style_all(cart);
    lv_obj_set_size(cart, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(cart, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(cart, LV_OPA_90, 0);
    lv_obj_set_style_border_color(cart, lv_color_hex(SEAL), 0);
    lv_obj_set_style_border_width(cart, 2, 0);
    lv_obj_set_style_radius(cart, 2, 0);
    lv_obj_set_style_pad_hor(cart, 16, 0);
    lv_obj_set_style_pad_ver(cart, 8, 0);
    lv_obj_set_flex_flow(cart, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_flex_cross_place(cart, LV_FLEX_ALIGN_END, 0);
    lv_obj_set_style_pad_row(cart, 2, 0);
    lv_obj_set_clickable(cart, false);
    lv_obj_align(cart, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_t *title = lv_label_create(cart);
    lv_label_set_text(title, "Sun and Moon");
    lv_obj_set_style_text_font(title, &art_font_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(INK), 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    s_sub = lv_label_create(cart);
    lv_label_set_text(s_sub, "");
    lv_obj_set_style_text_font(s_sub, &art_font_16, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(0x5A4E44), 0);

    s_note = label(page, &art_font_24, PAPER_TEXT, 0, 0);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_note, 760);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_note, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_note, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_note, 8, 0);
    lv_obj_set_style_pad_all(s_note, 16, 0);
    lv_obj_align(s_note, LV_ALIGN_CENTER, 0, -40);

    s_body = lv_obj_create(page);   /* the rest: hidden while there's no place or clock */
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_clickable(s_body, false);

    s_rise = tab(s_body);
    s_noon = tab(s_body);
    s_set = tab(s_body);

    /* the caption, on the near hill: the sun's day left, the moon right of its small disc */
    /* four lines a side, under the horizon's tabs (two rows from 434): a
       headline, then three */
    const int y0 = 500, dy = 22, lw = MOON_X - 30 - 84, rw = 940 - MOON_X - 34;
    s_length = line(s_body, &art_font_20, PAPER_TEXT, 84, y0, lw);
    s_change = line(s_body, &art_font_16, MINOR, 84, y0 + 4 + dy, lw);
    s_golden = line(s_body, &art_font_16, GOLDEN, 84, y0 + 4 + 2 * dy, lw);
    s_season = line(s_body, &art_font_16, MINOR, 84, y0 + 4 + 3 * dy, lw);
    s_phase = line(s_body, &art_font_20, PAPER_TEXT, MOON_X + 34, y0, rw);
    s_moon_rs = line(s_body, &art_font_16, MINOR, MOON_X + 34, y0 + 4 + dy, rw);
    s_full = line(s_body, &art_font_16, MINOR, MOON_X + 34, y0 + 4 + 2 * dy, rw);
    s_new = line(s_body, &art_font_16, MINOR, MOON_X + 34, y0 + 4 + 3 * dy, rw);

    s_job_lock = xSemaphoreCreateMutex();
    if (s_px && s_job_lock && xTaskCreatePinnedToCore(painter, "sky", 4096, NULL, 1, &s_painter, 1) != pdPASS) s_painter = NULL;
    s_tick = lv_timer_create(tick_cb, 60 * 1000, NULL);
    lv_timer_pause(s_tick);
    lv_obj_set_hidden(page, true);
    return page;
}
