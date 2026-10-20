/* clock_page.c - an analog clock (an lv_scale with line hands), the digital
 * time and date under it, and the month's calendar beside it. A timer in the
 * LVGL task moves the hands each second, only while the page is shown. */
#include "clock_page.h"
#include "time_sync.h"
#include <stdio.h>
#include <time.h>

#define FACE        420     /* the clock's diameter */
#define BG          0x0D1B2A
#define FACE_BG     0x1B263B
#define RIM         0x415A77
#define MINOR       0x778DA9
#define TEXT        0xE0F0FF
#define SECONDS     0xFF6B35

static lv_obj_t *s_page, *s_scale, *s_hour, *s_min, *s_sec, *s_time, *s_date, *s_cal;
static lv_timer_t *s_tick;
static int s_last_sec = -1, s_last_yday = -1;

/* the hour labels, clockwise from the top; 12 twice: the range's both ends */
static const char *s_hours[] = { "12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", NULL };

static lv_obj_t *hand(uint32_t color, int width) {
    lv_obj_t *l = lv_line_create(s_scale);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    return l;
}

static void update(bool force) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (!time_synced()) {   /* 1970 until NTP answers: show nothing wrong */
        lv_label_set_text(s_time, "--:--:--");
        lv_label_set_text(s_date, "Waiting for the time from " CONFIG_ARTSHOW_NTP_SERVER);
        lv_obj_set_hidden(s_cal, true);
        return;
    }
    if (!force && t.tm_sec == s_last_sec) return;
    s_last_sec = t.tm_sec;

    const int r = FACE / 2;
    lv_scale_set_line_needle_value(s_scale, s_hour, r * 50 / 100, (t.tm_hour % 12) * 5 + t.tm_min / 12);
    lv_scale_set_line_needle_value(s_scale, s_min, r * 75 / 100, t.tm_min);
    lv_scale_set_line_needle_value(s_scale, s_sec, r * 85 / 100, t.tm_sec);

    char buf[64];
    strftime(buf, sizeof buf, "%H:%M:%S", &t);
    lv_label_set_text(s_time, buf);
    if (force || t.tm_yday != s_last_yday) {   /* a new day: the date, and the calendar back to this month */
        s_last_yday = t.tm_yday;
        strftime(buf, sizeof buf, "%A %e %B %Y", &t);
        lv_label_set_text(s_date, buf);
        lv_calendar_set_today_date(s_cal, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        lv_calendar_set_month_shown(s_cal, t.tm_year + 1900, t.tm_mon + 1);
        lv_obj_set_hidden(s_cal, false);
    }
}

static void tick_cb(lv_timer_t *t) { update(false); }

void clock_page_shown(bool shown) {
    if (shown) {
        update(true);
        lv_timer_resume(s_tick);
    } else {
        lv_timer_pause(s_tick);
    }
}

static void face_create(void) {
    s_scale = lv_scale_create(s_page);
    lv_obj_set_size(s_scale, FACE, FACE);
    lv_obj_set_pos(s_scale, 80, 16);
    lv_scale_set_mode(s_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(s_scale, 0, 60);
    lv_scale_set_angle_range(s_scale, 360);
    lv_scale_set_rotation(s_scale, 270);            /* 0 at the top */
    lv_scale_set_total_tick_count(s_scale, 61);
    lv_scale_set_major_tick_every(s_scale, 5);
    lv_scale_set_label_show(s_scale, true);
    lv_scale_set_text_src(s_scale, s_hours);

    lv_obj_set_style_bg_color(s_scale, lv_color_hex(FACE_BG), 0);
    lv_obj_set_style_bg_opa(s_scale, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_scale, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_scale, 6, 0);
    lv_obj_set_style_border_color(s_scale, lv_color_hex(RIM), 0);
    lv_obj_set_style_pad_all(s_scale, 14, 0);
    lv_obj_set_style_arc_width(s_scale, 0, LV_PART_MAIN);       /* no circle line under the ticks */
    lv_obj_set_style_length(s_scale, 22, LV_PART_INDICATOR);    /* hours */
    lv_obj_set_style_line_width(s_scale, 5, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s_scale, lv_color_hex(TEXT), LV_PART_INDICATOR);
    lv_obj_set_style_text_font(s_scale, &lv_font_montserrat_32, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_scale, lv_color_hex(TEXT), LV_PART_INDICATOR);
    lv_obj_set_style_length(s_scale, 10, LV_PART_ITEMS);        /* minutes */
    lv_obj_set_style_line_width(s_scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(s_scale, lv_color_hex(MINOR), LV_PART_ITEMS);

    s_hour = hand(TEXT, 12);
    s_min = hand(TEXT, 7);
    s_sec = hand(SECONDS, 3);

    lv_obj_t *cap = lv_obj_create(s_scale);   /* over the hands' pivot */
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, 22, 22);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, lv_color_hex(SECONDS), 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_center(cap);
}

lv_obj_t *clock_page_create(lv_obj_t *parent) {
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_page, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_set_scrollable(s_page, false);

    face_create();

    s_time = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(TEXT), 0);
    lv_obj_set_style_text_align(s_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_time, FACE);
    lv_obj_set_pos(s_time, 80, 16 + FACE + 18);

    s_date = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_date, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_date, lv_color_hex(MINOR), 0);
    lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_date, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_date, FACE + 40);
    lv_obj_set_pos(s_date, 60, 16 + FACE + 82);

    s_cal = lv_calendar_create(s_page);
    lv_obj_set_size(s_cal, 440, 470);
    lv_obj_align(s_cal, LV_ALIGN_TOP_RIGHT, -40, 30);
    lv_obj_set_style_text_font(s_cal, &lv_font_montserrat_24, 0);
    lv_obj_set_style_bg_color(s_cal, lv_color_hex(FACE_BG), 0);
    lv_obj_set_style_border_color(s_cal, lv_color_hex(RIM), 0);
    lv_obj_set_style_text_color(s_cal, lv_color_hex(TEXT), 0);
    lv_obj_t *days = lv_calendar_get_btnmatrix(s_cal);
    lv_obj_set_style_bg_opa(days, LV_OPA_TRANSP, LV_PART_ITEMS);   /* the theme's white cells: off */
    lv_obj_set_style_text_color(days, lv_color_hex(TEXT), LV_PART_ITEMS);
    lv_calendar_add_header_arrow(s_cal);   /* other months, until the next day brings it back */

    s_tick = lv_timer_create(tick_cb, 200, NULL);   /* 200 ms: the second hand steps on time */
    lv_timer_pause(s_tick);
    lv_obj_set_hidden(s_page, true);
    return s_page;
}
