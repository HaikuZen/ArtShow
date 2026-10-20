#pragma once
/* Elecrow CrowPanel Advanced 7" / 10.1", hardware V1.2 (ESP32-P4) */
#define LCD_H_RES        1024
#define LCD_V_RES        600
#define PIN_LCD_BL       31     /* LCD_BK_EN -> MT9201 backlight driver, PWM */
#define PIN_LCD_RST      41     /* wired, but leave it alone (reset_gpio_num = -1) */
#define PIN_TP_SDA       45
#define PIN_TP_SCL       46
#define PIN_TP_RST       40
#define PIN_TP_INT       42
#define PIN_AMP_SD       30     /* speaker amps enable, ACTIVE LOW, 10k pull-up */
#define PIN_I2S_BCLK     22
#define PIN_I2S_WS       21
#define PIN_I2S_DOUT     23
#define PIN_BOOT_KEY     35     /* BOOT button (GPIO 0/1 are the 32 kHz crystal) */
#define PIN_C6_EN        32     /* ESP32-C6 (Wi-Fi) enable */
#define LDO_CHAN_DSI_PHY 3      /* P4 internal LDO3 at 2.5 V feeds the MIPI D-PHY */
#define LDO_MV_DSI_PHY   2500
#define PIN_SD_CLK       43     /* microSD: SDMMC slot 0 (IOMUX), 1-bit, CS tied to GND */
#define PIN_SD_CMD       44     /*   (Elecrow's P4 SD lesson; IO powered by LDO4 at 3.3 V) */
#define PIN_SD_D0        39
