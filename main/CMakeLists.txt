idf_component_register(SRCS "slideshow.c" "main.c" "board.c" "wifi.c" "wifi_ui.c" "audio.c" "brightness_ui.c" "transition.c" "clock_page.c" "menu.c" "time_sync.c"
                            "weather.c" "weather_icon.c" "weather_page.c"
                            "net.c" "artwork.c" "artwork_page.c" "now_showing.c" "astro.c" "sun_moon.c"
                            "settings.c" "settings_page.c" "night.c" "index.c"
                            "thumb.c" "gallery.c" "notes.c"
                            "gen_art.c" "generative.c" "albums.c" "album_wall.c" "music.c" "music_synth.c"
                            "fonts/art_font_16.c" "fonts/art_font_20.c" "fonts/art_font_24.c" "fonts/art_font_32.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_driver_i2c esp_driver_ledc esp_driver_gpio esp_hw_support esp_wifi esp_netif esp_event nvs_flash esp_driver_i2s
                                esp_driver_jpeg esp_driver_sdmmc sdmmc fatfs esp_timer espressif__esp_new_jpeg
                                esp_http_client mbedtls espressif__cjson esp_app_format sqlite3)
# the generated fonts include "lvgl.h" this way
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
