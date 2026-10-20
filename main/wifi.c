/* wifi.c - Wi-Fi through the onboard ESP32-C6 (esp_hosted): the plain
 * esp_wifi API, forwarded to the C6 over SDIO by esp_wifi_remote.
 * One task owns every esp_wifi call (each is an RPC to the C6): it scans, joins
 * the network saved on screen (NVS) or else the menuconfig one, and rejoins
 * after each drop. Everyone else just posts it a message. */
#include "wifi.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define NVS_NS "p4wifi"   /* the network saved from the screen */

static const char *TAG = "wifi";

typedef enum { M_SCAN, M_JOIN, M_FORGET, M_DROPPED, M_GOT_IP } msg_kind_t;
typedef struct {
    msg_kind_t kind;
    wifi_scan_cb_t scan_cb;
    int reason;
    esp_ip4_addr_t ip;
    char ssid[33], pass[65];
} msg_t;

static QueueHandle_t s_q;
static wifi_status_cb_t s_status_cb;
static volatile bool s_connected;
/* the Wi-Fi task's own state */
static char s_ssid[33], s_pass[65];
static bool s_have, s_save, s_leaving, s_retry;
static int s_tries;
static TickType_t s_retry_at;

/* a failed step logs and gives up on Wi-Fi only: the rest of the app keeps
   running. The C6 link is unproven, and ESP_ERROR_CHECK would reboot the board */
#define TRY(x) do { esp_err_t e_ = (x); \
        if (e_ != ESP_OK) { ESP_LOGE(TAG, "%s: %s - no Wi-Fi", #x, esp_err_to_name(e_)); status(WIFI_OFF, esp_err_to_name(e_)); return; } } while (0)

static void status(wifi_state_t st, const char *detail) {
    s_connected = st == WIFI_CONNECTED;
    if (s_status_cb) s_status_cb(st, s_ssid, detail ? detail : "");
}

static void post(const msg_t *m) {
    if (!s_q || xQueueSend(s_q, m, 0) != pdTRUE) ESP_LOGW(TAG, "Wi-Fi not running, or busy: dropped a request");
}

bool wifi_connected(void) { return s_connected; }
void wifi_on_status(wifi_status_cb_t cb) { s_status_cb = cb; }
void wifi_scan(wifi_scan_cb_t cb) { post(&(msg_t){ .kind = M_SCAN, .scan_cb = cb }); }
void wifi_forget(void) { post(&(msg_t){ .kind = M_FORGET }); }

void wifi_join(const char *ssid, const char *password) {
    msg_t m = { .kind = M_JOIN };
    snprintf(m.ssid, sizeof m.ssid, "%s", ssid);
    snprintf(m.pass, sizeof m.pass, "%s", password ? password : "");
    post(&m);
}

static void do_scan(wifi_scan_cb_t cb) {
    static wifi_ap_record_t ap[WIFI_MAX_NETS];
    static wifi_net_t nets[WIFI_MAX_NETS];
    uint16_t total = 0, max = WIFI_MAX_NETS;
    esp_err_t err = esp_wifi_scan_start(NULL, true);   /* blocking, ~2.5 s */
    if (err == ESP_OK) esp_wifi_scan_get_ap_num(&total);
    if (err == ESP_OK) err = esp_wifi_scan_get_ap_records(&max, ap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        if (cb) cb(NULL, -1);
        return;
    }
    int n = 0;   /* strongest first already; one entry per name, hidden ones out */
    for (int i = 0; i < max; i++) {
        const char *name = (const char *)ap[i].ssid;
        bool dup = !name[0];
        for (int j = 0; j < n && !dup; j++) dup = !strcmp(nets[j].ssid, name);
        if (dup) continue;
        snprintf(nets[n].ssid, sizeof nets[n].ssid, "%s", name);
        nets[n].rssi = ap[i].rssi;
        nets[n].open = ap[i].authmode == WIFI_AUTH_OPEN;
        n++;
    }
    ESP_LOGI(TAG, "%u networks, %d listed", total, n);
    for (int i = 0; i < n; i++) ESP_LOGI(TAG, "  %-32s rssi %d%s", nets[i].ssid, nets[i].rssi, nets[i].open ? "  (open)" : "");
    if (cb) cb(nets, n);
}

/* try again in 1, 2, 4 ... up to 30 s */
static void retry_later(int reason) {
    char detail[48];
    s_tries++;
    int wait = s_tries < 6 ? 1 << (s_tries - 1) : 30;
    s_retry = true;
    s_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(wait * 1000);
    ESP_LOGW(TAG, "not connected to \"%s\" (reason %d), retry in %d s", s_ssid, reason, wait);
    snprintf(detail, sizeof detail, "reason %d, retry in %d s", reason, wait);
    status(WIFI_RETRYING, detail);
}

static void apply_and_connect(void) {
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, s_ssid, strlen(s_ssid));
    memcpy(wc.sta.password, s_pass, strlen(s_pass));
    /* with a password, refuse anything weaker than WPA2; WPA3 (SAE) works too */
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
        retry_later(0);   /* no reason code: the call itself failed */
        return;
    }
    ESP_LOGI(TAG, "joining \"%s\"", s_ssid);
    status(WIFI_CONNECTING, NULL);
}

static void save_network(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", s_ssid);
    nvs_set_str(h, "pass", s_pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "\"%s\" saved", s_ssid);
}

static bool load_network(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t ls = sizeof s_ssid, lp = sizeof s_pass;
        bool ok = nvs_get_str(h, "ssid", s_ssid, &ls) == ESP_OK && nvs_get_str(h, "pass", s_pass, &lp) == ESP_OK;
        nvs_close(h);
        if (ok && s_ssid[0]) {
            ESP_LOGI(TAG, "network saved on screen: \"%s\"", s_ssid);
            return true;
        }
    }
    snprintf(s_ssid, sizeof s_ssid, "%s", CONFIG_APP_WIFI_SSID);   /* the menuconfig fallback */
    snprintf(s_pass, sizeof s_pass, "%s", CONFIG_APP_WIFI_PASSWORD);
    return s_ssid[0] != 0;
}

static void handle(const msg_t *m) {
    char detail[48];
    switch (m->kind) {
    case M_SCAN:
        do_scan(m->scan_cb);
        break;
    case M_JOIN:
        if (s_have) { s_leaving = true; esp_wifi_disconnect(); }
        memcpy(s_ssid, m->ssid, sizeof s_ssid);
        memcpy(s_pass, m->pass, sizeof s_pass);
        s_have = s_save = true;
        s_tries = 0;
        s_retry = false;
        apply_and_connect();
        break;
    case M_FORGET: {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
        if (s_have) { s_leaving = true; esp_wifi_disconnect(); }
        s_have = s_retry = s_save = false;
        ESP_LOGI(TAG, "network forgotten");
        status(WIFI_IDLE, "forgotten");
        s_ssid[0] = 0;
        break;
    }
    case M_DROPPED:
        if (m->reason == WIFI_REASON_ASSOC_LEAVE && s_leaving) { s_leaving = false; break; }   /* our own disconnect */
        if (s_have) retry_later(m->reason);
        break;
    case M_GOT_IP:
        s_tries = 0;
        s_leaving = false;
        if (s_save) { save_network(); s_save = false; }
        snprintf(detail, sizeof detail, IPSTR, IP2STR(&m->ip));
        ESP_LOGI(TAG, "connected to \"%s\", IP %s", s_ssid, detail);
        status(WIFI_CONNECTED, detail);
        break;
    }
}

static void wifi_task(void *arg) {
    msg_t m;
    do_scan(NULL);   /* one scan for the log */
    s_have = load_network();
    if (s_have) apply_and_connect();
    else {
        ESP_LOGI(TAG, "no network set (on screen, or idf.py menuconfig > CrowPanel Wi-Fi)");
        status(WIFI_IDLE, NULL);
    }
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_retry) {
            TickType_t now = xTaskGetTickCount();
            wait = (int32_t)(s_retry_at - now) > 0 ? s_retry_at - now : 0;
        }
        if (xQueueReceive(s_q, &m, wait) == pdTRUE) handle(&m);
        else { s_retry = false; apply_and_connect(); }   /* the backoff ran out */
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    msg_t m = { 0 };
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        m.kind = M_DROPPED;
        m.reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
        s_connected = false;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        m.kind = M_GOT_IP;
        m.ip = ((const ip_event_got_ip_t *)data)->ip_info.ip;
    } else {
        return;
    }
    post(&m);
}

void wifi_start(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        TRY(nvs_flash_erase());
        err = nvs_flash_init();
    }
    TRY(err);
    TRY(esp_netif_init());
    TRY(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&cfg));
    TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    TRY(esp_wifi_start());
    TRY(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL));
    TRY(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    s_q = xQueueCreate(8, sizeof(msg_t));
    if (!s_q || xTaskCreate(wifi_task, "wifi", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the Wi-Fi task - no Wi-Fi");
        status(WIFI_OFF, "no memory");
    }
}
