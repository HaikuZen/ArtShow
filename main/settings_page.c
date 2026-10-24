/* settings_page.c - two scrolling columns: the slideshow's settings and its
 * folders on the left; night mode and about the board on the right. A change
 * takes effect at once (the next picture, for the slideshow's) and is kept
 * in NVS: a slider's when it's let go, a switch's when it flips.
 * A blueprint: a cyanotype ground with a drafting grid (one tile, tiled),
 * chalk-white line work, numbered sections, sliders drawn as rulers with
 * ticks, outlined switches and boxes, a dash-dot centre line between the
 * columns and a drawing's title block top right. */
#include "settings_page.h"
#include "settings.h"
#include "night.h"
#include "slideshow.h"
#include "index.h"
#include "wifi.h"
#include "fonts/art_fonts.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "board.h"
#include "radio.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BG          0x1C4F8A   /* the cyanotype's blue */
#define MINOR       0xA9C4E0   /* notes, values: pale blue */
#define TEXT        0xF2F7FC   /* chalk */
#define MAX_FOLDERS 16
#define TILE        80         /* the grid: a line each 16 px, a heavier one each 80 */

/* seconds per picture: the slider's steps */
static const int STEPS[] = { 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 300, 600, 900, 1800, 3600 };
#define N_STEPS (int)(sizeof STEPS / sizeof STEPS[0])

static lv_obj_t *s_volume_v, *s_interval_v, *s_fade_v, *s_from_v, *s_to_v, *s_level_v, *s_hz_v, *s_hours, *s_sun_info;
static lv_obj_t *s_folders, *s_about, *s_index;
static char s_index_was[96];   /* the summary on show: a change after a scan lists the folders again */
static lv_timer_t *s_about_timer;
static char s_folder_names[MAX_FOLDERS][64];
static bool s_built;   /* the sliders' first values aren't changes */

/* ---- building blocks ---- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *txt) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

/* a small grey note, wrapping in its column */
static lv_obj_t *note(lv_obj_t *parent, const char *txt) {
    lv_obj_t *l = label(parent, &art_font_16, MINOR, txt);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b, flow);
    lv_obj_set_clickable(b, false);
    return b;
}

/* a column of the page: scrolls when its content is taller */
static lv_obj_t *column(lv_obj_t *page, int32_t x, int32_t w) {
    lv_obj_t *c = lv_obj_create(page);
    lv_obj_remove_style_all(c);
    lv_obj_set_pos(c, x, 64);
    lv_obj_set_size(c, w, 470);   /* down to 534: above the corner buttons */
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_style_pad_right(c, 12, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    return c;
}

/* "01  SLIDESHOW": a numbered section, spaced capitals over a rule */
static void heading(lv_obj_t *col, const char *txt) {
    static int n;
    char buf[48];
    snprintf(buf, sizeof buf, "%02d   %s", ++n, txt);
    lv_obj_t *l = label(col, &art_font_20, TEXT, buf);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_letter_space(l, 3, 0);
    lv_obj_set_style_pad_top(l, 8, 0);
    lv_obj_set_style_pad_bottom(l, 4, 0);
    lv_obj_set_style_border_side(l, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(l, 1, 0);
    lv_obj_set_style_border_color(l, lv_color_hex(TEXT), 0);
    lv_obj_set_style_border_opa(l, LV_OPA_60, 0);
}

/* a slider as a ruler: a hairline, the part up to the value heavier, ticks
 * under it; LVGL draws only the knob (an open circle) */
static void ruler_draw(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(s, &a);
    int min = lv_slider_get_min_value(s), max = lv_slider_get_max_value(s), v = lv_slider_get_value(s);
    int w = lv_area_get_width(&a) - 1, cy = (a.y1 + a.y2) / 2;
    int x = a.x1 + (max > min ? (int)((int64_t)w * (v - min) / (max - min)) : 0);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(TEXT);
    d.bg_opa = LV_OPA_50;
    lv_area_t r = { a.x1, cy, a.x2, cy };
    lv_draw_rect(layer, &d, &r);
    d.bg_opa = LV_OPA_COVER;
    r = (lv_area_t){ a.x1, cy - 1, x, cy + 1 };
    lv_draw_rect(layer, &d, &r);
    int n = max - min, step = n > 16 ? (n + 11) / 12 : 1;
    for (int k = 0; k <= n; k += step) {
        int tx = a.x1 + (n ? (int)((int64_t)w * k / n) : 0);
        bool big = k == 0 || k + step > n;
        d.bg_opa = big ? LV_OPA_COVER : LV_OPA_70;
        r = (lv_area_t){ tx, cy + 4, tx, cy + (big ? 11 : 8) };
        lv_draw_rect(layer, &d, &r);
    }
    if (n % step) {   /* the far end, off the step */
        r = (lv_area_t){ a.x2, cy + 4, a.x2, cy + 11 };
        lv_draw_rect(layer, &d, &r);
    }
}

/* the page: the drawing's border and the centre line between the columns
 * (the grid is the background's tile) */
static void sheet_draw(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_border_dsc_t b;
    lv_draw_border_dsc_init(&b);
    b.color = lv_color_hex(TEXT);
    b.opa = LV_OPA_50;
    b.width = 1;
    lv_area_t a = { 6, 6, 1017, 593 };
    lv_draw_border(layer, &b, &a);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(TEXT);
    d.bg_opa = LV_OPA_40;
    for (int y = 70; y < 534; y += 26) {   /* dash, dot */
        lv_area_t r = { 512, y, 512, y + 15 };
        lv_draw_rect(layer, &d, &r);
        r.y1 = r.y2 = y + 20;
        lv_draw_rect(layer, &d, &r);
    }
}

/* the grid, tone on tone, with a little grain */
static lv_image_dsc_t s_tile;

static void grid_make(void) {
    uint16_t *px = heap_caps_malloc(TILE * TILE * 2, MALLOC_CAP_SPIRAM);
    if (!px) return;
    lv_color_t ground = lv_color_hex(BG), chalk = lv_color_hex(TEXT);
    uint32_t seed = 4242;
    for (int y = 0; y < TILE; y++) {
        for (int x = 0; x < TILE; x++) {
            int opa = x == 0 || y == 0 ? 46 : x % 16 == 0 || y % 16 == 0 ? 20 : 0;
            seed = seed * 1664525u + 1013904223u;
            opa += (int)(seed >> 30);   /* 0..3 of grain */
            lv_color_t c = lv_color_mix(chalk, ground, (lv_opa_t)opa);
            px[y * TILE + x] = (uint16_t)(((c.red >> 3) << 11) | ((c.green >> 2) << 5) | (c.blue >> 3));
        }
    }
    s_tile = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = TILE, .h = TILE, .stride = TILE * 2 },
        .data_size = TILE * TILE * 2,
        .data = (const uint8_t *)px,
    };
}

/* outlined, chalk on blue; pressed or chosen: filled */
static void outline(lv_obj_t *o, lv_part_t part) {
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, part);
    lv_obj_set_style_border_color(o, lv_color_hex(TEXT), part);
    lv_obj_set_style_border_width(o, 2, part);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, part);
    lv_obj_set_style_shadow_width(o, 0, part);
    lv_obj_set_style_text_color(o, lv_color_hex(TEXT), part);
    lv_obj_set_style_bg_color(o, lv_color_hex(TEXT), part | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(o, LV_OPA_30, part | LV_STATE_PRESSED);
}

static void save_cb(lv_event_t *e) { settings_save(); }

/* "Title ........ value" over a slider; cb fills value and the setting */
static void slider_row(lv_obj_t *col, const char *title, int min, int max, int val, lv_event_cb_t cb, lv_obj_t **value) {
    lv_obj_t *row = box(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 10, 0);
    lv_obj_t *top = box(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(top, &art_font_20, TEXT, title);
    *value = label(top, &art_font_20, MINOR, "");
    lv_obj_t *s = lv_slider_create(row);
    lv_obj_set_width(s, LV_PCT(94));
    lv_obj_set_height(s, 18);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_MAIN);   /* the ruler is drawn */
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(BG), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_border_color(s, lv_color_hex(TEXT), LV_PART_KNOB);
    lv_obj_set_style_border_width(s, 2, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, lv_color_hex(TEXT), LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_add_event_cb(s, ruler_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_set_style_margin_left(s, 12, 0);   /* the knob sticks out */
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s, 16);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, save_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(s, LV_EVENT_VALUE_CHANGED, NULL);   /* the value's text */
}

static void switch_cb(lv_event_t *e) {
    bool *field = lv_event_get_user_data(e);
    *field = lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED);
    settings_save();
}

static void switch32_cb(lv_event_t *e) {
    int32_t *field = lv_event_get_user_data(e);
    *field = lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED);
    settings_save();
}

/* "Title ........ [switch]" for a bool setting (or, field32, an int32_t one) */
static lv_obj_t *switch_row32(lv_obj_t *col, const char *title, bool *field, int32_t *field32) {
    lv_obj_t *row = box(col, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(row, &art_font_20, TEXT, title);
    lv_obj_t *sw = lv_switch_create(row);
    outline(sw, LV_PART_MAIN);
    lv_obj_set_style_border_opa(sw, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(TEXT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_40, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(TEXT), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_state(sw, LV_STATE_CHECKED, field ? *field : *field32 != 0);
    lv_obj_set_ext_click_area(sw, 12);
    if (field) lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, field);
    else lv_obj_add_event_cb(sw, switch32_cb, LV_EVENT_VALUE_CHANGED, field32);
    return sw;
}

static void switch_row(lv_obj_t *col, const char *title, bool *field) { switch_row32(col, title, field, NULL); }

static lv_obj_t *s_radio_sw;

static int slider_value(lv_event_t *e) { return lv_slider_get_value(lv_event_get_current_target_obj(e)); }

/* ---- the slideshow ---- */

static void interval_cb(lv_event_t *e) {
    int s = STEPS[slider_value(e)];
    g_settings.interval_s = s;
    if (s < 60) lv_label_set_text_fmt(s_interval_v, "%d s", s);
    else if (s < 3600) lv_label_set_text_fmt(s_interval_v, s % 60 ? "%d min %d s" : "%d min", s / 60, s % 60);
    else lv_label_set_text(s_interval_v, "1 hour");
}

static void fade_cb(lv_event_t *e) {
    int v = slider_value(e);
    g_settings.fade_ms = v * 100;
    lv_label_set_text_fmt(s_fade_v, "%d.%d s", v / 10, v % 10);
}

static void folder_cb(lv_event_t *e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    settings_folder_set(s_folder_names[k], lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED));
}

/* the folders' checkboxes, from the card's list (built when shown: the
   list comes after boot) */
static void list_folders(void) {
    static char names[MAX_FOLDERS][64];
    static int counts[MAX_FOLDERS];
    int n = slideshow_folders(names, counts, MAX_FOLDERS);
    lv_obj_clean(s_folders);
    if (!n) {
        note(s_folders, "No pictures on the card");
        return;
    }
    for (int k = 0; k < n; k++) {
        snprintf(s_folder_names[k], sizeof s_folder_names[k], "%s", names[k]);
        lv_obj_t *cb = lv_checkbox_create(s_folders);
        char txt[96];
        snprintf(txt, sizeof txt, "%s  (%d)", names[k][0] ? names[k] : "The card's top folder", counts[k]);
        lv_checkbox_set_text(cb, txt);
        lv_obj_set_style_text_font(cb, &art_font_20, 0);
        lv_obj_set_style_text_color(cb, lv_color_hex(TEXT), 0);
        outline(cb, LV_PART_INDICATOR);   /* an open box; ticked: chalk with a blue tick */
        lv_obj_set_style_radius(cb, 2, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(cb, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(cb, lv_color_hex(TEXT), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(cb, lv_color_hex(BG), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_state(cb, LV_STATE_CHECKED, settings_folder_on(names[k]));
        lv_obj_set_ext_click_area(cb, 6);
        lv_obj_add_event_cb(cb, folder_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
    }
    note(s_folders, "Pictures from the folders ticked. With none ticked, all of them.");
}

/* ---- the index ---- */

static void rescan_cb(lv_event_t *e) {
    lv_label_set_text(s_index, "Scanning the card...");
    slideshow_rescan();
}

static void show_index(void) {
    char sum[96];
    index_summary(sum, sizeof sum);
    if (!strcmp(sum, s_index_was)) return;
    bool scanned = strstr(s_index_was, "Scanning") && !strstr(sum, "Scanning");
    snprintf(s_index_was, sizeof s_index_was, "%s", sum);
    lv_label_set_text(s_index, sum);
    if (scanned) list_folders();   /* new folders, or gone ones */
}

/* ---- night ---- */

static void show_night(void) {
    lv_obj_set_hidden(s_hours, g_settings.night_mode != NIGHT_HOURS);
    lv_obj_set_hidden(s_sun_info, g_settings.night_mode != NIGHT_SUN);
    if (g_settings.night_mode == NIGHT_SUN) {
        time_t rise, set;
        if (night_sun_times(&rise, &set)) {
            char a[8], b[8];
            struct tm tm;
            localtime_r(&set, &tm);
            strftime(a, sizeof a, "%H:%M", &tm);
            localtime_r(&rise, &tm);
            strftime(b, sizeof b, "%H:%M", &tm);
            lv_label_set_text_fmt(s_sun_info, "Today the sun sets at %s and rose at %s", a, b);
        } else {
            lv_label_set_text(s_sun_info, "Needs the place (it comes with the weather) and the clock");
        }
    }
}

static void mode_cb(lv_event_t *e) {
    g_settings.night_mode = lv_buttonmatrix_get_selected_button(lv_event_get_current_target_obj(e));
    settings_save();
    show_night();
    night_refresh();
}

static void hhmm(lv_obj_t *l, const char *what, int min) { lv_label_set_text_fmt(l, "%s%02d:%02d", what, min / 60, min % 60); }

static void from_cb(lv_event_t *e) {
    g_settings.night_from = slider_value(e) * 30;
    hhmm(s_from_v, "", g_settings.night_from);
    if (s_built) night_refresh();
}

static void to_cb(lv_event_t *e) {
    g_settings.night_to = slider_value(e) * 30;
    hhmm(s_to_v, "", g_settings.night_to);
    if (s_built) night_refresh();
}

/* the night's level: off, or from the steady floor up (below it, it flickers) */
static const int LEVELS[] = { 0, BL_STEADY_MIN, BL_STEADY_MIN + 5, BL_STEADY_MIN + 10, BL_STEADY_MIN + 15,
                              BL_STEADY_MIN + 25, BL_STEADY_MIN + 35 };
#define N_LEVELS (int)(sizeof LEVELS / sizeof LEVELS[0])

static void level_cb(lv_event_t *e) {
    g_settings.night_level = LEVELS[slider_value(e)];
    if (g_settings.night_level) lv_label_set_text_fmt(s_level_v, "%d %%", (int)g_settings.night_level);
    else lv_label_set_text(s_level_v, "Off");
    if (s_built) night_refresh();
}

/* the backlight's switching: the slider's steps */
static const int HZ[] = { 1000, 2000, 5000, 10000, 20000, 30000 };
#define N_HZ (int)(sizeof HZ / sizeof HZ[0])

static void hz_cb(lv_event_t *e) {
    g_settings.backlight_hz = HZ[slider_value(e)];
    lv_label_set_text_fmt(s_hz_v, "%d kHz", (int)(g_settings.backlight_hz / 1000));
    if (s_built) board_set_backlight_hz(g_settings.backlight_hz);   /* at once: compare by eye */
}

/* ---- the radio ---- */

static void station_cb(lv_event_t *e) {
    g_settings.radio_station = lv_buttonmatrix_get_selected_button(lv_event_get_current_target_obj(e));
    settings_save();
    radio_retune();
}

static void volume_cb(lv_event_t *e) {
    g_settings.radio_volume = slider_value(e) * 5;
    lv_label_set_text_fmt(s_volume_v, "%d", (int)g_settings.radio_volume);
}

/* the stations, one a row, the chosen one filled */
static void stations(lv_obj_t *col) {
    static const char *map[2 * RADIO_MAX_STATIONS + 1];
    int n = radio_count(), m = 0;
    for (int k = 0; k < n; k++) {
        if (k) map[m++] = "\n";
        map[m++] = radio_name(k);
    }
    map[m] = "";
    lv_obj_t *bm = lv_buttonmatrix_create(col);
    lv_buttonmatrix_set_map(bm, map);
    lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(bm, true);
    int k = g_settings.radio_station >= 0 && g_settings.radio_station < n ? g_settings.radio_station : 0;
    lv_buttonmatrix_set_button_ctrl(bm, k, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(bm, LV_PCT(100), n * 44);
    lv_obj_set_style_text_font(bm, &art_font_16, 0);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_set_style_pad_row(bm, 6, 0);
    outline(bm, LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(bm, 2, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bm, lv_color_hex(TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bm, lv_color_hex(BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(bm, station_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---- about ---- */

static void about_cb(lv_timer_t *t) {
    show_index();
    char ssid[40], ip[20];
    bool on = wifi_info(ssid, sizeof ssid, ip, sizeof ip);
    const esp_app_desc_t *app = esp_app_get_description();
    int64_t up = esp_timer_get_time() / 1000000;
    lv_label_set_text_fmt(s_about,
                          "Wi-Fi   %s%s\n"
                          "IP address   %s\n"
                          "Pictures on the card   %d\n"
                          "Free memory   %u KB internal, %u KB PSRAM\n"
                          "Running for   %lld d %lld h %lld min\n"
                          "Firmware   %s, built %s %s",
                          ssid[0] ? ssid : "none", on ? "" : " (not connected)", ip, slideshow_count(),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), up / 86400, up % 86400 / 3600,
                          up % 3600 / 60, app->version, app->date, app->time);
}

void settings_page_shown(bool shown) {
    if (shown) {
        list_folders();
        show_night();
        lv_obj_set_state(s_radio_sw, LV_STATE_CHECKED, g_settings.radio_on != 0);   /* the note button or the night may have switched it */
        about_cb(NULL);
        lv_timer_resume(s_about_timer);
    } else {
        lv_timer_pause(s_about_timer);
    }
}

/* ---- the page ---- */

lv_obj_t *settings_page_create(lv_obj_t *parent) {
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollable(page, false);
    grid_make();
    if (s_tile.data) {
        lv_obj_set_style_bg_image_src(page, &s_tile, 0);
        lv_obj_set_style_bg_image_tiled(page, true, 0);
    }
    lv_obj_add_event_cb(page, sheet_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_t *title = label(page, &art_font_24, TEXT, "Settings");   /* right of the menu button */
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_set_pos(title, 90, 14);

    /* the title block, top right: three cells */
    lv_obj_t *tb = box(page, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(tb, 372, 40);
    lv_obj_set_pos(tb, 1024 - 20 - 372, 14);
    lv_obj_set_style_border_color(tb, lv_color_hex(TEXT), 0);
    lv_obj_set_style_border_width(tb, 1, 0);
    char rev[16];
    snprintf(rev, sizeof rev, "REV %.7s", esp_app_get_description()->version);
    const char *cells[] = { "DWG ART-SET", "SCALE 1:1", rev };
    const int widths[] = { 132, 104, 136 };
    for (int k = 0; k < 3; k++) {
        lv_obj_t *c = box(tb, LV_FLEX_FLOW_ROW);
        lv_obj_set_size(c, widths[k], LV_PCT(100));
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (k) {
            lv_obj_set_style_border_side(c, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(c, 1, 0);
            lv_obj_set_style_border_color(c, lv_color_hex(TEXT), 0);
        }
        lv_obj_set_style_text_letter_space(label(c, &art_font_16, k == 2 ? MINOR : TEXT, cells[k]), 1, 0);
    }

    /* the slideshow */
    lv_obj_t *left = column(page, 24, 480);
    heading(left, "SLIDESHOW");
    int step = 0;
    while (step + 1 < N_STEPS && STEPS[step] < g_settings.interval_s) step++;
    slider_row(left, "Each picture", 0, N_STEPS - 1, step, interval_cb, &s_interval_v);
    slider_row(left, "Change of picture", 3, 30, g_settings.fade_ms / 100, fade_cb, &s_fade_v);
    switch_row(left, "Varied changes (else a dissolve)", &g_settings.varied);
    switch_row(left, "Shuffle (else by name)", &g_settings.shuffle);
    switch_row(left, "Fill the screen (crop)", &g_settings.fill);
    switch_row(left, "Only favourites (the Gallery's stars)", &g_settings.favourites_only);
    switch_row32(left, "Captions (artist and title, a while)", NULL, &g_settings.captions);
    heading(left, "FOLDERS");
    s_folders = box(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_folders, 10, 0);

    /* the card's index (index.c) */
    heading(left, "CARD INDEX");
    s_index = note(left, "");
    lv_obj_set_style_text_color(s_index, lv_color_hex(TEXT), 0);
    note(left, "New pictures on the card show after a rescan (pictures saved from Artwork of the Day show at once). "
               "Pictures that can't be shown are left out until they change.");
    lv_obj_t *b = lv_button_create(left);
    outline(b, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_t *bl = label(b, &art_font_20, TEXT, "RESCAN THE CARD");
    lv_obj_set_style_text_letter_space(bl, 2, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, rescan_cb, LV_EVENT_CLICKED, NULL);

    /* night, and about */
    lv_obj_t *right = column(page, 528, 472);
    heading(right, "NIGHT");
    lv_obj_t *bm = lv_buttonmatrix_create(right);
    static const char *modes[] = { "Off", "Sunset to sunrise", "Set hours", "" };
    lv_buttonmatrix_set_map(bm, modes);
    lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(bm, true);
    lv_buttonmatrix_set_button_ctrl(bm, g_settings.night_mode, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_buttonmatrix_set_button_width(bm, 1, 2);   /* the long one */
    lv_obj_set_size(bm, LV_PCT(100), 56);
    lv_obj_set_style_text_font(bm, &art_font_16, 0);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 0, 0);
    outline(bm, LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(bm, 2, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bm, lv_color_hex(TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bm, lv_color_hex(BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(bm, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_sun_info = note(right, "");
    s_hours = box(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_hours, 14, 0);
    slider_row(s_hours, "Dark from", 0, 47, g_settings.night_from / 30, from_cb, &s_from_v);
    slider_row(s_hours, "Until", 0, 47, g_settings.night_to / 30, to_cb, &s_to_v);
    int lv = 0;
    while (lv + 1 < N_LEVELS && LEVELS[lv] < g_settings.night_level) lv++;
    slider_row(right, "The screen at night", 0, N_LEVELS - 1, lv, level_cb, &s_level_v);
    note(right, "Dark once it's left untouched for 2 minutes; a touch lights it again.");
    int hz = N_HZ - 1;
    while (hz > 0 && HZ[hz] > g_settings.backlight_hz) hz--;
    slider_row(right, "Backlight switching", 0, N_HZ - 1, hz, hz_cb, &s_hz_v);
    note(right, "If the screen flickers when it's dim, try another speed: it changes at once.");

    heading(right, "RADIO");
    s_radio_sw = switch_row32(right, "Music with the slideshow", NULL, &g_settings.radio_on);
    stations(right);
    slider_row(right, "Volume", 1, 20, g_settings.radio_volume / 5, volume_cb, &s_volume_v);
    note(right, "Internet stations, quiet ones: the slideshow's note button (top right) switches it too. "
                "Others in config.json on the card: \"radio\": { \"stations\": [ { \"name\": ..., \"url\": ... } ] }.");

    heading(right, "ABOUT");
    s_about = label(right, &art_font_16, TEXT, "");
    lv_obj_set_style_text_line_space(s_about, 6, 0);

    s_about_timer = lv_timer_create(about_cb, 2000, NULL);
    lv_timer_pause(s_about_timer);
    s_built = true;
    lv_obj_set_hidden(page, true);
    return page;
}
