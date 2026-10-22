/* clock_page.c - a Bauhaus poster: on a charcoal ground, flat shapes in the
 * primaries (a red disc behind the clock, a blue bar, a yellow disc, cream
 * bars) drawn by the page itself; the clock after Max Bill's (a plain face,
 * bar markers, no numerals, square black hands, a red second hand with a
 * disc), drawn in its own draw event; the time big and the date in
 * lowercase under it; the month's calendar beside it, flat: today a red
 * disc, the days with a note blue. A timer in the LVGL task moves the hands
 * each second, only while the page is shown. Tap a day for its note. */
#include "clock_page.h"
#include "time_sync.h"
#include "notes.h"
#include "fonts/art_fonts.h"
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define FACE        400     /* the clock's diameter */
#define FACE_X      90
#define FACE_Y      34
#define GROUND      0x202227    /* the poster's ground: charcoal */
#define CREAM       0xEFE8DA
#define PAPER       0xF8F4EA
#define BLACK       0x1A1A1A
#define GREY        0x58544C    /* weekday names, on the calendar's paper */
#define HINT        0xB8B2A6    /* the note's hint, on the ground */
#define FAINT       0x9A9488    /* other months' days */
#define RED         0xD7322B
#define BLUE        0x1F4E9C
#define YELLOW      0xF2C230

static lv_obj_t *s_page, *s_face, *s_time, *s_date, *s_cal;
static lv_timer_t *s_tick;
static lv_obj_t *s_note;              /* under the calendar: today's note */
static lv_obj_t *s_editor, *s_ed_title, *s_ed_text, *s_ed_status;   /* a day's note, over everything */
static int s_ed_y, s_ed_m, s_ed_d;    /* ... that day */
#define MAX_MARKS 400
static lv_calendar_date_t s_marks[MAX_MARKS];   /* the days with a note: the calendar keeps this pointer */
static int s_last_sec = -1, s_last_yday = -1;
static struct tm s_now;               /* what the hands show */
static bool s_synced;

/* ---- drawing ---- */

static void rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t radius, uint32_t c) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(c);
    d.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

static void disc(lv_layer_t *layer, float cx, float cy, float r, uint32_t c) {
    rect(layer, lroundf(cx - r), lroundf(cy - r), lroundf(cx + r), lroundf(cy + r), LV_RADIUS_CIRCLE, c);
}

/* a bar from (x1, y1) to (x2, y2), square-ended */
static void bar(lv_layer_t *layer, float x1, float y1, float x2, float y2, int w, uint32_t c) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.width = w;
    d.color = lv_color_hex(c);
    lv_draw_line(layer, &d);
}

/* the poster: flat shapes behind everything */
static void poster_draw(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    const float cx = FACE_X + FACE / 2, cy = FACE_Y + FACE / 2;
    disc(layer, cx - 70, cy + 120, 250, RED);           /* the clock sits on a red disc, off its centre */
    rect(layer, 556, 0, 580, 599, 0, BLUE);             /* a blue bar between the clock and the month */
    disc(layer, 568, 92, 44, YELLOW);                   /* a yellow disc on it */
    rect(layer, 0, 578, 520, 589, 0, CREAM);            /* a cream rule along the bottom */
    rect(layer, 506, 520, 540, 554, 0, CREAM);          /* a cream square */
    bar(layer, 600, 560, 1000, 520, 6, CREAM);          /* and a cream diagonal */
}

/* the clock: a plain face, twelve bars (the quarters longer), minute ticks;
   square black hands, a red second hand with a disc, a blue centre */
static void face_draw(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const float r = FACE / 2.0f, cx = a.x1 + r, cy = a.y1 + r;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(PAPER);
    d.radius = LV_RADIUS_CIRCLE;
    d.border_width = 5;
    d.border_color = lv_color_hex(BLACK);
    lv_draw_rect(layer, &d, &a);
    for (int k = 0; k < 60; k++) {
        float an = k * (float)M_PI / 30, s = sinf(an), c = -cosf(an);
        if (k % 5 == 0) {
            float in = k % 15 == 0 ? 0.70f : 0.76f;
            bar(layer, cx + s * r * in, cy + c * r * in, cx + s * r * 0.90f, cy + c * r * 0.90f, k % 15 == 0 ? 12 : 8, BLACK);
        } else {
            bar(layer, cx + s * r * 0.86f, cy + c * r * 0.86f, cx + s * r * 0.90f, cy + c * r * 0.90f, 2, GREY);
        }
    }
    if (!s_synced) return;   /* no time yet: no hands */
    float h = ((s_now.tm_hour % 12) + s_now.tm_min / 60.0f) * (float)M_PI / 6;
    float m = (s_now.tm_min + s_now.tm_sec / 60.0f) * (float)M_PI / 30;
    float sec = s_now.tm_sec * (float)M_PI / 30;
#define HAND(an, back, len, w, col) \
    bar(layer, cx - sinf(an) * r * (back), cy + cosf(an) * r * (back), cx + sinf(an) * r * (len), cy - cosf(an) * r * (len), w, col)
    HAND(h, 0.12f, 0.52f, 16, BLACK);
    HAND(m, 0.12f, 0.80f, 10, BLACK);
    HAND(sec, 0.24f, 0.88f, 3, RED);
#undef HAND
    disc(layer, cx - sinf(sec) * r * 0.24f, cy + cosf(sec) * r * 0.24f, 10, RED);   /* the counterweight's disc */
    disc(layer, cx, cy, 11, BLACK);
    disc(layer, cx, cy, 6, BLUE);
}

/* the calendar's days, after LVGL's own styling: flat cells, today a red
   disc, a day with a note blue, the weekday names small and grey */
static void cal_draw(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    lv_draw_task_t *t = lv_event_get_param(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(t);
    if (base->part != LV_PART_ITEMS) return;
    int32_t id = base->id1;
    bool today = lv_buttonmatrix_has_button_ctrl(obj, id, LV_BUTTONMATRIX_CTRL_CUSTOM_1);
    bool marked = lv_buttonmatrix_has_button_ctrl(obj, id, LV_BUTTONMATRIX_CTRL_CUSTOM_2);
    bool other = id >= 7 && lv_buttonmatrix_has_button_ctrl(obj, id, LV_BUTTONMATRIX_CTRL_DISABLED);
    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(t);
    lv_draw_border_dsc_t *border = lv_draw_task_get_border_dsc(t);
    lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(t);
    if (border) border->opa = LV_OPA_TRANSP;
    if (fill) {
        fill->opa = today || marked ? LV_OPA_COVER : LV_OPA_TRANSP;
        fill->color = lv_color_hex(today ? RED : BLUE);
        fill->radius = today ? LV_RADIUS_CIRCLE : 0;
    }
    if (label) label->color = lv_color_hex(id < 7 ? GREY : other ? FAINT : today || marked ? PAPER : BLACK);
}

/* ---- the numbers ---- */

/* the days with a note, marked on the calendar (any month it shows) */
static void load_marks(void) {
    static notes_date_t days[MAX_MARKS];
    int n = notes_days(days, MAX_MARKS);
    for (int k = 0; k < n; k++) s_marks[k] = (lv_calendar_date_t){ .year = days[k].year, .month = days[k].month, .day = days[k].day };
    lv_calendar_set_highlighted_dates(s_cal, s_marks, n);
}

/* today's note under the calendar */
static void show_today_note(void) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char note[512];
    if (time_synced() && notes_get(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, note, sizeof note)) {
        lv_label_set_text_fmt(s_note, "today:  %s", note);
        lv_obj_set_style_text_color(s_note, lv_color_hex(PAPER), 0);
    } else {
        lv_label_set_text(s_note, "tap a day to write a note for it");
        lv_obj_set_style_text_color(s_note, lv_color_hex(HINT), 0);
    }
}

static void lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static void update(bool force) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    s_synced = time_synced();
    if (!s_synced) {   /* 1970 until NTP answers: show nothing wrong */
        lv_label_set_text(s_time, "--:--");
        lv_label_set_text(s_date, "waiting for the time from " CONFIG_ARTSHOW_NTP_SERVER);
        lv_obj_set_hidden(s_cal, true);
        lv_obj_set_hidden(s_note, true);
        lv_obj_invalidate(s_face);
        return;
    }
    if (!force && t.tm_sec == s_last_sec) return;
    s_last_sec = t.tm_sec;
    s_now = t;
    lv_obj_invalidate(s_face);

    char buf[64];
    strftime(buf, sizeof buf, "%H:%M", &t);
    lv_label_set_text(s_time, buf);
    if (force || t.tm_yday != s_last_yday) {   /* a new day: the date, and the calendar back to this month */
        s_last_yday = t.tm_yday;
        strftime(buf, sizeof buf, "%A %e %B %Y", &t);
        lower(buf);   /* Bauhaus: lowercase */
        lv_label_set_text(s_date, buf);
        lv_calendar_set_today_date(s_cal, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        lv_calendar_set_month_shown(s_cal, t.tm_year + 1900, t.tm_mon + 1);
        lv_obj_set_hidden(s_cal, false);
        lv_obj_set_hidden(s_note, false);
        show_today_note();
    }
}

static void tick_cb(lv_timer_t *t) { update(false); }

void clock_page_shown(bool shown) {
    if (shown) {
        load_marks();
        update(true);
        lv_timer_resume(s_tick);
    } else {
        lv_timer_pause(s_tick);
    }
}

/* ---- a day's note: the editor ---- */

static void editor_close(void) {
    lv_obj_set_hidden(s_editor, true);
}

static void save(const char *text) {
    if (!notes_set(s_ed_y, s_ed_m, s_ed_d, text)) {
        lv_label_set_text(s_ed_status, "It can't be saved: is the card in?");
        return;
    }
    load_marks();
    show_today_note();
    editor_close();
}

static void save_cb(lv_event_t *e) { save(lv_textarea_get_text(s_ed_text)); }
static void delete_cb(lv_event_t *e) { save(""); }
static void close_cb(lv_event_t *e) { editor_close(); }

static void editor_open(int y, int m, int d) {
    s_ed_y = y;
    s_ed_m = m;
    s_ed_d = d;
    struct tm t = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_hour = 12, .tm_isdst = -1 };
    mktime(&t);   /* the weekday */
    char title[64], note[512];
    strftime(title, sizeof title, "note for %A %e %B %Y", &t);
    lower(title);
    lv_label_set_text(s_ed_title, title);
    bool has = notes_get(y, m, d, note, sizeof note);
    lv_textarea_set_text(s_ed_text, has ? note : "");
    lv_label_set_text(s_ed_status, "");
    lv_obj_set_hidden(s_editor, false);
}

/* a day tapped on the calendar */
static void day_cb(lv_event_t *e) {
    lv_calendar_date_t d;
    if (lv_calendar_get_pressed_date(s_cal, &d) == LV_RESULT_OK && d.year > 1970) editor_open(d.year, d.month, d.day);
}

/* a flat block of a colour, its word in lowercase */
static lv_obj_t *ed_button(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, int32_t x, uint32_t color) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 150, 48);
    lv_obj_set_pos(b, x, 238);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color == YELLOW ? BLACK : PAPER), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* over everything (the corner buttons too), like the Wi-Fi panel */
static void editor_create(void) {
    s_editor = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_editor, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_editor, lv_color_hex(CREAM), 0);
    lv_obj_set_style_bg_opa(s_editor, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_editor, 0, 0);
    lv_obj_set_style_radius(s_editor, 0, 0);
    lv_obj_set_style_pad_all(s_editor, 0, 0);
    lv_obj_set_scrollable(s_editor, false);
    s_ed_title = lv_label_create(s_editor);
    lv_obj_set_style_text_font(s_ed_title, &art_font_24, 0);
    lv_obj_set_style_text_color(s_ed_title, lv_color_hex(BLACK), 0);
    lv_obj_set_pos(s_ed_title, 24, 16);
    s_ed_text = lv_textarea_create(s_editor);
    lv_obj_set_size(s_ed_text, 976, 168);
    lv_obj_set_pos(s_ed_text, 24, 58);
    lv_obj_set_style_text_font(s_ed_text, &art_font_20, 0);
    lv_obj_set_style_bg_color(s_ed_text, lv_color_hex(PAPER), 0);
    lv_obj_set_style_border_color(s_ed_text, lv_color_hex(BLACK), 0);
    lv_obj_set_style_border_width(s_ed_text, 3, 0);
    lv_obj_set_style_radius(s_ed_text, 0, 0);
    lv_obj_set_style_text_color(s_ed_text, lv_color_hex(BLACK), 0);
    lv_textarea_set_max_length(s_ed_text, 400);
    lv_textarea_set_placeholder_text(s_ed_text, "A note for this day");
    ed_button(s_editor, "save", save_cb, 24, RED);
    ed_button(s_editor, "delete", delete_cb, 190, BLACK);
    ed_button(s_editor, "close", close_cb, 356, BLUE);
    s_ed_status = lv_label_create(s_editor);
    lv_obj_set_style_text_font(s_ed_status, &art_font_16, 0);
    lv_obj_set_style_text_color(s_ed_status, lv_color_hex(RED), 0);
    lv_obj_set_pos(s_ed_status, 530, 252);
    lv_obj_t *kb = lv_keyboard_create(s_editor);
    lv_obj_set_size(kb, LV_PCT(100), 300);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_ed_text);
    lv_obj_add_event_cb(kb, save_cb, LV_EVENT_READY, NULL);    /* the keyboard's OK key */
    lv_obj_add_event_cb(kb, close_cb, LV_EVENT_CANCEL, NULL);  /* ... and its close key */
    lv_obj_set_hidden(s_editor, true);
}

/* ---- the page ---- */

static void cal_create(void) {
    s_cal = lv_calendar_create(s_page);
    lv_obj_set_size(s_cal, 400, 410);   /* today's note under it */
    lv_obj_set_pos(s_cal, 604, 26);
    lv_obj_set_style_text_font(s_cal, &lv_font_montserrat_24, 0);
    lv_obj_set_style_bg_color(s_cal, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(s_cal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_cal, lv_color_hex(BLACK), 0);
    lv_obj_set_style_border_width(s_cal, 4, 0);
    lv_obj_set_style_radius(s_cal, 0, 0);
    lv_obj_set_style_shadow_width(s_cal, 0, 0);
    lv_obj_set_style_text_color(s_cal, lv_color_hex(BLACK), 0);
    lv_obj_t *days = lv_calendar_get_btnmatrix(s_cal);
    lv_obj_set_style_bg_opa(days, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(days, 0, 0);
    /* the cells opaque (LVGL draws no fill for a transparent one, and its
       highlight is a fill); cal_draw then makes each what it should be */
    lv_obj_set_style_bg_color(days, lv_color_hex(PAPER), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(days, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(days, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(days, lv_color_hex(BLACK), LV_PART_ITEMS);
    lv_obj_add_event_cb(days, cal_draw, LV_EVENT_DRAW_TASK_ADDED, NULL);   /* after LVGL's own: it wins */
    lv_obj_t *head = lv_calendar_add_header_arrow(s_cal);   /* other months, until the next day brings it back */
    lv_obj_set_style_bg_color(head, lv_color_hex(BLACK), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(head, 0, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(PAPER), 0);
    for (uint32_t k = 0; k < lv_obj_get_child_count(head); k++) {
        lv_obj_t *c = lv_obj_get_child(head, k);
        lv_obj_set_style_text_color(c, lv_color_hex(PAPER), 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(k == 0 ? RED : BLUE), 0);   /* the arrows: a red and a blue block */
        lv_obj_set_style_radius(c, 0, 0);
        lv_obj_set_style_shadow_width(c, 0, 0);
    }
    lv_obj_add_event_cb(s_cal, day_cb, LV_EVENT_VALUE_CHANGED, NULL);   /* a day tapped: its note */
}

lv_obj_t *clock_page_create(lv_obj_t *parent) {
    s_page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_page, lv_color_hex(GROUND), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_page, false);
    lv_obj_add_event_cb(s_page, poster_draw, LV_EVENT_DRAW_MAIN_END, NULL);

    s_face = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_face);
    lv_obj_set_size(s_face, FACE, FACE);
    lv_obj_set_pos(s_face, FACE_X, FACE_Y);
    lv_obj_set_clickable(s_face, false);
    lv_obj_add_event_cb(s_face, face_draw, LV_EVENT_DRAW_MAIN, NULL);

    s_time = lv_label_create(s_page);   /* on the red disc */
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(PAPER), 0);
    lv_obj_set_style_text_letter_space(s_time, 4, 0);
    lv_obj_set_pos(s_time, FACE_X, FACE_Y + FACE + 14);

    s_date = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_date, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_date, lv_color_hex(PAPER), 0);   /* on the red disc and past it */
    lv_label_set_long_mode(s_date, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_date, 420, 30);
    lv_obj_set_pos(s_date, FACE_X + 2, FACE_Y + FACE + 76);

    cal_create();

    s_note = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_note, &art_font_20, 0);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_note, 400, 56);   /* 2 lines; the editor has it all */
    lv_obj_set_pos(s_note, 604, 448);
    lv_obj_set_hidden(s_note, true);
    editor_create();

    s_tick = lv_timer_create(tick_cb, 200, NULL);   /* 200 ms: the second hand steps on time */
    lv_timer_pause(s_tick);
    lv_obj_set_hidden(s_page, true);
    return s_page;
}
