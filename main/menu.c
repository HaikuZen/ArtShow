/* menu.c - the hamburger menu: a corner button opens a list of the pages;
 * picking one hides the page on show and shows that one. */
#include "menu.h"

#define MAX_PAGES 16

static menu_page_t s_pages[MAX_PAGES];
static int s_n, s_cur;
static lv_obj_t *s_list, *s_items[MAX_PAGES];

static void show(int k) {
    if (k == s_cur) return;
    const menu_page_t *old = &s_pages[s_cur], *new = &s_pages[k];
    if (old->page) lv_obj_set_hidden(old->page, true);
    if (old->shown) old->shown(false);
    if (new->page) lv_obj_set_hidden(new->page, false);
    if (new->shown) new->shown(true);
    lv_obj_set_state(s_items[s_cur], LV_STATE_CHECKED, false);
    lv_obj_set_state(s_items[k], LV_STATE_CHECKED, true);
    s_cur = k;
}

static void toggle_cb(lv_event_t *e) { lv_obj_set_hidden(s_list, !lv_obj_is_hidden(s_list)); }

static void pick_cb(lv_event_t *e) {
    show((int)(intptr_t)lv_event_get_user_data(e));
    lv_obj_set_hidden(s_list, true);
}

void menu_create(lv_obj_t *parent, const menu_page_t *pages, int n) {
    s_n = n < MAX_PAGES ? n : MAX_PAGES;
    for (int k = 0; k < s_n; k++) s_pages[k] = pages[k];

    /* the button, like the Wi-Fi and brightness ones in the other corners */
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_opa(l, LV_OPA_60, 0);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_set_ext_click_area(b, 24);   /* ~92 px to touch, out to the corner; it still looks 44 */
    lv_obj_add_event_cb(b, toggle_cb, LV_EVENT_CLICKED, NULL);

    /* the list: a flex column under the button (lv_list is deprecated in 9.6) */
    s_list = lv_obj_create(parent);
    lv_obj_set_size(s_list, 280, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list, 10, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_style_max_height(s_list, 600 - 64 - 8, 0);   /* the screen below the button: more pages scroll */
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x0D1B2A), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(0x415A77), 0);
    lv_obj_set_scrollable(s_list, true);
    lv_obj_align_to(s_list, b, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    for (int k = 0; k < s_n; k++) {
        lv_obj_t *item = lv_button_create(s_list);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_style_bg_color(item, lv_color_hex(0x1B263B), 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x415A77), LV_STATE_CHECKED);   /* the page on show */
        lv_obj_set_style_shadow_width(item, 0, 0);
        lv_obj_set_style_pad_ver(item, 8, 0);   /* ten pages in the screen's height */
        lv_obj_t *il = lv_label_create(item);
        lv_label_set_text(il, s_pages[k].label);
        lv_obj_set_style_text_font(il, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(il, lv_color_hex(0xE0F0FF), 0);
        lv_obj_add_event_cb(item, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        s_items[k] = item;
    }
    lv_obj_set_state(s_items[0], LV_STATE_CHECKED, true);
    lv_obj_set_hidden(s_list, true);
}

void menu_select(int k) {
    if (k >= 0 && k < s_n) show(k);
    lv_obj_set_hidden(s_list, true);
}
