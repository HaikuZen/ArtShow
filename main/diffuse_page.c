/* diffuse_page.c - the view. The picture on show is copied once at the
 * open (slideshow_screen(), after transition_finish(): the whole one), and
 * the effect paints a second buffer, the view's lv_image (64-byte aligned
 * for the PPA). A task on core 1 at priority 1, like the Generative page's
 * painter: 100 ms of work, then a tick back (10 ms); the view redrawn at most 4 times
 * a second. A tap on another effect restarts from the picture. The two
 * pictures (2.4 MB of PSRAM) stay once opened; an effect's scratch (up to
 * 3.7 MB) only while it runs.
 * A printmaker's ink drawer: the effects are chips in a dark tray along the
 * bottom, each with a swatch of the inks it prints with (drawn: its fixed
 * inks, or the picture's own colours as stripes, pixels, a wash or coral
 * dots); the one chosen outlined in cream, with its progress; Save a
 * seal-red chop. */
#include "diffuse_page.h"
#include "diffuse.h"
#include "slideshow.h"
#include "transition.h"
#include "esp_jpeg_enc.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "fonts/art_fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define W        1024
#define H        600
#define SAVE_DIR "/sdcard/Diffused"
#define NONE     -1
#define CHIP_W   100
#define CHIP_H   74
#define SWATCH_H 40
#define CREAM    0xF0EADC
#define SEAL     0xB0362A

static const char *TAG = "diffuse";

static lv_obj_t *s_page, *s_img, *s_btn, *s_btn_label, *s_status, *s_save, *s_tray;
static lv_obj_t *s_fx_btn[8];
static lv_image_dsc_t s_dsc;
static uint16_t *s_src, *s_dst;
static char s_path[256];             /* the picture's file, for the saved one's name */
static TaskHandle_t s_task;
static volatile int s_want = NONE;   /* an effect to run (the button), taken by the task */
static volatile bool s_stop, s_want_save, s_want_load;
static int s_fx = NONE;              /* the one run last (the task's) */
static bool s_finished;
static volatile int s_active = NONE;   /* the chip outlined (the UI's), and its progress, 0..1000 (the task's) */
static volatile int s_prog;
static uint32_t s_pic[4];              /* the picture's colours, a quadrant each: the chips that print in them */

/* the chips again: all (new colours), or only the one running (its progress) */
static void chips_redraw(bool all) {
    lvgl_port_lock(0);
    for (int k = 0; k < diffuse_nfx && k < 8; k++)
        if (s_fx_btn[k] && (all || k == s_active)) lv_obj_invalidate(s_fx_btn[k]);
    lvgl_port_unlock();
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void status(const char *txt) {
    lvgl_port_lock(0);
    lv_label_set_text(s_status, txt);
    lvgl_port_unlock();
}

/* the view again: all of it, or (while an effect runs) only above the tray:
   redrawing under it redraws the tray and its chips too, four times a
   second, and that took ~2 s from Melt's 15 */
static void refresh(bool all) {
    lvgl_port_lock(0);
    if (all) {
        lv_obj_invalidate(s_img);
    } else {
        lv_area_t a;
        lv_obj_get_coords(s_tray, &a);
        lv_area_t up = { 0, 0, W - 1, a.y1 - 1 };
        lv_obj_invalidate_area(s_img, &up);
    }
    lvgl_port_unlock();
}

/* "/sdcard/x/Paul Signac - Port.jpg" -> "Paul Signac - Port" */
static void base_name(char *out, size_t n) {
    const char *b = strrchr(s_path, '/');
    b = b ? b + 1 : s_path;
    const char *dot = strrchr(b, '.');
    snprintf(out, n, "%.*s", dot ? (int)(dot - b) : (int)strlen(b), b);
}

static void save(void) {
    char base[160], path[300], msg[200];
    base_name(base, sizeof base);
    mkdir(SAVE_DIR, 0777);
    snprintf(path, sizeof path, SAVE_DIR "/%s - %s.jpg", base, s_fx >= 0 ? diffuse_fx[s_fx].name : "original");
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = W;
    cfg.height = H;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    cfg.subsampling = JPEG_SUBSAMPLE_444;   /* the dots keep their inks */
    cfg.quality = 92;
    jpeg_enc_handle_t enc = NULL;
    const int cap = 1536 * 1024;
    uint8_t *out = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    int len = 0;
    if (out && jpeg_enc_open(&cfg, &enc) == JPEG_ERR_OK &&
        jpeg_enc_process(enc, (const uint8_t *)s_dst, W * H * 2, out, cap, &len) == JPEG_ERR_OK) {
        FILE *f = fopen(path, "wb");
        bool ok = f && fwrite(out, 1, len, f) == (size_t)len;
        if (f && fclose(f)) ok = false;
        if (ok) {
            slideshow_added(path);
            snprintf(msg, sizeof msg, "Saved in \"Diffused\" (%d KB): in the slideshow and the Gallery now", len / 1024);
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
    status(msg);
}

static void run(int k) {
    const diffuse_fx_t *fx = &diffuse_fx[k];
    diffuse_t d = { .src = s_src, .dst = s_dst, .w = W, .h = H, .seed = esp_random() };
    size_t m = fx->mem_size(W, H);
    d.mem = heap_caps_calloc(1, m, MALLOC_CAP_SPIRAM);
    if (!d.mem) {
        status("Not enough memory for it");
        return;
    }
    s_fx = k;
    s_finished = false;
    s_prog = 0;
    uint32_t t0 = now_ms(), shown = 0;
    fx->start(&d);
    refresh(true);
    bool done = false;
    while (!done && !s_stop && !s_want_load && s_want == NONE) {
        done = fx->step(&d, now_ms, now_ms() + 100);
        vTaskDelay(1);   /* the idle task, LVGL: a tick is 10 ms, so slices of 100 */
        if (done || now_ms() - shown >= 250) {
            static uint32_t chip_at;
            shown = now_ms();
            char st[96];
            if (done) snprintf(st, sizeof st, "%s: done in %.1f s", fx->name, (now_ms() - t0) / 1000.0f);
            else snprintf(st, sizeof st, "%s: %d of %d %s", fx->name, d.done, d.total, d.what);
            s_prog = done ? 1000 : d.total ? d.done * 1000 / d.total : 0;
            status(st);
            if (done || shown - chip_at >= 1000) {   /* the chip's progress: once a second is plenty */
                chip_at = shown;
                chips_redraw(false);
            }
            refresh(done);
        }
        if (s_want_save) {   /* a save halfway: what's there */
            s_want_save = false;
            save();
        }
    }
    s_finished = done;
    ESP_LOGI(TAG, "%s: %s in %lu ms", fx->name, done ? "done" : "stopped", (unsigned long)(now_ms() - t0));
    free(d.mem);
}

/* the picture on show into both buffers: here, after any run has stopped
   (the slideshow is paused: its screen stays) */
static void load(void) {
    const uint16_t *scr = slideshow_screen();
    if (!scr) return;
    memcpy(s_src, scr, W * H * 2);
    memcpy(s_dst, s_src, W * H * 2);
    for (int q = 0; q < 4; q++) {   /* the quadrants' averages, a pixel in 64 */
        uint32_t r = 0, g = 0, b = 0, n = 0;
        for (int y = (q / 2) * H / 2; y < (q / 2 + 1) * H / 2; y += 8)
            for (int x = (q % 2) * W / 2; x < (q % 2 + 1) * W / 2; x += 8, n++) {
                uint16_t p = s_src[y * W + x];
                r += p >> 11;
                g += (p >> 5) & 63;
                b += p & 31;
            }
        s_pic[q] = (r * 255 / (31 * n)) << 16 | (g * 255 / (63 * n)) << 8 | b * 255 / (31 * n);
    }
    s_fx = NONE;
    s_active = NONE;
    chips_redraw(true);
    status("Pick an effect: it runs on the board, as you watch");
    refresh(true);
}

static void task(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (s_want_load) {
            s_want_load = false;
            load();
        }
        if (s_want_save) {
            s_want_save = false;
            save();
        }
        int k = s_want;
        if (k == NONE || s_stop || s_want_load) continue;
        s_want = NONE;
        run(k);
    }
}

/* ------------------------------------------------------------------ the UI */

/* ---- the chips: drawn ---- */

static void fill(lv_layer_t *layer, lv_draw_rect_dsc_t *d, uint32_t color, int x1, int y1, int x2, int y2) {
    d->bg_color = lv_color_hex(color);
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, d, &a);
}

static void chip_draw(lv_event_t *e) {
    static const uint32_t NEWS[] = { 0x1C1B1A, 0xF1ECDF }, EPAPER[] = { 0x191E21, 0xE8E8E0, 0xB2252A, 0xE7C53C, 0x2F5DA8, 0x3A7A3C },
                          GB[] = { 0x0F380F, 0x306230, 0x8BAC0F, 0x9BBC0F };
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int x1 = a.x1 + 4, x2 = a.x2 - 4, y1 = a.y1 + 4, y2 = a.y1 + 4 + SWATCH_H - 1, w = x2 - x1 + 1;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    const uint32_t *inks = NULL;
    int n = 0;
    switch (k) {
    case 0: inks = NEWS; n = 2; break;
    case 1: inks = EPAPER; n = 6; break;
    case 2: inks = GB; n = 4; break;
    case 3: inks = s_pic; n = 4; break;
    }
    if (inks) {   /* the inks side by side */
        for (int i = 0; i < n; i++) fill(layer, &d, inks[i], x1 + w * i / n, y1, x1 + w * (i + 1) / n - 1, y2);
    } else if (k == 4) {   /* 8-bit: hard squares */
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 9; c++) {
                int cx = x1 + c * w / 9, cy = y1 + r * SWATCH_H / 4;
                fill(layer, &d, s_pic[(r + c * 3) & 3], cx, cy, x1 + (c + 1) * w / 9 - 1, y1 + (r + 1) * SWATCH_H / 4 - 1);
            }
    } else if (k == 5) {   /* Melt: a wash from one colour to the next (three gradients: a rect per
                              two pixels made each progress redraw slow down the effect) */
        for (int i = 0; i < 3; i++) {
            lv_color_t c[2] = { lv_color_hex(s_pic[i]), lv_color_hex(s_pic[i + 1]) };
            lv_grad_init_stops(&d.bg_grad, c, NULL, NULL, 2);
            lv_grad_horizontal_init(&d.bg_grad);
            lv_area_t r = { x1 + w * i / 3, y1, x1 + w * (i + 1) / 3 - 1, y2 };
            lv_draw_rect(layer, &d, &r);
        }
        d.bg_grad.dir = LV_GRAD_DIR_NONE;
    } else {   /* Coral: spots grown on the dark */
        fill(layer, &d, 0x16130F, x1, y1, x2, y2);
        d.radius = LV_RADIUS_CIRCLE;
        static const int8_t SPOT[][3] = { { 12, 12, 7 }, { 34, 26, 9 }, { 58, 10, 6 }, { 74, 28, 8 }, { 22, 32, 4 }, { 48, 34, 4 }, { 84, 8, 4 } };
        for (int i = 0; i < 7; i++) {
            int cx = x1 + SPOT[i][0] * w / 92, cy = y1 + SPOT[i][1], r = SPOT[i][2];
            fill(layer, &d, s_pic[i & 3] | 0x303030, cx - r, cy - r, cx + r, cy + r);
        }
        d.radius = 0;
    }
    if (k == s_active && s_prog < 1000) {   /* its progress: a hairline under the swatch */
        fill(layer, &d, CREAM, x1, y2 + 2, x1 + (int)((int64_t)w * s_prog / 1000), y2 + 3);
    }
}

static void chip_style(lv_obj_t *b, uint32_t ground) {
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, CHIP_W, CHIP_H);
    lv_obj_set_style_bg_color(b, lv_color_hex(ground), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_clip_corner(b, true, 0);
    lv_obj_set_style_border_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_opa(b, LV_OPA_20, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(CREAM), LV_STATE_CHECKED);   /* the chosen one */
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_border_width(b, 2, LV_STATE_CHECKED);
    lv_obj_set_style_translate_y(b, -4, LV_STATE_CHECKED);   /* lifted from the tray */
    lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_PRESSED);
}

static void fx_cb(lv_event_t *e) {
    s_want = (int)(intptr_t)lv_event_get_user_data(e);
    s_active = s_want;
    s_prog = 0;
    for (int k = 0; k < diffuse_nfx && k < 8; k++) lv_obj_set_state(s_fx_btn[k], LV_STATE_CHECKED, k == s_active);
    s_stop = false;
    if (s_task) xTaskNotifyGive(s_task);
}

static void save_cb(lv_event_t *e) {
    s_want_save = true;
    lv_label_set_text(s_status, "Saving ...");
    if (s_task) xTaskNotifyGive(s_task);
}

static void set_open(bool on) {
    if (on) {
        transition_finish();   /* the whole picture, not halfway through a change */
        if (!slideshow_screen() || !slideshow_now(s_path, sizeof s_path)) return;   /* nothing on show yet */
        if (!s_src) {
            s_src = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
            s_dst = heap_caps_aligned_alloc(64, W * H * 2, MALLOC_CAP_SPIRAM);
            if (!s_src || !s_dst) {
                ESP_LOGE(TAG, "no memory");
                return;
            }
            s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            s_dsc.header.w = W;
            s_dsc.header.h = H;
            s_dsc.header.stride = W * 2;
            s_dsc.data = (const uint8_t *)s_dst;
            s_dsc.data_size = W * H * 2;
            lv_image_set_src(s_img, &s_dsc);
        }
        /* the task copies the picture in, once a run left over has stopped */
        s_want = NONE;
        s_stop = false;
        s_want_load = true;
        if (!s_task && xTaskCreatePinnedToCore(task, "diffuse", 4096, NULL, 1, &s_task, 1) != pdPASS) ESP_LOGE(TAG, "no task");
        if (s_task) xTaskNotifyGive(s_task);
        lv_label_set_text(s_status, "");
    } else {
        s_stop = true;
        s_want = NONE;
    }
    lv_obj_set_hidden(s_page, !on);
    lv_label_set_text(s_btn_label, on ? LV_SYMBOL_CLOSE : LV_SYMBOL_TINT);
    slideshow_pause(on);
    slideshow_background(!on);   /* the CPU and PSRAM are the effect's */
}

static void button_cb(lv_event_t *e) { set_open(lv_obj_is_hidden(s_page)); }

void diffuse_page_slideshow(bool shown) {
    if (!shown && !lv_obj_is_hidden(s_page)) {
        s_stop = true;
        s_want = NONE;
        lv_obj_set_hidden(s_page, true);
        lv_label_set_text(s_btn_label, LV_SYMBOL_TINT);
        slideshow_background(true);
    }
    lv_obj_set_hidden(s_btn, !shown);
}

/* a chip: the swatch drawn on top, the name under it */
static lv_obj_t *tool(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *arg) {
    lv_obj_t *b = lv_button_create(parent);
    chip_style(b, 0x1C1A18);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &art_font_16, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(CREAM), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, CHIP_W - 8, 22);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    return b;
}

void diffuse_page_create(lv_obj_t *parent) {
    s_page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_page, false);
    lv_obj_set_clickable(s_page, true);   /* taps here don't reach the slideshow */
    s_img = lv_image_create(s_page);
    lv_obj_center(s_img);

    s_status = lv_label_create(s_page);   /* a dark capsule, top middle */
    lv_obj_set_style_text_font(s_status, &art_font_16, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(CREAM), 0);
    lv_obj_set_style_bg_color(s_status, lv_color_hex(0x0C0C0C), 0);
    lv_obj_set_style_bg_opa(s_status, LV_OPA_60, 0);
    lv_obj_set_style_border_color(s_status, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_status, LV_OPA_10, 0);
    lv_obj_set_style_border_width(s_status, 1, 0);
    lv_obj_set_style_pad_hor(s_status, 16, 0);
    lv_obj_set_style_pad_ver(s_status, 8, 0);
    lv_obj_set_style_radius(s_status, 20, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 16);

    /* the effects and Save along the bottom, between the corner buttons */
    lv_obj_t *bar = s_tray = lv_obj_create(s_page);   /* the tray */
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_set_style_pad_all(bar, 8, 0);
    lv_obj_set_style_pad_top(bar, 12, 0);   /* room for a chip lifted */
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0C0C0C), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_radius(bar, 12, 0);
    lv_obj_set_style_border_color(bar, lv_color_white(), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_10, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -8);
    for (int k = 0; k < diffuse_nfx && k < (int)(sizeof s_fx_btn / sizeof s_fx_btn[0]); k++) {
        s_fx_btn[k] = tool(bar, diffuse_fx[k].name, fx_cb, (void *)(intptr_t)k);
        lv_obj_add_event_cb(s_fx_btn[k], chip_draw, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)k);
    }
    s_save = tool(bar, "SAVE", save_cb, NULL);   /* a seal-red chop */
    lv_obj_set_style_bg_color(s_save, lv_color_hex(SEAL), 0);
    lv_obj_set_style_text_letter_space(lv_obj_get_child(s_save, 0), 2, 0);
    lv_obj_t *icon = lv_label_create(s_save);
    lv_label_set_text(icon, LV_SYMBOL_SAVE);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(CREAM), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_hidden(s_page, true);

    /* the button: top right, left of Now Showing's "?" */
    s_btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(s_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(s_btn, 0, 0);
    lv_obj_set_style_pad_all(s_btn, 10, 0);
    lv_obj_set_style_min_width(s_btn, 44, 0);
    s_btn_label = lv_label_create(s_btn);
    lv_label_set_text(s_btn_label, LV_SYMBOL_TINT);
    lv_obj_set_style_text_font(s_btn_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_btn_label, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_opa(s_btn_label, LV_OPA_60, 0);
    lv_obj_center(s_btn_label);
    lv_obj_align(s_btn, LV_ALIGN_TOP_RIGHT, -92, 12);
    lv_obj_set_ext_click_area(s_btn, 12);
    lv_obj_add_event_cb(s_btn, button_cb, LV_EVENT_CLICKED, NULL);
}
