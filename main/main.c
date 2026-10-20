/* main.c - ArtShow: the pictures on the microSD card as a full-screen
 * slideshow, with the Wi-Fi button (bottom right) and the brightness slider
 * (bottom left) over it. */
#include "board.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "slideshow.h"
#include "brightness_ui.h"
#include "nvs_flash.h"
#include "wifi.h"
#include "audio.h"

static const char *TAG = "app";

/* the brightness is kept in NVS, read before Wi-Fi starts (wifi_start inits it
   again: harmless) */
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
    board_init();
    slideshow_mount();         /* before wifi_start: see slideshow.h */
    nvs_init();
    if (lvgl_port_lock(0)) {   /* every LVGL call from outside the LVGL task holds the lock */
        slideshow_start(lv_screen_active());
        wifi_ui_create(lv_screen_active());   /* after the slideshow: the corner buttons stay on top */
        brightness_ui_create(lv_screen_active());
        lvgl_port_unlock();
    }
    audio_init();
    audio_beep(880, 150);
    wifi_start();
}
