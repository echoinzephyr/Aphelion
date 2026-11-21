#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    NET_STARTING, NET_SETUP, NET_CONNECTING, NET_ONLINE, NET_FAILED,
} network_phase_t;

typedef struct {
    network_phase_t phase;
    char ap[16];
    char key[13];
    char ip[16];
    char connected_ssid[33];
    bool saved;
} network_info_t;

typedef struct {
    char ssid[33];
    char password[65];
} wifi_credentials_t;

enum { MAX_NETWORKS = 32 };
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t bssid[6];
    const char *security;
    bool supported;
} nearby_network_t;

typedef struct {
    bool busy;
    bool failed;
    int64_t scanned_us;
    unsigned count;
    nearby_network_t items[MAX_NETWORKS];
} wifi_scan_list_t;

// Start the local AP and initialize the station interface for background scans.
esp_err_t network_start_hotspot(void);
esp_err_t network_collect_wifi(wifi_scan_list_t *list, bool collapse_by_name);
bool network_scan_request(void);
void network_scan_snapshot(wifi_scan_list_t *result);
void network_task(void *arg);
network_info_t network_snapshot(void);
bool network_join(const wifi_credentials_t *credentials);
esp_err_t web_start(void);
