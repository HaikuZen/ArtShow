/* slideshow.c - the JPEGs on the microSD card, full screen. One task mounts the
 * card, lists the pictures, decodes each with the P4's JPEG engine (a big one
 * with esp_new_jpeg, shrunk while decoding), scales it into a screen buffer,
 * and moves to it with one of transition.c's effects. A tap on the
 * left or right third of the screen goes back or forward. The card is mounted
 * once, before Wi-Fi starts (slideshow_mount). */
#include "slideshow.h"
#include "transition.h"
#include "settings.h"
#include "index.h"
#include "board_pins.h"
#include "driver/jpeg_decode.h"
#include "driver/sdmmc_host.h"
#include "esp_jpeg_dec.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "slides";

#define MOUNT        "/sdcard"
#define SD_FREQ_KHZ  SDMMC_FREQ_DEFAULT   /* 20 MHz; Elecrow's lesson uses 10000 if a card misbehaves */
#define MAX_FILES    2000
#define MAX_DEPTH    4                    /* folders below the card's root */
#define MAX_JPEG     SLIDESHOW_MAX_JPEG
#define DEC_MAX      (8 * 1024 * 1024)    /* a decoded picture, RGB565: bigger ones decode shrunk */
#define NOTE_NEXT    1                    /* task notifications from the tap handler */
#define NOTE_PREV    2
#define NOTE_TAP     3                    /* the middle third */
#define NOTE_LIST    4                    /* the list changed: a rescan, or a picture added */
#define ALIGN_UP(v, a) (((v) + (a) - 1) / (a) * (a))

static lv_obj_t *s_view, *s_msg;
static lv_image_dsc_t s_dsc;
static uint16_t *s_out;            /* screen-sized RGB565: on show (transition.c draws into it) */
static uint16_t *s_next;           /* screen-sized RGB565: the next picture, loaded ahead */
static volatile bool s_paused;     /* another page covers the slideshow */
static volatile bool s_asleep;     /* the screen is off for the night */
static volatile bool s_want_rescan, s_list_changed;
static SemaphoreHandle_t s_list_lock;   /* s_files' strings, against readers in other tasks */
static QueueHandle_t s_added;      /* paths put on the card meanwhile (Artwork of the Day) */
#define MAX_BAD 64
static const char *s_bad[MAX_BAD];  /* can't be shown: left out this session (and in the index) */
static int s_nbad;
static const char *s_why;          /* why the last load() failed, if it's the picture's fault */
static volatile int s_cur = -1;    /* the picture on show, in s_files */
static TaskHandle_t s_task;
static sdmmc_card_t *s_card;
static jpeg_decoder_handle_t s_jpeg;
static char **s_files;
static int s_nfiles;
static uint8_t *s_in;              /* the file, as read */
static size_t s_in_cap;
static uint16_t *s_dec;            /* the decoded picture, full size */
static size_t s_dec_cap;
static uint32_t s_acc[LCD_H_RES * 3];   /* the scaler's column sums: r, g, b */
static int s_xb[LCD_H_RES + 1];         /* the scaler's column bounds in the source */

/* ---- the card ---- */

/* a reset of the board (not of the card, whose power stays on) in the middle of
 * a read leaves the card sending data: it holds D0, the controller's first
 * clock update waits on it for ever, and the whole SDMMC host jams, esp_hosted's
 * slot for the C6 included. So first, by hand: clocks to end a block, then CMD0
 * (go idle), which a card takes in any state */
static void sd_unstick(void) {
    const gpio_config_t out = { .pin_bit_mask = BIT64(PIN_SD_CLK) | BIT64(PIN_SD_CMD), .mode = GPIO_MODE_OUTPUT };
    const gpio_config_t in = { .pin_bit_mask = BIT64(PIN_SD_D0), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&out);
    gpio_config(&in);
    gpio_set_level(PIN_SD_CMD, 1);
    gpio_set_level(PIN_SD_CLK, 0);
    int d0 = gpio_get_level(PIN_SD_D0);
    /* CMD0: start, host, index 0, argument 0, CRC7 0x4a, end bit */
    static const uint8_t cmd0[6] = { 0x40, 0x00, 0x00, 0x00, 0x00, 0x95 };
    for (int i = 0; i < 2000 + 48 + 80; i++) {   /* ~200 kHz */
        if (i >= 2000 && i < 2000 + 48) {
            int bit = i - 2000;
            gpio_set_level(PIN_SD_CMD, (cmd0[bit / 8] >> (7 - bit % 8)) & 1);
        } else {
            gpio_set_level(PIN_SD_CMD, 1);
        }
        esp_rom_delay_us(2);
        gpio_set_level(PIN_SD_CLK, 1);   /* the card samples CMD on the rising edge */
        esp_rom_delay_us(2);
        gpio_set_level(PIN_SD_CLK, 0);
    }
    ESP_LOGI(TAG, "card D0 %s, after CMD0 %s", d0 ? "idle" : "BUSY", gpio_get_level(PIN_SD_D0) ? "idle" : "busy");
    gpio_reset_pin(PIN_SD_CLK);
    gpio_reset_pin(PIN_SD_CMD);
    gpio_reset_pin(PIN_SD_D0);
}

bool slideshow_mount(void) {
    sd_unstick();
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    /* slot 0, by hand: on the P4 the default is slot 1, which esp_hosted holds for
       the C6. A mount that fails on slot 1 removes that slot, and Wi-Fi then asserts */
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SD_FREQ_KHZ;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;  /* 40..42 are the touch and LCD lines */
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,         /* never wipe someone's pictures */
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mc, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGW(TAG, "no microSD card (%s)", esp_err_to_name(err));
        return false;
    }
    sdmmc_card_print_info(stdout, s_card);
    return true;
}

/* ---- the picture list ---- */

/* s_files holds MAX_FILES pointers from the start: it never moves, readers
   in other tasks only take the lock against the strings being freed */
static void free_list(void) {
    for (int i = 0; i < s_nfiles; i++) free(s_files[i]);
    s_nfiles = 0;
    s_nbad = 0;
}

static void add_file(const char *path) {
    if (s_nfiles >= MAX_FILES) return;
    char *p = heap_caps_malloc(strlen(path) + 1, MALLOC_CAP_SPIRAM);
    if (!p) return;
    strcpy(p, path);
    s_files[s_nfiles++] = p;
}

static int by_name(const void *a, const void *b) {
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void shuffle(void) {
    for (int i = s_nfiles - 1; i > 0; i--) {
        int j = esp_random() % (i + 1);
        char *t = s_files[i]; s_files[i] = s_files[j]; s_files[j] = t;
    }
}

/* the order the Settings page asks for: a new shuffle, or names */
static bool s_shuffled;
static void reorder(void) {
    s_shuffled = g_settings.shuffle;
    if (s_shuffled) shuffle();
    else qsort(s_files, s_nfiles, sizeof *s_files, by_name);
}

/* the pictures: from the index (walking the card into it if it's empty),
   or, without one, from a walk of the card */
static void list_pictures(bool rescan) {
    xSemaphoreTake(s_list_lock, portMAX_DELAY);
    char shown[256] = "";   /* the picture on show: found again in the new list */
    if (s_cur >= 0 && s_cur < s_nfiles) snprintf(shown, sizeof shown, "%s", s_files[s_cur]);
    s_cur = -1;
    free_list();
    if (!s_files) s_files = heap_caps_calloc(MAX_FILES, sizeof *s_files, MALLOC_CAP_SPIRAM);
    if (s_files && index_is_open()) {
        if (rescan) index_scan();
        s_nfiles = index_list(s_files, MAX_FILES);
        if (!s_nfiles && !rescan && index_scan() > 0) s_nfiles = index_list(s_files, MAX_FILES);
    } else if (s_files) {
        index_walk(add_file);
    }
    reorder();
    for (int k = 0; shown[0] && k < s_nfiles; k++)
        if (!strcmp(s_files[k], shown)) s_cur = k;
    xSemaphoreGive(s_list_lock);
    ESP_LOGI(TAG, "%d pictures on the card%s", s_nfiles, s_nfiles >= MAX_FILES ? " (the most it lists)" : "");
}

/* ---- decoding and scaling ---- */

/* the whole file into s_in; false when it can't be read or is too big */
static bool read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "%s: can't open", path); return false; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = size > 0 && size <= MAX_JPEG;
    if (!ok) ESP_LOGW(TAG, "%s: %ld bytes, skipped", path, size);
    if (!ok) s_why = size > 0 ? "too big" : "empty";
    if (ok && (size_t)size > s_in_cap) {
        free(s_in);
        const jpeg_decode_memory_alloc_cfg_t mc = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
        s_in = jpeg_alloc_decoder_mem(size, &mc, &s_in_cap);
        if (!s_in) { s_in_cap = 0; ok = false; ESP_LOGW(TAG, "%s: no memory for %ld bytes", path, size); }
    }
    if (ok && fread(s_in, 1, size, f) != (size_t)size) { ok = false; ESP_LOGW(TAG, "%s: read failed", path); }
    fclose(f);
    *len = size;
    return ok;
}

/* where a w x h picture goes: the source rect (sx, sy, sw, sh) shown, and the
 * size (dw, dh) it takes, centered: whole with black bars, or cropped to fill
 * the screen (the Settings page's fill) */
typedef struct { int sx, sy, sw, sh, dw, dh; } place_t;

static place_t place(int w, int h) {
    place_t p = { .sw = w, .sh = h };
    bool wider = (int64_t)w * LCD_V_RES > (int64_t)h * LCD_H_RES;   /* than the screen */
    if (g_settings.fill) {
        if (wider) { p.sw = (int)((int64_t)h * LCD_H_RES / LCD_V_RES); p.sx = (w - p.sw) / 2; }
        else       { p.sh = (int)((int64_t)w * LCD_V_RES / LCD_H_RES); p.sy = (h - p.sh) / 2; }
        p.dw = LCD_H_RES;
        p.dh = LCD_V_RES;
    } else {
        if (wider) { p.dw = LCD_H_RES; p.dh = (int)((int64_t)h * LCD_H_RES / w); }
        else       { p.dh = LCD_V_RES; p.dw = (int)((int64_t)w * LCD_V_RES / h); }
    }
    if (p.sw < 1) p.sw = 1;
    if (p.sh < 1) p.sh = 1;
    if (p.dw < 1) p.dw = 1;
    if (p.dh < 1) p.dh = 1;
    return p;
}

/* s_dec at least need bytes (aligned for the JPEG engine's DMA) */
static bool dec_buf(size_t need) {
    if (need <= s_dec_cap) return true;
    free(s_dec);
    const jpeg_decode_memory_alloc_cfg_t mc = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    s_dec = jpeg_alloc_decoder_mem(need, &mc, &s_dec_cap);
    if (!s_dec) s_dec_cap = 0;
    return s_dec != NULL;
}

/* the JPEG engine: full size, fast. Into s_dec, with its row length */
static bool decode_hw(const char *path, size_t len, const jpeg_decode_picture_info_t *info, int *stride) {
    if (info->width % 8 || info->height % 8) return false;   /* the engine refuses those (logging errors) */
    /* the engine writes whole MCUs: 16 px wide when chroma is subsampled, 16 tall for 4:2:0 */
    int mx = info->sample_method == JPEG_DOWN_SAMPLING_YUV444 || info->sample_method == JPEG_DOWN_SAMPLING_GRAY ? 8 : 16;
    int my = info->sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8;
    int pw = ALIGN_UP((int)info->width, mx), ph = ALIGN_UP((int)info->height, my);
    if ((size_t)pw * ph * 2 > DEC_MAX || !dec_buf((size_t)pw * ph * 2)) return false;
    const jpeg_decode_cfg_t dc = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,   /* little-endian RGB565, as LVGL wants it */
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out = 0;
    esp_err_t err = jpeg_decoder_process(s_jpeg, &dc, s_in, len, (uint8_t *)s_dec, s_dec_cap, &out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: the JPEG engine can't (%s)", path, esp_err_to_name(err));
        return false;
    }
    *stride = pw;
    return true;
}

/* esp_new_jpeg, in software: shrunk 2^shift times while decoding, so a big
 * picture never needs its full size in memory. Into s_dec, *w x *h */
static bool decode_sw(const char *path, size_t len, int shift, int *w, int *h) {
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    if (shift) {   /* the scaler wants multiples of 8, and no less than 1/8: rounded up */
        *w = ALIGN_UP((*w + (1 << shift) - 1) >> shift, 8);
        *h = ALIGN_UP((*h + (1 << shift) - 1) >> shift, 8);
        if (*w < 8 || *h < 8) return false;
        cfg.scale.width = *w;
        cfg.scale.height = *h;
    }
    jpeg_dec_handle_t dec;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) return false;
    jpeg_dec_io_t io = { .inbuf = s_in, .inbuf_len = len };
    jpeg_dec_header_info_t hi;
    int olen = 0;
    jpeg_error_t err = jpeg_dec_parse_header(dec, &io, &hi);
    if (err == JPEG_ERR_OK) err = jpeg_dec_get_outbuf_len(dec, &olen);
    bool ok = err == JPEG_ERR_OK && olen >= *w * *h * 2 && (size_t)olen <= DEC_MAX && dec_buf(olen);
    if (ok) {
        io.outbuf = (uint8_t *)s_dec;
        err = jpeg_dec_process(dec, &io);
        ok = err == JPEG_ERR_OK;
    }
    jpeg_dec_close(dec);
    if (!ok) ESP_LOGW(TAG, "%s: the software decoder can't (%s)", path,
                      err == JPEG_ERR_NO_MORE_DATA || err == JPEG_ERR_BAD_DATA ? "damaged or cut short" :
                      err == JPEG_ERR_UNSUPPORT_FMT || err == JPEG_ERR_UNSUPPORT_STD ? "progressive, or another unsupported kind" :
                      err == JPEG_ERR_NO_MEM ? "no memory" : "error");
    return ok;
}

/* box filter: the source rect (sx, sy, sw, sh) of src onto the dest rect
 * (dx, dy, dw, dh) of a screen buffer. Each dest pixel averages the source
 * pixels under it (or repeats the nearest one, scaling up) */
static void scale(const uint16_t *src, int stride, int sx, int sy, int sw, int sh,
                  uint16_t *dst, int dx, int dy, int dw, int dh) {
    for (int x = 0; x <= dw; x++) s_xb[x] = sx + (int)((int64_t)x * sw / dw);
    for (int y = 0; y < dh; y++) {
        int y0 = sy + (int)((int64_t)y * sh / dh), y1 = sy + (int)((int64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        memset(s_acc, 0, dw * 3 * sizeof *s_acc);
        for (int r = y0; r < y1; r++) {
            const uint16_t *row = src + (size_t)r * stride;
            uint32_t *a = s_acc;
            for (int x = 0; x < dw; x++, a += 3) {
                int x0 = s_xb[x], x1 = s_xb[x + 1] > x0 ? s_xb[x + 1] : x0 + 1;
                for (int c = x0; c < x1; c++) {
                    uint16_t p = row[c];
                    a[0] += p >> 11;
                    a[1] += (p >> 5) & 0x3f;
                    a[2] += p & 0x1f;
                }
            }
        }
        uint16_t *out = dst + (size_t)(dy + y) * LCD_H_RES + dx;
        const uint32_t *a = s_acc;
        for (int x = 0; x < dw; x++, a += 3) {
            int x0 = s_xb[x], x1 = s_xb[x + 1] > x0 ? s_xb[x + 1] : x0 + 1;
            uint32_t n = (uint32_t)(x1 - x0) * (y1 - y0);
            out[x] = (uint16_t)((a[0] / n) << 11 | (a[1] / n) << 5 | a[2] / n);
        }
    }
}

/* a picture file into the screen buffer dst */
static bool load(const char *path, uint16_t *dst) {
    int64_t t0 = esp_timer_get_time();
    size_t len;
    s_why = NULL;   /* a failure is the card's, unless said otherwise */
    if (!read_file(path, &len)) return false;
    int64_t t1 = esp_timer_get_time();

    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(s_in, len, &info) != ESP_OK) {
        ESP_LOGW(TAG, "%s: not a JPEG", path);
        s_why = "not a JPEG";
        return false;
    }
    int w = info.width, h = info.height, stride = 0;
    /* how far it can shrink while decoding (1/2, 1/4, 1/8) and still cover its place */
    place_t p = place(w, h);
    int shift = 0;
    while (shift < 3 && (p.sw >> (shift + 1)) >= p.dw && (p.sh >> (shift + 1)) >= p.dh) shift++;
    while (shift < 3 && (size_t)(w >> shift) * (h >> shift) * 2 > DEC_MAX) shift++;

    /* the engine when it fits in DEC_MAX at full size, else (or when it fails) software */
    const char *how = "engine";
    if (!decode_hw(path, len, &info, &stride)) {
        how = "software";
        if (!decode_sw(path, len, shift, &w, &h)) {
            ESP_LOGW(TAG, "%s: %"PRIu32"x%"PRIu32" not shown", path, info.width, info.height);
            s_why = "can't be decoded (progressive, or damaged)";
            return false;
        }
        stride = w;
    }
    int64_t t2 = esp_timer_get_time();

    p = place(w, h);
    memset(dst, 0, LCD_H_RES * LCD_V_RES * sizeof *dst);
    scale(s_dec, stride, p.sx, p.sy, p.sw, p.sh, dst, (LCD_H_RES - p.dw) / 2, (LCD_V_RES - p.dh) / 2, p.dw, p.dh);
    ESP_LOGI(TAG, "%s: %"PRIu32"x%"PRIu32" (%s %dx%d), read %d ms, decode %d ms, scale %d ms", path,
             info.width, info.height, how, w, h, (int)((t1 - t0) / 1000), (int)((t2 - t1) / 1000),
             (int)((esp_timer_get_time() - t2) / 1000));
    index_good(path, info.width, info.height);
    return true;
}

/* ---- the screen ---- */

/* end a running transition, so s_next is free to write */
static void settle(void) {
    if (!lvgl_port_lock(0)) return;
    transition_finish();
    lvgl_port_unlock();
}

/* from the picture on show to s_next (just filled) */
static void show(void) {
    if (!lvgl_port_lock(0)) return;
    lv_obj_set_hidden(s_msg, true);
    transition_start(s_next);
    lvgl_port_unlock();
}

static void message(const char *txt) {
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text(s_msg, txt);
    lv_obj_set_hidden(s_msg, false);
    lvgl_port_unlock();
}

/* in the LVGL task: the screen's left third goes back, the right third forward */
static void tap_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !s_task) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    uint32_t note = p.x < LCD_H_RES / 3 ? NOTE_PREV : p.x >= LCD_H_RES * 2 / 3 ? NOTE_NEXT : NOTE_TAP;
    xTaskNotify(s_task, note, eSetValueWithOverwrite);
}

/* ---- the task ---- */

/* a tap within ms (NOTE_*), or 0. Taps in the middle count only with any */
/* the requests from other tasks, done here: the index is this task's */
static bool serve_requests(void) {
    bool changed = false;
    char path[256];
    while (s_added && xQueueReceive(s_added, path, 0) == pdTRUE) {   /* Artwork of the Day's saves */
        index_add(path);
        xSemaphoreTake(s_list_lock, portMAX_DELAY);
        add_file(path);
        xSemaphoreGive(s_list_lock);
        ESP_LOGI(TAG, "%s: added", path);
    }
    if (s_want_rescan) {
        s_want_rescan = false;
        list_pictures(true);
        changed = true;
    }
    if (changed) s_list_changed = true;
    return changed;
}

static int wait_note(int ms, bool any) {
    int64_t end = esp_timer_get_time() + ms * 1000LL;
    for (;;) {
        /* a request whose note a tap overwrote: served all the same */
        if ((s_want_rescan || (s_added && uxQueueMessagesWaiting(s_added))) && serve_requests()) return NOTE_LIST;
        int left = (int)((end - esp_timer_get_time()) / 1000);
        uint32_t note = 0;
        if (left <= 0 || xTaskNotifyWait(0, UINT32_MAX, &note, pdMS_TO_TICKS(left)) != pdTRUE) return 0;
        if (note == NOTE_LIST) {
            if (serve_requests()) return NOTE_LIST;   /* a new list: the caller starts over */
            continue;
        }
        if (any || note != NOTE_TAP) return (int)note;
    }
}

/* the index after k going dir; going forward past the last, a new order */
/* "/sdcard/DailyArt Pictures/x.jpg" -> "DailyArt Pictures"; "" at the top */
static void top_folder(const char *path, char *out, size_t n) {
    const char *p = path + strlen(MOUNT) + 1, *slash = strchr(p, '/');
    snprintf(out, n, "%.*s", slash ? (int)(slash - p) : 0, p);
}

static bool playable(int k) {
    for (int b = 0; b < s_nbad; b++)
        if (s_bad[b] == s_files[k]) return false;
    char f[128];
    top_folder(s_files[k], f, sizeof f);
    return settings_folder_on(f);
}

/* the next picture in a folder that's on (any, if they're all off); the
   order changes when the Settings page's shuffle does, else each round */
static int step(int k, int dir) {
    if (s_shuffled != g_settings.shuffle) {
        reorder();
        k = dir > 0 ? -1 : 0;   /* from the start of the new order */
    }
    for (int n = 0; n < s_nfiles; n++) {
        k = (k + dir + s_nfiles) % s_nfiles;
        if (k == 0 && dir > 0 && s_shuffled) shuffle();
        if (playable(k)) return k;
    }
    return (k + dir + s_nfiles) % s_nfiles;
}

/* picture *k into the hidden buffer, passing over those that fail in direction
 * dir (*k ends on the one loaded). false: none of them loads */
static bool prepare(int *k, int dir) {
    settle();
    for (int fails = 0; fails < s_nfiles; fails++) {
        if (load(s_files[*k], s_next)) return true;
        if (s_why) {   /* the picture's fault: out of the index's list, and out this session */
            index_bad(s_files[*k], s_why);
            if (s_nbad < MAX_BAD) s_bad[s_nbad++] = s_files[*k];
        }
        *k = step(*k, dir);
    }
    return false;
}

/* nothing to show: say why, and restart on a tap (mounting again means a card
 * init while Wi-Fi runs, which breaks esp_hosted's link: see slideshow.h) */
static void stop(const char *why) {
    message(why);
    while (!wait_note(60 * 1000, true)) {}
    esp_restart();
}

static void slideshow_task(void *arg) {
    const jpeg_decode_engine_cfg_t ec = { .timeout_ms = 2000 };
    if (jpeg_new_decoder_engine(&ec, &s_jpeg) != ESP_OK) {
        message("The JPEG decoder failed to start");
        vTaskDelete(NULL);
    }
    if (!s_card) stop(LV_SYMBOL_SD_CARD "  Insert a microSD card with JPEG pictures,\nthen tap the screen");
    index_open();   /* this task's: SQLite is built for one */
    list_pictures(false);

    int next = 0, dir = 1;
    bool ready = false;   /* picture next is in the hidden buffer already */
    s_list_changed = true;
    for (;;) {
        if (s_list_changed) {   /* a new list (boot, a rescan): from its first picture in a folder that's on */
            s_list_changed = false;
            if (!s_nfiles) stop(LV_SYMBOL_IMAGE "  No JPEG pictures on the card (.jpg or .jpeg, in any folder)\n"
                                "Put some on it, then tap the screen");
            next = step(s_nfiles - 1, 1);
            dir = 1;
            ready = false;
        }
        if (!ready && !prepare(&next, dir))   /* none loads: the card is gone, or all unreadable */
            stop(LV_SYMBOL_WARNING "  None of the pictures can be shown (baseline JPEGs only)\n"
                 "Check the card, then tap the screen");
        while (s_paused || s_asleep) wait_note(500, false);
        if (s_list_changed) continue;   /* rescanned meanwhile: what's ready may be gone */
        show();
        int cur = s_cur = next;
        int64_t due = esp_timer_get_time() + g_settings.interval_s * 1000000LL;

        /* the countdown: once the transition is done with s_next, the
           next picture loads into it, and shows when the time is up */
        ready = false;
        int note = wait_note(g_settings.fade_ms + 100, false);
        if (!note) {
            next = step(cur, 1);
            ready = prepare(&next, 1);   /* false: the loop's prepare tries again, and reports */
            note = wait_note((int)((due - esp_timer_get_time()) / 1000), false);
        }
        if (note == NOTE_PREV) {         /* back: the preloaded one isn't it */
            next = step(cur, -1);
            dir = -1;
            ready = false;
        } else {                         /* time's up, or forward: the preloaded one, if any */
            if (!ready) next = step(cur, 1);
            dir = 1;
        }
    }
}

void slideshow_pause(bool pause) { s_paused = pause; }

void slideshow_start(lv_obj_t *parent) {
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(parent, false);

    /* black; cache-line aligned: LVGL hands it to the PPA to draw */
    s_out = heap_caps_aligned_calloc(64, LCD_H_RES * LCD_V_RES, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_next = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_out || !s_next) { ESP_LOGE(TAG, "no memory for the screen buffers"); return; }
    s_dsc = (lv_image_dsc_t){
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RGB565,
            .w = LCD_H_RES,
            .h = LCD_V_RES,
            .stride = LCD_H_RES * sizeof(uint16_t),
        },
        .data_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
        .data = (const uint8_t *)s_out,
    };
    /* its pixels change in place: nothing to reload, LVGL's image cache is off (LV_CACHE_DEF_SIZE 0) */
    s_view = lv_image_create(parent);
    lv_image_set_src(s_view, &s_dsc);
    lv_obj_set_pos(s_view, 0, 0);
    if (!transition_init(s_view, s_out)) return;

    s_msg = lv_label_create(parent);
    lv_label_set_text(s_msg, "Looking for pictures...");
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_msg, LCD_H_RES - 120);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_msg, lv_color_hex(0xE0F0FF), 0);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_24, 0);
    lv_obj_center(s_msg);

    /* the images aren't clickable: their taps reach the screen */
    lv_obj_add_event_cb(parent, tap_cb, LV_EVENT_CLICKED, NULL);
    /* idle priority: a software decode runs ~6 s without a break, and at any
       higher one core 1's idle task starves and the task watchdog complains */
    s_list_lock = xSemaphoreCreateMutex();
    s_added = xQueueCreate(4, 256);
    /* 16 KB: SQLite's calls are deeper than the decoders' */
    xTaskCreatePinnedToCore(slideshow_task, "slides", 16 * 1024, NULL, tskIDLE_PRIORITY, &s_task, 1);
}

bool slideshow_now(char *path, size_t n) {
    xSemaphoreTake(s_list_lock, portMAX_DELAY);
    int k = s_cur;
    snprintf(path, n, "%s", k >= 0 && k < s_nfiles ? s_files[k] : "");
    xSemaphoreGive(s_list_lock);
    return path[0];
}

void slideshow_rescan(void) {
    s_want_rescan = true;
    if (s_task) xTaskNotify(s_task, NOTE_LIST, eSetValueWithOverwrite);
}

void slideshow_added(const char *path) {
    char p[256];
    snprintf(p, sizeof p, "%s", path);
    if (s_added && xQueueSend(s_added, p, 0) == pdTRUE && s_task) xTaskNotify(s_task, NOTE_LIST, eSetValueWithOverwrite);
}

const uint16_t *slideshow_screen(void) { return s_out; }

int slideshow_folders(char names[][64], int *counts, int max) {
    int n = 0;
    xSemaphoreTake(s_list_lock, portMAX_DELAY);
    for (int k = 0; k < s_nfiles; k++) {
        char f[64];
        top_folder(s_files[k], f, sizeof f);
        int i = 0;
        while (i < n && strcmp(names[i], f)) i++;
        if (i == n) {
            if (n == max) continue;
            snprintf(names[n], 64, "%s", f);
            counts[n++] = 0;
        }
        counts[i]++;
    }
    xSemaphoreGive(s_list_lock);
    return n;
}

int slideshow_count(void) { return s_nfiles; }

void slideshow_sleep(bool sleep) { s_asleep = sleep; }
