/* album_wall.c - the page and its worker. The page asks (a request: a
 * Spotlight album, a Wall page, a sync), the worker decodes covers from the
 * card (esp_new_jpeg, box-fitted) and fetches an album's details, then fills
 * the page under the LVGL lock. A sync (the list, then the covers missing)
 * gives way to any request between two covers and goes on afterwards.
 * A record room: the Spotlight's sleeve with its record half out (drawn: a
 * label in the cover's colour), the text in that colour as an accent; the
 * Wall's covers standing on walnut shelves. */
#include "album_wall.h"
#include "albums.h"
#include "wifi.h"
#include "fonts/art_fonts.h"
#include "esp_jpeg_dec.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BG        0x1E1916     /* the room's wall */
#define BUTTON    0x4A3A30
#define MINOR     0xB3A595
#define TEXT      0xF2E9DA
#define BLANK     0x2E2620     /* a sleeve without a cover */
#define WOOD      0x5A3B24
#define WOOD_LIT  0x8A6040
#define WOOD_DARK 0x2E1D12
#define BIG       440          /* the Spotlight's cover */
#define COVER_X   32
#define COVER_Y   4            /* in the Spotlight (itself at y 76) */
#define DISC_R    (BIG / 2 - 12)
#define DISC_X    (COVER_X + BIG - 70)   /* the record's middle: half out of the sleeve */
#define CARD_X    (DISC_X + DISC_R + 30)
#define SMALL     150          /* the Wall's */
#define COLS      6
#define ROWS      3
#define PER_PAGE  (COLS * ROWS)
#define SPOT_MS   30000        /* the next album */

static const char *TAG = "albumwall";

typedef enum { REQ_SPOT, REQ_WALL, REQ_SYNC, REQ_CLOSE, REQ_WAKE } req_kind_t;
typedef struct {
    req_kind_t kind;
    int k;                     /* the album (SPOT) or page (WALL) */
} req_t;

static lv_obj_t *s_page, *s_sub, *s_spot, *s_wall, *s_cover, *s_blank, *s_blank_title, *s_title, *s_artist, *s_meta, *s_tracks;
static lv_obj_t *s_prev, *s_next, *s_pages, *s_cells[PER_PAGE], *s_tabs, *s_modes;
static lv_color_t s_avg;   /* the Spotlight's cover's average: the record's label, the accents */
static lv_image_dsc_t s_cover_dsc, s_cell_dsc[PER_PAGE];
static uint16_t *s_cover_px, *s_cell_px[PER_PAGE];
static long long s_cell_id[PER_PAGE];
static int s_count, s_spot_k = -1, s_pageno, s_npages;
static bool s_wall_mode, s_shown;
static lv_timer_t *s_tick;
static QueueHandle_t s_q;
static volatile bool s_sync_wanted = true;   /* the list (once a boot, at the first showing; Sync), then the covers */
static bool s_list_due = true;               /* the list too, not only covers left from before */
static bool s_retry_misses;                  /* ... and Wikipedia's misses forgotten (the Sync button) */
static volatile bool s_downloading;          /* covers coming: no automatic Spotlight meanwhile */

/* the Spotlight's tabs: the tracks (Deezer), what Wikipedia says (its intro,
   personnel, reception). Their texts in PSRAM, written under the LVGL lock */
enum { TAB_TRACKS, TAB_ABOUT, TAB_CREDITS, TAB_RECEPTION, TABS };
static const size_t TAB_SIZE[TABS] = { 3000, 1600, 1600, 2600 };
static char *s_text[TABS];
static int s_tab;
static int s_wiki_state;                     /* 0 being looked up, 1 there (or found nothing), 2 can't now */
static int64_t s_hold_until;                 /* ms: a tab touched, the automatic Spotlight waits (reading) */
#define HOLD_MS (3 * 60 * 1000)

static void ask(req_kind_t kind, int k) {
    req_t r = { kind, k };
    if (s_q) xQueueSend(s_q, &r, 0);
}

static void status(const char *txt) {
    lvgl_port_lock(0);
    lv_label_set_text(s_sub, txt);
    lvgl_port_unlock();
}

/* ---- covers: the file, decoded, fitted into a square ---- */

static bool decode_fit(const char *path, uint16_t *dst, int side, uint32_t *avg) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *jpg = len > 0 ? heap_caps_malloc(len, MALLOC_CAP_SPIRAM) : NULL;
    bool ok = jpg && fread(jpg, 1, len, f) == (size_t)len;
    fclose(f);
    uint16_t *full = NULL;
    int w = 0, h = 0;
    if (ok) {
        jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
        cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        jpeg_dec_handle_t dec;
        jpeg_dec_io_t io = { .inbuf = jpg, .inbuf_len = len };
        jpeg_dec_header_info_t hi;
        int olen = 0;
        ok = jpeg_dec_open(&cfg, &dec) == JPEG_ERR_OK;
        if (ok) {
            ok = jpeg_dec_parse_header(dec, &io, &hi) == JPEG_ERR_OK && jpeg_dec_get_outbuf_len(dec, &olen) == JPEG_ERR_OK &&
                 (full = heap_caps_aligned_calloc(16, 1, olen, MALLOC_CAP_SPIRAM));
            if (ok) {
                io.outbuf = (uint8_t *)full;
                ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
                w = hi.width;
                h = hi.height;
            }
            jpeg_dec_close(dec);
        }
    }
    free(jpg);
    if (ok) {   /* box filter into side x side (covers are square) */
        uint64_t sr = 0, sg = 0, sb = 0;
        for (int y = 0; y < side; y++) {
            int y0 = y * h / side, y1 = (y + 1) * h / side;
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < side; x++) {
                int x0 = x * w / side, x1 = (x + 1) * w / side;
                if (x1 <= x0) x1 = x0 + 1;
                uint32_t r = 0, g = 0, b = 0, n = (x1 - x0) * (y1 - y0);
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) {
                        uint16_t p = full[yy * w + xx];
                        r += p >> 11;
                        g += (p >> 5) & 63;
                        b += p & 31;
                    }
                dst[y * side + x] = (uint16_t)((r / n) << 11 | (g / n) << 5 | b / n);
                sr += r / n;
                sg += g / n;
                sb += b / n;
            }
        }
        if (avg) {   /* the cover's average colour, 0xRRGGBB */
            uint64_t n = (uint64_t)side * side;
            *avg = (uint32_t)(sr * 255 / 31 / n) << 16 | (uint32_t)(sg * 255 / 63 / n) << 8 | (uint32_t)(sb * 255 / 31 / n);
        }
    }
    free(full);
    return ok;
}

static void set_image(lv_obj_t *img, lv_image_dsc_t *d, uint16_t *px, int side) {
    *d = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = side, .h = side, .stride = side * 2 },
        .data_size = side * side * 2,
        .data = (const uint8_t *)px,
    };
    lv_image_set_src(img, NULL);
    lv_image_set_src(img, d);
}

/* the chosen tab's text into the scrolling label (in the LVGL lock) */
static void show_tab(void) {
    const char *t = s_text[s_tab] ? s_text[s_tab] : "";
    if (s_tab == TAB_TRACKS) {
        lv_label_set_text(s_tracks, t[0] ? t : "The tracks come from Deezer...");
    } else if (t[0]) {
        lv_label_set_text_fmt(s_tracks, "From Wikipedia:\n\n%s", t);
    } else {
        static const char *none[] = { "", "No article about this album on Wikipedia", "No personnel on Wikipedia's page",
                                      "No reviews on Wikipedia's page" };
        lv_label_set_text(s_tracks, s_wiki_state == 0 ? "Looking it up on Wikipedia..."
                                    : s_wiki_state == 2 ? "Wikipedia can't be asked now: next time"
                                    : !s_text[TAB_ABOUT][0] ? none[TAB_ABOUT] : none[s_tab]);
    }
    lv_obj_scroll_to_y(lv_obj_get_parent(s_tracks), 0, LV_ANIM_OFF);
}

/* ---- the worker ---- */

static void spotlight(int k) {
    album_t a;
    if (k < 0 || !albums_get(k, 1, &a)) return;
    char path[80];
    uint32_t avg = BG;
    bool cover = albums_cover(a.id, true, path, sizeof path) && decode_fit(path, s_cover_px, BIG, &avg);
    lvgl_port_lock(0);
    s_spot_k = k;
    if (cover) set_image(s_cover, &s_cover_dsc, s_cover_px, BIG);
    lv_obj_set_hidden(s_cover, !cover);
    lv_obj_set_hidden(s_blank, cover);   /* none (yet, or at all): a plain square with the title */
    lv_label_set_text(s_blank_title, a.title);
    /* the page in the cover's colour, dark enough for the text */
    uint32_t bg = ((avg >> 16 & 255) * 3 / 10) << 16 | ((avg >> 8 & 255) * 3 / 10) << 8 | ((avg & 255) * 3 / 10);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(bg), 0);
    s_avg = lv_color_hex(avg);
    lv_color_t accent = lv_color_mix(s_avg, lv_color_hex(TEXT), 150);   /* light enough on the dark ground */
    lv_obj_set_style_text_color(s_artist, accent, 0);
    lv_obj_set_style_bg_color(s_tabs, accent, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_invalidate(s_spot);
    lv_label_set_text(s_title, a.title);
    lv_label_set_text(s_artist, a.artist);
    lv_label_set_text_fmt(s_meta, "%d   \xC2\xB7   %d tracks", a.year, a.tracks);
    for (int t = 0; t < TABS; t++) s_text[t][0] = 0;
    s_wiki_state = 0;
    show_tab();
    lvgl_port_unlock();
    /* label, genres and tracks: from Deezer, if it's there */
    static char *label, *genres, *tracks;   /* PSRAM: internal RAM is short */
    if (!label) {
        label = heap_caps_malloc(96, MALLOC_CAP_SPIRAM);
        genres = heap_caps_malloc(96, MALLOC_CAP_SPIRAM);
        tracks = heap_caps_malloc(3000, MALLOC_CAP_SPIRAM);
        if (!label || !genres || !tracks) return;
    }
    if (wifi_connected() && albums_details(a.id, label, 96, genres, 96, tracks, 3000)) {
        lvgl_port_lock(0);
        if (s_spot_k == k) {
            lv_label_set_text_fmt(s_meta, "%d   \xC2\xB7   %d tracks%s%s%s%s", a.year, a.tracks, label[0] ? "   \xC2\xB7   " : "", label,
                                  genres[0] ? "   \xC2\xB7   " : "", genres);
            snprintf(s_text[TAB_TRACKS], TAB_SIZE[TAB_TRACKS], "%s", tracks);
            show_tab();
        }
        lvgl_port_unlock();
    }
    /* what Wikipedia says: cached once looked up */
    static album_wiki_t *w;
    if (!w && !(w = heap_caps_malloc(sizeof *w, MALLOC_CAP_SPIRAM))) return;
    bool ok = albums_wiki(&a, w);   /* the cache first: no Wi-Fi needed then */
    lvgl_port_lock(0);
    if (s_spot_k == k) {
        s_wiki_state = ok ? 1 : 2;
        if (ok) {
            snprintf(s_text[TAB_ABOUT], TAB_SIZE[TAB_ABOUT], "%s", w->about);
            snprintf(s_text[TAB_CREDITS], TAB_SIZE[TAB_CREDITS], "%s", w->credits);
            snprintf(s_text[TAB_RECEPTION], TAB_SIZE[TAB_RECEPTION], "%s", w->reception);
        }
        show_tab();
    }
    lvgl_port_unlock();
}

static void wall(int page) {
    static album_t *a;   /* PSRAM */
    if (!a && !(a = heap_caps_calloc(PER_PAGE, sizeof *a, MALLOC_CAP_SPIRAM))) return;
    int n = albums_get(page * PER_PAGE, PER_PAGE, a);
    lvgl_port_lock(0);
    s_npages = s_count ? (s_count + PER_PAGE - 1) / PER_PAGE : 1;
    lv_label_set_text_fmt(s_pages, "%d / %d", page + 1, s_npages);
    for (int k = 0; k < PER_PAGE; k++) {
        lv_obj_set_hidden(s_cells[k], k >= n);
        if (k < n) s_cell_id[k] = a[k].id;
    }
    lvgl_port_unlock();
    for (int k = 0; k < n; k++) {
        if (uxQueueMessagesWaiting(s_q)) return;   /* another page asked: that one */
        char path[80];
        bool ok = albums_cover(a[k].id, false, path, sizeof path) && decode_fit(path, s_cell_px[k], SMALL, NULL);
        lvgl_port_lock(0);
        if (ok) set_image(s_cells[k], &s_cell_dsc[k], s_cell_px[k], SMALL);
        else lv_image_set_src(s_cells[k], NULL);
        lvgl_port_unlock();
    }
}

static void progress(const char *what) { status(what); }
static bool stop(void) { return uxQueueMessagesWaiting(s_q) > 0; }

static void sync(void) {
    char err[160] = "", sum[80];
    if (!albums_user()[0]) {
        status("Add your Deezer user ID to config.json on the card: \"deezer\": { \"user_id\": \"123...\" }");
        s_sync_wanted = false;
        return;
    }
    if (!wifi_connected()) {
        status("No Wi-Fi: the albums synced before, if any");
        return;   /* asked again at the next showing */
    }
    if (s_list_due) {
        if (albums_sync_list(progress, s_retry_misses, err, sizeof err) < 0) {
            status(err);
            s_sync_wanted = false;   /* Sync, or the next boot, tries again */
            return;
        }
        s_list_due = s_retry_misses = false;
        s_count = albums_count();
    }
    /* the covers: breaks off for any request, goes on afterwards (not the list again) */
    s_downloading = true;
    int left = albums_sync_covers(progress, stop);
    s_downloading = false;
    s_sync_wanted = left > 0;
    albums_summary(sum, sizeof sum);
    lvgl_port_lock(0);
    if (left) lv_label_set_text_fmt(s_sub, "%s   \xC2\xB7   %d covers to come", sum, left);
    else lv_label_set_text(s_sub, sum);
    lvgl_port_unlock();
}

static void worker(void *arg) {
    req_t r;
    for (;;) {
        /* a sync left to do goes on when nothing else is asked */
        if (xQueueReceive(s_q, &r, s_sync_wanted && s_shown ? pdMS_TO_TICKS(500) : portMAX_DELAY) != pdTRUE) {
            if (s_sync_wanted && s_shown) {
                bool empty = s_spot_k < 0;
                sync();
                if (empty && !s_wall_mode && s_count) ask(REQ_SPOT, 0);   /* the first album, once there are some */
            }
            continue;
        }
        if (r.kind == REQ_SPOT) spotlight(r.k);
        else if (r.kind == REQ_WALL) wall(r.k);
        else if (r.kind == REQ_CLOSE) albums_close();
        else if (r.kind == REQ_WAKE) continue;
        else s_sync_wanted = s_list_due = s_retry_misses = true;   /* Sync: the list again, and Wikipedia's misses */
    }
}

/* ---- the record and the shelves: drawn ---- */

static void disc_draw(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cx = a.x1 + DISC_X, cy = a.y1 + COVER_Y + BIG / 2;
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = LV_RADIUS_CIRCLE;
    r.bg_color = lv_color_hex(0x101010);
    r.shadow_width = 18;
    r.shadow_opa = LV_OPA_50;
    r.shadow_color = lv_color_black();
    lv_area_t d = { cx - DISC_R, cy - DISC_R, cx + DISC_R, cy + DISC_R };
    lv_draw_rect(layer, &r, &d);
    lv_draw_arc_dsc_t g;
    lv_draw_arc_dsc_init(&g);
    g.center.x = cx;
    g.center.y = cy;
    g.start_angle = 0;
    g.end_angle = 360;
    g.width = 1;
    g.color = lv_color_hex(0x2C2C2C);
    for (int rad = 78; rad < DISC_R - 4; rad += 7) {   /* the grooves; a wider gap between tracks */
        g.radius = rad;
        g.opa = rad % 5 ? LV_OPA_70 : LV_OPA_40;
        lv_draw_arc(layer, &g);
    }
    g.radius = DISC_R - 4;   /* the sheen: two faint wedges, opposite */
    g.width = DISC_R - 4 - 72;
    g.color = lv_color_white();
    g.opa = 14;
    g.start_angle = 300;
    g.end_angle = 335;
    lv_draw_arc(layer, &g);
    g.start_angle = 120;
    g.end_angle = 155;
    lv_draw_arc(layer, &g);
    r.shadow_width = 0;   /* the label, its rim, the hole */
    r.bg_color = lv_color_mix(s_avg, lv_color_black(), 200);
    d = (lv_area_t){ cx - 66, cy - 66, cx + 66, cy + 66 };
    lv_draw_rect(layer, &r, &d);
    r.bg_color = s_avg;
    d = (lv_area_t){ cx - 62, cy - 62, cx + 62, cy + 62 };
    lv_draw_rect(layer, &r, &d);
    r.bg_color = lv_color_mix(lv_color_hex(TEXT), s_avg, 90);
    d = (lv_area_t){ cx - 40, cy - 1, cx + 40, cy };   /* a printed line, as labels have */
    r.radius = 0;
    lv_draw_rect(layer, &r, &d);
    r.radius = LV_RADIUS_CIRCLE;
    r.bg_color = lv_color_hex(0x080808);
    d = (lv_area_t){ cx - 5, cy - 5, cx + 5, cy + 5 };
    lv_draw_rect(layer, &r, &d);
}

/* a plank under each row of the Wall, wall to wall */
static void shelves_draw(lv_event_t *e) {
    if (!s_wall_mode) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t w;
    lv_obj_get_coords(s_wall, &w);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    for (int row = 0; row < ROWS; row++) {
        int y = w.y1 + row * (SMALL + 12) + SMALL;
        lv_area_t a = { 0, y + 4, 1023, y + 12 };   /* its shadow on the wall */
        r.bg_color = lv_color_black();
        r.bg_opa = LV_OPA_40;
        lv_draw_rect(layer, &r, &a);
        r.bg_opa = LV_OPA_COVER;
        a = (lv_area_t){ 0, y, 1023, y + 7 };
        r.bg_color = lv_color_hex(WOOD);
        lv_draw_rect(layer, &r, &a);
        a.y2 = a.y1;
        r.bg_color = lv_color_hex(WOOD_LIT);
        lv_draw_rect(layer, &r, &a);
        a.y1 = a.y2 = y + 7;
        r.bg_color = lv_color_hex(WOOD_DARK);
        lv_draw_rect(layer, &r, &a);
    }
}

/* ---- the page's events ---- */

static void show_mode(void) {
    lv_obj_set_hidden(s_spot, s_wall_mode);
    lv_obj_set_hidden(s_wall, !s_wall_mode);
    lv_obj_set_hidden(s_prev, !s_wall_mode);
    lv_obj_set_hidden(s_next, !s_wall_mode);
    lv_obj_set_hidden(s_pages, !s_wall_mode);
    if (s_wall_mode) lv_obj_set_style_bg_color(s_page, lv_color_hex(BG), 0);
    lv_obj_invalidate(s_page);   /* the shelves */
}

static void spot_next(void) {
    if (s_count) ask(REQ_SPOT, (s_spot_k + 1 + esp_random() % (s_count > 1 ? s_count - 1 : 1)) % s_count);
}

static void tick_cb(lv_timer_t *t) {
    if (esp_timer_get_time() / 1000 < s_hold_until) return;   /* someone's reading */
    if (!s_wall_mode && !s_downloading) spot_next();   /* a download gives way to any request: not to this one */
}

static void cover_cb(lv_event_t *e) {
    lv_timer_reset(s_tick);
    spot_next();
}

static void tab_cb(lv_event_t *e) {
    s_tab = lv_buttonmatrix_get_selected_button(lv_event_get_current_target_obj(e));
    s_hold_until = esp_timer_get_time() / 1000 + HOLD_MS;
    show_tab();
}

static void cell_cb(lv_event_t *e) {   /* a cover on the Wall: into the Spotlight */
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    s_wall_mode = false;
    show_mode();
    lv_timer_reset(s_tick);
    ask(REQ_SPOT, s_pageno * PER_PAGE + k);
}

static void set_mode(bool wall) {
    s_wall_mode = wall;
    show_mode();
    if (s_wall_mode) ask(REQ_WALL, s_pageno);
    else if (s_count) ask(REQ_SPOT, s_spot_k >= 0 ? s_spot_k : 0);
}

static void mode_cb(lv_event_t *e) {
    set_mode(lv_buttonmatrix_get_selected_button(lv_event_get_current_target_obj(e)) == 1);
}

void album_wall_mode(bool wall) {
    if (wall == s_wall_mode) return;
    lv_buttonmatrix_set_button_ctrl(s_modes, wall ? 1 : 0, LV_BUTTONMATRIX_CTRL_CHECKED);   /* one_checked: the other unchecked */
    set_mode(wall);
}

static void page_cb(lv_event_t *e) {
    int p = s_pageno + (int)(intptr_t)lv_event_get_user_data(e);
    if (p < 0 || p >= s_npages) return;
    s_pageno = p;
    ask(REQ_WALL, p);
}

static void sync_cb(lv_event_t *e) {
    lv_label_set_text(s_sub, "Syncing...");
    ask(REQ_SYNC, 0);
}

void album_wall_shown(bool shown) {
    s_shown = shown;
    if (!shown) {
        lv_timer_pause(s_tick);
        ask(REQ_CLOSE, 0);   /* its files' internal RAM back */
        return;
    }
    if (s_sync_wanted) ask(REQ_WAKE, 0);   /* the worker waits for a request: this wakes it to sync */
    s_count = albums_count();
    if (s_count) {
        char sum[80];
        albums_summary(sum, sizeof sum);
        lv_label_set_text(s_sub, sum);
        if (s_wall_mode) ask(REQ_WALL, s_pageno);
        else ask(REQ_SPOT, s_spot_k >= 0 ? s_spot_k : 0);
    }
    lv_timer_resume(s_tick);
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

static lv_obj_t *small_button(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, int d) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(BUTTON), 0);
    lv_obj_set_style_text_color(b, lv_color_hex(TEXT), 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_size(b, 48, 40);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
    return b;
}

lv_obj_t *album_wall_create(lv_obj_t *parent) {
    lv_obj_t *page = s_page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollable(page, false);
    lv_obj_add_event_cb(page, shelves_draw, LV_EVENT_DRAW_MAIN, NULL);
    s_avg = lv_color_hex(BLANK);

    lv_obj_t *head = label(page, &art_font_24, TEXT);   /* right of the menu button */
    lv_label_set_text(head, "Album Wall");
    lv_obj_set_pos(head, 90, 12);
    s_sub = label(page, &art_font_16, MINOR);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_sub, 860);
    lv_obj_set_pos(s_sub, 91, 44);
    lv_label_set_text(s_sub, "Your favourite albums on Deezer");

    lv_obj_t *modes = lv_buttonmatrix_create(page);
    static const char *map[] = { "Spotlight", "Wall", "" };
    lv_buttonmatrix_set_map(modes, map);
    lv_buttonmatrix_set_button_ctrl_all(modes, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(modes, true);
    lv_buttonmatrix_set_button_ctrl(modes, 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(modes, 240, 40);
    lv_obj_set_pos(modes, 300, 4);
    lv_obj_set_style_text_font(modes, &art_font_16, 0);
    lv_obj_set_style_bg_opa(modes, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(modes, 0, 0);
    lv_obj_set_style_pad_all(modes, 0, 0);
    lv_obj_set_style_bg_color(modes, lv_color_hex(BUTTON), LV_PART_ITEMS);
    lv_obj_set_style_text_color(modes, lv_color_hex(TEXT), LV_PART_ITEMS);
    lv_obj_set_style_radius(modes, 2, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(modes, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(modes, lv_color_hex(TEXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(modes, lv_color_hex(BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(modes, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_modes = modes;
    s_prev = small_button(page, LV_SYMBOL_LEFT, page_cb, -1);
    lv_obj_set_pos(s_prev, 600, 4);
    s_pages = label(page, &art_font_20, TEXT);
    lv_obj_set_width(s_pages, 90);
    lv_obj_set_style_text_align(s_pages, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_pages, 652, 13);
    s_next = small_button(page, LV_SYMBOL_RIGHT, page_cb, 1);
    lv_obj_set_pos(s_next, 746, 4);
    lv_obj_t *sync = small_button(page, LV_SYMBOL_REFRESH, sync_cb, 0);
    lv_obj_align(sync, LV_ALIGN_TOP_RIGHT, -12, 4);

    /* the Spotlight: the cover left, its card right */
    s_spot = plain(page);
    lv_obj_set_size(s_spot, LV_PCT(100), 600 - 76);
    lv_obj_set_pos(s_spot, 0, 76);
    lv_obj_set_clickable(s_spot, false);
    lv_obj_add_event_cb(s_spot, disc_draw, LV_EVENT_DRAW_MAIN, NULL);
    s_cover = lv_image_create(s_spot);   /* the sleeve, over its record */
    lv_obj_set_pos(s_cover, COVER_X, COVER_Y);
    lv_obj_set_style_shadow_width(s_cover, 24, 0);
    lv_obj_set_style_shadow_offset_x(s_cover, 6, 0);
    lv_obj_set_style_shadow_offset_y(s_cover, 8, 0);
    lv_obj_set_style_shadow_opa(s_cover, LV_OPA_60, 0);
    lv_obj_set_style_shadow_color(s_cover, lv_color_black(), 0);
    lv_obj_set_clickable(s_cover, true);   /* a tap: another album */
    lv_obj_add_event_cb(s_cover, cover_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_cover, true);
    s_blank = lv_obj_create(s_spot);   /* for an album without a cover */
    lv_obj_remove_style_all(s_blank);
    lv_obj_set_size(s_blank, BIG, BIG);
    lv_obj_set_pos(s_blank, COVER_X, COVER_Y);
    lv_obj_set_style_bg_color(s_blank, lv_color_hex(BLANK), 0);
    lv_obj_set_style_bg_opa(s_blank, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_blank, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_blank, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_blank, 20, 0);
    lv_obj_add_event_cb(s_blank, cover_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *note = label(s_blank, &lv_font_montserrat_48, MINOR);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    s_blank_title = label(s_blank, &art_font_24, TEXT);
    lv_obj_set_width(s_blank_title, BIG - 60);
    lv_label_set_long_mode(s_blank_title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_blank_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_hidden(s_blank, true);
    lv_obj_t *card = plain(s_spot);
    lv_obj_set_pos(card, CARD_X, COVER_Y);
    lv_obj_set_size(card, 1024 - CARD_X - 20, 456);   /* above the Wi-Fi button */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    s_title = label(card, &art_font_32, TEXT);
    s_artist = label(card, &art_font_24, TEXT);
    s_meta = label(card, &art_font_16, MINOR);
    lv_obj_t *objs[] = { s_title, s_artist, s_meta };
    for (int k = 0; k < 3; k++) {
        lv_obj_set_width(objs[k], LV_PCT(100));
        lv_label_set_long_mode(objs[k], LV_LABEL_LONG_MODE_WRAP);
    }
    lv_obj_t *rule = plain(card);   /* under the title */
    lv_obj_set_size(rule, 64, 4);
    lv_obj_set_style_bg_color(rule, lv_color_hex(TEXT), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_move_to_index(rule, 1);
    lv_obj_t *tabs = s_tabs = lv_buttonmatrix_create(card);   /* what the text below is */
    static const char *tab_map[] = { "Tracks", "About", "Credits", "Reception", "" };
    lv_buttonmatrix_set_map(tabs, tab_map);
    lv_buttonmatrix_set_button_ctrl_all(tabs, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(tabs, true);
    lv_buttonmatrix_set_button_ctrl(tabs, 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(tabs, LV_PCT(100), 38);
    lv_obj_set_style_text_font(tabs, &art_font_16, 0);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tabs, 0, 0);
    lv_obj_set_style_pad_all(tabs, 0, 0);
    lv_obj_set_style_pad_column(tabs, 4, 0);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_TRANSP, LV_PART_ITEMS);   /* flat; the chosen one a block of the accent */
    lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(tabs, lv_color_hex(TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_color(tabs, lv_color_hex(0x141010), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tabs, lv_color_hex(MINOR), LV_PART_ITEMS);
    lv_obj_set_style_border_width(tabs, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_side(tabs, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_radius(tabs, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(tabs, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(tabs, tab_cb, LV_EVENT_VALUE_CHANGED, NULL);
    for (int t = 0; t < TABS; t++) s_text[t] = heap_caps_calloc(1, TAB_SIZE[t], MALLOC_CAP_SPIRAM);
    lv_obj_t *list = lv_obj_create(card);   /* the tab's text: scrolls when long */
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_style_pad_top(list, 6, 0);
    s_tracks = label(list, &art_font_16, TEXT);
    lv_obj_set_width(s_tracks, LV_PCT(100));
    lv_label_set_long_mode(s_tracks, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(s_tracks, 5, 0);

    /* the Wall */
    s_wall = plain(page);
    lv_obj_set_pos(s_wall, 40, 70);
    lv_obj_set_size(s_wall, COLS * SMALL + (COLS - 1) * 12, ROWS * SMALL + (ROWS - 1) * 12);
    lv_obj_set_flex_flow(s_wall, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_overflow_visible(s_wall, true);   /* the covers' shadows */
    lv_obj_set_style_pad_column(s_wall, 12, 0);
    lv_obj_set_style_pad_row(s_wall, 12, 0);
    for (int k = 0; k < PER_PAGE; k++) {
        s_cells[k] = lv_image_create(s_wall);
        lv_obj_set_size(s_cells[k], SMALL, SMALL);
        lv_obj_set_style_bg_color(s_cells[k], lv_color_hex(BLANK), 0);
        lv_obj_set_style_bg_opa(s_cells[k], LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(s_cells[k], 10, 0);   /* leaning on the wall */
        lv_obj_set_style_shadow_offset_x(s_cells[k], 4, 0);
        lv_obj_set_style_shadow_opa(s_cells[k], LV_OPA_50, 0);
        lv_obj_set_style_shadow_color(s_cells[k], lv_color_black(), 0);
        lv_obj_set_clickable(s_cells[k], true);
        lv_obj_add_event_cb(s_cells[k], cell_cb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
        s_cell_px[k] = heap_caps_aligned_calloc(64, SMALL * SMALL, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }
    s_cover_px = heap_caps_aligned_calloc(64, BIG * BIG, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    show_mode();

    s_tick = lv_timer_create(tick_cb, SPOT_MS, NULL);
    lv_timer_pause(s_tick);
    s_q = xQueueCreate(4, sizeof(req_t));
    /* stack in PSRAM, idle priority: as the artwork task */
    if (!s_q || !s_cover_px || xTaskCreateWithCaps(worker, "albumwall", 10 * 1024, NULL, tskIDLE_PRIORITY, NULL, MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGE(TAG, "no memory for the album wall");
    lv_obj_set_hidden(page, true);
    return page;
}
