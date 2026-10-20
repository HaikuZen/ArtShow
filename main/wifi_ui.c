/* wifi_ui.c - on-screen Wi-Fi setup: a status button in the corner of the screen
 * opens a panel to pick a network from a scan and type its password. wifi.c joins
 * it, and saves it in NVS once it connects. The Wi-Fi task calls back here, so
 * the callbacks take the LVGL lock. */
#include "wifi.h"
#include "esp_lvgl_port.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_btn_label, *s_panel, *s_list, *s_chosen, *s_pass, *s_status;
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
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
}

static void pick_cb(lv_event_t *e) {
    s_sel = (int)(intptr_t)lv_event_get_user_data(e);
    lv_label_set_text_fmt(s_chosen, "Network: %s", s_nets[s_sel].ssid);
    lv_textarea_set_text(s_pass, "");
    lv_obj_set_hidden(s_pass, s_nets[s_sel].open);
}

static void on_scan(const wifi_net_t *nets, int n) {
    if (!lvgl_port_lock(0)) return;
    lv_obj_clean(s_list);
    s_sel = -1;
    if (n < 0) list_text("Scan failed: Rescan in a moment");
    else if (n == 0) list_text("No networks found");
    for (int i = 0; i < n; i++) {
        s_nets[i] = nets[i];
        lv_obj_t *b = lv_button_create(s_list);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text_fmt(l, LV_SYMBOL_WIFI "  %s   %d dBm%s", nets[i].ssid, nets[i].rssi, nets[i].open ? "  open" : "");
        lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
        lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lvgl_port_unlock();
}

static void rescan(void) {
    lv_obj_clean(s_list);
    list_text("Scanning...");
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

    /* the setup panel, over everything on the top layer */
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 12, 0);   /* content: 1000 x 576 */
    lv_obj_set_scrollable(s_panel, false);
    lv_obj_set_hidden(s_panel, true);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, LV_SYMBOL_WIFI " Wi-Fi");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00C8FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 4);
    lv_obj_align(button(s_panel, "Close", close_cb), LV_ALIGN_TOP_RIGHT, -8, 0);
    lv_obj_align(button(s_panel, "Forget", forget_cb), LV_ALIGN_TOP_RIGHT, -140, 0);
    lv_obj_align(button(s_panel, LV_SYMBOL_REFRESH " Rescan", rescan_cb), LV_ALIGN_TOP_RIGHT, -290, 0);

    s_list = lv_obj_create(s_panel);
    lv_obj_set_size(s_list, 480, 260);   /* above the keyboard (bottom 250) */
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 0, 60);

    s_chosen = lv_label_create(s_panel);
    lv_label_set_text(s_chosen, "Pick a network");
    lv_obj_set_style_text_color(s_chosen, lv_color_hex(0xE0F0FF), 0);
    lv_obj_set_style_text_font(s_chosen, &lv_font_montserrat_24, 0);
    lv_obj_align(s_chosen, LV_ALIGN_TOP_LEFT, 500, 66);

    s_pass = lv_textarea_create(s_panel);
    lv_textarea_set_one_line(s_pass, true);
    lv_textarea_set_password_mode(s_pass, true);
    lv_textarea_set_max_length(s_pass, 64);
    lv_textarea_set_placeholder_text(s_pass, "Password");
    lv_obj_set_width(s_pass, 320);
    lv_obj_align(s_pass, LV_ALIGN_TOP_LEFT, 500, 108);
    lv_obj_align(button(s_panel, "Connect", connect_cb), LV_ALIGN_TOP_LEFT, 836, 106);

    s_status = lv_label_create(s_panel);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_status, 480);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xE0F0FF), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_status, "Starting Wi-Fi...");
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 500, 176);

    lv_obj_t *kb = lv_keyboard_create(s_panel);
    lv_obj_set_size(kb, LV_PCT(100), 250);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_pass);
    lv_obj_add_event_cb(kb, connect_cb, LV_EVENT_READY, NULL);   /* the keyboard's OK key */

    wifi_on_status(on_status);
}
