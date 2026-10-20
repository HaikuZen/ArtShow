/* main.c - ArtShow: the pictures on the microSD card as a full-screen
 * slideshow, with the menu (top left: the slideshow, the gallery, a clock page, the
 * weather, the sun and moon, the artwork of the day, settings), "?" (top right: what's on show), the Wi-Fi
 * button (bottom right) and the brightness slider (bottom left) over it. */
#include "board.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "slideshow.h"
#include "brightness_ui.h"
#include "clock_page.h"
#include "weather.h"
#include "weather_page.h"
#include "artwork.h"
#include "artwork_page.h"
#include "now_showing.h"
#include "sun_moon.h"
#include "gallery.h"
#include "settings.h"
#include "settings_page.h"
#include "night.h"
#include "net.h"
#include "menu.h"
#include "time_sync.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "audio.h"

static const char *TAG = "app";

/* the brightness is kept in NVS, read before Wi-Fi starts (wifi_start inits it
   again: harmless) */
/* the menu's slideshow page is the screen itself: hidden means paused (and
   its "?" button, Now Showing, goes with it) */
static void slideshow_shown(bool shown) {
    slideshow_pause(!shown);
    now_showing_slideshow(shown);
}

static void nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs: %s", esp_err_to_name(err));
}

void app_main(void) {
    ESP_LOGI(TAG, "ArtShow");
    time_sync_tz();            /* before anything shows or stores a local time */
    board_init();
    bool card = slideshow_mount();   /* before wifi_start: see slideshow.h */
    nvs_init();
    settings_load();                 /* before the slideshow: its order, timing, folders */
    board_set_backlight_hz(g_settings.backlight_hz);
    net_init();
    weather_config_load(card);       /* config.json on the card; the picked provider in NVS */
    if (lvgl_port_lock(0)) {   /* every LVGL call from outside the LVGL task holds the lock */
        lv_obj_t *scr = lv_screen_active();
        slideshow_start(scr);
        now_showing_create(scr);                    /* over the slideshow: its "?" button, top right */
        lv_obj_t *gallery = gallery_create(scr);
        lv_obj_t *clock = clock_page_create(scr);   /* the pages, then the corner buttons over them */
        lv_obj_t *weather = weather_page_create(scr);
        lv_obj_t *sky = sun_moon_create(scr);
        lv_obj_t *settings = settings_page_create(scr);
        lv_obj_t *artwork = artwork_page_create(scr);
        wifi_ui_create(scr);
        brightness_ui_create(scr);
        const menu_page_t pages[] = {
            { LV_SYMBOL_IMAGE "  Slideshow", NULL, slideshow_shown },   /* first: the Gallery's menu_select(0) */
            { LV_SYMBOL_LIST "  Gallery", gallery, gallery_shown },
            { LV_SYMBOL_BELL "  Clock", clock, clock_page_shown },
            { LV_SYMBOL_CHARGE "  Weather", weather, NULL },
            { LV_SYMBOL_LOOP "  Sun and Moon", sky, sun_moon_shown },
            { LV_SYMBOL_EYE_OPEN "  Artwork of the Day", artwork, NULL },
            { LV_SYMBOL_SETTINGS "  Settings", settings, settings_page_shown },
        };
        menu_create(scr, pages, sizeof pages / sizeof pages[0]);
        night_start();
        lvgl_port_unlock();
    }
    audio_init();
    audio_beep(880, 150);
    wifi_start();
    time_sync_start();         /* after wifi_start: it inits esp_netif */
    weather_start(weather_page_update);
    artwork_start(artwork_page_update);
}
