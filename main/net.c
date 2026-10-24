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

/* open the request and read the headers, following up to 3 redirects
   (Wikipedia answers a renamed page with one) */
static bool open_following(esp_http_client_handle_t h, const char *who, int *status, char *err, size_t n) {
    for (int k = 0;; k++) {
        esp_err_t e = esp_http_client_open(h, 0);
        if (e != ESP_OK) {
            snprintf(err, n, "Can't reach %s (%s)", who, esp_err_to_name(e));
            return false;
        }
        esp_http_client_fetch_headers(h);
        *status = esp_http_client_get_status_code(h);
        if (k == 3 || (*status != 301 && *status != 302 && *status != 307 && *status != 308)) return true;
        esp_http_client_flush_response(h, NULL);
        esp_http_client_set_redirection(h);   /* the Location: the next open goes there */
        esp_http_client_close(h);
    }
}

static esp_http_client_handle_t client_ev(const char *url, http_event_handle_cb on_event, void *arg) {
    const esp_http_client_config_t c = {
        .url = url,
        .event_handler = on_event,
        .user_data = arg,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,   /* Visual Crossing's request line is long */
        .user_agent = "ArtShow/1.0 (ESP32-P4 picture frame)",   /* Wikimedia asks for a telling one */
    };
    return esp_http_client_init(&c);
}

static esp_http_client_handle_t client(const char *url) { return client_ev(url, NULL, NULL); }

/* "https://it.wikipedia.org/w/..." -> "it.wikipedia.org" */
static void host_of(const char *url, char *host, size_t n) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    snprintf(host, n, "%.*s", (int)strcspn(p, "/?#"), p);
}

void net_keep_close(net_keep_t *k) {
    if (!k->h) return;
    esp_http_client_close(k->h);
    esp_http_client_cleanup(k->h);
    k->h = NULL;
    k->host[0] = 0;
}

/* GET with keep (may be NULL): its connection when it's to the same server */
static char *get(net_keep_t *k, const char *url, const char *who, size_t max, int *status, size_t *len, char *err, size_t n) {
    char host[64];
    host_of(url, host, sizeof host);
    esp_http_client_handle_t h;
    bool reused = k && k->h && !strcmp(k->host, host);
    if (reused) {
        h = k->h;
        esp_http_client_set_url(h, url);
    } else {
        if (k) net_keep_close(k);
        if (!(h = client(url))) {
            snprintf(err, n, "%s: out of memory", who);
            return NULL;
        }
        if (k) {
            k->h = h;
            snprintf(k->host, sizeof k->host, "%s", host);
        }
    }
    char *body = NULL;
    size_t got = 0, cap = 0;
    bool ok = false;
    if (!open_following(h, who, status, err, n)) goto out;
    for (;;) {
        if (cap - got < 2048) {
            cap = cap ? cap * 2 : 16 * 1024;
            char *b = cap <= max * 2 ? heap_caps_realloc(body, cap, MALLOC_CAP_SPIRAM) : NULL;
            if (!b) {
                snprintf(err, n, "%s: the answer is too big", who);
                goto out;
            }
            body = b;
        }
        int r = esp_http_client_read(h, body + got, cap - got - 1);
        if (r < 0) {
            snprintf(err, n, "%s: the connection broke", who);
            goto out;
        }
        if (r == 0) break;
        got += r;
        if (got > max) {
            snprintf(err, n, "%s: the answer is too big", who);
            goto out;
        }
    }
    body[got] = 0;
    if (len) *len = got;
    ok = true;
out:
    if (!ok) {
        free(body);
        body = NULL;
    }
    if (!k) {
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
    } else if (!ok) {
        net_keep_close(k);
        if (reused) return get(k, url, who, max, status, len, err, n);   /* the server let it go: once more, anew */
    }
    return body;
}

char *net_get(const char *url, const char *who, size_t max, int *status, size_t *len, char *err, size_t n) {
    return get(NULL, url, who, max, status, len, err, n);
}

esp_http_client_handle_t net_open(const char *url, const char *who, const char *key, const char *value,
                                  http_event_handle_cb on_event, void *arg, int *status, char *err, size_t n) {
    esp_http_client_handle_t h = client_ev(url, on_event, arg);
    if (!h) {
        snprintf(err, n, "%s: no memory", who);
        return NULL;
    }
    if (key) esp_http_client_set_header(h, key, value);
    if (open_following(h, who, status, err, n)) return h;
    esp_http_client_cleanup(h);
    return NULL;
}

size_t net_download(const char *url, const char *who, const char *path, size_t max, bool *too_big,
                    net_progress_t progress, char *err, size_t n) {
    *too_big = false;
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
    int status = 0;
    if (!open_following(h, who, &status, err, n)) goto out;
    if (status != 200) {
        snprintf(err, n, "%s: HTTP %d", who, status);
        goto out;
    }
    int64_t size = esp_http_client_get_content_length(h);
    if (size > (int64_t)max) {
        *too_big = true;
        snprintf(err, n, "%s: too big (%lld bytes)", who, size);
        goto out;
    }
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
        if (total > max) {
            *too_big = true;
            snprintf(err, n, "%s: too big", who);
            goto out;
        }
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

cJSON *net_get_json(const char *url, const char *who, char *err, size_t n) { return net_keep_get_json(NULL, url, who, err, n); }

cJSON *net_keep_get_json(net_keep_t *k, const char *url, const char *who, char *err, size_t n) {
    int status = 0;
    char *body = get(k, url, who, 256 * 1024, &status, NULL, err, n);
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
