/* sun_moon.c - the page. The day's numbers (two astro_day: ~0.56 s of
 * software doubles, the P4's FPU is single precision) are worked out when
 * it's shown on a new day or for a new place;
 * each minute meanwhile only the sun's dot and the moon's phase move. */
#include "sun_moon.h"
#include "astro.h"
#include "weather.h"
#include "time_sync.h"
#include "fonts/art_fonts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BG          0x0D1B2A
#define CARD_BG     0x1B263B
#define RIM         0x415A77
#define MINOR       0x778DA9
#define TEXT        0xE0F0FF
#define SUN         0xFFC93C
#define GOLDEN      0xFF9F43
#define MOON_LIT    0xE8E4C9
#define MOON_DARK   0x26324A
#define CHART_W     476
#define CHART_H     150
#define SAMPLES     97      /* the sun's altitude every 15 minutes */
#define MOON_D      170

static const char *TAG = "sunmoon";

static lv_obj_t *s_page, *s_sub, *s_note, *s_body, *s_chart, *s_moon;
static lv_obj_t *s_rise, *s_noon, *s_set, *s_length, *s_golden, *s_noon_alt;
static lv_obj_t *s_phase, *s_lit_label, *s_moon_rs, *s_full, *s_new, *s_season;
static lv_timer_t *s_tick;

static astro_day_t s_day;
static time_t s_day_start, s_day_end;   /* the day worked out; 0: none */
static double s_lat = NAN, s_lon = NAN;
static float s_alt[SAMPLES];            /* the chart's curve */
static double s_lit, s_elong;           /* the moon now */
static time_t s_now;

/* ---- drawing ---- */

static void line(lv_layer_t *layer, float x1, float y1, float x2, float y2, int w, uint32_t c) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.width = w;
    d.color = lv_color_hex(c);
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

static void rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t radius, uint32_t c) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(c);
    d.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

/* the sun's altitude through the day: above the horizon in gold, below dim;
   a dot for now */
static void chart_draw(lv_event_t *e) {
    if (!s_day_start) return;
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_content_coords(o, &a);
    float top = s_day.noon_alt > 10 ? s_day.noon_alt + 6 : 16, bottom = top - 100;   /* 100 degrees tall */
    float lo = 1e9;
    for (int k = 0; k < SAMPLES; k++) lo = fminf(lo, s_alt[k]);
    if (lo - 4 > bottom) bottom = lo - 4;
    float w = lv_area_get_width(&a) - 1, h = lv_area_get_height(&a) - 1;
#define X(k) (a.x1 + (k) * w / (SAMPLES - 1))
#define Y(alt) (a.y1 + (top - (alt)) * h / (top - bottom))
    line(layer, a.x1, Y(0), a.x2, Y(0), 1, MINOR);   /* the horizon */
    for (int k = 0; k + 1 < SAMPLES; k++) {
        bool up = s_alt[k] > 0 || s_alt[k + 1] > 0;
        line(layer, X(k), Y(s_alt[k]), X(k + 1), Y(s_alt[k + 1]), up ? 3 : 2, up ? SUN : RIM);
    }
    if (s_now >= s_day_start && s_now < s_day_end) {
        float f = (float)(s_now - s_day_start) / (s_day_end - s_day_start) * (SAMPLES - 1);
        float alt = astro_sun_alt(s_lat, s_lon, s_now);
        int32_t x = X(f), y = Y(alt);
        rect(layer, x - 7, y - 7, x + 7, y + 7, LV_RADIUS_CIRCLE, alt > -0.833 ? SUN : RIM);
    }
#undef X
#undef Y
}

/* the moon: a dark disk, and its lit part row by row: from the limb to the
   terminator (an ellipse, half as wide as cos of the phase). Waxing, the
   right side is lit, seen from the northern hemisphere */
static void moon_draw(lv_event_t *e) {
    if (!s_day_start) return;
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float r = lv_area_get_width(&a) / 2.0f, cx = a.x1 + r, cy = a.y1 + r;
    rect(layer, a.x1, a.y1, a.x2, a.y2, LV_RADIUS_CIRCLE, MOON_DARK);
    bool right = (s_elong < 180) == (s_lat >= 0);   /* the lit side */
    float c = 1 - 2 * (float)s_lit;                 /* the terminator, as a share of the half width */
    for (int32_t y = a.y1 + 1; y < a.y2; y++) {
        float dy = y + 0.5f - cy, half = sqrtf(fmaxf(r * r - dy * dy, 0));
        float x1 = right ? cx + c * half : cx - half, x2 = right ? cx + half : cx - c * half;
        if (x2 - x1 >= 1) rect(layer, lroundf(x1), y, lroundf(x2) - 1, y, 0, MOON_LIT);
    }
}

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

/* the whole day: once a day, or for a new place */
static void work_out_day(time_t now) {
    int64_t t0 = esp_timer_get_time();
    s_day_start = midnight(now, 0);
    s_day_end = midnight(now, 1);
    astro_day(s_lat, s_lon, s_day_start, s_day_end, now, &s_day);
    for (int k = 0; k < SAMPLES; k++)
        s_alt[k] = astro_sun_alt(s_lat, s_lon, s_day_start + (s_day_end - s_day_start) * k / (SAMPLES - 1));
    astro_day_t y;   /* yesterday, for the day length's change */
    astro_day(s_lat, s_lon, midnight(now, -1), s_day_start, now, &y);
    ESP_LOGI(TAG, "worked out in %d ms", (int)((esp_timer_get_time() - t0) / 1000));

    char a[32], b[32], c[32];
    hm(a, sizeof a, s_day.sunrise);
    lv_label_set_text(s_rise, a);
    hm(a, sizeof a, s_day.noon);
    lv_label_set_text(s_noon, a);
    hm(a, sizeof a, s_day.sunset);
    lv_label_set_text(s_set, a);

    if (s_day.sunrise && s_day.sunset && s_day.sunset > s_day.sunrise) {
        long len = s_day.sunset - s_day.sunrise, d = len - (y.sunset && y.sunrise ? y.sunset - y.sunrise : len);
        long ad = labs(d);
        if (ad >= 60) snprintf(c, sizeof c, "%ld min %ld s", ad / 60, ad % 60);
        else snprintf(c, sizeof c, "%ld s", ad);
        lv_label_set_text_fmt(s_length, "Day length %ld h %ld min   \xC2\xB7   %s %s than yesterday", len / 3600,
                              len % 3600 / 60, c, d < 0 ? "shorter" : "longer");
    } else {
        lv_label_set_text(s_length, s_day.up_all_day ? "The sun doesn't set today" : "The sun doesn't rise today");
    }
    if (s_day.golden_am[0] || s_day.golden_pm[1]) {
        hm(a, sizeof a, s_day.golden_am[0]);
        hm(b, sizeof b, s_day.golden_am[1]);
        char d[16], e[16];
        hm(d, sizeof d, s_day.golden_pm[0]);
        hm(e, sizeof e, s_day.golden_pm[1]);
        lv_label_set_text_fmt(s_golden, "Golden hours %s\xE2\x80\x93%s and %s\xE2\x80\x93%s", a, b, d, e);
    } else {
        lv_label_set_text(s_golden, "No golden hour today");
    }
    lv_label_set_text_fmt(s_noon_alt, "At noon the sun is %d\xC2\xB0 high", (int)lround(s_day.noon_alt));

    hm(a, sizeof a, s_day.moonrise);
    hm(b, sizeof b, s_day.moonset);
    lv_label_set_text_fmt(s_moon_rs, "Moonrise %s   \xC2\xB7   Moonset %s", a, b);
    when(a, sizeof a, s_day.next_full);
    when(b, sizeof b, s_day.next_new);
    lv_label_set_text_fmt(s_full, "Full moon   %s", a);
    lv_label_set_text_fmt(s_new, "New moon   %s", b);

    static const char *north[] = { "Spring equinox", "Summer solstice", "Autumn equinox", "Winter solstice" };
    static const char *south[] = { "Autumn equinox", "Winter solstice", "Spring equinox", "Summer solstice" };
    long days = lround((double)(midnight(s_day.next_season, 0) - s_day_start) / 86400);
    when(a, sizeof a, s_day.next_season);
    lv_label_set_text_fmt(s_season, "%s   %s   \xE2\x80\x94   in %ld day%s", (s_lat >= 0 ? north : south)[s_day.season], a,
                          days, days == 1 ? "" : "s");
    lv_obj_invalidate(s_chart);
}

/* each minute: the moon's phase and the sun's dot; the whole day if it's a
   new one, or the place moved */
static void update(void) {
    double lat, lon;
    char place[64];
    bool have = weather_place(&lat, &lon, place, sizeof place);
    const char *why = !have ? "The sun and moon need your place: it comes with the weather (a provider in "
                              "config.json), or set \"location\" there to \"lat,lon\""
                    : !time_synced() ? "Waiting for the clock" : NULL;
    lv_obj_set_hidden(s_note, !why);
    lv_obj_set_hidden(s_body, why);
    if (why) {
        lv_label_set_text(s_note, why);
        return;
    }
    time_t now = time(NULL);
    if (lat != s_lat || lon != s_lon || now >= s_day_end || now < s_day_start) {
        s_lat = lat;
        s_lon = lon;
        work_out_day(now);
    }
    s_now = now;
    struct tm tm;
    localtime_r(&now, &tm);
    char date[48];
    strftime(date, sizeof date, "%A %e %B %Y", &tm);
    lv_label_set_text_fmt(s_sub, "%s   \xC2\xB7   %s", place[0] ? place : "Here", date);

    s_elong = astro_moon_phase(now, &s_lit);   /* the moon now: one sun and one moon position */
    lv_label_set_text(s_phase, phase_name(s_elong));
    lv_label_set_text_fmt(s_lit_label, "%d%% lit, %s", (int)lround(s_lit * 100), s_elong < 180 ? "waxing" : "waning");
    lv_obj_invalidate(s_moon);
    lv_obj_invalidate(s_chart);
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

/* a caption over a big time: "Sunrise" / "07:38" */
static lv_obj_t *big_time(lv_obj_t *parent, const char *caption, int32_t x, int32_t y) {
    lv_label_set_text(label(parent, &art_font_16, MINOR, x, y), caption);
    return label(parent, &art_font_32, TEXT, x, y + 20);
}

lv_obj_t *sun_moon_create(lv_obj_t *parent) {
    lv_obj_t *page = s_page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollable(page, false);

    lv_label_set_text(label(page, &art_font_24, TEXT, 90, 12), "Sun and Moon");   /* right of the menu button */
    s_sub = label(page, &art_font_16, MINOR, 91, 44);

    s_note = label(page, &art_font_24, TEXT, 0, 0);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_note, 760);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_note, LV_ALIGN_CENTER, 0, 0);

    s_body = lv_obj_create(page);   /* all of the rest, hidden while there's no place or clock */
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_clickable(s_body, false);

    /* the sun */
    s_chart = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_chart);
    lv_obj_set_size(s_chart, CHART_W, CHART_H);
    lv_obj_set_pos(s_chart, 24, 80);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(CARD_BG), 0);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_chart, 12, 0);
    lv_obj_set_style_pad_all(s_chart, 10, 0);
    lv_obj_add_event_cb(s_chart, chart_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    static const char *hours[] = { "06:00", "12:00", "18:00" };
    for (int k = 0; k < 3; k++)   /* under the chart, at its quarters */
        lv_label_set_text(label(s_body, &art_font_16, MINOR, 24 + 10 + (CHART_W - 20) * (k + 1) / 4 - 20, 80 + CHART_H + 4),
                          hours[k]);
    s_rise = big_time(s_body, "Sunrise", 24, 258);
    s_noon = big_time(s_body, "Solar noon", 24 + 166, 258);
    s_set = big_time(s_body, "Sunset", 24 + 332, 258);
    s_length = label(s_body, &art_font_20, TEXT, 24, 330);
    s_golden = label(s_body, &art_font_20, GOLDEN, 24, 362);
    s_noon_alt = label(s_body, &art_font_16, MINOR, 24, 394);

    /* the moon */
    s_moon = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_moon);
    lv_obj_set_size(s_moon, MOON_D, MOON_D);
    lv_obj_set_pos(s_moon, 548, 80);
    lv_obj_add_event_cb(s_moon, moon_draw, LV_EVENT_DRAW_MAIN, NULL);
    s_phase = label(s_body, &art_font_32, TEXT, 548 + MOON_D + 28, 96);
    lv_label_set_long_mode(s_phase, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_phase, 1000 - (548 + MOON_D + 28));
    s_lit_label = label(s_body, &art_font_24, TEXT, 548 + MOON_D + 28, 186);
    s_moon_rs = label(s_body, &art_font_20, TEXT, 548, 278);
    s_full = label(s_body, &art_font_20, TEXT, 548, 330);
    s_new = label(s_body, &art_font_20, TEXT, 548, 362);

    /* the season, across the bottom */
    s_season = label(s_body, &art_font_24, TEXT, 24, 448);

    s_tick = lv_timer_create(tick_cb, 60 * 1000, NULL);
    lv_timer_pause(s_tick);
    lv_obj_set_hidden(page, true);
    return page;
}
