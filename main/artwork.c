/* artwork.c - a painting a day from the Met: its collection API's search
 * (highlighted paintings with images, ~420) picks one by the date; its object
 * record gives the text and a ~600 px JPEG; Wikidata gives its English
 * Wikipedia page, whose summary is the "about". The JPEG is decoded in
 * software and fitted into ART_W x ART_H. */
#include "artwork.h"
#include "net.h"
#include "wifi.h"
#include "time_sync.h"
#include "slideshow.h"
#include "esp_jpeg_dec.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define MET         "https://collectionapi.metmuseum.org/public/collection"
#define SEARCH      MET "/v1.1/search?isHighlight=true&hasImages=true&medium=Paintings&q=*&limit=1&offset="
#define SAVE_DIR    "/sdcard/Artwork of the Day"
#define STRIDE      ((ART_W * 2 + 63) & ~63)   /* bytes a row: 64-byte multiples, the PPA's cache line */
#define TRIES       8                          /* not public domain, or no image: another, STEP on */
#define STEP        97                         /* far on: the hits run by department, modern ones together */
#define ALIGN_UP(x, a) (((x) + (a) - 1) / (a) * (a))

static const char *TAG = "artwork";

static artwork_cb_t s_cb;
static TaskHandle_t s_task;
static volatile bool s_want_random, s_want_save;
static artwork_t s_art;
static uint16_t *s_px[2];          /* the picture, two buffers: one on show, one to fill */
static int s_cur;                  /* the one on show */
static uint8_t *s_jpeg;            /* the file of the one on show (~600 px), if there's no bigger one */
static size_t s_jpeg_len;
static char s_full[200];           /* its full-size picture (1.5-4 MB, up to ~4000 px): what Save keeps */
static uint32_t s_acc[ART_W * 3];  /* the scaler's column sums */
static int s_xb[ART_W + 1];

static void copy(char *dst, size_t n, const char *s) { snprintf(dst, n, "%s", s ? s : ""); }

/* ---- the picture ---- */

/* box filter (nearest when scaling up): src w x h into dst dw x dh */
static void fit(const uint16_t *src, int w, int h, uint16_t *dst, int dw, int dh) {
    for (int x = 0; x <= dw; x++) s_xb[x] = (int)((int64_t)x * w / dw);
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * h / dh), y1 = (int)((int64_t)(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        memset(s_acc, 0, dw * 3 * sizeof *s_acc);
        for (int r = y0; r < y1; r++) {
            const uint16_t *row = src + (size_t)r * w;
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
        uint16_t *out = (uint16_t *)((uint8_t *)dst + (size_t)y * STRIDE);
        const uint32_t *a = s_acc;
        for (int x = 0; x < dw; x++, a += 3) {
            int x0 = s_xb[x], x1 = s_xb[x + 1] > x0 ? s_xb[x + 1] : x0 + 1;
            uint32_t n = (uint32_t)(x1 - x0) * (y1 - y0);
            out[x] = (uint16_t)((a[0] / n) << 11 | (a[1] / n) << 5 | a[2] / n);
        }
    }
}

/* the JPEG into dst, fitted to the box; *w, *h: its size there */
static bool decode(const uint8_t *jpg, size_t len, uint16_t *dst, int *w, int *h, char *err, size_t n) {
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        snprintf(err, n, "The picture: no decoder");
        return false;
    }
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpg, .inbuf_len = len };
    jpeg_dec_header_info_t hi;
    jpeg_error_t e = jpeg_dec_parse_header(dec, &io, &hi);
    int sw = hi.width, sh = hi.height, shift = 0;
    /* a big one shrinks while decoding (to multiples of 8, as the decoder wants) */
    while (e == JPEG_ERR_OK && shift < 3 && (sw >> shift) > 2 * ART_W && (sh >> shift) > 2 * ART_H) shift++;
    if (e == JPEG_ERR_OK && shift) {
        jpeg_dec_close(dec);
        cfg.scale.width = sw = ALIGN_UP((hi.width + (1 << shift) - 1) >> shift, 8);
        cfg.scale.height = sh = ALIGN_UP((hi.height + (1 << shift) - 1) >> shift, 8);
        io = (jpeg_dec_io_t){ .inbuf = (uint8_t *)jpg, .inbuf_len = len };
        if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
            snprintf(err, n, "The picture: no decoder");
            return false;
        }
        e = jpeg_dec_parse_header(dec, &io, &hi);
    }
    int olen = 0;
    if (e == JPEG_ERR_OK) e = jpeg_dec_get_outbuf_len(dec, &olen);
    uint16_t *full = e == JPEG_ERR_OK && olen <= 16 * 1024 * 1024
                         ? heap_caps_aligned_calloc(16, 1, olen, MALLOC_CAP_SPIRAM) : NULL;
    if (full) {
        io.outbuf = (uint8_t *)full;
        e = jpeg_dec_process(dec, &io);
    }
    jpeg_dec_close(dec);
    if (!full || e != JPEG_ERR_OK) {
        snprintf(err, n, "The picture can't be decoded (%s)",
                 !full ? "too big" : e == JPEG_ERR_UNSUPPORT_FMT || e == JPEG_ERR_UNSUPPORT_STD ? "progressive?" : "damaged?");
        free(full);
        return false;
    }
    /* the largest size that fits the box, the same shape */
    if ((int64_t)sw * ART_H > (int64_t)sh * ART_W) {
        *w = ART_W;
        *h = (int)((int64_t)sh * ART_W / sw);
    } else {
        *h = ART_H;
        *w = (int)((int64_t)sw * ART_H / sh);
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    fit(full, sw, sh, dst, *w, *h);
    free(full);
    return true;
}

/* ---- the Met, Wikidata, Wikipedia ---- */

static int s_total;   /* the search's hits */

/* the search's idx-th hit: an object ID, or 0 */
static int search(int idx, char *err, size_t n) {
    char url[200];
    snprintf(url, sizeof url, SEARCH "%d", idx);
    cJSON *j = net_get_json(url, "The Met", err, n);
    if (!j) return 0;
    s_total = json_num(j, "total", 0);
    int id = json_num(j, "objectIDs.0", 0);
    cJSON_Delete(j);
    if (!id && !err[0]) snprintf(err, n, "The Met: nothing found");
    return id;
}

/* the summary of a Wikidata item's English Wikipedia page into s_art.about;
   false if it has none */
static bool wikipedia(const char *wikidata_url) {
    char err[128] = "", url[400], enc[300];
    const char *q = wikidata_url ? strrchr(wikidata_url, '/') : NULL;   /* ".../wiki/Q867510" */
    if (!q || q[1] != 'Q') return false;
    q++;
    snprintf(url, sizeof url,
             "https://www.wikidata.org/w/api.php?action=wbgetentities&props=sitelinks&sitefilter=enwiki&format=json&ids=%s", q);
    cJSON *j = net_get_json(url, "Wikidata", err, sizeof err);
    const cJSON *ent = j ? cJSON_GetObjectItemCaseSensitive(json_at(j, "entities"), q) : NULL;
    const char *title = json_str(ent, "sitelinks.enwiki.title");
    if (title) {
        char t[200];
        copy(t, sizeof t, title);
        for (char *p = t; *p; p++) if (*p == ' ') *p = '_';
        url_enc(enc, sizeof enc, t, NULL);
        snprintf(url, sizeof url, "https://en.wikipedia.org/api/rest_v1/page/summary/%s", enc);
    }
    cJSON_Delete(j);
    if (!title) {
        if (err[0]) ESP_LOGW(TAG, "%s", err);
        return false;
    }
    if (!(j = net_get_json(url, "Wikipedia", err, sizeof err))) {
        ESP_LOGW(TAG, "%s", err);
        return false;
    }
    copy(s_art.about, sizeof s_art.about, json_str(j, "extract"));
    cJSON_Delete(j);
    return s_art.about[0];
}

/* the idx-th hit (or, without a free picture, one STEP on, and on) into the buffer
   not on show; then it's the one on show */
static bool fetch(int idx, char *err, size_t n) {
    cJSON *obj = NULL;
    const char *img = NULL;
    for (int t = 0; t < TRIES && !img; t++) {
        int id = search((idx + t * STEP) % s_total, err, n);
        if (!id) return false;
        char url[120];
        snprintf(url, sizeof url, MET "/v1/objects/%d", id);
        cJSON_Delete(obj);
        if (!(obj = net_get_json(url, "The Met", err, n))) return false;
        img = json_str(obj, "primaryImageSmall");
        if (!cJSON_IsTrue(json_at(obj, "isPublicDomain")) || !img || !*img) img = NULL;
    }
    if (!img) {
        snprintf(err, n, "The Met: no free picture there");
        cJSON_Delete(obj);
        return false;
    }

    int status = 0;
    size_t len = 0;
    uint8_t *jpg = (uint8_t *)net_get(img, "The Met's pictures", 4 * 1024 * 1024, &status, &len, err, n);
    if (jpg && status != 200) {
        snprintf(err, n, "The Met's pictures: HTTP %d", status);
        free(jpg);
        jpg = NULL;
    }
    int next = !s_cur, w = 0, h = 0;
    if (!jpg || !decode(jpg, len, s_px[next], &w, &h, err, n)) {
        free(jpg);
        cJSON_Delete(obj);
        return false;
    }

    copy(s_art.title, sizeof s_art.title, json_str(obj, "title"));
    copy(s_art.artist, sizeof s_art.artist, json_str(obj, "artistDisplayName"));
    copy(s_art.bio, sizeof s_art.bio, json_str(obj, "artistDisplayBio"));
    copy(s_art.date, sizeof s_art.date, json_str(obj, "objectDate"));
    copy(s_art.medium, sizeof s_art.medium, json_str(obj, "medium"));
    copy(s_art.dims, sizeof s_art.dims, json_str(obj, "dimensions"));
    s_art.dims[strcspn(s_art.dims, "\r\n")] = 0;   /* the first line: the painting, not its frame */
    snprintf(s_art.museum, sizeof s_art.museum, "The Metropolitan Museum of Art, %s. %s",
             json_str(obj, "department") ?: "New York", json_str(obj, "creditLine") ?: "");
    s_art.about[0] = 0;
    char wd[120], artist_wd[120];
    copy(wd, sizeof wd, json_str(obj, "objectWikidata_URL"));
    copy(artist_wd, sizeof artist_wd, json_str(obj, "artistWikidata_URL"));
    copy(s_full, sizeof s_full, json_str(obj, "primaryImage"));
    cJSON_Delete(obj);
    /* most of them have no page of their own: then their painter's */
    s_art.about_of = wikipedia(wd) ? "the painting" : wikipedia(artist_wd) ? "the artist" : NULL;

    s_art.pixels = s_px[next];
    s_art.w = w;
    s_art.h = h;
    s_art.stride = STRIDE;
    s_cur = next;
    free(s_jpeg);
    s_jpeg = jpg;
    s_jpeg_len = len;
    ESP_LOGI(TAG, "%s, %s (%s): %dx%d, %u bytes, about %s: %u chars", s_art.title, s_art.artist, s_art.date, w, h,
             (unsigned)len, s_art.about_of ? s_art.about_of : "-", (unsigned)strlen(s_art.about));
    return true;
}

/* ---- saving ---- */

static void save_progress(size_t done, size_t total) {
    char m[64];
    if (total) snprintf(m, sizeof m, "Saving... %u%%", (unsigned)(done * 100 / total));
    else snprintf(m, sizeof m, "Saving... %u KB", (unsigned)(done / 1024));
    s_cb(NULL, m);
}

static void save(char *msg, size_t n) {
    if (!s_jpeg) {
        snprintf(msg, n, "No picture to save yet");
        return;
    }
    char name[260], path[300];
    snprintf(name, sizeof name, "%s - %s", s_art.artist[0] ? s_art.artist : "Unknown", s_art.title);
    for (char *p = name; *p; p++)   /* what FAT names can't hold */
        if (strchr("\\/:*?\"<>|", *p) || (unsigned char)*p < 0x20) *p = ' ';
    size_t k = strlen(name);
    if (k > 120) {   /* short enough, cut between UTF-8 characters */
        k = 120;
        while (k && ((unsigned char)name[k] & 0xC0) == 0x80) k--;
        name[k] = 0;
    }
    while (k && (name[k - 1] == ' ' || name[k - 1] == '.')) name[--k] = 0;
    if (mkdir(SAVE_DIR, 0777) != 0 && errno != EEXIST) {
        snprintf(msg, n, "Can't make the folder on the card");
        return;
    }
    snprintf(path, sizeof path, SAVE_DIR "/%s.jpg", name);
    struct stat st;
    if (stat(path, &st) == 0) {
        snprintf(msg, n, "Already on the card");
        return;
    }
    /* the full-size picture: the small one would look blocky full screen in
       the slideshow. Straight to the card, never whole in memory */
    bool too_big = false;
    if (s_full[0]) {
        size_t got = net_download(s_full, "The Met's pictures", path, SLIDESHOW_MAX_JPEG, &too_big, save_progress, msg, n);
        if (got) snprintf(msg, n, "Saved to the card, in \"Artwork of the Day\" (%u KB): in the slideshow after a restart",
                          (unsigned)(got / 1024));
        if (!too_big) return;
        /* more than the slideshow reads: the small one, then */
    }
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(s_jpeg, 1, s_jpeg_len, f) == s_jpeg_len;
    if (f && fclose(f) != 0) ok = false;
    if (!ok) {
        remove(path);
        snprintf(msg, n, "Can't write it on the card");
    } else {
        snprintf(msg, n, "Saved to the card, in \"Artwork of the Day\" (small: %s): in the slideshow after a restart",
                 too_big ? "the full size is too big for the slideshow" : "no bigger one");
    }
}

/* ---- the task ---- */

/* the local date as a day count, or -1 before the clock is set */
static int today(void) {
    if (!time_synced()) return -1;
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    return t.tm_year * 366 + t.tm_yday;
}

static void artwork_task(void *arg) {
    static char err[160];
    const char *said = NULL;   /* the last waiting status, said once */
    int shown_day = -2, backoff = 0;
    for (;;) {
        int day = today();
        bool random = s_want_random;
        s_want_random = false;
        const char *wait = !wifi_connected() ? "Waiting for Wi-Fi" : day < 0 ? "Waiting for the clock" : NULL;
        if (wait && wait != said && shown_day == -2) s_cb(NULL, wait);
        said = wait;
        if (!wait && (random || day != shown_day)) {
            if (shown_day == -2) s_cb(NULL, "Looking for today's painting...");
            err[0] = 0;
            /* the day's: the date hashed over the hits (Knuth's multiplicative hash) */
            bool ok = s_total || search(0, err, sizeof err);
            if (ok) ok = fetch((random ? esp_random() : (uint32_t)day * 2654435761u) % s_total, err, sizeof err);
            if (ok) {
                s_art.random = random;
                if (!random) shown_day = day;
                backoff = 0;
                s_cb(&s_art, NULL);
            } else {
                ESP_LOGW(TAG, "%s", err);
                s_cb(NULL, err);
                backoff = backoff ? (backoff * 2 > 1800 ? 1800 : backoff * 2) : 60;
            }
        }
        if (s_want_save) {
            s_want_save = false;
            save(err, sizeof err);
            ESP_LOGI(TAG, "save: %s", err);
            s_cb(NULL, err);
        }
        /* a look at the date (and Wi-Fi) every few seconds; after a failure,
           1, 2, 4 ... minutes; "Another" and "Save" wake it at once */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((backoff ? backoff : 5) * 1000));
    }
}

void artwork_start(artwork_cb_t cb) {
    s_cb = cb;
    for (int k = 0; k < 2; k++) s_px[k] = heap_caps_aligned_calloc(64, 1, STRIDE * ART_H, MALLOC_CAP_SPIRAM);
    if (!s_px[0] || !s_px[1] || xTaskCreate(artwork_task, "artwork", 12 * 1024, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "no memory for the artwork page");
        cb(NULL, "Out of memory");
    }
}

void artwork_another(void) {
    s_want_random = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void artwork_save(void) {
    s_want_save = true;
    if (s_task) xTaskNotifyGive(s_task);
}
