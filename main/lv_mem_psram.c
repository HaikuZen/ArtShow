/* lv_mem_psram.c - LVGL's memory (CONFIG_LV_USE_CUSTOM_MALLOC): from PSRAM
 * first, internal RAM only if PSRAM is out. With the C library's malloc,
 * every object, local style and draw-time buffer under 16 KB
 * (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) went to internal RAM: the pages'
 * objects alone took ~35 KB, and with the boot's TLS connections free
 * internal RAM fell low enough for TLS to fail (-0x008D, PSA's
 * insufficient memory). Wi-Fi, TLS and the SD card's DMA need it more. */
#include "lvgl.h"
#include "esp_heap_caps.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#define PREFER MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
#define ELSE   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;   /* no pools: the system heap */
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size) { return heap_caps_malloc_prefer(size, 2, PREFER, ELSE); }

void *lv_realloc_core(void *p, size_t new_size) { return heap_caps_realloc_prefer(p, new_size, 2, PREFER, ELSE); }

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

#endif
