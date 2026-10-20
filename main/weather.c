/* weather.c - the forecast from one of four providers, over HTTPS. The
 * settings (provider, API keys, place) are in config.json on the microSD card,
 * read once at boot; the task fetches every refresh_minutes and hands a
 * weather_t, the same for every provider, to the page. */
#include "weather.h"
#include "wifi.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define CONFIG_PATH "/sdcard/config.json"
#define MAX_BODY    (256 * 1024)
#define ERR_LEN     160
#define NPROV       4
#define NVS_NS      "artshow"
#define NVS_KEY     "wxprov"   /* the provider picked on the page */

static const char *TAG = "weather";

typedef bool (*fetch_fn)(weather_t *w, char *err, size_t n);

typedef struct {
    const char *id, *name;
    fetch_fn fetch;
    int min_refresh;          /* minutes: the free plan's daily calls */
} provider_t;

static struct {
    const provider_t *prov;   /* in use */
    char key[128];            /* its key: the fetchers read this one */
    char keys[NPROV][128];    /* every key in config.json, "" if none */
    char location[128], place[64], lang[16];
    bool imperial;
    int refresh_min;          /* config.json's; a provider may need more */
    char problem[ERR_LEN];    /* why there's no forecast: shown on the page */
} s_cfg;
static int s_keyed[NPROV], s_nkeyed;   /* the providers with a key */
static volatile int s_want = -1;       /* picked on the page, for the task to switch to */

static weather_cb_t s_cb;
static TaskHandle_t s_task;

/* ---- JSON: a dotted path ("weather.0.id": a digit indexes an array) ---- */

static const cJSON *at(const cJSON *o, const char *path) {
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

static double num(const cJSON *o, const char *path, double def) {
    const cJSON *v = at(o, path);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0]) return atof(v->valuestring);   /* some send "40" */
    return def;
}

static const char *str(const cJSON *o, const char *path) {
    const cJSON *v = at(o, path);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* ---- small helpers ---- */

static void copy(char *dst, size_t n, const char *s) { snprintf(dst, n, "%s", s ? s : ""); }

static void set_desc(weather_t *w, const char *s) {
    copy(w->desc, sizeof w->desc, s);
    w->desc[0] = toupper((unsigned char)w->desc[0]);
}

static void set_compass(weather_t *w, double deg) {
    static const char *d[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    if (deg >= 0) copy(w->wind_dir, sizeof w->wind_dir, d[(int)((fmod(deg, 360) + 22.5) / 45) % 8]);
}

/* a new day at the end of the list; NULL when it's full */
static wx_day_t *new_day(weather_t *w, int y, int m, int d) {
    if (w->ndays == WX_DAYS) return NULL;
    wx_day_t *day = &w->day[w->ndays++];
    struct tm t = { .tm_year = y - 1900, .tm_mon = m - 1, .tm_mday = d, .tm_hour = 12 };
    mktime(&t);
    *day = (wx_day_t){ .year = y, .mon = m, .mday = d, .wday = t.tm_wday, .pop = -1, .tmin = 1e9f, .tmax = -1e9f };
    return day;
}

static wx_day_t *new_day_ymd(weather_t *w, const char *ymd) {
    int y, m, d;
    if (!ymd || sscanf(ymd, "%d-%d-%d", &y, &m, &d) != 3) return NULL;
    return new_day(w, y, m, d);
}

static void url_enc(char *out, size_t n, const char *s) {
    size_t k = 0;
    for (; *s && k + 4 < n; s++) {
        unsigned char c = *s;
        if (isalnum(c) || strchr("-_.~,", c)) out[k++] = c;
        else k += snprintf(out + k, n - k, "%%%02X", c);
    }
    out[k] = 0;
}

/* "45.46,9.19": a place by coordinates */
static bool latlon(double *lat, double *lon) {
    char *e;
    *lat = strtod(s_cfg.location, &e);
    if (e == s_cfg.location || *e != ',') return false;
    const char *s = e + 1;
    *lon = strtod(s, &e);
    return e != s && *e == 0;
}

/* OpenWeatherMap's and Weatherbit's condition codes are the same scheme */
static wx_kind_t code_kind(int id) {
    if (id < 200) return WX_UNKNOWN;
    if (id < 300) return WX_STORM;
    if (id < 400) return WX_DRIZZLE;
    if (id < 600) return WX_RAIN;
    if (id < 700) return WX_SNOW;
    if (id < 800) return WX_FOG;
    if (id == 800) return WX_CLEAR;
    if (id <= 802) return WX_PARTLY;
    if (id <= 804) return WX_CLOUDY;
    if (id == 900) return WX_RAIN;   /* Weatherbit: "unknown precipitation" */
    return WX_UNKNOWN;
}

static wx_kind_t weatherapi_kind(int c) {
    if (c == 1000) return WX_CLEAR;
    if (c == 1003) return WX_PARTLY;
    if (c == 1006 || c == 1009) return WX_CLOUDY;
    if (c == 1030 || c == 1135 || c == 1147) return WX_FOG;
    if (c == 1087 || c >= 1273) return WX_STORM;
    if (c == 1072 || (c >= 1150 && c <= 1171)) return WX_DRIZZLE;
    if (c == 1063 || (c >= 1180 && c <= 1201) || (c >= 1240 && c <= 1246)) return WX_RAIN;
    if (c == 1066 || c == 1069 || c == 1114 || c == 1117 || (c >= 1204 && c <= 1237) || (c >= 1249 && c <= 1264))
        return WX_SNOW;
    return WX_UNKNOWN;
}

/* Visual Crossing's icon names (iconSet=icons2): "partly-cloudy-night", "thunder-rain", ... */
static wx_kind_t vc_kind(const char *icon, bool *night) {
    if (!icon) return WX_UNKNOWN;
    if (night) *night = strstr(icon, "night");
    if (strstr(icon, "thunder")) return WX_STORM;
    if (strstr(icon, "snow") || strstr(icon, "sleet") || strstr(icon, "hail")) return WX_SNOW;
    if (strstr(icon, "rain") || strstr(icon, "showers")) return WX_RAIN;
    if (strstr(icon, "fog")) return WX_FOG;
    if (strstr(icon, "partly")) return WX_PARTLY;
    if (strstr(icon, "cloudy") || strstr(icon, "wind")) return WX_CLOUDY;
    if (strstr(icon, "clear")) return WX_CLEAR;
    return WX_UNKNOWN;
}

/* ---- HTTPS ---- */

/* the body (in PSRAM, free it), or NULL and err. The URL holds the API key:
   never log it */
static char *http_get(const char *url, int *status, char *err, size_t n) {
    const esp_http_client_config_t c = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,   /* Visual Crossing's request line is long */
        .user_agent = "ArtShow",
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) {
        snprintf(err, n, "%s: out of memory", s_cfg.prov->name);
        return NULL;
    }
    char *body = NULL;
    size_t len = 0, cap = 0;
    esp_err_t e = esp_http_client_open(h, 0);
    if (e != ESP_OK) {
        snprintf(err, n, "Can't reach %s (%s)", s_cfg.prov->name, esp_err_to_name(e));
        goto out;
    }
    esp_http_client_fetch_headers(h);
    *status = esp_http_client_get_status_code(h);
    for (;;) {
        if (cap - len < 2048) {
            cap = cap ? cap * 2 : 16 * 1024;
            char *b = cap <= MAX_BODY ? heap_caps_realloc(body, cap, MALLOC_CAP_SPIRAM) : NULL;
            if (!b) {
                snprintf(err, n, "%s: the answer is too big", s_cfg.prov->name);
                free(body);
                body = NULL;
                goto out;
            }
            body = b;
        }
        int r = esp_http_client_read(h, body + len, cap - len - 1);
        if (r < 0) {
            snprintf(err, n, "%s: the connection broke", s_cfg.prov->name);
            free(body);
            body = NULL;
            goto out;
        }
        if (r == 0) break;
        len += r;
    }
    body[len] = 0;
out:
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return body;
}

/* the answer as JSON, or NULL and err: the provider's own message if it sent one */
static cJSON *get_json(const char *url, char *err, size_t n) {
    int status = 0;
    char *body = http_get(url, &status, err, n);
    if (!body) return NULL;
    cJSON *j = cJSON_Parse(body);
    if (status != 200) {
        const char *msg = j ? str(j, "message") : NULL;   /* OpenWeatherMap */
        if (!msg && j) msg = str(j, "error.message");      /* WeatherAPI */
        if (!msg && j) msg = str(j, "error");              /* Weatherbit */
        if (!msg && !j) msg = body;                        /* Visual Crossing: plain text */
        if (status == 204 && !(msg && *msg)) msg = "no data for this location";
        snprintf(err, n, "%s: HTTP %d%s%.100s", s_cfg.prov->name, status, msg && *msg ? ": " : "", msg ? msg : "");
        for (char *p = err; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        cJSON_Delete(j);
        j = NULL;
    } else if (!j) {
        snprintf(err, n, "%s: the answer isn't JSON", s_cfg.prov->name);
    }
    free(body);
    return j;
}

#define UNIT(m, i) (s_cfg.imperial ? (i) : (m))   /* metric or imperial */

/* ---- OpenWeatherMap: the free plan's current weather and 5 days in 3-hour steps ---- */

static void owm_place(char *q, size_t n) {
    double lat, lon;
    char e[200];
    if (latlon(&lat, &lon)) {
        snprintf(q, n, "lat=%.4f&lon=%.4f", lat, lon);
    } else {
        url_enc(e, sizeof e, s_cfg.location);
        snprintf(q, n, "q=%s", e);
    }
}

static bool fetch_owm(weather_t *w, char *err, size_t n) {
    char q[220], url[512];
    owm_place(q, sizeof q);
    snprintf(url, sizeof url, "https://api.openweathermap.org/data/2.5/weather?%s&units=%s&lang=%s&appid=%s",
             q, UNIT("metric", "imperial"), s_cfg.lang, s_cfg.key);
    cJSON *j = get_json(url, err, n);
    if (!j) return false;
    copy(w->place, sizeof w->place, str(j, "name"));
    w->temp = num(j, "main.temp", 0);
    w->feels = num(j, "main.feels_like", w->temp);
    w->humidity = num(j, "main.humidity", 0);
    w->wind = num(j, "wind.speed", 0) * UNIT(3.6, 1);   /* m/s, or mph */
    set_compass(w, num(j, "wind.deg", -1));
    set_desc(w, str(j, "weather.0.description"));
    w->kind = code_kind(num(j, "weather.0.id", 0));
    const char *icon = str(j, "weather.0.icon");        /* "10n": night */
    w->night = icon && strchr(icon, 'n');
    cJSON_Delete(j);

    snprintf(url, sizeof url, "https://api.openweathermap.org/data/2.5/forecast?%s&units=%s&lang=%s&appid=%s",
             q, UNIT("metric", "imperial"), s_cfg.lang, s_cfg.key);
    if (!(j = get_json(url, err, n))) return false;
    /* 3-hour steps into days, in the place's own time; a day's sky is the
       one nearest 13:00 */
    time_t tz = num(j, "city.timezone", 0);
    int best = 99;
    const cJSON *it;
    cJSON_ArrayForEach(it, at(j, "list")) {
        time_t t = (time_t)num(it, "dt", 0) + tz;
        struct tm tm;
        gmtime_r(&t, &tm);
        wx_day_t *d = w->ndays ? &w->day[w->ndays - 1] : NULL;
        if (!d || d->mday != tm.tm_mday) {
            if (!(d = new_day(w, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday))) break;
            d->pop = 0;
            best = 99;
        }
        d->tmin = fminf(d->tmin, num(it, "main.temp_min", d->tmin));
        d->tmax = fmaxf(d->tmax, num(it, "main.temp_max", d->tmax));
        int pop = num(it, "pop", 0) * 100 + 0.5;
        if (pop > d->pop) d->pop = pop;
        if (abs(tm.tm_hour - 13) < best) {
            best = abs(tm.tm_hour - 13);
            d->kind = code_kind(num(it, "weather.0.id", 0));
        }
    }
    cJSON_Delete(j);
    if (w->ndays) {   /* today's steps start now: count the temperature now too */
        w->day[0].tmin = fminf(w->day[0].tmin, w->temp);
        w->day[0].tmax = fmaxf(w->day[0].tmax, w->temp);
    }
    return true;
}

/* ---- WeatherAPI.com: one call, current and days (3 on the free plan) ---- */

static bool fetch_weatherapi(weather_t *w, char *err, size_t n) {
    char q[200], lang[40] = "", url[512];
    url_enc(q, sizeof q, s_cfg.location);   /* a name, or "lat,lon" */
    if (strcmp(s_cfg.lang, "en")) snprintf(lang, sizeof lang, "&lang=%s", s_cfg.lang);
    /* every hour of each day: ~70 KB, for the hourly strip */
    snprintf(url, sizeof url, "https://api.weatherapi.com/v1/forecast.json?key=%s&q=%s&days=%d&aqi=no&alerts=no%s",
             s_cfg.key, q, WX_DAYS, lang);
    cJSON *j = get_json(url, err, n);
    if (!j) return false;
    copy(w->place, sizeof w->place, str(j, "location.name"));
    w->temp = num(j, UNIT("current.temp_c", "current.temp_f"), 0);
    w->feels = num(j, UNIT("current.feelslike_c", "current.feelslike_f"), w->temp);
    w->humidity = num(j, "current.humidity", 0);
    w->wind = num(j, UNIT("current.wind_kph", "current.wind_mph"), 0);
    copy(w->wind_dir, sizeof w->wind_dir, str(j, "current.wind_dir"));
    set_desc(w, str(j, "current.condition.text"));
    w->kind = weatherapi_kind(num(j, "current.condition.code", 0));
    w->night = num(j, "current.is_day", 1) == 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, at(j, "forecast.forecastday")) {
        wx_day_t *d = new_day_ymd(w, str(it, "date"));
        if (!d) break;
        d->tmin = num(it, UNIT("day.mintemp_c", "day.mintemp_f"), 0);
        d->tmax = num(it, UNIT("day.maxtemp_c", "day.maxtemp_f"), 0);
        d->pop = fmax(num(it, "day.daily_chance_of_rain", -1), num(it, "day.daily_chance_of_snow", -1));
        d->kind = weatherapi_kind(num(it, "day.condition.code", 0));
    }
    /* the hours from this one on, across the days */
    double now = num(j, "location.localtime_epoch", 0);
    const cJSON *day, *h;
    cJSON_ArrayForEach(day, at(j, "forecast.forecastday")) {
        cJSON_ArrayForEach(h, at(day, "hour")) {
            int hh;
            const char *t = str(h, "time");   /* "2026-10-20 15:00", local */
            if (w->nhours == WX_HOURS || num(h, "time_epoch", 0) + 3600 <= now) continue;
            if (!t || sscanf(t, "%*d-%*d-%*d %d", &hh) != 1) continue;
            wx_hour_t *x = &w->hour[w->nhours++];
            x->hour = hh;
            x->temp = num(h, UNIT("temp_c", "temp_f"), 0);
            x->pop = fmax(num(h, "chance_of_rain", 0), num(h, "chance_of_snow", 0));
            x->kind = weatherapi_kind(num(h, "condition.code", 0));
            x->night = num(h, "is_day", 1) == 0;
        }
    }
    cJSON_Delete(j);
    return true;
}

/* ---- Weatherbit: current, and 16 days daily (we take 7); 50 calls a day free ---- */

static bool fetch_weatherbit(weather_t *w, char *err, size_t n) {
    char q[220], url[512];
    double lat, lon;
    if (latlon(&lat, &lon)) {
        snprintf(q, sizeof q, "lat=%.4f&lon=%.4f", lat, lon);
    } else {
        char e[200];
        url_enc(e, sizeof e, s_cfg.location);
        snprintf(q, sizeof q, "city=%s", e);
    }
    snprintf(url, sizeof url, "https://api.weatherbit.io/v2.0/current?%s&units=%s&lang=%s&key=%s",
             q, UNIT("M", "I"), s_cfg.lang, s_cfg.key);
    cJSON *j = get_json(url, err, n);
    if (!j) return false;
    copy(w->place, sizeof w->place, str(j, "data.0.city_name"));
    w->temp = num(j, "data.0.temp", 0);
    w->feels = num(j, "data.0.app_temp", w->temp);
    w->humidity = num(j, "data.0.rh", 0);
    w->wind = num(j, "data.0.wind_spd", 0) * UNIT(3.6, 1);   /* m/s, or mph */
    copy(w->wind_dir, sizeof w->wind_dir, str(j, "data.0.wind_cdir"));
    set_desc(w, str(j, "data.0.weather.description"));
    w->kind = code_kind(num(j, "data.0.weather.code", 0));
    const char *pod = str(j, "data.0.pod");   /* part of day: "d" or "n" */
    w->night = pod && *pod == 'n';
    cJSON_Delete(j);

    snprintf(url, sizeof url, "https://api.weatherbit.io/v2.0/forecast/daily?%s&units=%s&lang=%s&days=%d&key=%s",
             q, UNIT("M", "I"), s_cfg.lang, WX_DAYS, s_cfg.key);
    if (!(j = get_json(url, err, n))) return false;
    const cJSON *it;
    cJSON_ArrayForEach(it, at(j, "data")) {
        wx_day_t *d = new_day_ymd(w, str(it, "valid_date"));
        if (!d) break;
        d->tmin = num(it, "min_temp", 0);
        d->tmax = num(it, "max_temp", 0);
        d->pop = num(it, "pop", -1);
        d->kind = code_kind(num(it, "weather.code", 0));
    }
    cJSON_Delete(j);
    return true;
}

/* ---- Visual Crossing: one call, current and days; only the fields we show ---- */

static bool fetch_visualcrossing(weather_t *w, char *err, size_t n) {
    char q[200], url[640];
    url_enc(q, sizeof q, s_cfg.location);   /* a name, or "lat,lon" */
    snprintf(url, sizeof url,
             "https://weather.visualcrossing.com/VisualCrossingWebServices/rest/services/timeline/%s/next%ddays"
             "?unitGroup=%s&include=days,current&iconSet=icons2&lang=%s&contentType=json"
             "&elements=datetime,tempmax,tempmin,temp,feelslike,humidity,windspeed,winddir,precipprob,conditions,icon"
             "&key=%s",
             q, WX_DAYS - 1, UNIT("metric", "us"), s_cfg.lang, s_cfg.key);
    cJSON *j = get_json(url, err, n);
    if (!j) return false;
    copy(w->place, sizeof w->place, str(j, "resolvedAddress"));
    const cJSON *cur = at(j, "currentConditions");
    if (!cJSON_IsObject(cur)) cur = at(j, "days.0");
    w->temp = num(cur, "temp", 0);
    w->feels = num(cur, "feelslike", w->temp);
    w->humidity = num(cur, "humidity", 0);
    w->wind = num(cur, "windspeed", 0);   /* km/h, or mph */
    set_compass(w, num(cur, "winddir", -1));
    set_desc(w, str(cur, "conditions"));
    w->kind = vc_kind(str(cur, "icon"), &w->night);
    const cJSON *it;
    cJSON_ArrayForEach(it, at(j, "days")) {
        wx_day_t *d = new_day_ymd(w, str(it, "datetime"));
        if (!d) break;
        d->tmin = num(it, "tempmin", 0);
        d->tmax = num(it, "tempmax", 0);
        d->pop = num(it, "precipprob", -1);
        d->kind = vc_kind(str(it, "icon"), NULL);
    }
    cJSON_Delete(j);
    return true;
}

static const provider_t s_providers[NPROV] = {
    { "openweathermap", "OpenWeatherMap", fetch_owm, 10 },
    { "weatherapi", "WeatherAPI", fetch_weatherapi, 10 },
    { "weatherbit", "Weatherbit", fetch_weatherbit, 60 },        /* 50 calls a day, 2 per refresh */
    { "visualcrossing", "Visual Crossing", fetch_visualcrossing, 15 },
};

/* ---- config.json ---- */

static const char TEMPLATE[] =
    "{\n"
    "  \"weather\": {\n"
    "    \"provider\": \"openweathermap\",\n"
    "    \"providers\": \"openweathermap, weatherapi, weatherbit, visualcrossing\",\n"
    "    \"location\": \"Rome,IT\",\n"
    "    \"place_name\": \"\",\n"
    "    \"units\": \"metric\",\n"
    "    \"lang\": \"en\",\n"
    "    \"refresh_minutes\": 30,\n"
    "    \"api_keys\": {\n"
    "      \"openweathermap\": \"\",\n"
    "      \"weatherapi\": \"\",\n"
    "      \"weatherbit\": \"\",\n"
    "      \"visualcrossing\": \"\"\n"
    "    }\n"
    "  }\n"
    "}\n";

static int saved_provider(void) {
    char id[24] = "";
    size_t n = sizeof id;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, NVS_KEY, id, &n);
        nvs_close(h);
    }
    for (int k = 0; k < NPROV; k++)
        if (!strcmp(id, s_providers[k].id)) return k;
    return -1;
}

static void use(int k) {
    s_cfg.prov = &s_providers[k];
    copy(s_cfg.key, sizeof s_cfg.key, s_cfg.keys[k]);
}

static void config_parse(const char *text) {
    cJSON *j = cJSON_Parse(text);
    if (!j) {
        snprintf(s_cfg.problem, ERR_LEN, "config.json isn't valid JSON, near: %.40s", cJSON_GetErrorPtr());
        return;
    }
    const cJSON *wx = at(j, "weather");
    if (!cJSON_IsObject(wx)) {
        snprintf(s_cfg.problem, ERR_LEN, "config.json has no \"weather\" section");
        cJSON_Delete(j);
        return;
    }
    const char *id = str(wx, "provider");
    int want = -1;
    for (int k = 0; k < NPROV; k++) {
        char path[48];
        snprintf(path, sizeof path, "api_keys.%s", s_providers[k].id);
        copy(s_cfg.keys[k], sizeof s_cfg.keys[k], str(wx, path));
        if (s_cfg.keys[k][0]) s_keyed[s_nkeyed++] = k;
        if (id && !strcasecmp(id, s_providers[k].id)) want = k;
    }
    copy(s_cfg.location, sizeof s_cfg.location, str(wx, "location"));
    copy(s_cfg.place, sizeof s_cfg.place, str(wx, "place_name"));
    const char *lang = str(wx, "lang");
    url_enc(s_cfg.lang, sizeof s_cfg.lang, lang && *lang ? lang : "en");
    const char *units = str(wx, "units");
    s_cfg.imperial = units && !strcasecmp(units, "imperial");
    s_cfg.refresh_min = num(wx, "refresh_minutes", 30);
    cJSON_Delete(j);

    /* the one picked on the page, else config.json's, else the first with a key */
    int saved = saved_provider();
    if (saved >= 0 && s_cfg.keys[saved][0]) want = saved;
    if (want < 0 || !s_cfg.keys[want][0]) want = s_nkeyed ? s_keyed[0] : want;
    if (!s_nkeyed && want >= 0)
        snprintf(s_cfg.problem, ERR_LEN, "config.json: no API key for %s (\"api_keys\" > \"%s\")",
                 s_providers[want].name, s_providers[want].id);
    else if (!s_nkeyed)
        snprintf(s_cfg.problem, ERR_LEN, "config.json: no API key: put one under \"api_keys\"");
    else if (!s_cfg.location[0])
        snprintf(s_cfg.problem, ERR_LEN, "config.json: no \"location\" (a city, \"Rome,IT\", or \"lat,lon\")");
    else {
        use(want);
        ESP_LOGI(TAG, "%s (%d with a key), %s, %s, every %d min", s_cfg.prov->name, s_nkeyed, s_cfg.location,
                 s_cfg.imperial ? "imperial" : "metric", s_cfg.refresh_min);   /* not the key */
    }
}


/* cJSON's nodes in PSRAM: WeatherAPI's hourly answer parses into ~3000 small
   allocations, which would otherwise take ~200 KB of internal RAM */
static void *psram_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

void weather_config_load(bool card) {
    cJSON_InitHooks(&(cJSON_Hooks){ .malloc_fn = psram_malloc, .free_fn = free });
    if (!card) {
        snprintf(s_cfg.problem, ERR_LEN, "No microSD card: the weather's settings are in config.json on it");
        return;
    }
    FILE *f = fopen(CONFIG_PATH, "rb");
    if (!f) {
        f = fopen(CONFIG_PATH, "wb");
        bool ok = f && fwrite(TEMPLATE, 1, sizeof TEMPLATE - 1, f) == sizeof TEMPLATE - 1;
        if (f) fclose(f);
        snprintf(s_cfg.problem, ERR_LEN, ok ? "There was no config.json on the card: there's one now. On a computer, "
                                              "set a provider and its API key in it, then restart"
                                            : "No config.json on the card, and it can't be written there");
        ESP_LOGW(TAG, "%s", s_cfg.problem);
        return;
    }
    char *text = calloc(1, 16 * 1024);
    if (text) fread(text, 1, 16 * 1024 - 1, f);
    fclose(f);
    if (text) config_parse(text);
    else snprintf(s_cfg.problem, ERR_LEN, "config.json: out of memory");
    free(text);
    if (s_cfg.problem[0]) ESP_LOGW(TAG, "%s", s_cfg.problem);
}

/* ---- the task ---- */

static void weather_task(void *arg) {
    if (s_cfg.problem[0]) {   /* nothing to fetch until the card is fixed */
        s_cb(NULL, s_cfg.problem);
        s_task = NULL;
        vTaskDelete(NULL);
    }
    static weather_t w;
    static char err[ERR_LEN];
    int backoff = 0;   /* s; after a failure: 1, 2, 4 ... minutes, up to the refresh */
    for (;;) {
        if (!wifi_connected()) {
            s_cb(NULL, "Waiting for Wi-Fi");
            while (!wifi_connected()) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        }
        int want = s_want;
        if (want >= 0) {   /* picked on the page: use it from now on, and next boot */
            s_want = -1;
            use(want);
            backoff = 0;
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                if (nvs_set_str(h, NVS_KEY, s_cfg.prov->id) == ESP_OK) nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGI(TAG, "now %s", s_cfg.prov->name);
        }
        memset(&w, 0, sizeof w);
        w.provider = s_cfg.prov->name;
        w.imperial = s_cfg.imperial;
        err[0] = 0;
        int wait = (s_cfg.refresh_min > s_cfg.prov->min_refresh ? s_cfg.refresh_min : s_cfg.prov->min_refresh) * 60;
        if (s_cfg.prov->fetch(&w, err, sizeof err)) {
            if (s_cfg.place[0]) copy(w.place, sizeof w.place, s_cfg.place);
            ESP_LOGI(TAG, "%s: %.1f, %s, %d days", w.place, w.temp, w.desc, w.ndays);
            s_cb(&w, NULL);
            backoff = 0;
        } else {
            ESP_LOGW(TAG, "%s", err);
            s_cb(NULL, err);
            backoff = backoff ? backoff * 2 : 60;
            if (backoff < wait) wait = backoff;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait * 1000));
    }
}

void weather_start(weather_cb_t cb) {
    s_cb = cb;
    if (xTaskCreate(weather_task, "weather", 12 * 1024, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "no task");
    }
}

void weather_refresh(void) {
    if (s_task) xTaskNotifyGive(s_task);
}

int weather_providers(const char **names, int *current) {
    for (int k = 0; k < s_nkeyed; k++) {
        names[k] = s_providers[s_keyed[k]].name;
        if (s_cfg.prov == &s_providers[s_keyed[k]]) *current = k;
    }
    return s_cfg.problem[0] ? 0 : s_nkeyed;
}

void weather_use_provider(int k) {
    if (k < 0 || k >= s_nkeyed) return;
    s_want = s_keyed[k];
    weather_refresh();
}
