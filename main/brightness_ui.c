/* brightness_ui.c - the backlight slider, folded into a corner button. */
#include "brightness_ui.h"
#include "board.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "bright";

#define NVS_NS      "artshow"
#define NVS_KEY     "bright"
#define LEVEL_MIN   5       /* % : below it the screen looks off */
#define LEVEL_DEF   80
#define CLOSE_MS    5000    /* the slider folds away after this long untouched */

static lv_obj_t *s_slider;
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
    lv_obj_set_hidden(s_slider, true);
    lv_timer_pause(t);
}

/* each touch keeps the slider open CLOSE_MS longer */
static void keep_open(void) {
    lv_timer_reset(s_close);
    lv_timer_resume(s_close);
}

static void toggle_cb(lv_event_t *e) {
    bool open = lv_obj_is_hidden(s_slider);
    lv_obj_set_hidden(s_slider, !open);
    if (open) keep_open();
    else lv_timer_pause(s_close);
}

static void slide_cb(lv_event_t *e) {
    board_set_brightness(lv_slider_get_value(s_slider));
    keep_open();
}

static void release_cb(lv_event_t *e) {
    save_level(lv_slider_get_value(s_slider));   /* once per drag, not per step: flash wear */
    keep_open();
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

    s_slider = lv_slider_create(parent);
    lv_obj_set_width(s_slider, 360);
    lv_slider_set_range(s_slider, LEVEL_MIN, 100);
    lv_slider_set_value(s_slider, level, LV_ANIM_OFF);
    lv_obj_align_to(s_slider, b, LV_ALIGN_OUT_RIGHT_MID, 28, 0);
    lv_obj_set_ext_click_area(s_slider, 20);   /* a thin bar: easier to catch */
    lv_obj_add_event_cb(s_slider, slide_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_slider, release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_set_hidden(s_slider, true);

    s_close = lv_timer_create(close_cb, CLOSE_MS, NULL);
    lv_timer_pause(s_close);
}

int brightness_ui_level(void) { return s_slider ? lv_slider_get_value(s_slider) : load_level(); }
