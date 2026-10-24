/* generative.c - the page: the canvas full screen (an lv_image of it), a
 * caption; the piece's kind (all in turn, or one) top left; Recolour, Hold,
 * Next and Save top right. A task paints in 25 ms slices (giving
 * the CPU back between them) and redraws the screen 4 times a second; a
 * finished piece stays a minute and a half, then the next algorithm starts.
 * The palette: 5 colours of the picture the slideshow shows.
 * The chrome, after the computer-art shows of the 1960s: a wall label at
 * the bottom (the palette as five paint chips: a tap recolours; the
 * title, painting or finished, the settings, the colours' source, the
 * music) and the buttons in one dark console, top right. */
#include "generative.h"
#include "gen_art.h"
#include "slideshow.h"
#include "settings.h"
#include "index.h"
#include "thumb.h"
#include "music.h"
#include "board_pins.h"
#include "fonts/art_fonts.h"
#include "esp_jpeg_enc.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define HOLD_MS   (90 * 1000)   /* a finished piece stays */
#define SLICE_MS  25
#define SAVE_DIR  "/sdcard/Generated"

static const char *TAG = "generative";

#define LABEL_W   680
#define LABEL_H   64
#define CHIP_W    18
#define INK       0xF0EDE6
#define GREY      0xA8A39A
#define CONSOLE   LV_OPA_50   /* the dark capsules' opacity over the piece */

static lv_obj_t *s_page, *s_img, *s_title, *s_state, *s_detail, *s_status, *s_chip[GEN_COLORS];
static lv_image_dsc_t s_dsc;
static gen_canvas_t s_c;
static TaskHandle_t s_task;
static volatile bool s_shown, s_want_next, s_want_save, s_want_recolour;
static lv_obj_t *s_hold_label, *s_music_label;
static int s_algo = -1;

/* ---- the palette: from the slideshow's picture ---- */

static const uint32_t DEFAULT_PAL[GEN_COLORS] = { 0x0B1026, 0x1F3A7A, 0x3C6EB4, 0xD9B44A, 0xF4E7B0 };

static int luma(uint32_t c) { return ((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255); }

static int dist2(uint32_t a, uint32_t b) {
    int dr = (int)((a >> 16) & 255) - (int)((b >> 16) & 255), dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255),
        db = (int)(a & 255) - (int)(b & 255);
    return dr * dr + dg * dg + db * db;
}

/* the most common colours (4 bits a channel), each unlike the ones before,
   darkest first; the darkest made darker still: the ground */
static void palette_from(const uint16_t *src, int w, int h, int step, uint32_t *pal) {
    static uint16_t *hist;   /* 8 KB, PSRAM */
    if (!hist && !(hist = heap_caps_malloc(4096 * sizeof *hist, MALLOC_CAP_SPIRAM))) {
        memcpy(pal, DEFAULT_PAL, sizeof DEFAULT_PAL);
        return;
    }
    memset(hist, 0, 4096 * sizeof *hist);
    int n = 0;
    for (int y = step / 2; y < h; y += step)
        for (int x = step / 2; x < w; x += step) {
            uint16_t p = src[y * w + x];
            int r = p >> 11, g = (p >> 5) & 63, b = p & 31;
            if (r + g / 2 + b < 3) continue;   /* the black bars around a picture */
            hist[(r >> 1) << 8 | (g >> 2) << 4 | (b >> 1)]++;
            n++;
        }
    int k = 0;
    for (int min = 70 * 70; k < GEN_COLORS && min >= 20 * 20; min /= 2) {   /* less picky if it's short of colours */
        for (int round = 0; round < 4096 && k < GEN_COLORS; round++) {
            int best = -1;
            for (int i = 0; i < 4096; i++)
                if (hist[i] && (best < 0 || hist[i] > hist[best])) best = i;
            if (best < 0) break;
            uint32_t c = ((best >> 8) * 17) << 16 | (((best >> 4) & 15) * 17) << 8 | ((best & 15) * 17);
            hist[best] = 0;
            bool far = true;
            for (int j = 0; j < k; j++) far &= dist2(c, pal[j]) >= min;
            if (far) pal[k++] = c;
        }
    }
    if (n < 60 || k < GEN_COLORS) {
        memcpy(pal, DEFAULT_PAL, sizeof DEFAULT_PAL);
        return;
    }
    for (int i = 0; i < GEN_COLORS; i++)   /* darkest first */
        for (int j = i + 1; j < GEN_COLORS; j++)
            if (luma(pal[j]) < luma(pal[i])) {
                uint32_t t = pal[i];
                pal[i] = pal[j];
                pal[j] = t;
            }
    uint32_t g = pal[0];
    pal[0] = ((g >> 16 & 255) * 2 / 5) << 16 | ((g >> 8 & 255) * 2 / 5) << 8 | ((g & 255) * 2 / 5);
}

/* the painter's name, from the slideshow's file ("Paul Signac - ...") */
static void palette_source(char *buf, size_t n) {
    char path[256];
    buf[0] = 0;
    if (!slideshow_now(path, sizeof path)) return;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *sep = strstr(base, " - ");
    if (sep) snprintf(buf, n, "%.*s", (int)(sep - base), base);
}

/* ---- saving: the canvas as a JPEG, into the slideshow ---- */

static void save(void) {
    char path[160], msg[200];
    mkdir(SAVE_DIR, 0777);
    snprintf(path, sizeof path, SAVE_DIR "/%s.jpg", s_c.name);
    struct stat st;
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = s_c.w;
    cfg.height = s_c.h;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.subsampling = JPEG_SUBSAMPLE_444;   /* thin lines keep their colour */
    cfg.quality = 90;
    jpeg_enc_handle_t enc = NULL;
    const int cap = 1024 * 1024;
    uint8_t *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    int len = 0;
    bool ok = false;
    if (stat(path, &st) == 0) {
        snprintf(msg, sizeof msg, "Already on the card");
    } else if (out && jpeg_enc_open(&cfg, &enc) == JPEG_ERR_OK &&
               jpeg_enc_process(enc, (const uint8_t *)s_c.px, s_c.w * s_c.h * 2, out, cap, &len) == JPEG_ERR_OK) {
        FILE *f = fopen(path, "wb");
        ok = f && fwrite(out, 1, len, f) == (size_t)len;
        if (f && fclose(f)) ok = false;
        if (ok) {
            slideshow_added(path);
            snprintf(msg, sizeof msg, "Saved in \"Generated\" (%d KB): in the slideshow and the Gallery now", len / 1024);
        } else {
            remove(path);
            snprintf(msg, sizeof msg, "Can't write it on the card");
        }
    } else {
        snprintf(msg, sizeof msg, "Can't make the JPEG");
    }
    if (enc) jpeg_enc_close(enc);
    free(out);
    ESP_LOGI(TAG, "save %s: %s", path, msg);
    lvgl_port_lock(0);
    lv_label_set_text(s_status, msg);
    lv_obj_set_hidden(s_status, false);
    lvgl_port_unlock();
}

/* ---- the painter ---- */

/* the painter's name, from a picture's path ("Paul Signac - ...") */
static void artist_of(const char *path, char *buf, size_t n) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *sep = strstr(base, " - ");
    snprintf(buf, n, "%.*s", sep ? (int)(sep - base) : 0, base);
}

/* the colours of another painting on the card: its Gallery thumbnail (a
   4096 px picture would take seconds to decode) */
static bool palette_random(char *who, size_t n) {
    char path[256];
    uint8_t *jpg;
    size_t len;
    static uint16_t *px;
    if (!px) px = heap_caps_aligned_calloc(16, 176 * 112, sizeof *px, MALLOC_CAP_SPIRAM);
    int w, h;
    if (!px || !index_random_thumb(path, sizeof path, &jpg, &len)) return false;
    bool ok = thumb_decode(jpg, len, px, &w, &h);
    free(jpg);
    if (!ok) return false;
    palette_from(px, w, h, 2, s_c.pal);
    artist_of(path, who, n);
    return true;
}

/* a new piece: the next kind in turn (or the one picked), a new seed, the
   colours of the slideshow's picture; recolour: the same piece, another
   painting's colours */
static void start_piece(bool recolour) {
    char who[96] = "";
    if (recolour) {
        if (!palette_random(who, sizeof who)) {
            lvgl_port_lock(0);
            lv_label_set_text(s_status, "No thumbnails yet to take colours from (they come as pictures are shown)");
            lv_obj_set_hidden(s_status, false);
            lvgl_port_unlock();
            return;
        }
    } else {
        int mode = g_settings.gen_mode;
        s_algo = mode >= 0 && mode < gen_nalgos ? mode : (s_algo + 1) % gen_nalgos;
        s_c.seed = esp_random() % 10000;
        const uint16_t *screen = slideshow_screen();
        if (screen) palette_from(screen, LCD_H_RES, LCD_V_RES, 8, s_c.pal);
        else memcpy(s_c.pal, DEFAULT_PAL, sizeof DEFAULT_PAL);
        palette_source(who, sizeof who);
    }
    s_c.dirty_y0 = 1 << 30;
    s_c.dirty_y1 = -1;
    gen_algos[s_algo].start(&s_c);
    char tune[64];   /* its music: the same kind, seed and colours */
    music_piece(s_c.seed, s_algo, s_c.pal, tune, sizeof tune);
    /* the label: the title (the caption up to its first dot), then the rest */
    static const char DOT[] = "   \xC2\xB7   ";
    const char *cut = strstr(s_c.caption, DOT);
    int tlen = cut ? (int)(cut - s_c.caption) : (int)strlen(s_c.caption);
    const char *rest = cut ? cut + strlen(DOT) : "";
    lvgl_port_lock(0);
    lv_label_set_text_fmt(s_title, "%.*s", tlen, s_c.caption);
    lv_label_set_text(s_state, "PAINTING");
    lv_label_set_text_fmt(s_detail, "%s%s%s%s%smusic: %s", rest, rest[0] ? DOT : "", who[0] ? "colours from " : "", who,
                          who[0] ? DOT : "", tune);
    for (int k = 0; k < GEN_COLORS; k++) lv_obj_set_style_bg_color(s_chip[k], lv_color_hex(s_c.pal[k]), 0);
    lv_obj_set_hidden(s_status, true);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "%s%s", s_c.caption, recolour ? " (recoloured)" : "");
}

/* the rows painted since last time, redrawn: only those (a Hitomezashi row,
   not the whole screen) */
static void redraw(void) {
    if (s_c.dirty_y1 < s_c.dirty_y0) return;
    lv_area_t a = { 0, s_c.dirty_y0 < 0 ? 0 : s_c.dirty_y0, s_c.w - 1, s_c.dirty_y1 >= s_c.h ? s_c.h - 1 : s_c.dirty_y1 };
    s_c.dirty_y0 = 1 << 30;
    s_c.dirty_y1 = -1;
    lvgl_port_lock(0);
    lv_obj_invalidate_area(s_img, &a);
    lvgl_port_unlock();
}

static void painter(void *arg) {
    int64_t last_draw = 0, done_at = 0;
    for (;;) {
        if (!s_shown) {   /* not on show: nothing to do till it is */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        int64_t now = esp_timer_get_time() / 1000;
        bool expired = done_at && !g_settings.gen_hold && now - done_at > HOLD_MS;
        if (s_algo < 0 || s_want_next || expired) {
            s_want_next = false;
            done_at = 0;
            start_piece(false);
        }
        if (s_want_recolour) {
            s_want_recolour = false;
            done_at = 0;
            start_piece(true);
        }
        if (s_want_save) {
            s_want_save = false;
            save();
        }
        if (!done_at && gen_algos[s_algo].step(&s_c, SLICE_MS)) {
            done_at = esp_timer_get_time() / 1000;
            ESP_LOGI(TAG, "%s: finished", s_c.name);
            lvgl_port_lock(0);
            lv_label_set_text(s_state, "FINISHED");
            lvgl_port_unlock();
        }
        now = esp_timer_get_time() / 1000;
        if (now - last_draw > 250 || done_at) {   /* the screen: 4 times a second at most */
            redraw();
            last_draw = now;
        }
        vTaskDelay(done_at ? pdMS_TO_TICKS(200) : 1);   /* the CPU back: the idle task, LVGL */
    }
}

/* ---- the page ---- */

static void next_cb(lv_event_t *e) { s_want_next = true; }

static void recolour_cb(lv_event_t *e) { s_want_recolour = true; }

static void show_music(void) { lv_label_set_text(s_music_label, g_settings.gen_music ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE); }

static void music_cb(lv_event_t *e) {
    g_settings.gen_music = !g_settings.gen_music;
    settings_save();
    show_music();
    music_play(g_settings.gen_music && s_shown);
}

static void show_hold(void) { lv_label_set_text(s_hold_label, g_settings.gen_hold ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE); }

/* hold: a finished piece stays till Next (the icon: what a tap does) */
static void hold_cb(lv_event_t *e) {
    g_settings.gen_hold = !g_settings.gen_hold;
    settings_save();
    show_hold();
    lv_label_set_text(s_status, g_settings.gen_hold ? "Held: this piece stays till Next" : "Each piece now stays 90 s, then the next");
    lv_obj_set_hidden(s_status, false);
}

static void mode_cb(lv_event_t *e) {
    g_settings.gen_mode = (int32_t)lv_dropdown_get_selected(lv_event_get_current_target_obj(e)) - 1;   /* 0: all in turn */
    settings_save();
    s_want_next = true;
}

static void save_cb(lv_event_t *e) {
    lv_label_set_text(s_status, "Saving...");
    lv_obj_set_hidden(s_status, false);
    s_want_save = true;
}

void generative_shown(bool shown) {
    s_shown = shown;
    music_play(shown && g_settings.gen_music);
    slideshow_background(!shown);   /* the slideshow's thumbnails wait: the CPU and PSRAM are this page's */
    if (shown && s_task) xTaskNotifyGive(s_task);
}

/* a button of the console: no ground of its own, a pale icon */
static lv_obj_t *button(lv_obj_t *parent, const char *sym, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(b, 48, 44);
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(INK), 0);
    lv_obj_set_style_text_opa(l, LV_OPA_80, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

/* a dark capsule over the piece */
static void capsule(lv_obj_t *o) {
    lv_obj_set_style_bg_color(o, lv_color_hex(0x0C0C0C), 0);
    lv_obj_set_style_bg_opa(o, CONSOLE, 0);
    lv_obj_set_style_radius(o, 24, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_white(), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_10, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *generative_create(lv_obj_t *parent) {
    s_c.w = LCD_H_RES;
    s_c.h = LCD_V_RES;
    s_c.px = heap_caps_aligned_calloc(64, s_c.w * s_c.h, sizeof(uint16_t), MALLOC_CAP_SPIRAM);   /* the PPA's lines */

    lv_obj_t *page = s_page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollable(page, false);

    s_img = lv_image_create(page);
    if (s_c.px) {
        s_dsc = (lv_image_dsc_t){
            .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = s_c.w, .h = s_c.h, .stride = s_c.w * 2 },
            .data_size = s_c.w * s_c.h * 2,
            .data = (const uint8_t *)s_c.px,
        };
        lv_image_set_src(s_img, &s_dsc);
    }
    lv_obj_set_pos(s_img, 0, 0);

    /* the console, top right: music, recolour, hold, next, save */
    lv_obj_t *con = lv_obj_create(page);
    lv_obj_remove_style_all(con);
    capsule(con);
    lv_obj_set_size(con, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_pad_hor(con, 6, 0);
    lv_obj_set_style_pad_column(con, 6, 0);
    lv_obj_set_flex_flow(con, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(con, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(con, false);
    lv_obj_align(con, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_t *music = button(con, LV_SYMBOL_VOLUME_MAX, music_cb);
    s_music_label = lv_obj_get_child(music, 0);
    button(con, LV_SYMBOL_TINT, recolour_cb);
    lv_obj_t *hold = button(con, LV_SYMBOL_PAUSE, hold_cb);
    s_hold_label = lv_obj_get_child(hold, 0);
    show_hold();
    button(con, LV_SYMBOL_NEXT, next_cb);
    button(con, LV_SYMBOL_SAVE, save_cb);
    show_music();
    music_init();

    /* the piece's kind, right of the menu button */
    static char opts[200] = "All in turn";
    for (int k = 0; k < gen_nalgos; k++) snprintf(opts + strlen(opts), sizeof opts - strlen(opts), "\n%s", gen_algos[k].title);
    lv_obj_t *dd = lv_dropdown_create(page);
    lv_dropdown_set_options_static(dd, opts);
    lv_dropdown_set_selected(dd, g_settings.gen_mode + 1 >= 0 && g_settings.gen_mode < gen_nalgos ? g_settings.gen_mode + 1 : 0);
    lv_obj_set_width(dd, 210);
    lv_obj_set_pos(dd, 92, 12);   /* clear of the menu button's touch area (to x 80) */
    lv_obj_set_style_text_font(dd, &art_font_16, 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(INK), 0);
    capsule(dd);
    lv_obj_set_height(dd, 52);
    lv_obj_set_style_pad_left(dd, 18, 0);
    lv_obj_set_style_pad_ver(dd, 15, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, &art_font_20, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x161616), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(INK), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(0x3A3A3A), 0);
    lv_obj_add_event_cb(dd, mode_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* the wall label: bottom, between the corner buttons; the chips recolour */
    lv_obj_t *lab = lv_obj_create(page);
    lv_obj_remove_style_all(lab);
    lv_obj_set_size(lab, LABEL_W, LABEL_H);
    lv_obj_align(lab, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(lab, lv_color_hex(0x0C0C0C), 0);
    lv_obj_set_style_bg_opa(lab, LV_OPA_70, 0);
    lv_obj_set_style_radius(lab, 4, 0);
    lv_obj_set_style_clip_corner(lab, true, 0);
    lv_obj_set_scrollable(lab, false);
    for (int k = 0; k < GEN_COLORS; k++) {
        lv_obj_t *c = s_chip[k] = lv_obj_create(lab);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, CHIP_W, LABEL_H);
        lv_obj_set_pos(c, k * (CHIP_W + 1), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(DEFAULT_PAL[k]), 0);
        lv_obj_add_event_cb(c, recolour_cb, LV_EVENT_CLICKED, NULL);
    }
    const int tx = GEN_COLORS * (CHIP_W + 1) + 14, tw = LABEL_W - tx - 14;
    s_title = text(lab, &art_font_20, INK);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_title, tw - 110, 24);   /* DOTS needs a height */
    lv_obj_set_pos(s_title, tx, 8);
    s_state = text(lab, &art_font_16, GREY);
    lv_obj_set_style_text_letter_space(s_state, 2, 0);
    lv_obj_set_width(s_state, 104);
    lv_obj_set_style_text_align(s_state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_state, tx + tw - 104, 11);
    s_detail = text(lab, &art_font_16, GREY);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_detail, tw, 22);
    lv_obj_set_pos(s_detail, tx, 36);
    s_status = lv_label_create(page);
    lv_obj_set_style_text_font(s_status, &art_font_16, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xFFC93C), 0);
    capsule(s_status);
    lv_obj_set_style_pad_hor(s_status, 16, 0);
    lv_obj_set_style_pad_ver(s_status, 8, 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -14 - LABEL_H - 10);   /* over the label */
    lv_obj_set_hidden(s_status, true);

    /* below the slideshow's priority would starve it; at 1 with a tick's
       pause between slices, the idle task and LVGL get their turn */
    if (!s_c.px || xTaskCreatePinnedToCore(painter, "generative", 6 * 1024, NULL, 1, &s_task, 1) != pdPASS)
        ESP_LOGE(TAG, "no memory for the generative page");
    lv_obj_set_hidden(page, true);
    return page;
}
