/* demo.c - the demo tour: a table of steps, each a page (main.c's menu
 * table, by its index) and how long it stays, run by a 500 ms LVGL timer.
 * It only shows things: it never saves, never touches Wi-Fi or Settings.
 * A touch (LVGL's inactive time falls) pauses it; 2 minutes untouched and
 * it starts again from the slideshow. */
#include "demo.h"
#include "settings.h"
#include "menu.h"
#include "night.h"
#include "now_showing.h"
#include "diffuse_page.h"
#include "diffuse.h"
#include "album_wall.h"
#include "radio_ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

#define TICK_MS    500
#define RESUME_MS  (2 * 60 * 1000)   /* untouched this long: the tour again */
#define START_MS   3000              /* switched on (Settings): a moment to see it */

static const char *TAG = "demo";

/* main.c's pages[] */
enum { SLIDESHOW, GALLERY, CLOCK, WEATHER, SUN_MOON, ARTWORK, GENERATIVE, ALBUM_WALL };

typedef enum { SHOW, NOW_SHOWING, DIFFUSE, ALBUM_SPOT, ALBUM_SHELVES } act_t;

typedef struct {
    act_t act;
    int page;         /* SHOW: the menu's page */
    int ms;
} step_t;

static const step_t s_steps[] = {
    { SHOW, SLIDESHOW, 45000 },     /* three pictures and their wall labels at 15 s */
    { NOW_SHOWING, 0, 15000 },      /* the picture, its story, the record turning */
    { DIFFUSE, 0, 12000 },          /* 8-bit: 0.2 s, then it stays */
    { SHOW, SLIDESHOW, 15000 },
    { SHOW, GALLERY, 10000 },
    { SHOW, CLOCK, 10000 },
    { SHOW, WEATHER, 10000 },
    { SHOW, SUN_MOON, 10000 },
    { SHOW, ARTWORK, 10000 },
    { SHOW, GENERATIVE, 30000 },    /* painting, with its music */
    { ALBUM_SPOT, 0, 14000 },
    { ALBUM_SHELVES, 0, 10000 },
};
#define N_STEPS (int)(sizeof s_steps / sizeof s_steps[0])
#define DIFFUSE_FX "8-bit"          /* quick, and plain to see */

static bool s_on, s_paused;
static int s_step = -1, s_left;
static uint32_t s_idle;   /* LVGL's inactive time at the last tick */

static int fx_by_name(const char *name) {
    for (int k = 0; k < diffuse_nfx; k++)
        if (!strcmp(diffuse_fx[k].name, name)) return k;
    return -1;   /* none: the view opens, nothing runs */
}

static void close_overlays(void) {
    now_showing_open(false);
    diffuse_page_open(false, -1);
}

static void run(int k) {
    const step_t *st = &s_steps[k];
    ESP_LOGI(TAG, "step %d", k);
    s_step = k;
    s_left = st->ms;
    close_overlays();
    switch (st->act) {
    case SHOW:
        menu_select(st->page);
        break;
    case NOW_SHOWING:
        menu_select(SLIDESHOW);
        now_showing_open(true);
        break;
    case DIFFUSE:
        menu_select(SLIDESHOW);
        diffuse_page_open(true, fx_by_name(DIFFUSE_FX));
        break;
    case ALBUM_SPOT:
        menu_select(ALBUM_WALL);
        album_wall_mode(false);
        break;
    case ALBUM_SHELVES:
        menu_select(ALBUM_WALL);
        album_wall_mode(true);
        break;
    }
}

static void begin(int delay_ms) {
    s_paused = false;
    s_step = -1;
    s_left = delay_ms;
    radio_ui_on();   /* the tour has its music (the Generative page and the Album Wall: their own or none) */
}

static void tick(lv_timer_t *t) {
    uint32_t idle = lv_display_get_inactive_time(NULL);
    bool touched = idle < s_idle;
    s_idle = idle;
    if (!g_settings.demo) {
        if (s_on) ESP_LOGI(TAG, "off");
        s_on = false;
        return;
    }
    if (!s_on) {   /* switched on: the switch's own touch doesn't count */
        s_on = true;
        ESP_LOGI(TAG, "on");
        begin(START_MS);
        return;
    }
    if (touched && !s_paused) {
        s_paused = true;
        ESP_LOGI(TAG, "paused: a touch");
    }
    if (s_paused) {
        if (idle < RESUME_MS || night_dark()) return;
        ESP_LOGI(TAG, "on again");
        begin(0);
    }
    if (night_dark()) return;   /* nobody sees it */
    if ((s_left -= TICK_MS) > 0) return;
    if (s_step == N_STEPS - 1) album_wall_mode(false);   /* the page as it opens */
    run((s_step + 1) % N_STEPS);
}

void demo_start(void) {
    s_idle = lv_display_get_inactive_time(NULL);
    lv_timer_create(tick, TICK_MS, NULL);
}
