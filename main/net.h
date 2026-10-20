#pragma once
/* HTTPS GETs and JSON, shared by the pages that fetch from the internet
 * (weather, artwork). Call from a task with ~12 KB of stack, never from LVGL's */
#include <stddef.h>
#include "cJSON.h"
#include "esp_http_client.h"

/* cJSON's nodes in PSRAM; call once at boot, before any parse */
void net_init(void);
/* GET url over HTTPS: the body (PSRAM, NUL-terminated, free() it) and its
 * length, or NULL and err ("<who>: ..."). *status: the HTTP status. The URL may
 * hold an API key: it is never logged */
char *net_get(const char *url, const char *who, size_t max, int *status, size_t *len, char *err, size_t n);
/* GET url straight into the file path (16 KB at a time: no big buffer); a
 * failure removes the file. progress (may be NULL) hears every 256 KB; total
 * is 0 if the server didn't say. More than max bytes (said before, or as it
 * comes): *too_big, nothing kept. The size written, or 0 and err */
typedef void (*net_progress_t)(size_t done, size_t total);
size_t net_download(const char *url, const char *who, const char *path, size_t max, bool *too_big,
                    net_progress_t progress, char *err, size_t n);
/* GET and parse JSON (up to 256 KB); not 200: NULL, and err holds the
 * server's own message when it sent one */
cJSON *net_get_json(const char *url, const char *who, char *err, size_t n);

/* a connection kept open between GETs to one server: a new TLS connection
 * costs ~2 s (Wikipedia's). Zero it first; a GET to another server replaces it */
typedef struct {
    esp_http_client_handle_t h;
    char host[64];
} net_keep_t;
cJSON *net_keep_get_json(net_keep_t *k, const char *url, const char *who, char *err, size_t n);
void net_keep_close(net_keep_t *k);

/* a dotted path into JSON: "weather.0.id" (a number indexes an array) */
const cJSON *json_at(const cJSON *o, const char *path);
double json_num(const cJSON *o, const char *path, double def);   /* a number, or a string holding one */
const char *json_str(const cJSON *o, const char *path);          /* NULL if not a string */
/* s with %XX for anything but letters, digits and -_.~ (and those in keep) */
void url_enc(char *out, size_t n, const char *s, const char *keep);
