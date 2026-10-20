/* settings_page.c - two scrolling columns: the slideshow's settings and its
 * folders on the left; night mode and about the board on the right. A change
 * takes effect at once (the next picture, for the slideshow's) and is kept
 * in NVS: a slider's when it's let go, a switch's when it flips. */
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
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BG          0x0D1B2A
#define MINOR       0x778DA9
#define TEXT        0xE0F0FF
#define MAX_FOLDERS 16

/* seconds per picture: the slider's steps */
static const int STEPS[] = { 5, 10, 15, 20, 30, 45, 60, 90, 120, 180, 300, 600, 900, 1800, 3600 };
#define N_STEPS (int)(sizeof STEPS / sizeof STEPS[0])

static lv_obj_t *s_interval_v, *s_fade_v, *s_from_v, *s_to_v, *s_level_v, *s_hz_v, *s_hours, *s_sun_info;
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

static void heading(lv_obj_t *col, const char *txt) {
    lv_obj_t *l = label(col, &art_font_24, TEXT, txt);
    lv_obj_set_style_pad_top(l, 6, 0);
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

/* "Title ........ [switch]" for a bool setting */
static void switch_row(lv_obj_t *col, const char *title, bool *field) {
    lv_obj_t *row = box(col, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(row, &art_font_20, TEXT, title);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_state(sw, LV_STATE_CHECKED, *field);
    lv_obj_set_ext_click_area(sw, 12);
    lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, field);
}

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
    lv_obj_set_pos(label(page, &art_font_24, TEXT, "Settings"), 90, 12);   /* right of the menu button */

    /* the slideshow */
    lv_obj_t *left = column(page, 24, 480);
    heading(left, "Slideshow");
    int step = 0;
    while (step + 1 < N_STEPS && STEPS[step] < g_settings.interval_s) step++;
    slider_row(left, "Each picture", 0, N_STEPS - 1, step, interval_cb, &s_interval_v);
    slider_row(left, "Change of picture", 3, 30, g_settings.fade_ms / 100, fade_cb, &s_fade_v);
    switch_row(left, "Varied changes (else a dissolve)", &g_settings.varied);
    switch_row(left, "Shuffle (else by name)", &g_settings.shuffle);
    switch_row(left, "Fill the screen (crop)", &g_settings.fill);
    switch_row(left, "Only favourites (the Gallery's stars)", &g_settings.favourites_only);
    heading(left, "Folders");
    s_folders = box(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_folders, 10, 0);

    /* the card's index (index.c) */
    heading(left, "Card index");
    s_index = note(left, "");
    lv_obj_set_style_text_color(s_index, lv_color_hex(TEXT), 0);
    note(left, "New pictures on the card show after a rescan (pictures saved from Artwork of the Day show at once). "
               "Pictures that can't be shown are left out until they change.");
    lv_obj_t *b = lv_button_create(left);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x415A77), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *bl = label(b, &art_font_20, TEXT, "Rescan the card");
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, rescan_cb, LV_EVENT_CLICKED, NULL);

    /* night, and about */
    lv_obj_t *right = column(page, 528, 472);
    heading(right, "Night");
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

    heading(right, "About");
    s_about = label(right, &art_font_16, TEXT, "");
    lv_obj_set_style_text_line_space(s_about, 6, 0);

    s_about_timer = lv_timer_create(about_cb, 2000, NULL);
    lv_timer_pause(s_about_timer);
    s_built = true;
    lv_obj_set_hidden(page, true);
    return page;
}
