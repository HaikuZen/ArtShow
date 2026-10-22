/* music.c - the player: a task on core 0 (the painter is on core 1) renders
 * 256 samples (16 ms) at a time and writes them to the I2S DMA, which paces
 * it. A new piece is handed over under a lock: the synthesizer is one
 * instance, the task's. Stopping fades out over 0.3 s, then the amps go off. */
#include "music.h"
#include "music_synth.h"
#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define BLOCK 256

static const char *TAG = "music";

static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static volatile bool s_want, s_quiet, s_on;   /* on: wanted and not quiet */
static struct {
    bool pending;
    uint32_t seed, pal[5];
    int style;
} s_next;

static void player(void *arg) {
    static int16_t buf[BLOCK];
    bool playing = false;
    float out = 0;   /* the fade: 1 playing, down to 0 stopping */
    for (;;) {
        if (!s_on && !playing) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (s_on && !playing) {
            audio_stream(true);
            playing = true;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_next.pending) {
            s_next.pending = false;
            synth_set(s_next.seed, s_next.style, s_next.pal, NULL, 0);
        }
        xSemaphoreGive(s_lock);
        synth_render(buf, BLOCK, AUDIO_RATE);
        float target = s_on ? 1.0f : 0.0f, step = (target - out) / (0.3f * AUDIO_RATE / BLOCK);
        for (int i = 0; i < BLOCK; i++) buf[i] = (int16_t)(buf[i] * (out + step * i / BLOCK));
        out += step;
        if (out < 0.01f && !s_on) {   /* faded out: silence (the DMA's auto clear), the amps off */
            out = 0;
            playing = false;
            vTaskDelay(pdMS_TO_TICKS(40));
            audio_stream(false);
            continue;
        }
        if (out > 1) out = 1;
        audio_write(buf, BLOCK);
    }
}

void music_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock || xTaskCreatePinnedToCore(player, "music", 3072, NULL, 5, &s_task, 0) != pdPASS)
        ESP_LOGE(TAG, "no task");
}

void music_piece(uint32_t seed, int style, const uint32_t *pal, char *desc, size_t n) {
    if (!s_lock) return;
    char d[64];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_next.seed = seed;
    s_next.style = style;
    memcpy(s_next.pal, pal, sizeof s_next.pal);
    s_next.pending = true;
    xSemaphoreGive(s_lock);
    /* the description only: worked out the same way, without touching the synthesizer the task plays */
    synth_describe(seed, style, pal, d, sizeof d);
    if (desc) snprintf(desc, n, "%s", d);
    ESP_LOGI(TAG, "%s", d);
}

static void update(void) {
    s_on = s_want && !s_quiet;
    if (s_task) xTaskNotifyGive(s_task);
}

void music_play(bool on) {
    s_want = on;
    update();
}

void music_quiet(bool quiet) {
    s_quiet = quiet;
    update();
}
