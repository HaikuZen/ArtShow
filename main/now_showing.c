/* now_showing.c - the picture on show, small (from the slideshow's screen
 * buffer), with its title and artist from the file name and Wikipedia's
 * summary beside it. The lookup: the painting on CONFIG_ARTSHOW_WIKI_LANG's
 * Wikipedia (one search: the rate-limited kind of call); failing that, the
 * artist's page there, then on English Wikipedia (page summaries by title:
 * cached, generously limited). The last few answers are kept. */
#include "now_showing.h"
#include "slideshow.h"
#include "transition.h"
#include "net.h"
#include "wifi.h"
#include "fonts/art_fonts.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <stdio.h>
#include <string.h>

#define BG       0x0D1B2A
#define RIM      0x415A77
#define MINOR    0x778DA9
#define TEXT     0xE0F0FF
#define WARN     0xFF6B35
#define THUMB_W  480       /* the picture, small: the screen's shape */
#define THUMB_H  281
#define SCREEN_W 1024
#define SCREEN_H 600
#define CACHE    16
#define PATH_LEN 256

static const char *TAG = "nowshow";

typedef struct {
    char path[PATH_LEN];
    char about[1600];
    const char *about_of;   /* "the painting", "the artist"; NULL: nothing found */
} entry_t;

static entry_t *s_cache;    /* PSRAM, CACHE of them; the lookup task's alone */
static int s_slot;
static QueueHandle_t s_q;   /* the path to look up */
static net_keep_t s_conn;   /* one connection for a lookup's calls (most go to one server) */

/* the page's, LVGL task's */
static lv_obj_t *s_page, *s_btn, *s_btn_label, *s_sub, *s_img, *s_folder, *s_info, *s_title, *s_artist, *s_about_of, *s_about, *s_note;
static lv_image_dsc_t s_dsc;
static uint16_t *s_thumb;
static char s_path[PATH_LEN];   /* the picture on the page */

/* ---- the file name ---- */

/* "/sdcard/DailyArt Pictures/Paul Signac - Il pino.jpg": the folder, the
   artist and the title. DailyArt writes ':' as '_' */
static void parse(const char *path, char *folder, char *artist, char *title, size_t n) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dir = strncmp(path, "/sdcard/", 8) ? path : path + 8;
    snprintf(folder, n, "%.*s", base - dir > 0 ? (int)(base - dir - 1) : 0, dir);
    char name[PATH_LEN];
    snprintf(name, sizeof name, "%s", base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    for (char *p = name; *p; p++) if (*p == '_') *p = p[1] == ' ' ? ':' : ' ';
    char *sep = strstr(name, " - ");
    if (sep) {
        *sep = 0;
        snprintf(artist, n, "%s", name);
        snprintf(title, n, "%s", sep + 3);
    } else {
        artist[0] = 0;
        snprintf(title, n, "%s", name);
    }
}

/* ---- Wikipedia ---- */

static bool has_any(const char *s, const char *const *words) {
    for (; s && *words; words++)
        if (strcasestr(s, *words)) return true;
    return false;
}

/* in a page's short description, what makes it a painting's (or a print's) */
static const char *const PAINTING[] = { "dipint", "quadro", "affresc", "painting", "xilograf", "woodblock",
                                        "acquerell", "watercolo", "gemälde", "tableau", "pintura", NULL };
/* in an artist's page, what makes it one */
static const char *const ARTIST[] = { "pittor", "painter", "artist", "incisor", "printmaker", "illustrat",
                                      "pittric", "scultor", "sculptor", "xilograf", "fotograf", "photographer", "maler", "peintre", NULL };

/* a page's summary, or NULL: none (404), or err set (429: busy) */
static cJSON *summary(const char *lang, const char *page, char *err, size_t n) {
    char t[200], enc[400], url[480];
    snprintf(t, sizeof t, "%s", page);
    for (char *p = t; *p; p++) if (*p == ' ') *p = '_';
    url_enc(enc, sizeof enc, t, NULL);
    snprintf(url, sizeof url, "https://%s.wikipedia.org/api/rest_v1/page/summary/%s", lang, enc);
    char e[160] = "";
    cJSON *j = net_keep_get_json(&s_conn, url, "Wikipedia", e, sizeof e);
    if (!j && !strstr(e, "HTTP 404")) snprintf(err, n, "%s", e);   /* not found is no error */
    return j;
}

/* the painting: the top hit of a search for "title artist", if its page says
   it's a painting by that artist (not the artist's own page) */
static bool find_painting(entry_t *e, const char *lang, const char *artist, const char *title, char *err, size_t n) {
    const char *surname = strrchr(artist, ' ');
    surname = surname ? surname + 1 : artist;
    if (!*surname) return false;
    char q[400], enc[600], url[760];
    snprintf(q, sizeof q, "%s %s", title, artist);
    url_enc(enc, sizeof enc, q, NULL);
    snprintf(url, sizeof url, "https://%s.wikipedia.org/w/api.php?action=query&list=search&srlimit=1&format=json&srsearch=%s",
             lang, enc);
    cJSON *j = net_keep_get_json(&s_conn, url, "Wikipedia", err, n);
    char hit[200] = "";
    snprintf(hit, sizeof hit, "%s", j ? json_str(j, "query.search.0.title") ?: "" : "");
    cJSON_Delete(j);
    if (!hit[0] || strcasestr(hit, surname)) return false;
    cJSON *s = summary(lang, hit, err, n);
    const char *desc = json_str(s, "description"), *text = json_str(s, "extract");
    bool ok = text && has_any(desc, PAINTING) && ((desc && strcasestr(desc, surname)) || strcasestr(text, surname));
    if (ok) {
        snprintf(e->about, sizeof e->about, "%s", text);
        e->about_of = "the painting";
    }
    cJSON_Delete(s);
    return ok;
}

static bool find_artist(entry_t *e, const char *lang, const char *artist, char *err, size_t n) {
    if (!*artist) return false;
    cJSON *s = summary(lang, artist, err, n);
    if (!s && !err[0]) {   /* no page by that exact name ("James Dixon Innes" is "James Dickson Innes"): a search */
        const char *surname = strrchr(artist, ' ');
        surname = surname ? surname + 1 : artist;
        char enc[300], url[440], hit[200] = "";
        url_enc(enc, sizeof enc, artist, NULL);
        snprintf(url, sizeof url, "https://%s.wikipedia.org/w/api.php?action=query&list=search&srlimit=1&format=json&srsearch=%s",
                 lang, enc);
        cJSON *j = net_keep_get_json(&s_conn, url, "Wikipedia", err, n);
        snprintf(hit, sizeof hit, "%s", j ? json_str(j, "query.search.0.title") ?: "" : "");
        cJSON_Delete(j);
        if (hit[0] && strcasestr(hit, surname)) s = summary(lang, hit, err, n);   /* the name, nearly */
    }
    const char *type = json_str(s, "type"), *desc = json_str(s, "description"), *text = json_str(s, "extract");
    bool ok = text && type && !strcmp(type, "standard") && (has_any(desc, ARTIST) || has_any(text, ARTIST));
    if (ok) {
        snprintf(e->about, sizeof e->about, "%s", text);
        e->about_of = "the artist";
    }
    cJSON_Delete(s);
    return ok;
}

/* ---- the page's side ---- */

static void status(const char *txt, bool warn) {
    lv_label_set_text(s_sub, txt ? txt : "");
    lv_obj_set_style_text_color(s_sub, lv_color_hex(warn ? WARN : MINOR), 0);
}

/* from the lookup task: e (an answer) or a status, for path */
static void deliver(const char *path, const entry_t *e, const char *st, bool warn) {
    lvgl_port_lock(0);
    if (!strcmp(path, s_path)) {   /* still the picture on the page */
        if (e) {
            char buf[64];
            snprintf(buf, sizeof buf, "From Wikipedia, about %s:", e->about_of ? e->about_of : "");
            lv_label_set_text(s_about_of, e->about_of ? buf : "");
            lv_obj_set_hidden(s_about_of, !e->about_of);
            lv_label_set_text(s_about, e->about);
            lv_obj_set_hidden(s_about, !e->about_of);
            status(e->about_of ? NULL : "Nothing about it on Wikipedia", false);
        } else {
            status(st, warn);
        }
    }
    lvgl_port_unlock();
}

/* ---- the lookup task ---- */

static void lookup_task(void *arg) {
    static char path[PATH_LEN], folder[PATH_LEN], artist[PATH_LEN], title[PATH_LEN], err[160];
    for (;;) {
        xQueueReceive(s_q, path, portMAX_DELAY);
        entry_t *e = NULL;
        for (int k = 0; k < CACHE; k++)
            if (!strcmp(s_cache[k].path, path)) e = &s_cache[k];
        if (e) {
            deliver(path, e, NULL, false);
            continue;
        }
        if (!wifi_connected()) {
            deliver(path, NULL, "No Wi-Fi: nothing from Wikipedia", true);
            continue;
        }
        deliver(path, NULL, "Looking it up on Wikipedia...", false);
        parse(path, folder, artist, title, PATH_LEN);
        e = &s_cache[s_slot];
        memset(e, 0, sizeof *e);
        err[0] = 0;
        const char *lang = CONFIG_ARTSHOW_WIKI_LANG;
        bool found = find_painting(e, lang, artist, title, err, sizeof err) ||
                     find_artist(e, lang, artist, err, sizeof err) ||
                     (strcmp(lang, "en") && find_artist(e, "en", artist, err, sizeof err));
        net_keep_close(&s_conn);
        ESP_LOGI(TAG, "%s: %s%s", title, e->about_of ? e->about_of : "nothing", err[0] ? " (an error on the way)" : "");
        if (!found && err[0]) {   /* busy, or no answer: not kept, the next look tries again */
            ESP_LOGW(TAG, "%s", err);
            deliver(path, NULL, strstr(err, "HTTP 429") ? "Wikipedia is busy (too many requests): try again in a minute" : err,
                    true);
            continue;
        }
        snprintf(e->path, sizeof e->path, "%s", path);
        s_slot = (s_slot + 1) % CACHE;
        deliver(path, e, NULL, false);
    }
}

/* ---- the page ---- */

/* the screen's picture, box-filtered down into the thumbnail */
static void make_thumb(const uint16_t *src) {
    for (int y = 0; y < THUMB_H; y++) {
        int y0 = y * SCREEN_H / THUMB_H, y1 = (y + 1) * SCREEN_H / THUMB_H;
        for (int x = 0; x < THUMB_W; x++) {
            int x0 = x * SCREEN_W / THUMB_W, x1 = (x + 1) * SCREEN_W / THUMB_W;
            uint32_t r = 0, g = 0, b = 0, n = (x1 - x0) * (y1 - y0);
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint16_t p = src[yy * SCREEN_W + xx];
                    r += p >> 11;
                    g += (p >> 5) & 0x3f;
                    b += p & 0x1f;
                }
            s_thumb[y * THUMB_W + x] = (uint16_t)((r / n) << 11 | (g / n) << 5 | b / n);
        }
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, bool wrap) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    if (wrap) {
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    return l;
}

/* open over the slideshow, or close; the slideshow waits meanwhile */
static void set_open(bool on) {
    lv_obj_set_hidden(s_page, !on);
    lv_label_set_text(s_btn_label, on ? LV_SYMBOL_CLOSE : "?");
    slideshow_pause(on);
    if (!on) return;
    char path[PATH_LEN];
    bool have = slideshow_now(path, sizeof path);
    lv_obj_set_hidden(s_note, have);
    lv_obj_set_hidden(s_info, !have);
    lv_obj_set_hidden(s_img, !have);
    lv_obj_set_hidden(s_folder, !have);
    if (!have) {
        s_path[0] = 0;
        status(NULL, false);
        return;
    }
    transition_finish();   /* the picture whole, not halfway through a change */
    const uint16_t *src = slideshow_screen();
    if (src && s_thumb) {
        make_thumb(src);
        lv_image_set_src(s_img, &s_dsc);
        lv_obj_invalidate(s_img);
    }
    if (!strcmp(path, s_path)) return;   /* the same one: its text is there */

    snprintf(s_path, sizeof s_path, "%s", path);
    char folder[PATH_LEN], artist[PATH_LEN], title[PATH_LEN];
    parse(path, folder, artist, title, PATH_LEN);
    lv_label_set_text(s_title, title);
    lv_label_set_text(s_artist, artist);
    lv_obj_set_hidden(s_artist, !artist[0]);
    lv_label_set_text_fmt(s_folder, "On the card, in %s", folder[0] ? folder : "the top folder");
    lv_label_set_text(s_about, "");
    lv_obj_set_hidden(s_about_of, true);
    lv_obj_scroll_to_y(s_info, 0, LV_ANIM_OFF);
    status(NULL, false);
    if (s_q) xQueueOverwrite(s_q, path);
}

static void button_cb(lv_event_t *e) { set_open(lv_obj_is_hidden(s_page)); }

void now_showing_slideshow(bool shown) {
    if (!shown && !lv_obj_is_hidden(s_page)) {   /* another page: this one goes with the slideshow */
        lv_obj_set_hidden(s_page, true);
        lv_label_set_text(s_btn_label, "?");
    }
    lv_obj_set_hidden(s_btn, !shown);
}

void now_showing_create(lv_obj_t *parent) {
    lv_obj_t *page = s_page = lv_obj_create(parent);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollable(page, false);

    lv_obj_t *head = label(page, &art_font_24, TEXT, false);   /* right of the menu button */
    lv_label_set_text(head, "Now Showing");
    lv_obj_set_pos(head, 90, 12);
    s_sub = label(page, &art_font_16, MINOR, false);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_sub, 900);
    lv_obj_set_pos(s_sub, 91, 44);

    /* the picture, small; where it is under it */
    s_thumb = heap_caps_aligned_calloc(64, 1, THUMB_W * THUMB_H * 2, MALLOC_CAP_SPIRAM);
    s_dsc = (lv_image_dsc_t){
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                    .w = THUMB_W, .h = THUMB_H, .stride = THUMB_W * 2 },
        .data_size = THUMB_W * THUMB_H * 2,
        .data = (const uint8_t *)s_thumb,
    };
    s_img = lv_image_create(page);
    lv_obj_set_pos(s_img, 24, 80);
    lv_obj_set_style_outline_width(s_img, 1, 0);
    lv_obj_set_style_outline_color(s_img, lv_color_hex(RIM), 0);
    s_folder = label(page, &art_font_16, MINOR, false);
    lv_label_set_long_mode(s_folder, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_folder, THUMB_W);
    lv_obj_set_pos(s_folder, 24, 80 + THUMB_H + 12);

    /* its card: scrolls when the text is long */
    s_info = lv_obj_create(page);
    lv_obj_remove_style_all(s_info);
    lv_obj_set_size(s_info, SCREEN_W - 24 - THUMB_W - 24 - 20, 456);
    lv_obj_set_pos(s_info, 24 + THUMB_W + 24, 80);
    lv_obj_set_flex_flow(s_info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_info, 8, 0);
    lv_obj_set_style_pad_right(s_info, 8, 0);
    lv_obj_set_scroll_dir(s_info, LV_DIR_VER);
    s_title = label(s_info, &art_font_32, TEXT, true);
    s_artist = label(s_info, &art_font_24, TEXT, true);
    s_about_of = label(s_info, &art_font_16, MINOR, true);   /* Wikipedia's text asks for credit */
    lv_obj_set_style_pad_top(s_about_of, 8, 0);
    s_about = label(s_info, &art_font_20, TEXT, true);
    lv_obj_set_style_text_line_space(s_about, 4, 0);

    s_note = label(page, &art_font_24, TEXT, false);
    lv_label_set_text(s_note, "No picture on show yet");
    lv_obj_align(s_note, LV_ALIGN_CENTER, 0, 0);

    s_cache = heap_caps_calloc(CACHE, sizeof *s_cache, MALLOC_CAP_SPIRAM);
    s_q = xQueueCreate(1, PATH_LEN);
    /* stack in PSRAM, idle priority: as the artwork task */
    if (!s_cache || !s_q || xTaskCreateWithCaps(lookup_task, "nowshow", 12 * 1024, NULL, tskIDLE_PRIORITY, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the lookups");
        if (s_q) vQueueDelete(s_q);
        s_q = NULL;
    }

    lv_obj_set_hidden(page, true);

    /* the button, top right, like the other corner ones: over the page, to close it too */
    s_btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(s_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(s_btn, 0, 0);
    lv_obj_set_style_pad_all(s_btn, 10, 0);
    lv_obj_set_style_min_width(s_btn, 44, 0);   /* "?" is narrower than the other icons */
    s_btn_label = lv_label_create(s_btn);
    lv_label_set_text(s_btn_label, "?");
    lv_obj_set_style_text_font(s_btn_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_btn_label, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_opa(s_btn_label, LV_OPA_60, 0);
    lv_obj_center(s_btn_label);
    lv_obj_align(s_btn, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_set_ext_click_area(s_btn, 24);   /* ~92 px to touch, out to the corner */
    lv_obj_add_event_cb(s_btn, button_cb, LV_EVENT_CLICKED, NULL);
}
