/* audio.c - the speaker: plain 16-bit I2S into the board's amps, no codec,
 * no MCLK. The amps' enable (GPIO 30) is ACTIVE LOW: low = on. */
#include "audio.h"
#include "board_pins.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

#define RATE 16000
static i2s_chan_handle_t s_tx;

void audio_init(void) {
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, NULL));
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS,
                      .dout = PIN_I2S_DOUT, .din = I2S_GPIO_UNUSED },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &sc));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
}

void audio_beep(int hz, int ms) {
    static int16_t buf[2 * 256];                   /* stereo frames: the same sample in both slots */
    int frames = RATE * ms / 1000;
    gpio_set_level(PIN_AMP_SD, 0);                 /* amps on */
    for (int done = 0; done < frames; ) {
        int n = frames - done < 256 ? frames - done : 256;
        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t)(6000 * sinf(2 * (float)M_PI * hz * (done + i) / RATE));
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        size_t w;
        i2s_channel_write(s_tx, buf, n * 4, &w, portMAX_DELAY);
        done += n;
    }
    vTaskDelay(pdMS_TO_TICKS(30));                 /* let the DMA drain before the amps go off */
    gpio_set_level(PIN_AMP_SD, 1);                 /* amps off */
}
