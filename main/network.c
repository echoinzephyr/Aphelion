#include "network.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "network";
enum { HAS_IP = BIT0, DISCONNECTED = BIT1, STA_STOPPED = BIT2 };
static EventGroupHandle_t events;
static QueueHandle_t requests;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static network_info_t info;
static esp_netif_t *station;
static wifi_scan_list_t nearby;
typedef struct {
    bool scan;
    wifi_credentials_t credentials;
} network_command_t;

network_info_t network_snapshot(void)
{
    portENTER_CRITICAL(&lock);
    network_info_t copy = info;
    portEXIT_CRITICAL(&lock);
    return copy;
}

bool network_join(const wifi_credentials_t *credentials)
{
    network_command_t command = { .credentials = *credentials };
    return xQueueSend(requests, &command, 0) == pdTRUE;
}

bool network_scan_request(void)
{
    portENTER_CRITICAL(&lock);
    bool queued = nearby.busy;
    nearby.busy = true;
    portEXIT_CRITICAL(&lock);
    if (queued) return true;
    network_command_t command = { .scan = true };
    if (xQueueSend(requests, &command, 0) == pdTRUE) return true;
    portENTER_CRITICAL(&lock);
    nearby.busy = false;
    portEXIT_CRITICAL(&lock);
    return false;
}

void network_scan_snapshot(wifi_scan_list_t *result)
{
    portENTER_CRITICAL(&lock);
    *result = nearby;
    portEXIT_CRITICAL(&lock);
}

static const char *security_name(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "other";
    }
}

esp_err_t network_collect_wifi(wifi_scan_list_t *list, bool collapse_by_name)
{
    memset(list, 0, sizeof(*list));
    uint16_t total = 0;
    esp_err_t err = esp_wifi_scan_get_ap_num(&total);
    for (unsigned i = 0; err == ESP_OK && i < total && list->count < MAX_NETWORKS; ++i) {
        wifi_ap_record_t ap;
        err = esp_wifi_scan_get_ap_record(&ap);
        if (err != ESP_OK || (collapse_by_name && !ap.ssid[0])) continue;
        const char *name = (const char *)ap.ssid;
        const char *security = security_name(ap.authmode);
        unsigned j = 0;
        while (collapse_by_name && j < list->count &&
               (strcmp(list->items[j].ssid, name) || strcmp(list->items[j].security, security))) {
            ++j;
        }
        if (collapse_by_name && j < list->count) continue; // records arrive strongest first
        nearby_network_t *item = &list->items[list->count++];
        strlcpy(item->ssid, name, sizeof(item->ssid));
        item->rssi = ap.rssi;
        item->channel = ap.primary;
        memcpy(item->bssid, ap.bssid, sizeof(item->bssid));
        item->security = security;
        item->supported = strcmp(security, "other") != 0;
    }
    esp_wifi_clear_ap_list();
    list->failed = err != ESP_OK;
    list->scanned_us = esp_timer_get_time();
    return err;
}

static void scan_networks(void)
{
    static wifi_scan_list_t list;
    const wifi_scan_config_t config = { .scan_time.active.max = 120 };
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_scan_start(&config, true);
    if (err == ESP_OK) {
        err = network_collect_wifi(&list, true);
    } else {
        list = (wifi_scan_list_t){ .failed = true, .scanned_us = esp_timer_get_time() };
    }
    portENTER_CRITICAL(&lock);
    nearby = list;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "Scan: %u networks (%s)", list.count, esp_err_to_name(err));
}

static void set_status(network_phase_t phase, const char *ip, bool saved)
{
    portENTER_CRITICAL(&lock);
    info.phase = phase;
    info.saved = saved;
    strlcpy(info.ip, ip, sizeof(info.ip));
    portEXIT_CRITICAL(&lock);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(events, HAS_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(events, DISCONNECTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        xEventGroupSetBits(events, STA_STOPPED);
    }
}

static esp_err_t start_access_point(nvs_handle_t storage)
{
    wifi_config_t ap = { .ap = {
        .channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK,
    }};
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "AP MAC");
    network_info_t initial = { .phase = NET_SETUP };
    snprintf(initial.ap, sizeof(initial.ap), "Aphelion-%02X%02X", mac[4], mac[5]);
    size_t key_size = sizeof(initial.key);
    if (nvs_get_str(storage, "ap_key", initial.key, &key_size) != ESP_OK || key_size != sizeof(initial.key)) {
        static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        uint8_t random[12];
        esp_fill_random(random, sizeof(random)); // RF is running
        for (size_t i = 0; i < sizeof(random); ++i) {
            initial.key[i] = alphabet[random[i] & 31];
        }
        initial.key[12] = 0;
        ESP_RETURN_ON_ERROR(nvs_set_str(storage, "ap_key", initial.key), TAG, "save setup key");
        ESP_RETURN_ON_ERROR(nvs_commit(storage), TAG, "commit setup key");
    }
    strlcpy((char *)ap.ap.ssid, initial.ap, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, initial.key, sizeof(ap.ap.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "enable AP");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "AP config");
    xEventGroupWaitBits(events, STA_STOPPED, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
    portENTER_CRITICAL(&lock);
    info = initial;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "Setup AP %s at http://192.168.4.1", initial.ap);
    return ESP_OK;
}

static esp_err_t start_network(nvs_handle_t *storage)
{
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "NVS init");
    ESP_RETURN_ON_ERROR(nvs_open("aphelion_net", NVS_READWRITE, storage), TAG, "open settings");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "TCP/IP");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    station = esp_netif_create_default_wifi_sta();
    if (!station || !esp_netif_create_default_wifi_ap()) {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi driver");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "RAM config");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL), TAG, "Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL), TAG, "IP events");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "radio mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "radio start");
    ESP_RETURN_ON_ERROR(start_access_point(*storage), TAG, "setup AP");
    return ESP_OK;
}

esp_err_t network_start_hotspot(void)
{
    events = xEventGroupCreate();
    if (!events) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t storage = 0;
    esp_err_t err = start_network(&storage);
    if (storage) {
        nvs_close(storage);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    return err;
}

static bool connect_station(const wifi_credentials_t *credentials, char ip[16])
{
    wifi_config_t config = {0};
    // IDF uses fixed-width fields, including non-NUL-terminated 32-byte SSIDs.
    memcpy(config.sta.ssid, credentials->ssid, strlen(credentials->ssid));
    memcpy(config.sta.password, credentials->password, strlen(credentials->password));
    config.sta.threshold.authmode = credentials->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    xEventGroupClearBits(events, HAS_IP | DISCONNECTED | STA_STOPPED);
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        return false;
    }
    EventBits_t result = xEventGroupWaitBits(events, HAS_IP | DISCONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    esp_netif_ip_info_t addresses;
    if (!(result & HAS_IP) || (result & DISCONNECTED) ||
        esp_netif_get_ip_info(station, &addresses) != ESP_OK || !addresses.ip.addr) {
        return false;
    }
    snprintf(ip, 16, IPSTR, IP2STR(&addresses.ip));
    return true;
}

void network_task(void *arg)
{
    nvs_handle_t storage;
    requests = xQueueCreate(2, sizeof(network_command_t));
    events = xEventGroupCreate();
    if (!requests || !events || start_network(&storage) != ESP_OK || web_start() != ESP_OK) {
        ESP_LOGE(TAG, "Network startup failed");
        set_status(NET_FAILED, "", false);
        vTaskDelete(NULL);
        return;
    }

    wifi_credentials_t saved = {0};
    size_t len = sizeof(saved);
    bool has_saved = nvs_get_blob(storage, "station", &saved, &len) == ESP_OK &&
                     len == sizeof(saved) && saved.ssid[0] && !saved.ssid[32] && !saved.password[64];
    if (!has_saved) {
        network_scan_request(); // first-time setup can show nearby networks immediately
    }
    bool online = false;
    int64_t retry_us = 0;
    for (;;) {
        network_command_t cmd = {0};
        bool submitted = xQueueReceive(requests, &cmd, pdMS_TO_TICKS(250)) == pdTRUE;
        if (submitted && cmd.scan) {
            scan_networks();
            continue;
        }
        if (online) {
            if (!submitted && !(xEventGroupGetBits(events) & DISCONNECTED)) continue;
            esp_wifi_set_mode(WIFI_MODE_AP);
            xEventGroupWaitBits(events, STA_STOPPED, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
            online = false;
            set_status(NET_FAILED, "", has_saved);
            retry_us = esp_timer_get_time() + 2000000;
        }
        if (!submitted) {
            if (!has_saved || esp_timer_get_time() < retry_us) continue;
            cmd.credentials = saved;
        }
        set_status(NET_CONNECTING, "", has_saved);
        char ip[16];
        if (connect_station(&cmd.credentials, ip)) {
            bool persisted = true;
            if (submitted) {
                persisted = nvs_set_blob(storage, "station", &cmd.credentials, sizeof(saved)) == ESP_OK &&
                            nvs_commit(storage) == ESP_OK;
                if (persisted) {
                    saved = cmd.credentials;
                    has_saved = true;
                }
            }
            portENTER_CRITICAL(&lock);
            memcpy(info.connected_ssid, cmd.credentials.ssid, sizeof(info.connected_ssid));
            portEXIT_CRITICAL(&lock);
            set_status(NET_ONLINE, ip, persisted);
            online = true;
            ESP_LOGI(TAG, "Station online at http://%s (saved=%d)", ip, persisted);
        } else {
            ESP_LOGW(TAG, "Station connection failed; setup AP is available");
            set_status(NET_FAILED, "", has_saved);
            if (esp_wifi_set_mode(WIFI_MODE_AP) == ESP_OK) {
                xEventGroupWaitBits(events, STA_STOPPED, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
            }
            retry_us = esp_timer_get_time() + 20000000;
        }
    }
}
