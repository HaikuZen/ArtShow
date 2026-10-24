/* radio.c - an internet station on the speaker. Two tasks: the reader pulls
 * the stream over HTTP into a buffer in PSRAM (~6 s of it: Wi-Fi's hiccups
 * pass unheard), the player decodes it (esp_audio_codec's MP3 or AAC, told
 * apart by the frames themselves) and writes it to I2S at the stream's own
 * rate, which paces it. The level is low and changes slowly: a fade in over
 * 4 s, out over 0.6 s. A new station or a stop: the reader drops the
 * connection and has the player empty the buffer before the next one. */
#include "radio.h"
#include "audio.h"
#include "net.h"
#include "settings.h"
#include "wifi.h"
#include "esp_audio_simple_dec.h"
#include "esp_mp3_dec.h"
#include "esp_aac_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "radio";

#define BUF_BYTES   (96 * 1024)   /* 6 s at 128 kbit/s */
#define PREBUF      (32 * 1024)   /* 2 s in hand before it plays (and after a gap) */
#define READ_BYTES  4096
#define IN_BYTES    4096          /* the player's bite of the stream */
#define PCM_BYTES   (2048 * 2 * 2)   /* a frame's samples: HE-AAC's 2048, stereo */
#define FADE_IN_S   4.0f
#define FADE_OUT_S  0.6f

typedef struct {
    char name[48];
    char url[160];
} station_t;

/* quiet ones: no talk, no beat, no ads; plain HTTP (no TLS handshake) */
static station_t s_st[RADIO_MAX_STATIONS] = {
    { "Drone Zone (SomaFM)", "http://ice2.somafm.com/dronezone-128-mp3" },
    { "Deep Space One (SomaFM)", "http://ice2.somafm.com/deepspaceone-128-mp3" },
    { "Ambient Sleeping Pill", "http://radio.stereoscenic.com/asp-s" },
    { "Radio Swiss Classic", "http://stream.srg-ssr.ch/m/rsc_de/mp3_128" },
    { "France Musique Baroque", "http://icecast.radiofrance.fr/francemusiquebaroque-midfi.mp3" },
    { "Radio Paradise Mellow", "http://stream.radioparadise.com/mellow-128" },
};
static int s_n = 6;

static StreamBufferHandle_t s_sb;
static TaskHandle_t s_reader, s_player;
static SemaphoreHandle_t s_lock;   /* s_status and s_info */
static char s_status[96];
static radio_info_t s_info;        /* the stream's, for the screen */
#define META_BYTES 4080                /* a title's most: 255 x 16 */
static char *s_meta;                   /* the reader's: PSRAM */
static volatile bool s_want, s_on;   /* on: wanted (and the tasks made) */
static volatile bool s_reset;                 /* the reader asks the player to empty the buffer */
static volatile int s_gen;                    /* each retune */

static void set_status(const char *fmt, const char *arg) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_status, sizeof s_status, fmt, arg);
    xSemaphoreGive(s_lock);
}

bool radio_status(char *buf, size_t n) {
    if (!s_lock || !s_want) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(buf, n, "%s", s_status);
    xSemaphoreGive(s_lock);
    return true;
}

bool radio_info(radio_info_t *out) {
    if (!s_lock || !s_on) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_info;
    snprintf(out->status, sizeof out->status, "%s", s_status);
    xSemaphoreGive(s_lock);
    return true;
}

static int station(void) {
    int k = g_settings.radio_station;
    return k >= 0 && k < s_n ? k : 0;
}

/* ---- the reader: the stream into the buffer ---- */

/* wait up to ms, or until a radio_* call wakes it */
static void doze(int ms) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms)); }

/* the response's headers: icy-metaint (the audio bytes between two titles),
   icy-br (the bit rate) */
static esp_err_t on_header(esp_http_client_event_t *e) {
    if (e->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    int *metaint = e->user_data;
    if (!strcasecmp(e->header_key, "icy-metaint")) *metaint = atoi(e->header_value);
    if (!strcasecmp(e->header_key, "icy-br")) s_info.kbps = atoi(e->header_value);   /* the reader's own moment: no lock needed */
    return ESP_OK;
}

static bool valid_utf8(const unsigned char *p, int n) {
    for (int i = 0; i < n;) {
        unsigned char c = p[i];
        int more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (more < 0 || i + more >= n) return false;
        for (int k = 1; k <= more; k++)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        i += more + 1;
    }
    return true;
}

/* UTF-8 as it is; else Latin-1 (older servers), made UTF-8 */
static void to_utf8(const char *in, int n, char *out, size_t size) {
    bool utf8 = valid_utf8((const unsigned char *)in, n);
    size_t o = 0;
    for (int i = 0; i < n && o + 3 < size; i++) {
        unsigned char c = in[i];
        if (utf8 || c < 0x80) out[o++] = c;
        else {
            out[o++] = 0xC0 | c >> 6;
            out[o++] = 0x80 | (c & 0x3F);
        }
    }
    out[o] = 0;
}

/* "StreamTitle='Artist - Title';StreamUrl='';" (padded with NULs) */
static void got_meta(const char *m, int n) {
    const char *p = memmem(m, n, "StreamTitle='", 13);
    if (!p) return;
    p += 13;
    const char *end = memmem(p, m + n - p, "';", 2);   /* not the first ': "Don't" */
    if (!end) end = memchr(p, 0, m + n - p);
    if (!end) end = m + n;
    char t[sizeof s_info.title];
    to_utf8(p, end - p, t, sizeof t);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(t, s_info.title)) {
        snprintf(s_info.title, sizeof s_info.title, "%s", t);
        ESP_LOGI(TAG, "now: %s", t);
    }
    xSemaphoreGive(s_lock);
}

/* bytes into the buffer, unless it's stopped or retuned meanwhile */
static void push(const uint8_t *p, int n, int gen) {
    for (int done = 0; done < n && s_on && gen == s_gen;)
        done += xStreamBufferSend(s_sb, p + done, n - done, pdMS_TO_TICKS(200));
}

static void reader(void *arg) {
    uint8_t *buf = heap_caps_malloc(READ_BYTES, MALLOC_CAP_SPIRAM);
    s_meta = heap_caps_malloc(META_BYTES, MALLOC_CAP_SPIRAM);
    int backoff = 0;
    for (;;) {
        if (!s_on) {
            doze(portMAX_DELAY);
            continue;
        }
        const station_t *st = &s_st[station()];
        if (!wifi_connected()) {
            set_status("No Wi-Fi", NULL);
            doze(2000);
            continue;
        }
        int gen = s_gen;
        s_reset = true;   /* the last station's bytes out first */
        for (int k = 0; s_reset && k < 100; k++) vTaskDelay(pdMS_TO_TICKS(20));
        set_status("Tuning in to %s", st->name);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_info = (radio_info_t){ 0 };
        snprintf(s_info.station, sizeof s_info.station, "%s", st->name);
        xSemaphoreGive(s_lock);
        char err[96] = "";
        int status = 0, metaint = 0;
        bool heard = false;
        /* Icy-MetaData: the server puts the title in the stream every metaint bytes */
        esp_http_client_handle_t h = net_open(st->url, st->name, "Icy-MetaData", "1", on_header, &metaint, &status,
                                              err, sizeof err);
        if (h && status != 200) snprintf(err, sizeof err, "%s: HTTP %d", st->name, status);
        else if (h) {
            ESP_LOGI(TAG, "%s", st->name);   /* not the URL: one from config.json could hold a key */
            int audio = metaint, meta = -1, mlen = 0;   /* audio bytes to a title; a title's bytes to come (-1: none) */
            while (s_on && gen == s_gen) {
                int n = esp_http_client_read(h, (char *)buf, READ_BYTES);
                if (n <= 0) {
                    snprintf(err, sizeof err, "%s: the stream stopped", st->name);
                    break;
                }
                if (!heard) {
                    heard = true;
                    backoff = 0;
                    set_status("%s", st->name);
                }
                if (!metaint) {
                    push(buf, n, gen);
                    continue;
                }
                for (int i = 0; i < n;) {   /* audio, a length byte (x16), the title, audio ... */
                    if (meta < 0 && audio > 0) {
                        int k = n - i < audio ? n - i : audio;
                        push(buf + i, k, gen);
                        i += k;
                        audio -= k;
                    } else if (meta < 0) {
                        meta = buf[i++] * 16;
                        mlen = 0;
                    } else {
                        int k = n - i < meta ? n - i : meta;
                        if (s_meta && mlen + k <= META_BYTES) memcpy(s_meta + mlen, buf + i, k);
                        mlen += k;
                        i += k;
                        meta -= k;
                    }
                    if (meta == 0) {
                        if (s_meta && mlen && mlen <= META_BYTES) got_meta(s_meta, mlen);
                        meta = -1;
                        audio = metaint;
                    }
                }
            }
        }
        if (h) {
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
        }
        if (s_on && gen == s_gen) {   /* it failed, not stopped: again in 2, 4 ... 60 s */
            ESP_LOGW(TAG, "%s", err);
            set_status("No signal: %s", err);
            backoff = backoff ? (backoff * 2 > 60 ? 60 : backoff * 2) : 2;
            for (int t = 0; t < backoff * 10 && s_on && gen == s_gen; t++) doze(100);
        }
    }
}

/* ---- the player: the buffer to the speaker ---- */

/* MP3 or ADTS AAC, from the first frame header in the bytes */
static esp_audio_simple_dec_type_t sniff(const uint8_t *p, int n) {
    for (int i = 0; i + 1 < n; i++) {
        if (p[i] != 0xFF || (p[i + 1] & 0xE0) != 0xE0) continue;
        if ((p[i + 1] & 0xF6) == 0xF0) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;   /* 12 sync bits, layer 0 */
        if (p[i + 1] & 0x06) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;             /* a layer */
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

/* *codec: "MP3" or "AAC" */
static esp_audio_simple_dec_handle_t open_decoder(const uint8_t *p, int n, const char **codec) {
    esp_audio_simple_dec_type_t t = sniff(p, n);
    if (t == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) return NULL;
    *codec = t == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC ? "AAC" : "MP3";
    esp_aac_dec_cfg_t aac = ESP_AAC_DEC_CONFIG_DEFAULT();
    aac.aac_plus_enable = true;   /* HE-AAC ("aacp") streams */
    esp_audio_simple_dec_cfg_t c = { .dec_type = t };
    if (t == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) {
        c.dec_cfg = &aac;
        c.cfg_size = sizeof aac;
    }
    esp_audio_simple_dec_handle_t d = NULL;
    if (esp_audio_simple_dec_open(&c, &d) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "no %s decoder", esp_audio_simple_dec_get_name(t));
        return NULL;
    }
    return d;
}

static void player(void *arg) {
    uint8_t *in = heap_caps_malloc(IN_BYTES, MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(PCM_BYTES, MALLOC_CAP_SPIRAM), *lr = heap_caps_malloc(PCM_BYTES, MALLOC_CAP_SPIRAM);
    esp_audio_simple_dec_handle_t dec = NULL;
    int have = 0, rate = 0;
    bool held = false, playing = false;
    float gain = 0;
    for (;;) {
        if (s_reset) {
            if (dec) esp_audio_simple_dec_close(dec);
            dec = NULL;
            have = 0;
            playing = false;
            xStreamBufferReset(s_sb);   /* the reader waits meanwhile: nobody is blocked on it */
            s_reset = false;
        }
        bool want = s_on;
        if (held && !want && gain < 0.002f) {   /* faded out */
            audio_close();
            held = playing = false;
        }
        if (!held && !want) {   /* polled: notifications are the stream buffer's own */
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!playing) {   /* (re)filling: 2 s in hand first */
            if (!want || xStreamBufferBytesAvailable(s_sb) < PREBUF) {
                if (held && !want) gain = 0;   /* nothing left to fade */
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            playing = true;
        }
        have += xStreamBufferReceive(s_sb, in + have, IN_BYTES - have, pdMS_TO_TICKS(100));
        if (!have) {   /* ran dry: wait for 2 s again */
            ESP_LOGW(TAG, "the buffer ran dry");
            playing = false;
            continue;
        }
        if (!dec && !(dec = open_decoder(in, have, &s_info.codec))) {
            have = 0;   /* no frame in it yet: the next bite */
            continue;
        }
        esp_audio_simple_dec_raw_t raw = { .buffer = in, .len = have };
        esp_audio_simple_dec_out_t out = { .buffer = (uint8_t *)pcm, .len = PCM_BYTES };
        esp_audio_err_t e = esp_audio_simple_dec_process(dec, &raw, &out);
        if (e != ESP_AUDIO_ERR_OK && e != ESP_AUDIO_ERR_CONTINUE && e != ESP_AUDIO_ERR_DATA_LACK) {
            ESP_LOGW(TAG, "decode: %d", e);
            if (!raw.consumed) raw.consumed = have;   /* damaged: past it */
        }
        memmove(in, in + raw.consumed, have - raw.consumed);
        have -= raw.consumed;
        if (!out.decoded_size) continue;
        esp_audio_simple_dec_info_t info;
        if (esp_audio_simple_dec_get_info(dec, &info) != ESP_AUDIO_ERR_OK || !info.sample_rate || !info.channel ||
            info.bits_per_sample != 16)
            continue;
        if (!held || (int)info.sample_rate != rate) {
            if (held) audio_close();
            rate = info.sample_rate;
            audio_open(rate);   /* waits for the Generative page's music to fade */
            held = true;
            ESP_LOGI(TAG, "%d Hz, %d channel%s", rate, info.channel, info.channel > 1 ? "s" : "");
        }
        if (s_info.rate != rate || s_info.channels != info.channel || (!s_info.kbps && info.bitrate)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_info.rate = rate;
            s_info.channels = info.channel;
            if (!s_info.kbps) s_info.kbps = info.bitrate / 1000;   /* no icy-br */
            xSemaphoreGive(s_lock);
        }
        int frames = out.decoded_size / (2 * info.channel);
        /* the level: the slider's square (a quiet one by default), reached slowly */
        float v = g_settings.radio_volume / 100.0f, target = want ? v * v : 0;
        float step = target > gain ? 1.0f / (FADE_IN_S * rate) : -1.0f / (FADE_OUT_S * rate);
        for (int i = 0; i < frames; i++) {
            gain += step;
            if ((step > 0 && gain > target) || (step < 0 && gain < target)) gain = target;
            int l = pcm[i * info.channel], r = info.channel > 1 ? pcm[i * info.channel + 1] : l;
            lr[2 * i] = (int16_t)(l * gain);
            lr[2 * i + 1] = (int16_t)(r * gain);
        }
        audio_write_stereo(lr, frames);
    }
}

/* ---- the calls ---- */

void radio_config(bool card) {
    FILE *f = card ? fopen("/sdcard/config.json", "rb") : NULL;
    if (!f) return;
    char *txt = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
    size_t len = txt ? fread(txt, 1, 16 * 1024 - 1, f) : 0;
    fclose(f);
    if (!txt) return;
    txt[len] = 0;
    cJSON *j = cJSON_Parse(txt);
    free(txt);
    const cJSON *list = json_at(j, "radio.stations");
    int n = 0;
    for (int k = 0; cJSON_IsArray(list) && k < cJSON_GetArraySize(list) && n < RADIO_MAX_STATIONS; k++) {
        const cJSON *o = cJSON_GetArrayItem(list, k);
        const char *name = json_str(o, "name"), *url = json_str(o, "url");
        if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) continue;
        snprintf(s_st[n].name, sizeof s_st[n].name, "%s", name && *name ? name : "A station");
        snprintf(s_st[n].url, sizeof s_st[n].url, "%s", url);
        n++;
    }
    if (n) s_n = n;
    cJSON_Delete(j);
    ESP_LOGI(TAG, "%d stations%s", s_n, n ? " from config.json" : "");
}

int radio_count(void) { return s_n; }
const char *radio_name(int k) { return k >= 0 && k < s_n ? s_st[k].name : ""; }

/* the tasks when first wanted: nothing is spent on a radio never played */
static bool start(void) {
    if (s_player) return true;
    esp_mp3_dec_register();
    esp_aac_dec_register();
    s_lock = xSemaphoreCreateMutex();
    s_sb = xStreamBufferCreateWithCaps(BUF_BYTES, 1, MALLOC_CAP_SPIRAM);
    /* the player on core 0 (the painters are on core 1), above the idle
       priority: it must keep up. The reader at idle priority, as the other
       network tasks (a TLS handshake for an https station takes seconds of
       CPU). Neither writes flash: stacks in PSRAM */
    if (!s_lock || !s_sb ||
        xTaskCreatePinnedToCoreWithCaps(player, "radio", 6 * 1024, NULL, 5, &s_player, 0, MALLOC_CAP_SPIRAM) != pdPASS ||
        xTaskCreateWithCaps(reader, "radio_net", 6 * 1024, NULL, tskIDLE_PRIORITY, &s_reader, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "no tasks");
        return false;
    }
    return true;
}

static void update(void) {
    bool on = s_want;
    if (on && !start()) return;
    s_on = on;
    if (s_reader) xTaskNotifyGive(s_reader);   /* out of a doze; a send it's in just returns early */
}

void radio_play(bool on) {
    s_want = on;
    update();
}

void radio_retune(void) {
    s_gen++;
    update();
}
