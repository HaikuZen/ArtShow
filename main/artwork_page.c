/* artwork_page.c - an art book open on a walnut table: the painting on the
 * left page, a plate (fitted into ART_W x ART_H) on a deep tint of its own
 * colours; its catalogue entry on the right page, cream paper (scrolls when
 * Wikipedia's summary is long); folios from the day of the year. The book
 * keeps clear of the corner buttons (styled for dark grounds): they sit on
 * the table. Two buttons top right: another painting, and save this one. */
#include "artwork_page.h"
#include "fonts/art_fonts.h"
#include "esp_lvgl_port.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TABLE  0x1C1714
#define CREAM  0xEDE3D0   /* on the table */
#define DIM    0x9C9184
#define WARN   0xFF6B35
#define PAPER  0xF3EDE0
#define EDGE   0xD8CFBC   /* the pages under the top one */
#define INK    0x2A2420
#define GREY   0x6E665C
#define SEAL   0xB0362A
#define BOOK_X 20
#define BOOK_Y 70
#define BOOK_H 464         /* to y 534: the bottom corner buttons are below */
#define SPINE  596
#define BOOK_R 1004
#define PLATE  0x2A2420    /* the left page until a painting gives its tint */

static lv_obj_t *s_page, *s_sub, *s_note, *s_img, *s_info, *s_folio_l, *s_folio_r, *s_plate_no;
static lv_color_t s_tint;
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

/* the book: its shadow, the pages under (a few edges), the left page in
 * the painting's tint, the right page's paper, the spine's shade, and a
 * keyline round the plate */
static void book_draw(lv_event_t *e) {
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    lv_area_t a = { BOOK_X + 4, BOOK_Y + 8, BOOK_R + 4, BOOK_Y + BOOK_H + 8 };
    d.bg_color = lv_color_black();
    d.bg_opa = LV_OPA_50;
    d.radius = 4;
    lv_draw_rect(layer, &d, &a);
    d.radius = 0;
    d.bg_opa = LV_OPA_COVER;
    for (int i = 3; i >= 1; i--) {   /* the page block, stepping out at the outer edges and the foot */
        d.bg_color = lv_color_mix(lv_color_hex(EDGE), lv_color_hex(0x8C8270), 255 - i * 50);
        a = (lv_area_t){ BOOK_X - i, BOOK_Y + i, BOOK_R + i, BOOK_Y + BOOK_H - 1 + i };
        lv_draw_rect(layer, &d, &a);
    }
    d.bg_color = s_tint;
    a = (lv_area_t){ BOOK_X, BOOK_Y, SPINE - 1, BOOK_Y + BOOK_H - 1 };
    lv_draw_rect(layer, &d, &a);
    d.bg_color = lv_color_hex(PAPER);
    a = (lv_area_t){ SPINE, BOOK_Y, BOOK_R, BOOK_Y + BOOK_H - 1 };
    lv_draw_rect(layer, &d, &a);
    d.bg_color = lv_color_black();   /* the gutter: darker into the spine, both sides */
    for (int i = 0; i < 24; i++) {
        d.bg_opa = (lv_opa_t)((24 - i) * (24 - i) * 90 / (24 * 24));
        a = (lv_area_t){ SPINE + i, BOOK_Y, SPINE + i, BOOK_Y + BOOK_H - 1 };
        lv_draw_rect(layer, &d, &a);
        a.x1 = a.x2 = SPINE - 1 - i;
        lv_draw_rect(layer, &d, &a);
    }
    if (s_have && !lv_obj_is_hidden(s_img)) {
        lv_area_t c;
        lv_obj_get_coords(s_img, &c);
        lv_draw_border_dsc_t b;
        lv_draw_border_dsc_init(&b);
        b.color = lv_color_white();
        b.opa = LV_OPA_30;
        b.width = 1;
        a = (lv_area_t){ c.x1 - 5, c.y1 - 5, c.x2 + 5, c.y2 + 5 };
        lv_draw_border(layer, &b, &a);
    }
}

/* the left page: the painting's average, deep */
static lv_color_t tint_of(const artwork_t *a) {
    uint32_t r = 0, g = 0, b = 0, n = 0;
    for (int y = 0; y < a->h; y += 4) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)a->pixels + y * a->stride);
        for (int x = 0; x < a->w; x += 4, n++) {
            r += row[x] >> 11;
            g += (row[x] >> 5) & 63;
            b += row[x] & 31;
        }
    }
    if (!n) return lv_color_hex(PLATE);
    lv_color_t c = lv_color_make(r * 255 / (31 * n), g * 255 / (63 * n), b * 255 / (31 * n));
    return lv_color_mix(c, lv_color_hex(0x100C0A), 70);   /* ~27 % of it over near-black */
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
    lv_obj_set_style_text_color(s_sub, lv_color_hex(warn ? WARN : DIM), 0);
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
    lv_obj_t *page = s_page = lv_obj_create(parent);
    s_tint = lv_color_hex(PLATE);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(TABLE), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollable(page, false);
    lv_obj_add_event_cb(page, book_draw, LV_EVENT_DRAW_MAIN, NULL);

    /* top: right of the menu button */
    lv_obj_t *head = label(page, &art_font_24, CREAM);
    lv_label_set_text(head, "Artwork of the Day");
    lv_obj_set_pos(head, 90, 12);
    s_sub = label(page, &art_font_16, DIM);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_sub, 760, 22);   /* DOTS needs a height */
    lv_obj_set_pos(s_sub, 91, 42);
    lv_obj_t *save = button(page, LV_SYMBOL_SAVE, save_cb);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_t *another = button(page, LV_SYMBOL_SHUFFLE, another_cb);
    lv_obj_align_to(another, save, LV_ALIGN_OUT_LEFT_MID, -12, 0);

    /* the painting, centred on the left page */
    lv_obj_t *frame = lv_obj_create(page);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, ART_W, ART_H);
    lv_obj_set_pos(frame, BOOK_X + (SPINE - BOOK_X - ART_W) / 2, BOOK_Y + (BOOK_H - ART_H) / 2 - 6);
    lv_obj_set_clickable(frame, false);
    s_img = lv_image_create(frame);
    lv_obj_center(s_img);
    s_folio_l = label(page, &art_font_16, DIM);
    lv_obj_set_pos(s_folio_l, BOOK_X + 14, BOOK_Y + BOOK_H - 26);

    /* its entry, on the right page */
    const int px = SPINE + 32, pw = BOOK_R - 24 - px;
    s_plate_no = label(page, &art_font_16, SEAL);
    lv_obj_set_pos(s_plate_no, px, BOOK_Y + 18);
    lv_obj_t *rule = lv_obj_create(page);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, pw, 1);
    lv_obj_set_pos(rule, px, BOOK_Y + 44);
    lv_obj_set_style_bg_color(rule, lv_color_hex(SEAL), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    s_info = lv_obj_create(page);
    lv_obj_remove_style_all(s_info);
    lv_obj_set_size(s_info, pw + 8, BOOK_H - 56 - 34);
    lv_obj_set_pos(s_info, px, BOOK_Y + 56);
    lv_obj_set_flex_flow(s_info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_info, 8, 0);
    lv_obj_set_style_pad_right(s_info, 8, 0);
    lv_obj_set_scroll_dir(s_info, LV_DIR_VER);
    lv_obj_set_style_bg_color(s_info, lv_color_hex(GREY), LV_PART_SCROLLBAR);
    s_title = wrapped(s_info, &art_font_32, INK);
    s_artist = wrapped(s_info, &art_font_24, SEAL);
    s_bio = wrapped(s_info, &art_font_16, GREY);
    s_when = wrapped(s_info, &art_font_20, INK);
    s_dims = wrapped(s_info, &art_font_16, GREY);
    s_about_of = wrapped(s_info, &art_font_16, GREY);   /* Wikipedia's text asks for credit */
    lv_obj_set_style_pad_top(s_about_of, 8, 0);
    s_about = wrapped(s_info, &art_font_20, INK);
    lv_obj_set_style_text_line_space(s_about, 4, 0);
    s_museum = wrapped(s_info, &art_font_16, GREY);
    lv_obj_set_style_pad_top(s_museum, 8, 0);
    lv_obj_set_hidden(s_info, true);
    s_folio_r = label(page, &art_font_16, GREY);
    lv_obj_set_width(s_folio_r, 80);
    lv_obj_set_style_text_align(s_folio_r, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_folio_r, BOOK_R - 14 - 80, BOOK_Y + BOOK_H - 26);

    /* no painting yet: why, on the right page */
    s_note = label(page, &art_font_24, INK);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_note, pw);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_note, px, BOOK_Y + 180);

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
    s_tint = tint_of(a);
    lv_obj_invalidate(s_page);

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

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (a->random) {
        snprintf(s_line, sizeof s_line, "Another from the Met's highlights (the day's comes back tomorrow)");
        lv_label_set_text(s_plate_no, "PLATE  \xC2\xB7  ANOTHER");
    } else {
        strftime(s_line, sizeof s_line, "%A %e %B %Y", &t);
        lv_label_set_text_fmt(s_plate_no, "PLATE %d  \xC2\xB7  THE MET", t.tm_yday + 1);
    }
    lv_label_set_text_fmt(s_folio_l, "%d", 2 * t.tm_yday + 2);   /* a plate a day, two pages a plate */
    lv_label_set_text_fmt(s_folio_r, "%d", 2 * t.tm_yday + 3);
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
    lv_obj_set_hidden(s_plate_no, !s_have);
    lv_obj_set_hidden(s_folio_l, !s_have);
    lv_obj_set_hidden(s_folio_r, !s_have);
    if (!s_have) lv_label_set_text(s_note, st ? st : "");
    /* a status over a painting: orange if it went wrong, not for news */
    bool warn = st && strncmp(st, "Sav", 3) && strncmp(st, "Already", 7) && strncmp(st, "Looking", 7);
    status(s_have ? st : NULL, warn);
    lvgl_port_unlock();
}
