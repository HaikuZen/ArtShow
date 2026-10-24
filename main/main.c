/* main.c - ArtShow: the pictures on the microSD card as a full-screen
 * slideshow, with the menu (top left: the slideshow, the gallery, a clock page, the
 * weather, the sun and moon, the artwork of the day, generative art, the album
 * wall, settings), "?" (top right: what's on show), the Wi-Fi
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
#include "diffuse_page.h"
#include "sun_moon.h"
#include "gallery.h"
#include "generative.h"
#include "album_wall.h"
#include "albums.h"
#include "settings.h"
#include "settings_page.h"
#include "night.h"
#include "net.h"
#include "menu.h"
#include "time_sync.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "audio.h"
#include "radio.h"
#include "radio_ui.h"

static const char *TAG = "app";

/* the brightness is kept in NVS, read before Wi-Fi starts (wifi_start inits it
   again: harmless) */
/* the menu's slideshow page is the screen itself: hidden means paused (and
   its "?" button, Now Showing, goes with it) */
static void slideshow_shown(bool shown) {
    slideshow_pause(!shown);
    now_showing_slideshow(shown);
    diffuse_page_slideshow(shown);
    radio_ui_slideshow(shown);
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
    if (card) albums_config();       /* ... and the Deezer user ID */
    radio_config(card);              /* ... and the radio's stations */
    if (lvgl_port_lock(0)) {   /* every LVGL call from outside the LVGL task holds the lock */
        lv_obj_t *scr = lv_screen_active();
        slideshow_start(scr);
        now_showing_create(scr);                    /* over the slideshow: its "?" button, top right */
        slideshow_on_show(now_showing_caption);     /* ... and its wall label */
        diffuse_page_create(scr);                   /* ... and the drop left of it */
        radio_ui_create(scr);                       /* ... and the radio's note left of that */
        lv_obj_t *gallery = gallery_create(scr);
        lv_obj_t *clock = clock_page_create(scr);   /* the pages, then the corner buttons over them */
        lv_obj_t *weather = weather_page_create(scr);
        lv_obj_t *sky = sun_moon_create(scr);
        lv_obj_t *gen = generative_create(scr);
        lv_obj_t *albums = album_wall_create(scr);
        lv_obj_t *settings = settings_page_create(scr);
        lv_obj_t *artwork = artwork_page_create(scr);
        wifi_ui_create(scr);
        brightness_ui_create(scr);
        const menu_page_t pages[] = {
            { LV_SYMBOL_IMAGE "  Slideshow", NULL, slideshow_shown, 0x2A2420 },   /* first: the Gallery's menu_select(0) */
            { LV_SYMBOL_LIST "  Gallery", gallery, gallery_shown, 0x7A2228 },
            { LV_SYMBOL_BELL "  Clock", clock, clock_page_shown, 0xD7322B },
            { LV_SYMBOL_CHARGE "  Weather", weather, weather_page_shown, 0x2F4A7A },
            { LV_SYMBOL_LOOP "  Sun and Moon", sky, sun_moon_shown, 0xC8692B },
            { LV_SYMBOL_EYE_OPEN "  Artwork of the Day", artwork, NULL, 0x6B4A2E },
            { LV_SYMBOL_SHUFFLE "  Generative", gen, generative_shown, 0x7A4FA0 },
            { LV_SYMBOL_AUDIO "  Album Wall", albums, album_wall_shown, 0xB08A3E },
            { LV_SYMBOL_SETTINGS "  Settings", settings, settings_page_shown, 0x1C4F8A },
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
