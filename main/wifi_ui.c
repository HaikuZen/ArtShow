/* wifi_ui.c - on-screen Wi-Fi setup: a status button in the corner of the screen
 * opens a panel to pick a network from a scan and type its password. wifi.c joins
 * it, and saves it in NVS once it connects. The Wi-Fi task calls back here, so
 * the callbacks take the LVGL lock.
 * The panel, a 1950s Bakelite radio in azure: the networks are stations
 * on an azure-lit dial glass in a steel bezel, each with its signal's bars
 * (drawn), the one picked under a bright needle; pale knobs for the
 * buttons, an azure "Tune in", a keyboard of pale keys on navy Bakelite. */
#include "wifi.h"
#include "esp_lvgl_port.h"
#include "fonts/art_fonts.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BAKELITE 0x0B1A2A   /* navy */
#define LIGHT    0x8FD3FF   /* the dial's light: azure */
#define GLASS    0x0E2A44
#define STEEL    0x9FB8CC   /* the bezel: steel */
#define PALE     0xE6F2FB   /* the knobs and keys: pale azure */
#define NAVY     0x10283F   /* their ink: deep navy */
#define NEEDLE   0x2E8BD8   /* the needle and "Tune in": azure */
#define MIST     0xDCEEFA

static lv_obj_t *s_btn_label, *s_panel, *s_list, *s_chosen, *s_pass, *s_status, *s_rows[WIFI_MAX_NETS];
static wifi_net_t s_nets[WIFI_MAX_NETS];
static int s_sel = -1;

static void on_status(wifi_state_t st, const char *ssid, const char *detail) {
    uint32_t color;   /* the corner icon: connected or not, nothing more */
    char line[128];
    switch (st) {
    case WIFI_CONNECTED:
        color = 0xFFFFFF;
        snprintf(line, sizeof line, "Connected to %s\nIP %s", ssid, detail);
        break;
    case WIFI_CONNECTING:
        color = 0x808080;
        snprintf(line, sizeof line, "Connecting to %s...", ssid);
        break;
    case WIFI_RETRYING:
        color = 0x808080;
        snprintf(line, sizeof line, "Can't join %s\n(%s)", ssid, detail);
        break;
    case WIFI_IDLE:
        color = 0x808080;
        snprintf(line, sizeof line, "No network set: pick one");
        break;
    default:
        color = 0x808080;
        snprintf(line, sizeof line, "Wi-Fi is off (%s)", detail);
    }
    if (!s_btn_label || !lvgl_port_lock(0)) return;
    lv_obj_set_style_text_color(s_btn_label, lv_color_hex(color), 0);
    lv_obj_set_style_text_opa(s_btn_label, st == WIFI_CONNECTED ? LV_OPA_COVER : LV_OPA_60, 0);
    lv_label_set_text(s_status, line);
    lvgl_port_unlock();
}

/* the scan list: a scrolling flex column (lv_list is deprecated in LVGL 9.6) */
static void list_text(const char *txt) {
    lv_obj_t *l = lv_label_create(s_list);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &art_font_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(LIGHT), 0);
    lv_obj_set_style_pad_all(l, 10, 0);
}

/* a station's signal: four bars, lit by its strength */
static void bars_draw(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int rssi = s_nets[i].rssi, lit = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(LIGHT);
    d.radius = 1;
    int base = (a.y1 + a.y2) / 2 + 9;
    for (int k = 0; k < 4; k++) {
        d.bg_opa = k < lit ? LV_OPA_COVER : LV_OPA_20;
        lv_area_t r = { a.x2 - 50 + k * 9, base - 6 - k * 4, a.x2 - 45 + k * 9, base };
        lv_draw_rect(layer, &d, &r);
    }
}

static void pick_cb(lv_event_t *e) {
    s_sel = (int)(intptr_t)lv_event_get_user_data(e);
    for (int i = 0; i < WIFI_MAX_NETS; i++) if (s_rows[i]) lv_obj_set_state(s_rows[i], LV_STATE_CHECKED, i == s_sel);
    lv_label_set_text_fmt(s_chosen, "Tuned to  %s", s_nets[s_sel].ssid);
    lv_textarea_set_text(s_pass, "");
    lv_obj_set_hidden(s_pass, s_nets[s_sel].open);
}

static void on_scan(const wifi_net_t *nets, int n) {
    if (!lvgl_port_lock(0)) return;
    lv_obj_clean(s_list);
    memset(s_rows, 0, sizeof s_rows);
    s_sel = -1;
    if (n < 0) list_text("Scan failed: Rescan in a moment");
    else if (n == 0) list_text("No networks found");
    for (int i = 0; i < n; i++) {
        s_nets[i] = nets[i];
        lv_obj_t *b = s_rows[i] = lv_button_create(s_list);   /* a station on the dial */
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_PCT(100), 46);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(LIGHT), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(b, lv_color_hex(LIGHT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_10, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(b, lv_color_hex(NEEDLE), LV_STATE_CHECKED);   /* the needle */
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_LEFT, LV_STATE_CHECKED);
        lv_obj_set_style_border_width(b, 4, LV_STATE_CHECKED);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text_fmt(l, "%s%s", nets[i].ssid, nets[i].open ? "   \xC2\xB7  open" : "");
        lv_obj_set_style_text_font(l, &art_font_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(LIGHT), 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(l, 360, 24);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_add_event_cb(b, bars_draw, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lvgl_port_unlock();
}

static void rescan(void) {
    lv_obj_clean(s_list);
    memset(s_rows, 0, sizeof s_rows);
    list_text("Tuning across the band...");
    s_sel = -1;
    wifi_scan(on_scan);
}

static void connect_cb(lv_event_t *e) {
    if (s_sel < 0) {
        lv_label_set_text(s_status, "Pick a network first");
        return;
    }
    wifi_join(s_nets[s_sel].ssid, s_nets[s_sel].open ? "" : lv_textarea_get_text(s_pass));
}

static void open_cb(lv_event_t *e) { lv_obj_set_hidden(s_panel, false); rescan(); }
static void close_cb(lv_event_t *e) { lv_obj_set_hidden(s_panel, true); }
static void rescan_cb(lv_event_t *e) { rescan(); }
static void forget_cb(lv_event_t *e) { wifi_forget(); }

static lv_obj_t *button(lv_obj_t *parent, const char *txt, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* a pale knob (or, in azure, the one that tunes in) */
static lv_obj_t *knob(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, uint32_t ground, uint32_t ink) {
    lv_obj_t *b = button(parent, txt, cb);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_obj_set_style_text_font(l, &art_font_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(ink), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(ground), 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_set_style_pad_ver(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 8, 0);
    lv_obj_set_style_shadow_offset_y(b, 3, 0);
    lv_obj_set_style_shadow_color(b, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_60, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    return b;
}

void wifi_ui_create(lv_obj_t *parent) {
    /* over the slideshow, in the corner: the Wi-Fi icon, white when connected,
       dim grey when not, and the way into the setup panel */
    lv_obj_t *b = button(parent, LV_SYMBOL_WIFI, open_cb);
    s_btn_label = lv_obj_get_child(b, 0);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    lv_obj_set_style_text_color(s_btn_label, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_opa(s_btn_label, LV_OPA_60, 0);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_set_ext_click_area(b, 24);   /* ~92 px to touch, out to the corner; it still looks 44 */

    /* the setup panel, over everything on the top layer: the radio */
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(BAKELITE), 0);
    lv_obj_set_style_bg_grad_color(s_panel, lv_color_hex(0x050D16), 0);
    lv_obj_set_style_bg_grad_dir(s_panel, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 12, 0);   /* content: 1000 x 576 */
    lv_obj_set_scrollable(s_panel, false);
    lv_obj_set_hidden(s_panel, true);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "WIRELESS");
    lv_obj_set_style_text_color(title, lv_color_hex(LIGHT), 0);
    lv_obj_set_style_text_font(title, &art_font_32, 0);
    lv_obj_set_style_text_letter_space(title, 6, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_obj_t *close = knob(s_panel, "Close", close_cb, PALE, NAVY);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -8, 0);
    lv_obj_t *forget = knob(s_panel, "Forget", forget_cb, PALE, NAVY);
    lv_obj_align_to(forget, close, LV_ALIGN_OUT_LEFT_MID, -14, 0);
    lv_obj_t *re = knob(s_panel, "Rescan", rescan_cb, PALE, NAVY);
    lv_obj_align_to(re, forget, LV_ALIGN_OUT_LEFT_MID, -14, 0);

    s_list = lv_obj_create(s_panel);   /* the dial glass, lit azure from behind, in a steel bezel */
    lv_obj_set_size(s_list, 480, 260);   /* above the keyboard (bottom 250) */
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 2, 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x16406A), 0);
    lv_obj_set_style_bg_grad_color(s_list, lv_color_hex(GLASS), 0);
    lv_obj_set_style_bg_grad_dir(s_list, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(STEEL), 0);
    lv_obj_set_style_border_width(s_list, 3, 0);
    lv_obj_set_style_radius(s_list, 12, 0);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(LIGHT), LV_PART_SCROLLBAR);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, 60);

    s_chosen = lv_label_create(s_panel);
    lv_label_set_text(s_chosen, "Pick a station on the dial");
    lv_obj_set_style_text_color(s_chosen, lv_color_hex(LIGHT), 0);
    lv_obj_set_style_text_font(s_chosen, &art_font_24, 0);
    lv_label_set_long_mode(s_chosen, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_chosen, 490, 30);
    lv_obj_align(s_chosen, LV_ALIGN_TOP_LEFT, 500, 66);

    s_pass = lv_textarea_create(s_panel);   /* a pale field */
    lv_textarea_set_one_line(s_pass, true);
    lv_textarea_set_password_mode(s_pass, true);
    lv_textarea_set_max_length(s_pass, 64);
    lv_textarea_set_placeholder_text(s_pass, "Password");
    lv_obj_set_width(s_pass, 320);
    lv_obj_set_style_bg_color(s_pass, lv_color_hex(PALE), 0);
    lv_obj_set_style_text_color(s_pass, lv_color_hex(NAVY), 0);
    lv_obj_set_style_text_font(s_pass, &art_font_20, 0);
    lv_obj_set_style_border_color(s_pass, lv_color_hex(STEEL), 0);
    lv_obj_set_style_border_width(s_pass, 2, 0);
    lv_obj_set_style_radius(s_pass, 8, 0);
    lv_obj_set_style_border_color(s_pass, lv_color_hex(LIGHT), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(s_pass, lv_color_hex(0x7A93A8), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_align(s_pass, LV_ALIGN_TOP_LEFT, 500, 108);
    lv_obj_t *tune = knob(s_panel, "Tune in", connect_cb, NEEDLE, PALE);
    lv_obj_align(tune, LV_ALIGN_TOP_LEFT, 836, 106);

    s_status = lv_label_create(s_panel);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_status, 480);
    lv_obj_set_style_text_color(s_status, lv_color_hex(MIST), 0);
    lv_obj_set_style_text_font(s_status, &art_font_20, 0);
    lv_label_set_text(s_status, "Starting Wi-Fi...");
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 500, 176);

    lv_obj_t *kb = lv_keyboard_create(s_panel);   /* pale keys on navy Bakelite */
    lv_obj_set_size(kb, LV_PCT(100), 250);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x07121E), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(PALE), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(NAVY), LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0xA9C7DF), LV_PART_ITEMS | LV_STATE_CHECKED);   /* the control keys */
    lv_obj_set_style_text_color(kb, lv_color_hex(NAVY), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(LIGHT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_keyboard_set_textarea(kb, s_pass);
    lv_obj_add_event_cb(kb, connect_cb, LV_EVENT_READY, NULL);   /* the keyboard's OK key */

    wifi_on_status(on_status);
}
