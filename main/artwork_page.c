/* artwork_page.c - the painting on the left (fitted into ART_W x ART_H), its
 * card on the right (scrolls when Wikipedia's summary is long), and two
 * buttons top right: another painting, and save this one to the card. */
#include "artwork_page.h"
#include "fonts/art_fonts.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BG     0x0D1B2A
#define MINOR  0x778DA9
#define TEXT   0xE0F0FF
#define WARN   0xFF6B35

static lv_obj_t *s_sub, *s_note, *s_img, *s_info;
static lv_obj_t *s_title, *s_artist, *s_bio, *s_when, *s_dims, *s_about_of, *s_about, *s_museum;
static lv_image_dsc_t s_dsc[2];   /* one per pixel buffer: a new source each time */
static int s_d;
static char s_line[96];           /* under the heading: the date, or "another" */
static bool s_have;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *wrapped(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = label(parent, font, color);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

/* like the corner buttons */
static lv_obj_t *button(lv_obj_t *parent, const char *sym, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x808080), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void status(const char *txt, bool warn) {
    if (txt) lv_label_set_text_fmt(s_sub, "%s%s%s", s_line, s_line[0] ? "   \xC2\xB7   " : "", txt);
    else lv_label_set_text(s_sub, s_line);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(warn ? WARN : MINOR), 0);
}

static void another_cb(lv_event_t *e) {
    status("Looking for another painting...", false);
    artwork_another();
}

static void save_cb(lv_event_t *e) {
    status("Saving...", false);
    artwork_save();
}

lv_obj_t *artwork_page_create(lv_obj_t *parent) {
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollable(page, false);

    /* top: right of the menu button */
    lv_obj_t *head = label(page, &art_font_24, TEXT);
    lv_label_set_text(head, "Artwork of the Day");
    lv_obj_set_pos(head, 90, 12);
    s_sub = label(page, &art_font_16, MINOR);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_sub, 760);
    lv_obj_set_pos(s_sub, 91, 44);
    lv_obj_t *save = button(page, LV_SYMBOL_SAVE, save_cb);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_t *another = button(page, LV_SYMBOL_SHUFFLE, another_cb);
    lv_obj_align_to(another, save, LV_ALIGN_OUT_LEFT_MID, -12, 0);

    /* the painting, centered in its box */
    lv_obj_t *frame = lv_obj_create(page);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, ART_W, ART_H);
    lv_obj_set_pos(frame, 24, 76);
    lv_obj_set_clickable(frame, false);
    s_img = lv_image_create(frame);
    lv_obj_center(s_img);

    /* its card */
    s_info = lv_obj_create(page);
    lv_obj_remove_style_all(s_info);
    lv_obj_set_size(s_info, 1024 - 24 - ART_W - 24 - 20, ART_H);
    lv_obj_set_pos(s_info, 24 + ART_W + 24, 76);
    lv_obj_set_flex_flow(s_info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_info, 8, 0);
    lv_obj_set_style_pad_right(s_info, 8, 0);
    lv_obj_set_scroll_dir(s_info, LV_DIR_VER);
    s_title = wrapped(s_info, &art_font_32, TEXT);
    s_artist = wrapped(s_info, &art_font_24, TEXT);
    s_bio = wrapped(s_info, &art_font_16, MINOR);
    s_when = wrapped(s_info, &art_font_20, TEXT);
    s_dims = wrapped(s_info, &art_font_16, MINOR);
    s_about_of = wrapped(s_info, &art_font_16, MINOR);   /* Wikipedia's text asks for credit */
    lv_obj_set_style_pad_top(s_about_of, 8, 0);
    s_about = wrapped(s_info, &art_font_20, TEXT);
    lv_obj_set_style_text_line_space(s_about, 4, 0);
    s_museum = wrapped(s_info, &art_font_16, MINOR);
    lv_obj_set_style_pad_top(s_museum, 8, 0);
    lv_obj_set_hidden(s_info, true);

    /* no painting yet: why */
    s_note = label(page, &art_font_24, TEXT);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_note, 760);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_note, LV_ALIGN_CENTER, 0, 0);

    lv_obj_set_hidden(page, true);
    return page;
}

static void set(lv_obj_t *l, const char *txt) {
    lv_label_set_text(l, txt);
    lv_obj_set_hidden(l, !txt[0]);
}

static void show(const artwork_t *a) {
    lv_image_dsc_t *d = &s_dsc[s_d ^= 1];
    *d = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                    .w = a->w, .h = a->h, .stride = a->stride },
        .data_size = a->stride * a->h,
        .data = (const uint8_t *)a->pixels,
    };
    lv_image_set_src(s_img, d);
    lv_obj_center(s_img);

    set(s_title, a->title);
    set(s_artist, a->artist);
    set(s_bio, a->bio);
    char buf[200];
    snprintf(buf, sizeof buf, "%s%s%s", a->date, a->date[0] && a->medium[0] ? "  \xC2\xB7  " : "", a->medium);
    set(s_when, buf);
    set(s_dims, a->dims);
    snprintf(buf, sizeof buf, "From Wikipedia, about %s:", a->about_of ? a->about_of : "");
    set(s_about_of, a->about[0] ? buf : "");
    set(s_about, a->about);
    set(s_museum, a->museum);
    lv_obj_scroll_to_y(s_info, 0, LV_ANIM_OFF);

    if (a->random) {
        snprintf(s_line, sizeof s_line, "Another from the Met's highlights (the day's comes back tomorrow)");
    } else {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        strftime(s_line, sizeof s_line, "%A %e %B %Y", &t);
    }
}

void artwork_page_update(const artwork_t *a, const char *st) {
    lvgl_port_lock(0);
    if (a) {
        show(a);
        s_have = true;
    }
    lv_obj_set_hidden(s_info, !s_have);
    lv_obj_set_hidden(s_img, !s_have);
    lv_obj_set_hidden(s_note, s_have);
    if (!s_have) lv_label_set_text(s_note, st ? st : "");
    /* a status over a painting: orange if it went wrong, not for news */
    bool warn = st && strncmp(st, "Sav", 3) && strncmp(st, "Already", 7) && strncmp(st, "Looking", 7);
    status(s_have ? st : NULL, warn);
    lvgl_port_unlock();
}
