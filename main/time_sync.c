/* time_sync.c - the time zone, and SNTP (lwIP's, through esp_netif). */
#include "time_sync.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "esp_log.h"
#include <stdlib.h>
#include <time.h>

static const char *TAG = "time";
static volatile bool s_synced;

/* in lwIP's task */
static void synced_cb(struct timeval *tv) {
    s_synced = true;
    time_t now = tv->tv_sec;
    struct tm t;
    localtime_r(&now, &t);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &t);
    ESP_LOGI(TAG, "synced: %s (%s)", buf, CONFIG_ARTSHOW_TZ);
}

/* SNTP's first tries fail before Wi-Fi has an IP, and it then waits long to
   retry: ask again as soon as there's one (after every reconnection too) */
static void got_ip_cb(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_netif_sntp_start();
}

void time_sync_tz(void) {
    setenv("TZ", CONFIG_ARTSHOW_TZ, 1);
    tzset();
}

void time_sync_start(void) {
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_ARTSHOW_NTP_SERVER);
    cfg.sync_cb = synced_cb;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) ESP_LOGE(TAG, "SNTP: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "SNTP with %s", CONFIG_ARTSHOW_NTP_SERVER);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_cb, NULL);   /* the default loop: wifi_start made it */
}

bool time_synced(void) { return s_synced; }
