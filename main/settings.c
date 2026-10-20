/* settings.c - the settings, in NVS. A version number in front: a blob of
 * another size (an older layout) is dropped for the defaults. */
#include "settings.h"
#include "nvs.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

#define NVS_NS     "artshow"
#define NVS_KEY    "settings"
#define NVS_SKIP   "skipdirs"
#define VERSION    1

static const char *TAG = "settings";

/* menuconfig's bools are missing from sdkconfig.h when off */
#ifndef CONFIG_ARTSHOW_VARIED_TRANSITIONS
#define CONFIG_ARTSHOW_VARIED_TRANSITIONS 0
#endif
#ifndef CONFIG_ARTSHOW_SHUFFLE
#define CONFIG_ARTSHOW_SHUFFLE 0
#endif
#ifndef CONFIG_ARTSHOW_FILL
#define CONFIG_ARTSHOW_FILL 0
#endif

settings_t g_settings = {
    .interval_s = CONFIG_ARTSHOW_INTERVAL_S,
    .fade_ms = CONFIG_ARTSHOW_FADE_MS,
    .varied = CONFIG_ARTSHOW_VARIED_TRANSITIONS,
    .shuffle = CONFIG_ARTSHOW_SHUFFLE,
    .fill = CONFIG_ARTSHOW_FILL,
    .night_mode = NIGHT_OFF,
    .night_from = 23 * 60,
    .night_to = 7 * 60,
    .night_level = 0,
    .backlight_hz = 30000,   /* board.c's at boot */
};

typedef struct {
    uint32_t version;
    settings_t s;
} blob_t;

/* the folders switched off, as "\nA\nB\n" ("" for none): each is "\nname\n" */
static char s_skip[1024];
static SemaphoreHandle_t s_lock;

void settings_load(void) {
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    /* new fields go at the end: an older, shorter blob fills the start and the
       defaults stay in the rest */
    blob_t b = { .version = VERSION, .s = g_settings };
    size_t n = sizeof b;
    if (nvs_get_blob(h, NVS_KEY, &b, &n) == ESP_OK && n <= sizeof b && b.version == VERSION) g_settings = b.s;
    if (g_settings.night_level > 0 && g_settings.night_level < BL_STEADY_MIN) g_settings.night_level = BL_STEADY_MIN;   /* kept from before the floor */
    n = sizeof s_skip;
    if (nvs_get_str(h, NVS_SKIP, s_skip, &n) != ESP_OK) s_skip[0] = 0;
    nvs_close(h);
    ESP_LOGI(TAG, "%ld s a picture, %ld ms changes%s%s%s; night %ld", (long)g_settings.interval_s, (long)g_settings.fade_ms,
             g_settings.varied ? ", varied" : "", g_settings.shuffle ? ", shuffled" : "", g_settings.fill ? ", filling" : "",
             (long)g_settings.night_mode);
}

void settings_save(void) {
    blob_t b = { .version = VERSION, .s = g_settings };
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, NVS_KEY, &b, sizeof b) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

bool settings_folder_on(const char *folder) {
    char key[130];
    snprintf(key, sizeof key, "\n%s\n", folder);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool off = strstr(s_skip, key);
    xSemaphoreGive(s_lock);
    return !off;
}

void settings_folder_set(const char *folder, bool on) {
    char key[130];
    snprintf(key, sizeof key, "\n%s\n", folder);
    size_t k = strlen(key) - 1;   /* "\nname": what goes, the "\n" after it stays */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char *p = strstr(s_skip, key);
    if (p) memmove(p, p + k, strlen(p + k) + 1);
    if (!strcmp(s_skip, "\n")) s_skip[0] = 0;
    if (!on && strlen(s_skip) + k + 1 < sizeof s_skip) {
        if (!s_skip[0]) strcpy(s_skip, "\n");
        strcat(s_skip, key + 1);   /* "name\n" after the "\n" ending the list */
    }
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, NVS_SKIP, s_skip) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "folder \"%s\" %s", folder, on ? "on" : "off");
}
