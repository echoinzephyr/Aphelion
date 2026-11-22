#include "device_clock.h"
#include "device_settings.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"

#define NTP_PRIMARY "ntp.aliyun.com"
#define NTP_SECONDARY "pool.ntp.org"

enum clock_source { CLOCK_UNSET, CLOCK_NTP, CLOCK_BROWSER };
static const char *const TAG = "ntp";
static atomic_int source;

static void on_sync(struct timeval *time)
{
    atomic_store(&source, CLOCK_NTP);
    ESP_LOGI(TAG, "Clock synchronized");
}

// Called by the network task after the station has an IP and DNS configuration.
void device_clock_sync(void)
{
    static bool initialized;
    if (!initialized) {
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
        esp_sntp_config_t config =
            ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST(NTP_PRIMARY, NTP_SECONDARY));
#else
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_PRIMARY);
#endif
        config.start = false;
        config.wait_for_sync = false;
        config.sync_cb = on_sync;
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not initialize SNTP: %s", esp_err_to_name(err));
            return;
        }
        initialized = true;
    }
    const esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not start SNTP: %s", esp_err_to_name(err));
    }
}

bool device_clock_set(int64_t epoch_ms)
{
    const struct timeval stamp = {
        .tv_sec = epoch_ms / 1000,
        .tv_usec = (epoch_ms % 1000) * 1000,
    };
    if (settimeofday(&stamp, NULL) != 0) return false;
    atomic_store(&source, CLOCK_BROWSER);
    return true;
}

bool device_clock_read(struct tm *local)
{
    return atomic_load(&source) != CLOCK_UNSET && device_local_time(time(NULL), local, NULL);
}

esp_err_t device_clock_status(httpd_req_t *req)
{
    const enum clock_source current_source = atomic_load(&source);
    struct tm local;
    long offset;
    char json[140];
    char stamp[24];
    if (current_source != CLOCK_UNSET && device_local_time(time(NULL), &local, &offset) &&
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &local)) {
        const long minutes = labs(offset) / 60;
        snprintf(json, sizeof(json),
                 "{\"synced\":true,\"local\":\"%s%c%02ld:%02ld\",\"source\":\"%s\",\"offset\":%ld}",
                 stamp, offset < 0 ? '-' : '+', minutes / 60, minutes % 60,
                 current_source == CLOCK_NTP ? "ntp" : "browser", offset);
    } else {
        snprintf(json, sizeof(json), "{\"synced\":false,\"local\":null}");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}
