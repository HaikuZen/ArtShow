/* net.c - HTTPS GETs (esp_http_client and IDF's certificate bundle) and JSON
 * helpers, for the pages that fetch from the internet. */
#include "net.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cJSON's nodes in PSRAM: WeatherAPI's hourly answer parses into ~3000 small
   allocations, which would otherwise take ~200 KB of internal RAM */
static void *psram_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

void net_init(void) { cJSON_InitHooks(&(cJSON_Hooks){ .malloc_fn = psram_malloc, .free_fn = free }); }

char *net_get(const char *url, const char *who, size_t max, int *status, size_t *len, char *err, size_t n) {
    const esp_http_client_config_t c = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,   /* Visual Crossing's request line is long */
        .user_agent = "ArtShow/1.0 (ESP32-P4 picture frame)",   /* Wikimedia asks for a telling one */
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) {
        snprintf(err, n, "%s: out of memory", who);
        return NULL;
    }
    char *body = NULL;
    size_t got = 0, cap = 0;
    esp_err_t e = esp_http_client_open(h, 0);
    if (e != ESP_OK) {
        snprintf(err, n, "Can't reach %s (%s)", who, esp_err_to_name(e));
        goto out;
    }
    esp_http_client_fetch_headers(h);
    *status = esp_http_client_get_status_code(h);
    for (;;) {
        if (cap - got < 2048) {
            cap = cap ? cap * 2 : 16 * 1024;
            char *b = cap <= max * 2 ? heap_caps_realloc(body, cap, MALLOC_CAP_SPIRAM) : NULL;
            if (!b) {
                snprintf(err, n, "%s: the answer is too big", who);
                free(body);
                body = NULL;
                goto out;
            }
            body = b;
        }
        int r = esp_http_client_read(h, body + got, cap - got - 1);
        if (r < 0) {
            snprintf(err, n, "%s: the connection broke", who);
            free(body);
            body = NULL;
            goto out;
        }
        if (r == 0) break;
        got += r;
        if (got > max) {
            snprintf(err, n, "%s: the answer is too big", who);
            free(body);
            body = NULL;
            goto out;
        }
    }
    body[got] = 0;
    if (len) *len = got;
out:
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return body;
}

size_t net_download(const char *url, const char *who, const char *path, net_progress_t progress, char *err, size_t n) {
    const esp_http_client_config_t c = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .user_agent = "ArtShow/1.0 (ESP32-P4 picture frame)",
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    char *buf = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
    FILE *f = NULL;
    size_t total = 0;
    bool ok = false;
    if (!h || !buf) {
        snprintf(err, n, "%s: out of memory", who);
        goto out;
    }
    esp_err_t e = esp_http_client_open(h, 0);
    if (e != ESP_OK) {
        snprintf(err, n, "Can't reach %s (%s)", who, esp_err_to_name(e));
        goto out;
    }
    esp_http_client_fetch_headers(h);
    int status = esp_http_client_get_status_code(h);
    if (status != 200) {
        snprintf(err, n, "%s: HTTP %d", who, status);
        goto out;
    }
    int64_t size = esp_http_client_get_content_length(h);
    if (!(f = fopen(path, "wb"))) {
        snprintf(err, n, "Can't write on the card (%s)", strerror(errno));
        goto out;
    }
    for (;;) {
        int r = esp_http_client_read(h, buf, 16 * 1024);
        if (r < 0) {
            snprintf(err, n, "%s: the connection broke", who);
            goto out;
        }
        if (r == 0) break;
        if (fwrite(buf, 1, r, f) != (size_t)r) {
            snprintf(err, n, "Can't write on the card (full?)");
            goto out;
        }
        total += r;
        if (progress && (total >> 18) != ((total - r) >> 18)) progress(total, size > 0 ? size : 0);
    }
    ok = total > 0;
    if (!ok) snprintf(err, n, "%s: nothing came", who);
out:
    if (f && fclose(f) != 0 && ok) {
        snprintf(err, n, "Can't write on the card");
        ok = false;
    }
    if (f && !ok) remove(path);
    if (h) {
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
    }
    free(buf);
    return ok ? total : 0;
}

cJSON *net_get_json(const char *url, const char *who, char *err, size_t n) {
    int status = 0;
    char *body = net_get(url, who, 256 * 1024, &status, NULL, err, n);
    if (!body) return NULL;
    cJSON *j = cJSON_Parse(body);
    if (status != 200) {
        const char *msg = j ? json_str(j, "message") : NULL;   /* OpenWeatherMap, the Met */
        if (!msg && j) msg = json_str(j, "error.message");      /* WeatherAPI */
        if (!msg && j) msg = json_str(j, "error");              /* Weatherbit */
        if (!msg && j) msg = json_str(j, "detail");             /* Wikipedia */
        if (!msg && !j) msg = body;                             /* Visual Crossing: plain text */
        if (status == 204 && !(msg && *msg)) msg = "no data for this location";
        snprintf(err, n, "%s: HTTP %d%s%.100s", who, status, msg && *msg ? ": " : "", msg ? msg : "");
        for (char *p = err; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        cJSON_Delete(j);
        j = NULL;
    } else if (!j) {
        snprintf(err, n, "%s: the answer isn't JSON", who);
    }
    free(body);
    return j;
}

const cJSON *json_at(const cJSON *o, const char *path) {
    char key[32];
    while (o && *path) {
        size_t k = strcspn(path, ".");
        if (k >= sizeof key) return NULL;
        memcpy(key, path, k);
        key[k] = 0;
        path += k + (path[k] == '.');
        o = isdigit((unsigned char)key[0]) && cJSON_IsArray(o) ? cJSON_GetArrayItem(o, atoi(key))
                                                                 : cJSON_GetObjectItemCaseSensitive(o, key);
    }
    return o;
}

double json_num(const cJSON *o, const char *path, double def) {
    const cJSON *v = json_at(o, path);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) return atof(v->valuestring);   /* some send "40" */
    return def;
}

const char *json_str(const cJSON *o, const char *path) {
    const cJSON *v = json_at(o, path);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

void url_enc(char *out, size_t n, const char *s, const char *keep) {
    size_t k = 0;
    for (; *s && k + 4 < n; s++) {
        unsigned char c = *s;
        if (isalnum(c) || strchr("-_.~", c) || (keep && strchr(keep, c))) out[k++] = c;
        else k += snprintf(out + k, n - k, "%%%02X", c);
    }
    out[k] = 0;
}
