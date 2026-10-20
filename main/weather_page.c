/* weather_page.c - the place and when it was updated on top, the weather now
 * (a big icon, the temperature, the sky, feels like, humidity, wind), the
 * next 24 hours as a strip to swipe (WeatherAPI only), and a card per day:
 * weekday, icon, high and low, chance of rain. */
#include "weather_page.h"
#include "weather_icon.h"
#include "time_sync.h"
#include "esp_lvgl_port.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BG       0x0D1B2A
#define CARD_BG  0x1B263B
#define RIM      0x415A77
#define MINOR    0x778DA9
#define TEXT     0xE0F0FF
#define RAIN     0x4FA3FF
#define WARN     0xFF6B35
#define DEG      "\xC2\xB0"

static lv_obj_t *s_place, *s_sub, *s_note, *s_now, *s_icon, *s_temp, *s_desc, *s_details, *s_days, *s_hours;
static struct { lv_obj_t *card, *name, *date, *icon, *hi, *lo, *pop; } s_day[WX_DAYS];
static struct { lv_obj_t *cell, *time, *icon, *temp, *pop; } s_hour[WX_HOURS];
static int s_nhours;         /* in the forecast on show */
static char s_updated[64];   /* "Updated 13:05 • OpenWeatherMap" */
static bool s_have;          /* a forecast on show */

static const char *WDAY[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

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
    lv_obj_set_style_text_color(s_sub, lv_color_hex(MINOR), 0);
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
    lv_obj_set_width(dd, 250);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_color(dd, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_30, 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(RIM), 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(list, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(CARD_BG), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(RIM), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(RIM), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_align_to(dd, refresh, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    lv_obj_add_event_cb(dd, provider_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void day_card(lv_obj_t *parent, int k) {
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 128, 186);
    lv_obj_set_style_bg_color(c, lv_color_hex(CARD_BG), 0);
    lv_obj_set_style_border_color(c, lv_color_hex(RIM), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_pad_all(c, 10, 0);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_clickable(c, false);
    s_day[k].card = c;
    s_day[k].name = label(c, &lv_font_montserrat_24, TEXT);
    s_day[k].date = label(c, &lv_font_montserrat_14, MINOR);
    s_day[k].icon = weather_icon_create(c, 64);
    lv_obj_t *t = plain(c);   /* high and low side by side */
    lv_obj_set_size(t, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(t, 8, 0);
    s_day[k].hi = label(t, &lv_font_montserrat_24, TEXT);
    s_day[k].lo = label(t, &lv_font_montserrat_24, MINOR);
    s_day[k].pop = label(c, &lv_font_montserrat_14, RAIN);
    lv_obj_set_hidden(c, true);
}

static void hour_cell(lv_obj_t *parent, int k) {
    lv_obj_t *c = plain(parent);
    lv_obj_set_size(c, 70, LV_PCT(100));
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c, 1, 0);
    lv_obj_set_scrollable(c, false);   /* a swipe moves the strip */
    s_hour[k].cell = c;
    s_hour[k].time = label(c, &lv_font_montserrat_14, MINOR);
    s_hour[k].icon = weather_icon_create(c, 40);
    s_hour[k].temp = label(c, &lv_font_montserrat_24, TEXT);
    s_hour[k].pop = label(c, &lv_font_montserrat_14, RAIN);
    lv_obj_set_hidden(c, true);
}

lv_obj_t *weather_page_create(lv_obj_t *parent) {
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollable(page, false);

    /* top: right of the menu button */
    s_place = label(page, &lv_font_montserrat_32, TEXT);
    lv_label_set_long_mode(s_place, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_place, 560);   /* the provider picker on its right */
    lv_obj_set_pos(s_place, 90, 14);
    s_sub = label(page, &lv_font_montserrat_14, MINOR);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_sub, 820);
    lv_obj_set_pos(s_sub, 92, 54);

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

    /* now */
    s_now = plain(page);
    lv_obj_set_size(s_now, 940, 140);
    lv_obj_set_pos(s_now, 60, 80);
    s_icon = weather_icon_create(s_now, 140);
    lv_obj_set_pos(s_icon, 0, 0);
    s_temp = label(s_now, &lv_font_montserrat_48, TEXT);
    lv_obj_set_pos(s_temp, 170, 2);
    s_desc = label(s_now, &lv_font_montserrat_32, TEXT);
    lv_label_set_long_mode(s_desc, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_desc, 760);
    lv_obj_set_pos(s_desc, 170, 58);
    s_details = label(s_now, &lv_font_montserrat_24, MINOR);
    lv_obj_set_pos(s_details, 170, 104);
    lv_obj_set_hidden(s_now, true);

    /* the hours: a row wider than the screen, swiped sideways */
    s_hours = lv_obj_create(page);
    lv_obj_set_size(s_hours, 1000, 118);
    lv_obj_set_pos(s_hours, 12, 224);
    lv_obj_set_style_bg_color(s_hours, lv_color_hex(CARD_BG), 0);
    lv_obj_set_style_border_color(s_hours, lv_color_hex(RIM), 0);
    lv_obj_set_style_border_width(s_hours, 1, 0);
    lv_obj_set_style_radius(s_hours, 14, 0);
    lv_obj_set_style_pad_all(s_hours, 6, 0);
    lv_obj_set_style_pad_column(s_hours, 4, 0);
    lv_obj_set_flex_flow(s_hours, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(s_hours, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_hours, LV_SCROLLBAR_MODE_OFF);
    for (int k = 0; k < WX_HOURS; k++) hour_cell(s_hours, k);
    lv_obj_set_hidden(s_hours, true);

    /* the days */
    s_days = plain(page);
    lv_obj_set_size(s_days, 1000, 188);   /* above the corner buttons */
    lv_obj_set_pos(s_days, 12, 346);
    lv_obj_set_flex_flow(s_days, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_days, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int k = 0; k < WX_DAYS; k++) day_card(s_days, k);

    /* no forecast yet: why */
    s_note = label(page, &lv_font_montserrat_24, TEXT);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_note, 760);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_note, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_note, "Waiting for the weather");

    lv_obj_set_hidden(page, true);
    return page;
}

static void show(const weather_t *w) {
    const char *t = w->imperial ? DEG "F" : DEG "C";
    lv_label_set_text(s_place, w->place);
    weather_icon_set(s_icon, w->kind, w->night);
    lv_label_set_text_fmt(s_temp, "%ld%s", lroundf(w->temp), t);
    lv_label_set_text(s_desc, w->desc);
    lv_label_set_text_fmt(s_details, "Feels like %ld" DEG "   " LV_SYMBOL_BULLET "   Humidity %d%%   " LV_SYMBOL_BULLET
                          "   Wind %ld %s %s", lroundf(w->feels), w->humidity, lroundf(w->wind),
                          w->imperial ? "mph" : "km/h", w->wind_dir);
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
    /* no hours (not WeatherAPI): the days move up into their place */
    lv_obj_set_y(s_days, s_nhours ? 346 : 250);

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
        strftime(s_updated, sizeof s_updated, "Updated %H:%M   " LV_SYMBOL_BULLET "   ", &tm);
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
        lv_label_set_text_fmt(s_sub, "%s   " LV_SYMBOL_BULLET "   %s", s_updated, status);
        lv_obj_set_style_text_color(s_sub, lv_color_hex(WARN), 0);
    } else {
        lv_label_set_text(s_sub, s_updated);
        lv_obj_set_style_text_color(s_sub, lv_color_hex(MINOR), 0);
    }
    lvgl_port_unlock();
}
