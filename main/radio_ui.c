/* radio_ui.c - the slideshow's radio button, top right left of the drop:
 * a note, cream while the radio is on, grey when off. A tap switches it
 * (kept in g_settings.radio_on); a dark capsule under it says the station,
 * or what's wrong, for a few seconds. The radio plays on the slideshow
 * and the pages that say radio_ui_here(true) when shown. */
#include "radio_ui.h"
#include "radio.h"
#include "settings.h"
#include "fonts/art_fonts.h"
#include <string.h>

#define CREAM     0xF0EADC
#define SHOW_MS   5000

static lv_obj_t *s_btn, *s_icon, *s_toast, *s_text;
static lv_timer_t *s_timer;
static int s_left;   /* the toast's time left, ms */
static bool s_here = true;   /* on a page with the radio (the slideshow first, at boot) */
static lv_timer_t *s_apply;

static void paint(void) {
    bool on = g_settings.radio_on;
    lv_obj_set_style_text_color(s_icon, lv_color_hex(on ? CREAM : 0x808080), 0);
    lv_obj_set_style_text_opa(s_icon, on ? LV_OPA_90 : LV_OPA_60, 0);
}

static void tick(lv_timer_t *t) {
    char st[96];
    if (!radio_status(st, sizeof st)) strcpy(st, "Radio off");
    if (!st[0]) strcpy(st, "Quiet: the night");
    if (strcmp(lv_label_get_text(s_text), st)) {
        lv_label_set_text(s_text, st);
        s_left = SHOW_MS;   /* news: a while longer */
    }
    if ((s_left -= 500) <= 0) {
        lv_obj_set_hidden(s_toast, true);
        lv_timer_pause(t);
    }
}

static void toast(void) {
    s_left = SHOW_MS;
    lv_obj_set_hidden(s_toast, false);
    lv_timer_resume(s_timer);
    tick(s_timer);
}

static void click_cb(lv_event_t *e) {
    g_settings.radio_on = !g_settings.radio_on;
    settings_save();
    radio_play(g_settings.radio_on);
    paint();
    toast();
}

/* the menu hides one page, then shows the next: the radio follows once
   both are said, so going between two of its pages doesn't drop the stream */
static void apply_cb(lv_timer_t *t) {
    lv_timer_pause(t);
    radio_play(s_here && g_settings.radio_on);
}

void radio_ui_here(bool here) {
    s_here = here;
    lv_timer_reset(s_apply);
    lv_timer_resume(s_apply);
}

void radio_ui_slideshow(bool shown) {
    paint();   /* the Settings page may have switched it */
    lv_obj_set_hidden(s_btn, !shown);
    if (!shown) {
        lv_obj_set_hidden(s_toast, true);
        lv_timer_pause(s_timer);
    }
    radio_ui_here(shown);
}

void radio_ui_create(lv_obj_t *parent) {
    s_btn = lv_button_create(parent);   /* as the "?" and the drop */
    lv_obj_set_style_bg_color(s_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(s_btn, 0, 0);
    lv_obj_set_style_pad_all(s_btn, 10, 0);
    lv_obj_set_style_min_width(s_btn, 44, 0);
    s_icon = lv_label_create(s_btn);
    lv_label_set_text(s_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(s_icon, &lv_font_montserrat_24, 0);
    lv_obj_center(s_icon);
    lv_obj_align(s_btn, LV_ALIGN_TOP_RIGHT, -172, 12);
    lv_obj_set_ext_click_area(s_btn, 12);
    lv_obj_add_event_cb(s_btn, click_cb, LV_EVENT_CLICKED, NULL);
    paint();

    /* the capsule, its right edge under the button's (no shadow: over the transitions) */
    s_toast = lv_obj_create(parent);
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x0C0C0C), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_toast, LV_OPA_10, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_pad_hor(s_toast, 18, 0);
    lv_obj_set_style_pad_ver(s_toast, 9, 0);
    lv_obj_set_clickable(s_toast, false);
    lv_obj_align(s_toast, LV_ALIGN_TOP_RIGHT, -172, 70);
    s_text = lv_label_create(s_toast);
    lv_obj_set_style_text_font(s_text, &art_font_16, 0);
    lv_obj_set_style_text_color(s_text, lv_color_hex(CREAM), 0);
    lv_obj_set_style_text_letter_space(s_text, 1, 0);
    lv_label_set_text(s_text, "");
    lv_obj_set_hidden(s_toast, true);
    s_timer = lv_timer_create(tick, 500, NULL);
    lv_timer_pause(s_timer);
    s_apply = lv_timer_create(apply_cb, 20, NULL);
    lv_timer_pause(s_apply);
    radio_play(g_settings.radio_on);   /* the slideshow is the first page: the menu says nothing at boot */
}
