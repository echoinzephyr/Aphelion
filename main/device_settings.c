#include "device_settings.h"
#include "device_clock.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

typedef struct {
    const char *iana;
    const char *posix;
} time_zone_t;

static const time_zone_t zones[] = {
    {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Shanghai", "CST-8"},
    {"Asia/Taipei", "CST-8"},
    {"Asia/Singapore", "SGT-8"},
    {"Asia/Tokyo", "JST-9"},
    {"Asia/Seoul", "KST-9"},
    {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Dubai", "GST-4"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
    {"UTC", "UTC0"},
};

static const char *const features[] = {"oled", "env", "sound", "tools"};
enum { FEATURE_MASK = (1 << (sizeof(features) / sizeof(features[0]))) - 1 };
static const char *const nvs_namespace = "aphelion_ui";
typedef struct {
    uint8_t mask;
    char tz[32];
} ui_settings_t;

static ui_settings_t current = {.mask = FEATURE_MASK, .tz = "Asia/Hong_Kong"};
static SemaphoreHandle_t settings_mutex;

static const time_zone_t *find_zone(const char *name)
{
    if (name) {
        for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); ++i) {
            if (!strcmp(name, zones[i].iana)) return &zones[i];
        }
    }
    return NULL;
}

static esp_err_t apply_zone(const time_zone_t *zone)
{
    if (setenv("TZ", zone->posix, 1) != 0) return ESP_ERR_NO_MEM;
    tzset();
    return ESP_OK;
}

esp_err_t device_settings_init(void)
{
    settings_mutex = xSemaphoreCreateMutex();
    if (!settings_mutex) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    if (nvs_open(nvs_namespace, NVS_READONLY, &handle) == ESP_OK) {
        ui_settings_t stored;
        size_t size = sizeof(stored);
        if (nvs_get_blob(handle, "settings", &stored, &size) == ESP_OK &&
            size == sizeof(stored) && memchr(stored.tz, '\0', sizeof(stored.tz)) &&
            find_zone(stored.tz)) {
            current = stored;
        }
        nvs_close(handle);
    }
    return apply_zone(find_zone(current.tz));
}

bool device_local_time(time_t utc, struct tm *local, long *offset)
{
    xSemaphoreTake(settings_mutex, portMAX_DELAY);
    bool ok = localtime_r(&utc, local) != NULL;
    if (ok && offset) {
        struct tm wall = *local;
        *offset = (long)(timegm(&wall) - utc);
    }
    xSemaphoreGive(settings_mutex);
    return ok;
}

static esp_err_t update_settings(const ui_settings_t *next)
{
    xSemaphoreTake(settings_mutex, portMAX_DELAY);
    const bool zone_changed = strcmp(next->tz, current.tz) != 0;
    esp_err_t err = ESP_OK;
    if (zone_changed) err = apply_zone(find_zone(next->tz));
    if (err == ESP_OK && (zone_changed || next->mask != current.mask)) {
        nvs_handle_t handle;
        err = nvs_open(nvs_namespace, NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            err = nvs_set_blob(handle, "settings", next, sizeof(*next));
            if (err == ESP_OK) err = nvs_commit(handle);
            nvs_close(handle);
        }
        if (err == ESP_OK) current = *next;
        else if (zone_changed) apply_zone(find_zone(current.tz));
    }
    xSemaphoreGive(settings_mutex);
    return err;
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

esp_err_t web_settings_get(httpd_req_t *req)
{
    char json[160];
    snprintf(json, sizeof(json),
             "{\"tz\":\"%s\",\"features\":{\"oled\":%s,\"env\":%s,\"sound\":%s,\"tools\":%s}}",
             current.tz, current.mask & 1 ? "true" : "false", current.mask & 2 ? "true" : "false",
             current.mask & 4 ? "true" : "false", current.mask & 8 ? "true" : "false");
    return send_json(req, json);
}

static cJSON *read_json(httpd_req_t *req)
{
    char body[256];
    if (!req->content_len || req->content_len >= sizeof(body)) return NULL;
    size_t used = 0;
    while (used < req->content_len) {
        const int bytes = httpd_req_recv(req, body + used, req->content_len - used);
        if (bytes <= 0) return NULL;
        used += bytes;
    }
    if (memchr(body, '\0', used)) return NULL;
    body[used] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(body, used + 1, NULL, true);
    if (cJSON_IsObject(root)) return root;
    cJSON_Delete(root);
    return NULL;
}

static bool settings_header(httpd_req_t *req)
{
    char header[16];
    return httpd_req_get_hdr_value_str(req, "X-Aphelion", header, sizeof(header)) == ESP_OK &&
           !strcmp(header, "settings");
}

static bool read_zone(const cJSON *root, ui_settings_t *settings)
{
    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "tz");
    if (!tz) return true;
    const time_zone_t *zone = find_zone(cJSON_GetStringValue(tz));
    if (!zone) return false;
    strcpy(settings->tz, zone->iana);
    return true;
}

static bool read_features(const cJSON *root, ui_settings_t *settings)
{
    const cJSON *group = cJSON_GetObjectItemCaseSensitive(root, "features");
    if (!group) return true;
    if (!cJSON_IsObject(group)) return false;
    for (size_t i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
        const cJSON *field = cJSON_GetObjectItemCaseSensitive(group, features[i]);
        if (!field) continue;
        if (!cJSON_IsBool(field)) return false;
        if (cJSON_IsTrue(field)) settings->mask |= 1u << i;
        else settings->mask &= ~(1u << i);
    }
    return true;
}

esp_err_t web_settings_put(httpd_req_t *req)
{
    if (!settings_header(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Use the device WebUI");
    }
    cJSON *root = read_json(req);
    ui_settings_t next = current;
    const bool valid = root && read_zone(root, &next) && read_features(root, &next);
    cJSON_Delete(root);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid device settings");
    if (update_settings(&next) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save device settings");
    }
    return web_settings_get(req);
}

esp_err_t web_time_set(httpd_req_t *req)
{
    if (!settings_header(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Use the device WebUI");
    }
    cJSON *root = read_json(req);
    ui_settings_t next = current;
    const double epoch = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, "epoch_ms"));
    const bool valid = root && isfinite(epoch) && epoch >= 946684800000.0 && epoch < 4102444800000.0 &&
                       floor(epoch) == epoch && read_zone(root, &next);
    cJSON_Delete(root);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid device time");
    if (update_settings(&next) != ESP_OK || !device_clock_set((int64_t)epoch)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not set device time");
    }
    return send_json(req, "{\"ok\":true}");
}
