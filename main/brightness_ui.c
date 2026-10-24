/* brightness_ui.c - the backlight slider, folded into a corner button.
 * A sunrise dimmer: in a dark capsule (no shadow: it sits over the
 * slideshow's transitions), a track from night blue to sunlight, a drawn
 * sun for the knob whose rays grow with the level, a dim sun at the left
 * end, the level in cream at the right. */
#include "brightness_ui.h"
#include "board.h"
#include "nvs.h"
#include "esp_log.h"
#include "fonts/art_fonts.h"
#include <math.h>

static const char *TAG = "bright";

#define NVS_NS      "artshow"
#define NVS_KEY     "bright"
#define LEVEL_MIN   BL_STEADY_MIN   /* % : below it the backlight flickers */
#define LEVEL_DEF   80
#define CLOSE_MS    5000    /* the slider folds away after this long untouched */

#define TRACK_W     300
#define NIGHT       0x1F3A66
#define SUN         0xF6C95A
#define CREAM       0xF0EADC

static lv_obj_t *s_box, *s_slider, *s_value;
static lv_timer_t *s_close;

static int load_level(void) {
    nvs_handle_t h;
    int32_t v = LEVEL_DEF;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, NVS_KEY, &v);
        nvs_close(h);
    }
    return v < LEVEL_MIN ? LEVEL_MIN : v > 100 ? 100 : (int)v;
}

static void save_level(int v) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_i32(h, NVS_KEY, v) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "brightness %d %%", v);
}

static void close_cb(lv_timer_t *t) {
    lv_obj_set_hidden(s_box, true);
    lv_timer_pause(t);
}

/* each touch keeps the slider open CLOSE_MS longer */
static void keep_open(void) {
    lv_timer_reset(s_close);
    lv_timer_resume(s_close);
}

static void toggle_cb(lv_event_t *e) {
    bool open = lv_obj_is_hidden(s_box);
    lv_obj_set_hidden(s_box, !open);
    if (open) keep_open();
    else lv_timer_pause(s_close);
}

static void slide_cb(lv_event_t *e) {
    board_set_brightness(lv_slider_get_value(s_slider));
    lv_label_set_text_fmt(s_value, "%d %%", (int)lv_slider_get_value(s_slider));
    keep_open();
}

static void release_cb(lv_event_t *e) {
    save_level(lv_slider_get_value(s_slider));   /* once per drag, not per step: flash wear */
    keep_open();
}

/* ---- drawn: the track, the sun ---- */

/* a sun at (cx, cy): a disc of radius r, 8 rays of length len (none: 0), at
   opa; hollow: a ring */
static void sun(lv_layer_t *layer, int cx, int cy, int r, int len, lv_opa_t opa, bool hollow) {
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = lv_color_hex(SUN);
    ln.opa = opa;
    ln.width = 2;
    ln.round_start = ln.round_end = 1;
    for (int k = 0; k < 8 && len > 0; k++) {
        float a = k * (float)M_PI / 4;
        ln.p1.x = cx + cosf(a) * (r + 3);
        ln.p1.y = cy + sinf(a) * (r + 3);
        ln.p2.x = cx + cosf(a) * (r + 3 + len);
        ln.p2.y = cy + sinf(a) * (r + 3 + len);
        lv_draw_line(layer, &ln);
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(SUN);
    d.bg_opa = hollow ? LV_OPA_TRANSP : opa;
    d.border_color = lv_color_hex(SUN);
    d.border_width = hollow ? 2 : 0;
    d.border_opa = opa;
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };
    lv_draw_rect(layer, &d, &a);
}

static void track_draw(lv_event_t *e) {   /* under the knob: the night-to-day bar */
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(s_slider, &a);
    int cy = (a.y1 + a.y2) / 2;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    lv_color_t c[2] = { lv_color_hex(NIGHT), lv_color_hex(SUN) };
    lv_grad_init_stops(&d.bg_grad, c, NULL, NULL, 2);
    lv_grad_horizontal_init(&d.bg_grad);
    lv_area_t t = { a.x1, cy - 3, a.x2, cy + 3 };
    lv_draw_rect(layer, &d, &t);
}

static void knob_draw(lv_event_t *e) {   /* the sun: brighter and longer-rayed with the level */
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(s_slider, &a);
    int v = lv_slider_get_value(s_slider), w = lv_area_get_width(&a) - 1;
    int f = (v - LEVEL_MIN) * 100 / (100 - LEVEL_MIN);   /* 0..100 */
    int cx = a.x1 + w * f / 100, cy = (a.y1 + a.y2) / 2;
    sun(layer, cx, cy, 9, 2 + f * 6 / 100, (lv_opa_t)(170 + f * 85 / 100), false);
}

/* the sun reaches 20 px from its middle: past the slider's ends and its
   height (36); without this LVGL clips it there */
static void ext_cb(lv_event_t *e) { lv_event_set_ext_draw_size(e, 22); }

static void dim_draw(lv_event_t *e) {   /* the left end: a faint, hollow sun */
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &a);
    sun(layer, (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, 5, 0, LV_OPA_60, true);
}

void brightness_ui_create(lv_obj_t *parent) {
    int level = load_level();
    board_set_brightness(level);

    /* the corner button, styled like the Wi-Fi one in the other corner */
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_opa(l, LV_OPA_60, 0);
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 12, -12);
    lv_obj_set_ext_click_area(b, 24);   /* ~92 px to touch, out to the corner; it still looks 44 */
    lv_obj_add_event_cb(b, toggle_cb, LV_EVENT_CLICKED, NULL);

    /* the capsule: a dim sun, the slider, the level */
    s_box = lv_obj_create(parent);
    lv_obj_remove_style_all(s_box);
    lv_obj_set_size(s_box, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(0x0C0C0C), 0);
    lv_obj_set_style_bg_opa(s_box, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_box, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_box, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_box, LV_OPA_10, 0);
    lv_obj_set_style_border_width(s_box, 1, 0);
    lv_obj_set_style_pad_left(s_box, 16, 0);
    lv_obj_set_style_pad_right(s_box, 18, 0);
    lv_obj_set_style_pad_column(s_box, 16, 0);
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(s_box, false);
    lv_obj_align_to(s_box, b, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    lv_obj_t *dim = lv_obj_create(s_box);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, 14, 14);
    lv_obj_set_clickable(dim, false);
    lv_obj_add_event_cb(dim, dim_draw, LV_EVENT_DRAW_MAIN, NULL);

    s_slider = lv_slider_create(s_box);
    lv_obj_set_size(s_slider, TRACK_W, 36);   /* room for the sun's rays */
    lv_obj_set_style_margin_hor(s_slider, 14, 0);   /* ... at both ends, inside the capsule */
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_TRANSP, LV_PART_MAIN);   /* track and sun are drawn */
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_slider, 0, LV_PART_KNOB);
    lv_obj_set_style_outline_width(s_slider, 0, LV_STATE_FOCUS_KEY);
    lv_slider_set_range(s_slider, LEVEL_MIN, 100);
    lv_slider_set_value(s_slider, level, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s_slider, 12);
    lv_obj_add_event_cb(s_slider, track_draw, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_add_event_cb(s_slider, knob_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(s_slider, ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(s_slider);
    lv_obj_add_event_cb(s_slider, slide_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_slider, release_cb, LV_EVENT_RELEASED, NULL);

    s_value = lv_label_create(s_box);
    lv_obj_set_style_text_font(s_value, &art_font_16, 0);
    lv_obj_set_style_text_color(s_value, lv_color_hex(CREAM), 0);
    lv_obj_set_style_text_letter_space(s_value, 1, 0);
    lv_obj_set_width(s_value, 50);
    lv_obj_set_style_text_align(s_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text_fmt(s_value, "%d %%", level);
    lv_obj_set_hidden(s_box, true);

    s_close = lv_timer_create(close_cb, CLOSE_MS, NULL);
    lv_timer_pause(s_close);
}

int brightness_ui_level(void) { return s_slider ? lv_slider_get_value(s_slider) : load_level(); }
