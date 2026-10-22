/* sky_view.c - the canvas, the images showing it, and the painter task:
 * core 1 at idle priority, as the slideshow's decoder (a picture doesn't
 * yield for 0.4-4 s, and at priority 1 beside the artwork's decoder core 1's
 * idle task starved: the task watchdog). A job is the latest wish: an older
 * one not started is dropped */
#include "sky_view.h"
#include "astro.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

#define IMAGES 4

static const char *TAG = "sky";

static uint16_t *s_px;
static lv_image_dsc_t s_dsc;
static lv_obj_t *s_img[IMAGES];
static int s_nimg;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static struct {
    sky_t sky;
    double lat, lon;
    time_t now;
    bool day;
    float mx, my, mr;
} s_job;

static void painter(void *arg) {
    static sky_t sky;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        sky = s_job.sky;
        double lat = s_job.lat, lon = s_job.lon;
        time_t now = s_job.now;
        bool day = s_job.day;
        float mx = s_job.mx, my = s_job.my, mr = s_job.mr;
        xSemaphoreGive(s_lock);
        int64_t t0 = esp_timer_get_time();
        if (day) {
            double alt, az, lit;
            astro_sun_pos(lat, lon, now, &alt, &az);
            sky.sun_alt = alt;
            sky.sun_az = az;
            astro_moon_pos(lat, lon, now, &alt, &az);
            sky.moon_alt = alt;
            sky.moon_az = az;
            sky.moon_elong = astro_moon_phase(now, &lit);
            sky.moon_lit = lit;
        } else {   /* no place or clock yet: a night without a moon */
            sky.sun_alt = -30;
            sky.moon_alt = -90;
            if (!sky.lat) sky.lat = 45;
        }
        sky.minute = (uint32_t)(now / 60);
        sky_paint(&sky, s_px, SKY_W, SKY_H);
        if (day && mr > 0) sky_paint_moon(&sky, s_px, SKY_W, SKY_H, mx, my, mr);
        ESP_LOGD(TAG, "painted in %d ms", (int)((esp_timer_get_time() - t0) / 1000));
        lvgl_port_lock(0);
        for (int k = 0; k < s_nimg; k++) lv_obj_invalidate(s_img[k]);
        lvgl_port_unlock();
    }
}

lv_obj_t *sky_view_image(lv_obj_t *parent) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_px = heap_caps_aligned_alloc(64, SKY_W * SKY_H * 2, MALLOC_CAP_SPIRAM);   /* 64-byte aligned for the PPA */
        if (s_px) {
            memset(s_px, 0, SKY_W * SKY_H * 2);
            s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            s_dsc.header.w = SKY_W;
            s_dsc.header.h = SKY_H;
            s_dsc.header.stride = SKY_W * 2;
            s_dsc.data = (const uint8_t *)s_px;
            s_dsc.data_size = SKY_W * SKY_H * 2;
            if (xTaskCreatePinnedToCore(painter, "sky", 4096, NULL, tskIDLE_PRIORITY, &s_task, 1) != pdPASS) s_task = NULL;
        }
        if (!s_px || !s_task) ESP_LOGE(TAG, "no canvas or no painter");
    }
    lv_obj_t *img = lv_image_create(parent);
    if (s_px) lv_image_set_src(img, &s_dsc);
    if (s_nimg < IMAGES) s_img[s_nimg++] = img;
    return img;
}

void sky_view_paint(const sky_t *base, double lat, double lon, time_t now, bool day, float mx, float my, float mr) {
    if (!s_task) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_job.sky = *base;
    s_job.lat = lat;
    s_job.lon = lon;
    s_job.now = now;
    s_job.day = day;
    s_job.mx = mx;
    s_job.my = my;
    s_job.mr = mr;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

uint32_t sky_view_place(double lat, double lon) {
    return (uint32_t)lround(lat * 100) * 40000u + (uint32_t)lround(lon * 100 + 20000);
}
