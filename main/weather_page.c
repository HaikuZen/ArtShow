/* weather_page.c - the weather as a woodblock print: the place's sky now
 * (sky_view.c: the same hills as the Sun and Moon page) with the weather in
 * it (clouds, rain, snow, fog, a storm), and on it: a paper cartouche top
 * left (the place, the temperature, the sky, feels like, humidity, wind,
 * when it was updated), the next 24 hours as a row of small paper cards to
 * swipe (WeatherAPI only), and a paper card per day (weekday, icon in a print's
 * inks, high and low, chance of rain). The sky is painted again each
 * minute while shown (the sun moves) and when a forecast comes */
#include "weather_page.h"
#include "weather_icon.h"
#include "sky_view.h"
#include "fonts/art_fonts.h"
#include "time_sync.h"
#include "esp_lvgl_port.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PAPER    0xF2E8D2
#define INK      0x2A2420
#define INK_SOFT 0x6A5E52
#define SEAL     0xA8322A
#define NOTE_TXT 0xE8E0CC
#define RAIN     0x2F5D8A
#define WARN     0xC2401F
#define DEG      "\xC2\xB0"

static lv_obj_t *s_cart, *s_place, *s_sub, *s_note, *s_now, *s_temp, *s_desc, *s_details, *s_days, *s_hours;
static struct { lv_obj_t *card, *name, *date, *icon, *hi, *lo, *pop; } s_day[WX_DAYS];
static struct { lv_obj_t *cell, *time, *icon, *temp, *pop; } s_hour[WX_HOURS];
static int s_nhours;         /* in the forecast on show */
static char s_updated[64];   /* "Updated 13:05 · OpenWeatherMap" */
static bool s_have;          /* a forecast on show */
static wx_kind_t s_kind;     /* the sky now, for the picture */
static lv_timer_t *s_tick;
static bool s_shown;

static const char *WDAY[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ---- the picture ---- */

/* the weather in the sky: how covered, how wet, white, misty */
static void weather_sky(wx_kind_t k, sky_t *s) {
    s->cover = s->rain = s->snow = s->fog = 0;
    s->storm = false;
    switch (k) {
    case WX_CLEAR: s->cover = 0.1f; break;
    case WX_PARTLY: s->cover = 0.45f; break;
    case WX_CLOUDY: s->cover = 0.85f; break;
    case WX_FOG: s->cover = 0.6f, s->fog = 1; break;
    case WX_DRIZZLE: s->cover = 0.8f, s->rain = 0.35f; break;
    case WX_RAIN: s->cover = 0.9f, s->rain = 0.8f; break;
    case WX_SNOW: s->cover = 0.85f, s->snow = 1; break;
    case WX_STORM: s->cover = 1, s->rain = 1, s->storm = true; break;
    default: s->cover = 0.3f; break;
    }
}

static void paint(void) {
    double lat = 0, lon = 0;
    char name[8];
    bool place = weather_place(&lat, &lon, name, sizeof name);
    bool day = place && time_synced();
    time_t now = time(NULL);
    static sky_t sky;
    memset(&sky, 0, sizeof sky);
    sky.lat = place ? lat : 45;
    sky.place = place ? sky_view_place(lat, lon) : 1;
    sky.day = (uint32_t)(now / 86400);
    if (s_have) weather_sky(s_kind, &sky);
    sky_view_paint(&sky, lat, lon, now, day, 0, 0, 0);
}

static void tick_cb(lv_timer_t *t) { paint(); }

void weather_page_shown(bool shown) {
    s_shown = shown;
    if (shown) {
        paint();
        lv_timer_resume(s_tick);
    } else {
        lv_timer_pause(s_tick);
    }
}

/* ---- the page ---- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *plain(lv_obj_t *parent) {   /* a transparent box for layout */
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_clickable(o, false);
    return o;
}

static void refresh_cb(lv_event_t *e) {
    lv_label_set_text(s_sub, s_have ? "Updating..." : "");
    weather_refresh();
}

/* another provider (one with a key in config.json) */
static void provider_cb(lv_event_t *e) {
    lv_obj_t *dd = lv_event_get_current_target_obj(e);
    char name[32];
    lv_dropdown_get_selected_str(dd, name, sizeof name);
    lv_label_set_text_fmt(s_sub, "Updating from %s...", name);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(INK_SOFT), 0);
    weather_use_provider(lv_dropdown_get_selected(dd));
}

/* the providers with a key, as a dropdown left of the refresh button; none
   with only one */
static void provider_picker(lv_obj_t *page, lv_obj_t *refresh) {
    const char *names[4];
    int cur = 0, n = weather_providers(names, &cur);
    if (n < 2) return;
    char opts[96] = "";
    for (int k = 0; k < n; k++)
        snprintf(opts + strlen(opts), sizeof opts - strlen(opts), "%s%s", k ? "\n" : "", names[k]);
    lv_obj_t *dd = lv_dropdown_create(page);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, cur);
    lv_obj_set_width(dd, 230);
    lv_obj_set_style_text_font(dd, &art_font_20, 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(INK), 0);
    lv_obj_set_style_bg_color(dd, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_90, 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(SEAL), 0);
    lv_obj_set_style_border_width(dd, 2, 0);
    lv_obj_set_style_radius(dd, 2, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, &art_font_20, 0);
    lv_obj_set_style_text_color(list, lv_color_hex(INK), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(PAPER), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(SEAL), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0xE0D2B4), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_align_to(dd, refresh, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    lv_obj_add_event_cb(dd, provider_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* a paper card with a seal-red rule on top, as a print's label: a day's,
   an hour's */
static lv_obj_t *paper_card(lv_obj_t *parent, int32_t w, int32_t h) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_color(c, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);   /* the icons' moon bites the card's colour */
    lv_obj_set_style_border_color(c, lv_color_hex(SEAL), 0);
    lv_obj_set_style_border_width(c, 4, 0);
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(c, 2, 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 6, 0);
    lv_obj_set_style_pad_row(c, 2, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_clickable(c, false);
    return c;
}

/* a day's card: name, date, icon, high and low, rain: ~190 px of content */
static void day_card(lv_obj_t *parent, int k) {
    lv_obj_t *c = paper_card(parent, 116, 204);
    s_day[k].card = c;
    s_day[k].name = label(c, &art_font_24, INK);
    s_day[k].date = label(c, &art_font_16, INK_SOFT);
    s_day[k].icon = weather_icon_create(c, 56);
    weather_icon_print(s_day[k].icon, true);
    lv_obj_t *t = plain(c);   /* high and low side by side */
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(t, 8, 0);
    s_day[k].hi = label(t, &art_font_24, INK);
    s_day[k].lo = label(t, &art_font_24, INK_SOFT);
    s_day[k].pop = label(c, &art_font_16, RAIN);
    lv_obj_set_hidden(c, true);
}

/* an hour's card, like the days' but smaller: time, icon, temperature, rain */
static void hour_cell(lv_obj_t *parent, int k) {
    lv_obj_t *c = paper_card(parent, 72, 128);
    lv_obj_set_style_pad_all(c, 4, 0);
    lv_obj_set_style_pad_top(c, 6, 0);
    lv_obj_set_style_pad_row(c, 0, 0);
    s_hour[k].cell = c;
    s_hour[k].time = label(c, &art_font_16, INK_SOFT);
    s_hour[k].icon = weather_icon_create(c, 38);
    weather_icon_print(s_hour[k].icon, true);
    s_hour[k].temp = label(c, &art_font_20, INK);
    s_hour[k].pop = label(c, &art_font_16, RAIN);
    lv_obj_set_hidden(c, true);
}

lv_obj_t *weather_page_create(lv_obj_t *parent) {
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(page, false);
    sky_view_image(page);   /* the picture, shared with the Sun and Moon page */

    /* the cartouche: right of the menu button */
    s_cart = lv_obj_create(page);
    lv_obj_remove_style_all(s_cart);
    lv_obj_set_size(s_cart, 520, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_cart, 84, 14);
    lv_obj_set_style_bg_color(s_cart, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(s_cart, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_cart, lv_color_hex(SEAL), 0);
    lv_obj_set_style_border_width(s_cart, 2, 0);
    lv_obj_set_style_radius(s_cart, 2, 0);
    lv_obj_set_style_pad_hor(s_cart, 16, 0);
    lv_obj_set_style_pad_ver(s_cart, 8, 0);
    lv_obj_set_style_pad_row(s_cart, 0, 0);
    lv_obj_set_flex_flow(s_cart, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_clickable(s_cart, false);
    s_place = label(s_cart, &art_font_24, INK);
    lv_label_set_long_mode(s_place, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_place, LV_PCT(100), lv_font_get_line_height(&art_font_24));
    /* now: the temperature big, the sky and the details beside it */
    s_now = plain(s_cart);
    lv_obj_set_size(s_now, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_now, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_now, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_now, 16, 0);
    s_temp = label(s_now, &lv_font_montserrat_48, INK);
    lv_obj_t *col = plain(s_now);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    s_desc = label(col, &art_font_20, INK);
    lv_label_set_long_mode(s_desc, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_desc, LV_PCT(100), lv_font_get_line_height(&art_font_20));
    s_details = label(col, &art_font_16, INK_SOFT);
    lv_label_set_long_mode(s_details, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_details, LV_PCT(100), lv_font_get_line_height(&art_font_16));
    lv_obj_set_hidden(s_now, true);
    s_sub = label(s_cart, &art_font_16, INK_SOFT);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_sub, LV_PCT(100), lv_font_get_line_height(&art_font_16));

    lv_obj_t *b = lv_button_create(page);   /* like the corner buttons */
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    lv_obj_t *bl = label(b, &lv_font_montserrat_24, 0x808080);
    lv_label_set_text(bl, LV_SYMBOL_REFRESH);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_add_event_cb(b, refresh_cb, LV_EVENT_CLICKED, NULL);
    provider_picker(page, b);

    /* the hours: a row of small cards wider than the screen, swiped sideways */
    s_hours = plain(page);
    lv_obj_set_clickable(s_hours, true);
    lv_obj_set_size(s_hours, 1000, 132);
    lv_obj_set_pos(s_hours, 12, 246);
    lv_obj_set_style_pad_column(s_hours, 8, 0);
    lv_obj_set_flex_flow(s_hours, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_hours, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_scrollable(s_hours, true);
    lv_obj_set_scroll_dir(s_hours, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_hours, LV_SCROLLBAR_MODE_OFF);
    for (int k = 0; k < WX_HOURS; k++) hour_cell(s_hours, k);
    lv_obj_set_hidden(s_hours, true);

    /* the days: paper cards on the hills, between the corner buttons */
    s_days = plain(page);
    lv_obj_set_size(s_days, 872, 204);
    lv_obj_set_pos(s_days, 76, 600 - 8 - 204);
    lv_obj_set_flex_flow(s_days, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_days, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int k = 0; k < WX_DAYS; k++) day_card(s_days, k);

    /* no forecast yet: why */
    s_note = label(page, &art_font_24, NOTE_TXT);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_note, 760);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_note, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_note, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_note, 8, 0);
    lv_obj_set_style_pad_all(s_note, 16, 0);
    lv_obj_align(s_note, LV_ALIGN_CENTER, 0, 40);
    lv_label_set_text(s_note, "Waiting for the weather");
    lv_label_set_text(s_place, "Weather");

    s_tick = lv_timer_create(tick_cb, 60 * 1000, NULL);
    lv_timer_pause(s_tick);
    lv_obj_set_hidden(page, true);
    return page;
}

static void show(const weather_t *w) {
    const char *t = w->imperial ? DEG "F" : DEG "C";
    lv_label_set_text(s_place, w->place);
    s_kind = w->kind;
    lv_label_set_text_fmt(s_temp, "%ld%s", lroundf(w->temp), t);
    lv_label_set_text(s_desc, w->desc);
    lv_label_set_text_fmt(s_details, "Feels like %ld" DEG "  \xC2\xB7  Humidity %d%%  \xC2\xB7  Wind %ld %s %s", lroundf(w->feels),
                          w->humidity, lroundf(w->wind), w->imperial ? "mph" : "km/h", w->wind_dir);
    for (int k = 0; k < WX_HOURS; k++) {
        lv_obj_set_hidden(s_hour[k].cell, k >= w->nhours);
        if (k >= w->nhours) continue;
        const wx_hour_t *h = &w->hour[k];
        if (k == 0) lv_label_set_text(s_hour[k].time, "Now");
        else lv_label_set_text_fmt(s_hour[k].time, "%02d:00", h->hour);
        weather_icon_set(s_hour[k].icon, h->kind, h->night);
        lv_label_set_text_fmt(s_hour[k].temp, "%ld" DEG, lroundf(h->temp));
        if (h->pop > 0) lv_label_set_text_fmt(s_hour[k].pop, "%d%%", h->pop);
        else lv_label_set_text(s_hour[k].pop, "");
    }
    s_nhours = w->nhours;
    lv_obj_scroll_to_x(s_hours, 0, LV_ANIM_OFF);

    for (int k = 0; k < WX_DAYS; k++) {
        lv_obj_set_hidden(s_day[k].card, k >= w->ndays);
        if (k >= w->ndays) continue;
        const wx_day_t *d = &w->day[k];
        lv_label_set_text(s_day[k].name, k == 0 ? "Today" : WDAY[d->wday % 7]);
        lv_label_set_text_fmt(s_day[k].date, "%d %s", d->mday, MON[(d->mon + 11) % 12]);
        weather_icon_set(s_day[k].icon, d->kind, false);
        lv_label_set_text_fmt(s_day[k].hi, "%ld" DEG, lroundf(d->tmax));
        lv_label_set_text_fmt(s_day[k].lo, "%ld" DEG, lroundf(d->tmin));
        if (d->pop >= 0) lv_label_set_text_fmt(s_day[k].pop, "Rain %d%%", d->pop);
        else lv_label_set_text(s_day[k].pop, "");
    }

    s_updated[0] = 0;
    if (time_synced()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(s_updated, sizeof s_updated, "Updated %H:%M  \xC2\xB7  ", &tm);
    }
    snprintf(s_updated + strlen(s_updated), sizeof s_updated - strlen(s_updated), "%s", w->provider);
}

void weather_page_update(const weather_t *w, const char *status) {
    lvgl_port_lock(0);
    if (w) {
        show(w);
        s_have = true;
    }
    lv_obj_set_hidden(s_now, !s_have);
    lv_obj_set_hidden(s_days, !s_have);
    lv_obj_set_hidden(s_hours, !s_have || !s_nhours);
    lv_obj_set_hidden(s_note, s_have);
    if (!s_have) {
        lv_label_set_text(s_note, status ? status : "");
        lv_label_set_text(s_place, "Weather");
        lv_label_set_text(s_sub, "");
    } else if (status) {   /* the forecast on show is the last one that came */
        lv_label_set_text_fmt(s_sub, "%s  \xC2\xB7  %s", s_updated, status);
        lv_obj_set_style_text_color(s_sub, lv_color_hex(WARN), 0);
    } else {
        lv_label_set_text(s_sub, s_updated);
        lv_obj_set_style_text_color(s_sub, lv_color_hex(INK_SOFT), 0);
    }
    if (w && s_shown) paint();   /* the new sky */
    lvgl_port_unlock();
}
