/* gallery.c - the page and its worker. The page asks (a request: filter,
 * artist, page), the worker queries the index and decodes the thumbnails,
 * then fills the cells under the LVGL lock, one by one. A newer request
 * stops the work on an older one. 15 cells and 21 artist buttons, made once:
 * paging, not an object per picture (2000 pictures would not fit). */
#include "gallery.h"
#include "index.h"
#include "thumb.h"
#include "menu.h"
#include "slideshow.h"
#include "fonts/art_fonts.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define BG        0x0D1B2A
#define CARD_BG   0x1B263B
#define RIM       0x415A77
#define MINOR     0x778DA9
#define TEXT      0xE0F0FF
#define GOLD      0xFFC93C
#define COLS      5
#define ROWS      3
#define PER_PAGE  (COLS * ROWS)
#define ART_COLS  3
#define ART_ROWS  7
#define ART_PAGE  (ART_COLS * ART_ROWS)
#define CELL_W    184
#define GRID_Y    74

static const char *TAG = "gallery";

typedef enum { MODE_ALL, MODE_FAVS, MODE_ARTISTS, MODE_BAD } gallery_mode_t;

typedef struct {
    uint32_t seq;          /* this request's number: a newer one makes it stale */
    bool artists;          /* the artists' list, else pictures */
    index_filter_t filter;
    char artist[96];
    int page;
} req_t;

/* the page's state: the LVGL task's */
static lv_obj_t *s_page, *s_sub, *s_modes, *s_prev, *s_next, *s_pages, *s_grid, *s_artist_grid;
static struct {
    lv_obj_t *cell, *frame, *img, *note, *star, *title, *artist;
    char path[256];
    bool fav, bad;
    lv_image_dsc_t dsc;
} s_cell[PER_PAGE];
static struct {
    lv_obj_t *btn, *label;
    char name[96];
} s_art[ART_PAGE];
static gallery_mode_t s_mode;
static bool s_picked;              /* MODE_ARTISTS: an artist picked (else their list) */
static char s_artist[96];          /* ... that one ("" for the pictures with none) */
static int s_pageno, s_npages;
static uint32_t s_thumbs_seen;
static lv_timer_t *s_refresh;

/* shared with the worker */
static QueueHandle_t s_q;
static volatile uint32_t s_seq;
static uint16_t *s_px[PER_PAGE];   /* the decoded thumbnails, PSRAM */
static index_item_t *s_items;      /* the worker's query */

/* ---- requests ---- */

static void request(void) {
    req_t r = { .seq = ++s_seq, .page = s_pageno };
    r.artists = s_mode == MODE_ARTISTS && !s_picked;
    r.filter = s_mode == MODE_FAVS ? INDEX_FAVOURITES : s_mode == MODE_BAD ? INDEX_BAD
             : s_mode == MODE_ARTISTS ? INDEX_ARTIST : INDEX_ALL;
    snprintf(r.artist, sizeof r.artist, "%s", s_artist);
    if (s_q) xQueueOverwrite(s_q, &r);
}

/* ---- the worker's results, applied in the LVGL task's lock ---- */

static void set_pager(int total, int per) {
    s_npages = total ? (total + per - 1) / per : 1;
    if (s_pageno >= s_npages) s_pageno = s_npages - 1;
    lv_label_set_text_fmt(s_pages, "%d / %d", s_pageno + 1, s_npages);
    bool more = s_npages > 1;
    lv_obj_set_hidden(s_prev, !more);
    lv_obj_set_hidden(s_next, !more);
    lv_obj_set_hidden(s_pages, !more);
}

static void apply_items(const req_t *r, const index_item_t *it, int n, int total) {
    lv_obj_set_hidden(s_grid, false);
    lv_obj_set_hidden(s_artist_grid, true);
    set_pager(total, PER_PAGE);
    int missing = 0;
    for (int k = 0; k < PER_PAGE; k++) {
        lv_obj_set_hidden(s_cell[k].cell, k >= n);
        if (k >= n) continue;
        snprintf(s_cell[k].path, sizeof s_cell[k].path, "%s", it[k].path);
        s_cell[k].fav = it[k].fav;
        s_cell[k].bad = it[k].bad;
        lv_label_set_text(s_cell[k].title, it[k].title);
        lv_label_set_text(s_cell[k].artist, it[k].artist[0] ? it[k].artist : "\xE2\x80\x94");
        lv_obj_set_hidden(s_cell[k].img, true);   /* until the worker has its thumbnail */
        lv_obj_set_hidden(s_cell[k].star, it[k].bad);
        lv_obj_invalidate(s_cell[k].star);
        lv_label_set_text(s_cell[k].note, it[k].bad ? it[k].note : it[k].thumb ? "" : "Not shown yet");
        if (!it[k].bad && !it[k].thumb) missing++;
    }
    const char *more = missing ? "   \xC2\xB7   thumbnails come as the pictures are shown (and while this page is open)" : "";
    switch (r->filter) {
    case INDEX_FAVOURITES:
        if (total) lv_label_set_text_fmt(s_sub, "%d favourite%s%s", total, total == 1 ? "" : "s", more);
        else lv_label_set_text(s_sub, "No favourites yet: tap a picture's star");
        break;
    case INDEX_ARTIST:
        lv_label_set_text_fmt(s_sub, "%s   \xC2\xB7   %d picture%s   \xC2\xB7   Artists again for the list%s",
                              r->artist[0] ? r->artist : "No artist", total, total == 1 ? "" : "s", more);
        break;
    case INDEX_BAD:
        if (total) lv_label_set_text_fmt(s_sub, "%d can't be shown: fix them on a computer, then Rescan (Settings)", total);
        else lv_label_set_text(s_sub, "None: every picture on the card can be shown");
        break;
    default:
        lv_label_set_text_fmt(s_sub, "%d pictures, by artist%s", total, more);
    }
}

static void apply_artists(char (*names)[96], const int *counts, int n, int total) {
    lv_obj_set_hidden(s_grid, true);
    lv_obj_set_hidden(s_artist_grid, false);
    set_pager(total, ART_PAGE);
    for (int k = 0; k < ART_PAGE; k++) {
        lv_obj_set_hidden(s_art[k].btn, k >= n);
        if (k >= n) continue;
        snprintf(s_art[k].name, sizeof s_art[k].name, "%s", names[k]);
        lv_label_set_text_fmt(s_art[k].label, "%s  (%d)", names[k][0] ? names[k] : "No artist", counts[k]);
    }
    lv_label_set_text_fmt(s_sub, "%d artists: pick one", total);
}

/* ---- the worker ---- */

static void worker(void *arg) {
    static char names[ART_PAGE][96];
    static int counts[ART_PAGE];
    req_t r;
    for (;;) {
        xQueueReceive(s_q, &r, portMAX_DELAY);
        int total = 0;
        if (r.artists) {
            int n = index_artists(names, counts, r.page * ART_PAGE, ART_PAGE, &total);
            lvgl_port_lock(0);
            if (r.seq == s_seq) apply_artists(names, counts, n, total);
            lvgl_port_unlock();
            continue;
        }
        int n = index_page(r.filter, r.artist, r.page * PER_PAGE, PER_PAGE, s_items, &total);
        lvgl_port_lock(0);
        if (r.seq == s_seq) apply_items(&r, s_items, n, total);
        lvgl_port_unlock();
        for (int k = 0; k < n && r.seq == s_seq; k++) {
            uint8_t *jpg;
            size_t len;
            int w, h;
            if (!s_items[k].thumb || !s_px[k] || !index_thumb_get(s_items[k].path, &jpg, &len)) continue;
            bool ok = thumb_decode(jpg, len, s_px[k], &w, &h);   /* the cell's image is hidden meanwhile */
            free(jpg);
            if (!ok) continue;
            lvgl_port_lock(0);
            if (r.seq == s_seq) {
                s_cell[k].dsc = (lv_image_dsc_t){
                    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = w, .h = h, .stride = w * 2 },
                    .data_size = w * h * 2,
                    .data = (const uint8_t *)s_px[k],
                };
                lv_image_set_src(s_cell[k].img, NULL);
                lv_image_set_src(s_cell[k].img, &s_cell[k].dsc);
                lv_obj_center(s_cell[k].img);
                lv_obj_set_hidden(s_cell[k].img, false);
                lv_label_set_text(s_cell[k].note, "");
            }
            lvgl_port_unlock();
        }
    }
}

/* ---- the star: drawn (the fonts have none) ---- */

static void star_draw(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    float cx = (a.x1 + a.x2) / 2.0f, cy = (a.y1 + a.y2) / 2.0f + 1, R = lv_area_get_width(&a) / 2.0f - 3, r = R * 0.45f;
    lv_draw_rect_dsc_t bg;   /* a dark disc: the star shows on any picture */
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_black();
    bg.bg_opa = LV_OPA_50;
    bg.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(layer, &bg, &a);
    lv_point_precise_t p[11];
    for (int i = 0; i <= 10; i++) {
        float ang = (-90 + i * 36) * (float)M_PI / 180, rad = i % 2 ? r : R;
        p[i].x = cx + cosf(ang) * rad;
        p[i].y = cy + sinf(ang) * rad;
    }
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = lv_color_hex(s_cell[k].fav ? GOLD : MINOR);
    t.opa = s_cell[k].fav ? LV_OPA_COVER : LV_OPA_70;
    for (int i = 0; i < 10; i++) {   /* a fan from the middle */
        t.p[0].x = cx;
        t.p[0].y = cy;
        t.p[1] = p[i];
        t.p[2] = p[i + 1];
        lv_draw_triangle(layer, &t);
    }
}

static void star_cb(lv_event_t *e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    s_cell[k].fav = !s_cell[k].fav;
    index_set_fav(s_cell[k].path, s_cell[k].fav);
    lv_obj_invalidate(s_cell[k].star);
    if (s_mode == MODE_FAVS) request();   /* unstarred: off the list */
}

/* ---- the rest of the page's events ---- */

static void cell_cb(lv_event_t *e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_cell[k].bad) return;
    slideshow_jump(s_cell[k].path);
    menu_select(0);   /* the slideshow: the menu's first page */
}

static void artist_cb(lv_event_t *e) {
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    snprintf(s_artist, sizeof s_artist, "%s", s_art[k].name);
    s_picked = true;
    s_pageno = 0;
    request();
}

static void mode_cb(lv_event_t *e) {
    s_mode = lv_buttonmatrix_get_selected_button(lv_event_get_current_target_obj(e));
    s_picked = false;   /* Artists again: their list */
    s_pageno = 0;
    request();
}

static void page_cb(lv_event_t *e) {
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int p = s_pageno + d;
    if (p < 0 || p >= s_npages) return;
    s_pageno = p;
    request();
}

/* thumbnails made meanwhile (the slideshow's, in the background): redraw */
static void refresh_cb(lv_timer_t *t) {
    uint32_t v = index_thumbs_version();
    if (v == s_thumbs_seen) return;
    s_thumbs_seen = v;
    if (!(s_mode == MODE_ARTISTS && !s_picked)) request();
}

void gallery_shown(bool shown) {
    if (shown) {
        s_thumbs_seen = index_thumbs_version();
        request();
        lv_timer_resume(s_refresh);
    } else {
        lv_timer_pause(s_refresh);
    }
}

/* ---- the page ---- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *plain(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *sym, int d) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(RIM), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_size(b, 48, 40);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
    return b;
}

static void make_cell(int k) {
    lv_obj_t *c = s_cell[k].cell = plain(s_grid);
    lv_obj_set_size(c, CELL_W, 150);
    lv_obj_t *f = s_cell[k].frame = lv_obj_create(c);   /* the thumbnail's box */
    lv_obj_remove_style_all(f);
    lv_obj_set_size(f, THUMB_W + 8, THUMB_H);
    lv_obj_set_style_bg_color(f, lv_color_hex(CARD_BG), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(f, 6, 0);
    lv_obj_set_style_clip_corner(f, true, 0);
    lv_obj_set_scrollable(f, false);
    lv_obj_add_event_cb(f, cell_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    s_cell[k].img = lv_image_create(f);
    lv_obj_set_clickable(s_cell[k].img, false);
    lv_obj_set_hidden(s_cell[k].img, true);
    s_cell[k].note = label(f, &art_font_16, MINOR);
    lv_label_set_long_mode(s_cell[k].note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_cell[k].note, THUMB_W - 16);
    lv_obj_set_style_text_align(s_cell[k].note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_cell[k].note);
    lv_obj_t *s = s_cell[k].star = plain(f);
    lv_obj_set_size(s, 32, 32);
    lv_obj_align(s, LV_ALIGN_TOP_RIGHT, -3, 3);
    lv_obj_set_clickable(s, true);
    lv_obj_set_ext_click_area(s, 10);
    lv_obj_add_event_cb(s, star_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)k);
    lv_obj_add_event_cb(s, star_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    s_cell[k].title = label(c, &art_font_16, TEXT);
    lv_label_set_long_mode(s_cell[k].title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_cell[k].title, CELL_W - 4);
    lv_obj_set_pos(s_cell[k].title, 2, THUMB_H + 4);
    s_cell[k].artist = label(c, &art_font_16, MINOR);
    lv_label_set_long_mode(s_cell[k].artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_cell[k].artist, CELL_W - 4);
    lv_obj_set_pos(s_cell[k].artist, 2, THUMB_H + 24);
    s_px[k] = heap_caps_aligned_calloc(16, THUMB_W * THUMB_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
}

lv_obj_t *gallery_create(lv_obj_t *parent) {
    lv_obj_t *page = s_page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollable(page, false);

    lv_obj_t *head = label(page, &art_font_24, TEXT);   /* right of the menu button */
    lv_label_set_text(head, "Gallery");
    lv_obj_set_pos(head, 90, 12);
    s_sub = label(page, &art_font_16, MINOR);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_sub, 900);
    lv_obj_set_pos(s_sub, 91, 44);

    s_modes = lv_buttonmatrix_create(page);
    static const char *modes[] = { "All", "Favourites", "Artists", "Can't show", "" };
    lv_buttonmatrix_set_map(s_modes, modes);
    lv_buttonmatrix_set_button_ctrl_all(s_modes, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(s_modes, true);
    lv_buttonmatrix_set_button_ctrl(s_modes, 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(s_modes, 470, 44);
    lv_obj_set_pos(s_modes, 250, 2);
    lv_obj_set_style_text_font(s_modes, &art_font_16, 0);
    lv_obj_set_style_bg_opa(s_modes, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_modes, 0, 0);
    lv_obj_set_style_pad_all(s_modes, 0, 0);
    lv_obj_add_event_cb(s_modes, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* the pager, top right */
    s_prev = small_button(page, LV_SYMBOL_LEFT, -1);
    lv_obj_set_pos(s_prev, 760, 4);
    s_pages = label(page, &art_font_20, TEXT);
    lv_obj_set_width(s_pages, 90);
    lv_obj_set_style_text_align(s_pages, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_pages, 812, 13);
    s_next = small_button(page, LV_SYMBOL_RIGHT, 1);
    lv_obj_set_pos(s_next, 906, 4);

    /* the pictures */
    s_grid = plain(page);
    lv_obj_set_pos(s_grid, 24, GRID_Y);
    lv_obj_set_size(s_grid, 976, 3 * 150 + 2 * 6);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_grid, (976 - COLS * CELL_W) / (COLS - 1), 0);
    lv_obj_set_style_pad_row(s_grid, 6, 0);
    for (int k = 0; k < PER_PAGE; k++) make_cell(k);

    /* the artists */
    s_artist_grid = plain(page);
    lv_obj_set_pos(s_artist_grid, 24, GRID_Y);
    lv_obj_set_size(s_artist_grid, 976, ART_ROWS * 56 + (ART_ROWS - 1) * 8);
    lv_obj_set_flex_flow(s_artist_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_artist_grid, 14, 0);
    lv_obj_set_style_pad_row(s_artist_grid, 8, 0);
    for (int k = 0; k < ART_PAGE; k++) {
        lv_obj_t *b = s_art[k].btn = lv_button_create(s_artist_grid);
        lv_obj_set_size(b, (976 - 2 * 14) / ART_COLS, 56);
        lv_obj_set_style_bg_color(b, lv_color_hex(CARD_BG), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        s_art[k].label = label(b, &art_font_20, TEXT);
        lv_label_set_long_mode(s_art[k].label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(s_art[k].label, LV_PCT(100));
        lv_obj_center(s_art[k].label);
        lv_obj_add_event_cb(b, artist_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    }
    lv_obj_set_hidden(s_artist_grid, true);

    s_items = heap_caps_calloc(PER_PAGE, sizeof *s_items, MALLOC_CAP_SPIRAM);
    s_q = xQueueCreate(1, sizeof(req_t));
    if (!s_items || !s_q || xTaskCreate(worker, "gallery", 8 * 1024, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the gallery");
        s_q = NULL;
    }
    s_refresh = lv_timer_create(refresh_cb, 2000, NULL);
    lv_timer_pause(s_refresh);
    lv_obj_set_hidden(page, true);
    return page;
}
