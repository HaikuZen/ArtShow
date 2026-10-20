#pragma once
/* the weather forecast: one of four providers (OpenWeatherMap, WeatherAPI,
 * Weatherbit, Visual Crossing), set in config.json on the microSD card. A task
 * fetches it over HTTPS and refreshes it every few minutes */
#include <stdbool.h>

#define WX_DAYS 7
#define WX_HOURS 24

typedef enum { WX_UNKNOWN, WX_CLEAR, WX_PARTLY, WX_CLOUDY, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_STORM } wx_kind_t;

typedef struct {
    int year, mon, mday, wday;     /* mon 1..12, wday 0 = Sunday */
    float tmin, tmax;
    int pop;                       /* chance of rain or snow, %; < 0: not given */
    wx_kind_t kind;
} wx_day_t;

typedef struct {
    int hour;                      /* 0..23, the place's time */
    float temp;
    int pop;                       /* % */
    wx_kind_t kind;
    bool night;
} wx_hour_t;

typedef struct {
    const char *provider;          /* its name, for the screen */
    bool imperial;                 /* °F and mph, else °C and km/h */
    char place[64];
    char desc[64];                 /* now: "Scattered clouds" */
    wx_kind_t kind;
    bool night;
    float temp, feels, wind;
    int humidity;                  /* % */
    char wind_dir[4];              /* "NE"; "" if not given */
    int ndays;                     /* today first */
    wx_day_t day[WX_DAYS];
    int nhours;                    /* from this hour on; 0: the provider gives none (only WeatherAPI does) */
    wx_hour_t hour[WX_HOURS];
} weather_t;

/* w: the forecast, or NULL; status: what's wrong, or NULL. Runs in the weather
 * task: take the LVGL lock to touch the UI */
typedef void (*weather_cb_t)(const weather_t *w, const char *status);

/* read /sdcard/config.json (a template is written there if there's none); call
 * once after the card mount and nvs_flash_init (the provider picked on the page
 * is kept in NVS). card: slideshow_mount() found one */
void weather_config_load(bool card);
/* start the task; it waits for Wi-Fi */
void weather_start(weather_cb_t cb);
/* fetch now (the page's refresh button) */
void weather_refresh(void);
/* the providers with a key in config.json (up to 4): their names, and in
 * *current which one is in use; 0 if config.json is no good */
int weather_providers(const char **names, int *current);
/* switch to names[k] of weather_providers (kept for the next boot) and fetch now */
void weather_use_provider(int k);
