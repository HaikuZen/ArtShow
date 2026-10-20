/* board.c - Elecrow CrowPanel Advanced 7" / 10.1" (V1.2, ESP32-P4), IDF 6.x:
 * power, I2C, backlight, the EK79007 MIPI-DSI panel, the GT911 touch, LVGL. */
#include "board.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "board";

#define DSI_LANES      2
#define DSI_LANE_MBPS  900    /* Elecrow's verified values (Lesson07, V1.2) */
#define DPI_CLK_MHZ    51     /* 1414 x 645 per frame with porches: ~56 Hz */
/* the D-PHY PLL reference: IDF 6's _DEFAULT is XTAL, which only rev >= 3.0 has;
   a rev v1.x chip abort()s on it inside esp_lcd_new_dsi_bus. PLL_F20M for v1.x */
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#define DSI_PHY_CLK    MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY
#else
#define DSI_PHY_CLK    MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT
#endif
#define BL_PWM_HZ      30000
#define BL_DUTY_BITS   LEDC_TIMER_11_BIT
#define BL_DUTY_FLOOR  200    /* of 2047: below it the MT9201 barely lights */

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;

i2c_master_bus_handle_t board_i2c_bus(void) { return s_i2c; }

static void ldo_up(int chan, int mv) {
    esp_ldo_channel_handle_t h;
    const esp_ldo_channel_config_t c = { .chan_id = chan, .voltage_mv = mv };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&c, &h));   /* kept for the app's lifetime */
}

static void i2c_init(void) {
    const i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_TP_SDA, .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_i2c));
    /* who answers - the first bring-up question. A V1.2 10.1" shows 0x5d (GT911)
       and 0x2f (STC8 battery helper); no ES8311 despite the schematic */
    char seen[96] = ""; int n = 0;
    for (int a = 0x08; a < 0x78 && n < (int)sizeof seen - 6; a++)
        if (i2c_master_probe(s_i2c, a, 20) == ESP_OK) n += snprintf(seen + n, sizeof seen - n, " 0x%02x", a);
    ESP_LOGI(TAG, "I2C scan:%s", n ? seen : " nothing answered");
}

static void backlight_init(void) {
    const ledc_timer_config_t tc = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = BL_DUTY_BITS,
                                     .timer_num = LEDC_TIMER_0, .freq_hz = BL_PWM_HZ, .clk_cfg = LEDC_AUTO_CLK };
    const ledc_channel_config_t cc = { .gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
                                       .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0 };
    ESP_ERROR_CHECK(ledc_timer_config(&tc));
    ESP_ERROR_CHECK(ledc_channel_config(&cc));   /* dark until the first frame is in */
}

void board_set_brightness(int pct) {
    uint32_t duty = pct <= 0 ? 0 : BL_DUTY_FLOOR + (uint32_t)(pct > 100 ? 100 : pct) * (2047 - BL_DUTY_FLOOR) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* the speaker amps off from the start: active LOW enable, pulled up on the board */
static void amp_off(void) {
    const gpio_config_t g = { .pin_bit_mask = 1ULL << PIN_AMP_SD, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&g);
    gpio_set_level(PIN_AMP_SD, 1);
}

static void panel_init(void) {
    ldo_up(LDO_CHAN_DSI_PHY, LDO_MV_DSI_PHY);   /* the D-PHY's supply, before the bus */
    ldo_up(4, 3300);                            /* Elecrow's examples take LDO4 too */

    esp_lcd_dsi_bus_handle_t bus;
    const esp_lcd_dsi_bus_config_t bc = { .bus_id = 0, .num_data_lanes = DSI_LANES,
                                          .phy_clk_src = DSI_PHY_CLK,
                                          .lane_bit_rate_mbps = DSI_LANE_MBPS };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bc, &bus));

    esp_lcd_panel_io_handle_t dbi;
    const esp_lcd_dbi_io_config_t dc = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus, &dc, &dbi));

    const esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565,   /* IDF 6.x; 5.x called it .pixel_format */
        .num_fbs = 2,                              /* LVGL draws into these (avoid_tearing) */
        .video_timing = {
            .h_size = LCD_H_RES, .v_size = LCD_V_RES,
            .hsync_back_porch = 160, .hsync_pulse_width = 70, .hsync_front_porch = 160,
            .vsync_back_porch = 23, .vsync_pulse_width = 10, .vsync_front_porch = 12,
        },
    };
    const ek79007_vendor_config_t vendor = {
        .mipi_config = { .dsi_bus = bus, .dpi_config = &dpi, .lane_num = DSI_LANES },
    };
    const esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = -1,                      /* GPIO 41 is wired; Elecrow's driver leaves it alone */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(dbi, &pc, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));   /* the video stream starts here */
}

static void touch_init(void) {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io));
    /* driver_data naming the address makes the driver run the GT911's address
       latch (INT driven during reset); without it the reads can fail every boot */
    static esp_lcd_touch_io_gt911_config_t addr = { .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS };
    const esp_lcd_touch_config_t tp = {
        .x_max = LCD_H_RES, .y_max = LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST, .int_gpio_num = PIN_TP_INT,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },   /* right on the bench board */
        .driver_data = &addr,
    };
    esp_err_t err = ESP_FAIL;
    for (int i = 1; i <= 5 && err != ESP_OK; i++) {
        err = esp_lcd_touch_new_i2c_gt911(io, &tp, &s_touch);
        if (err != ESP_OK) { ESP_LOGW(TAG, "GT911 attempt %d: %s", i, esp_err_to_name(err)); s_touch = NULL; vTaskDelay(pdMS_TO_TICKS(250)); }
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "no GT911: running without touch");
    else ESP_LOGI(TAG, "GT911 ready");
}

static void lvgl_init(void) {
    const lvgl_port_cfg_t lc = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lc));
    const lvgl_port_display_cfg_t dc = {
        .panel_handle = s_panel,
        .buffer_size = LCD_H_RES * LCD_V_RES,      /* full screen: the panel's own buffers */
        .double_buffer = true,
        .hres = LCD_H_RES, .vres = LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_spiram = true, .direct_mode = true },
    };
    const lvgl_port_display_dsi_cfg_t dsi = { .flags = { .avoid_tearing = true } };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&dc, &dsi);
    if (!disp) { ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed"); abort(); }
    if (s_touch) {
        const lvgl_port_touch_cfg_t tc = { .disp = disp, .handle = s_touch };
        if (!lvgl_port_add_touch(&tc)) ESP_LOGE(TAG, "lvgl_port_add_touch failed");
    }
}

void board_init(void) {
    amp_off();
    i2c_init();
    backlight_init();
    panel_init();
    touch_init();
    lvgl_init();
    board_set_brightness(80);
    ESP_LOGI(TAG, "CrowPanel P4 up: %dx%d, %d lanes x %d Mbps, %d MHz dpi", LCD_H_RES, LCD_V_RES, DSI_LANES, DSI_LANE_MBPS, DPI_CLK_MHZ);
}
