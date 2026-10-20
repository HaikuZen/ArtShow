#pragma once
/* the wall clock: the time zone, and NTP over Wi-Fi (menuconfig > ArtShow clock) */
#include <stdbool.h>

/* set the time zone and start SNTP; call after wifi_start (which inits esp_netif).
 * SNTP keeps trying until Wi-Fi is up, then re-syncs every hour */
void time_sync_start(void);
/* the clock has been set by NTP at least once since boot */
bool time_synced(void);
