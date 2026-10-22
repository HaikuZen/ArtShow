/* diffuse_page.c - the view. The picture on show is copied once at the
 * open (slideshow_screen(), after transition_finish(): the whole one), and
 * the effect paints a second buffer, the view's lv_image (64-byte aligned
 * for the PPA). A task on core 1 at priority 1, like the Generative page's
 * painter: 100 ms of work, then a tick back (10 ms); the view redrawn at most 4 times
 * a second. A tap on another effect restarts from the picture. The two
 * pictures (2.4 MB of PSRAM) stay once opened; an effect's scratch (up to
 * 3.7 MB) only while it runs */
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define W        1024
#define H        600
#define SAVE_DIR "/sdcard/Diffused"
#define NONE     -1

static const char *TAG = "diffuse";

static lv_obj_t *s_page, *s_img, *s_btn, *s_btn_label, *s_status, *s_save;
static lv_obj_t *s_fx_btn[8];
static lv_image_dsc_t s_dsc;
static uint16_t *s_src, *s_dst;
static char s_path[256];             /* the picture's file, for the saved one's name */
static TaskHandle_t s_task;
static volatile int s_want = NONE;   /* an effect to run (the button), taken by the task */
static volatile bool s_stop, s_want_save, s_want_load;
static int s_fx = NONE;              /* the one run last (the task's) */
static bool s_finished;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void status(const char *txt) {
    lvgl_port_lock(0);
    lv_label_set_text(s_status, txt);
    lvgl_port_unlock();
}

static void refresh(void) {
    lvgl_port_lock(0);
    lv_obj_invalidate(s_img);
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
    uint32_t t0 = now_ms(), shown = 0;
    fx->start(&d);
    refresh();
    bool done = false;
    while (!done && !s_stop && !s_want_load && s_want == NONE) {
        done = fx->step(&d, now_ms, now_ms() + 100);
        vTaskDelay(1);   /* the idle task, LVGL: a tick is 10 ms, so slices of 100 */
        if (done || now_ms() - shown >= 250) {
            shown = now_ms();
            char st[96];
            if (done) snprintf(st, sizeof st, "%s: done in %.1f s", fx->name, (now_ms() - t0) / 1000.0f);
            else snprintf(st, sizeof st, "%s: %d of %d %s", fx->name, d.done, d.total, d.what);
            status(st);
            refresh();
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
    s_fx = NONE;
    status("Pick an effect: it runs on the board, as you watch");
    refresh();
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

static void fx_cb(lv_event_t *e) {
    s_want = (int)(intptr_t)lv_event_get_user_data(e);
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

static lv_obj_t *tool(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *arg) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_pad_ver(b, 10, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE0E0E0), 0);
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

    s_status = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_bg_color(s_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_status, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(s_status, 10, 0);
    lv_obj_set_style_pad_ver(s_status, 6, 0);
    lv_obj_set_style_radius(s_status, 6, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 18);

    /* the effects and Save along the bottom, between the corner buttons */
    lv_obj_t *bar = lv_obj_create(s_page);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 860, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_style_pad_row(bar, 8, 0);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -14);
    for (int k = 0; k < diffuse_nfx && k < (int)(sizeof s_fx_btn / sizeof s_fx_btn[0]); k++)
        s_fx_btn[k] = tool(bar, diffuse_fx[k].name, fx_cb, (void *)(intptr_t)k);
    s_save = tool(bar, LV_SYMBOL_SAVE "  Save", save_cb, NULL);
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
