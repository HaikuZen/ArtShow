/* menu.c - the hamburger menu: a corner button opens a list of the pages;
 * picking one hides the page on show and shows that one.
 * The list is a museum's floor guide: a cream leaflet, each page a room
 * (a Roman numeral in seal red, its name, its icon in the page's own
 * colour), the room on show marked with a seal-red rule. */
#include "menu.h"
#include "fonts/art_fonts.h"
#include <string.h>

#define MAX_PAGES 16
#define PAPER     0xF4EFE4
#define RIM       0xCFC6B4
#define INK       0x2A2420
#define GREY      0x8A8174
#define SEAL      0xB0362A
#define HERE      0xEAE2D2   /* the room on show */

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

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *txt) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

/* a room: "IV   Weather   (icon)". The label's leading LV_SYMBOL (U+F000..F8FF,
   three bytes in UTF-8: not in the art fonts) becomes the icon */
static lv_obj_t *room(lv_obj_t *list, int k) {
    static const char *ROMAN[MAX_PAGES] = { "I", "II", "III", "IV", "V", "VI", "VII", "VIII",
                                            "IX", "X", "XI", "XII", "XIII", "XIV", "XV", "XVI" };
    const char *label = s_pages[k].label;
    char sym[4] = "";
    if ((uint8_t)label[0] == 0xEF && label[1] && label[2]) {
        memcpy(sym, label, 3);
        label += 3;
        while (*label == ' ') label++;
    }
    lv_obj_t *item = lv_button_create(list);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(item, 10, 0);
    lv_obj_set_style_pad_ver(item, 9, 0);   /* ten rooms in the screen's height */
    lv_obj_set_style_pad_column(item, 6, 0);
    lv_obj_set_style_radius(item, 2, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(HERE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(item, lv_color_hex(HERE), LV_STATE_CHECKED);   /* the room on show */
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(item, lv_color_hex(SEAL), LV_STATE_CHECKED);
    lv_obj_set_style_border_side(item, LV_BORDER_SIDE_LEFT, LV_STATE_CHECKED);
    lv_obj_set_style_border_width(item, 3, LV_STATE_CHECKED);
    lv_obj_t *num = text(item, &art_font_16, SEAL, ROMAN[k]);
    lv_obj_set_width(num, 44);
    lv_obj_set_style_text_letter_space(num, 1, 0);
    lv_obj_t *name = text(item, &art_font_20, INK, label);
    lv_obj_set_flex_grow(name, 1);
    if (sym[0]) {
        lv_obj_t *icon = text(item, &lv_font_montserrat_24, s_pages[k].color ? s_pages[k].color : GREY, sym);
        lv_obj_set_width(icon, 30);
        lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_add_event_cb(item, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    return item;
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

    /* the leaflet: a flex column under the button (lv_list is deprecated in 9.6) */
    s_list = lv_obj_create(parent);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 320, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_list, 12, 0);
    lv_obj_set_style_pad_top(s_list, 14, 0);
    lv_obj_set_style_pad_row(s_list, 2, 0);
    lv_obj_set_style_max_height(s_list, 600 - 64 - 8, 0);   /* the screen below the button: more pages scroll */
    lv_obj_set_style_bg_color(s_list, lv_color_hex(PAPER), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_list, lv_color_hex(RIM), 0);
    lv_obj_set_style_border_width(s_list, 1, 0);
    lv_obj_set_style_radius(s_list, 3, 0);
    lv_obj_set_style_outline_width(s_list, 2, 0);   /* a dark edge, not a shadow: the leaflet sits over the
                                                        slideshow's transitions, and a shadow's buffers each
                                                        frame left an SD read without its DMA buffer once */
    lv_obj_set_style_outline_color(s_list, lv_color_black(), 0);
    lv_obj_set_style_outline_opa(s_list, LV_OPA_40, 0);
    lv_obj_set_scrollable(s_list, true);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_align_to(s_list, b, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    lv_obj_t *head = text(s_list, &art_font_16, GREY, "ARTSHOW  \xC2\xB7  FLOOR GUIDE");   /* over a rule */
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_style_text_letter_space(head, 2, 0);
    lv_obj_set_style_pad_left(head, 10, 0);
    lv_obj_set_style_pad_bottom(head, 8, 0);
    lv_obj_set_style_margin_bottom(head, 4, 0);
    lv_obj_set_style_border_side(head, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(head, 1, 0);
    lv_obj_set_style_border_color(head, lv_color_hex(RIM), 0);
    for (int k = 0; k < s_n; k++) s_items[k] = room(s_list, k);
    lv_obj_set_state(s_items[0], LV_STATE_CHECKED, true);
    lv_obj_set_hidden(s_list, true);
}

void menu_select(int k) {
    if (k >= 0 && k < s_n) show(k);
    lv_obj_set_hidden(s_list, true);
}
