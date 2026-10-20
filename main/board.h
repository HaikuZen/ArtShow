#pragma once
#include "driver/i2c_master.h"

/* power, I2C, backlight, panel, touch and the LVGL task; call once from app_main */
void board_init(void);
/* backlight 0..100 % (0 = off) */
void board_set_brightness(int pct);
/* the backlight's PWM frequency (30 kHz at boot): the Settings page's "Backlight
 * switching", for a screen that flickers when dim */
void board_set_backlight_hz(uint32_t hz);
/* the shared I2C bus (GT911 at 0x5d, the STC8 helper at 0x2f) */
i2c_master_bus_handle_t board_i2c_bus(void);
