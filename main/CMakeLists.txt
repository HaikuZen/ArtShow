idf_component_register(SRCS "slideshow.c" "main.c" "board.c" "wifi.c" "wifi_ui.c" "audio.c" "brightness_ui.c" "transition.c" "clock_page.c" "menu.c" "time_sync.c"
                            "weather.c" "weather_icon.c" "weather_page.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_driver_i2c esp_driver_ledc esp_driver_gpio esp_hw_support esp_wifi esp_netif esp_event nvs_flash esp_driver_i2s
                                esp_driver_jpeg esp_driver_sdmmc sdmmc fatfs esp_timer espressif__esp_new_jpeg
                                esp_http_client mbedtls espressif__cjson)
