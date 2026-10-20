#pragma once
/* Wi-Fi through the C6. A Wi-Fi task does the work; every call here returns at once */
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

#define WIFI_MAX_NETS 20

typedef enum { WIFI_OFF, WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_RETRYING } wifi_state_t;
typedef struct { char ssid[33]; int rssi; bool open; } wifi_net_t;
/* both callbacks run in the Wi-Fi task: take the LVGL lock to touch the UI.
 * detail: the IP when connected, the reason when retrying */
typedef void (*wifi_status_cb_t)(wifi_state_t state, const char *ssid, const char *detail);
typedef void (*wifi_scan_cb_t)(const wifi_net_t *nets, int n);   /* n < 0: the scan failed */

/* bring up Wi-Fi, log one scan, then join the network saved on screen (NVS), else the
 * one in menuconfig (CrowPanel Wi-Fi), and rejoin after each drop */
void wifi_start(void);
bool wifi_connected(void);
/* the network's name and our IP ("-" when not connected); false: not connected.
 * esp_netif only, no call to the C6: any task */
bool wifi_info(char *ssid, size_t ns, char *ip, size_t ni);
/* set before wifi_start to see every change */
void wifi_on_status(wifi_status_cb_t cb);
void wifi_scan(wifi_scan_cb_t cb);
/* join this network instead; saved in NVS once it connects (a wrong password never is) */
void wifi_join(const char *ssid, const char *password);
/* drop and erase the saved network (the menuconfig one, if any, is back at the next boot) */
void wifi_forget(void);
/* the screen's Wi-Fi button and setup panel (wifi_ui.c); call holding the LVGL lock */
void wifi_ui_create(lv_obj_t *parent);
