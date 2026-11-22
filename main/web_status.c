#include "network.h"
#include "web_status.h"
#include "sdkconfig.h"
#if !CONFIG_APHELION_TOOLBOX || CONFIG_APHELION_TOOLBOX_DISPLAY
#include "oled.h"
#include "sensors.h"
#endif
#include "web_wifi.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#include "esp_http_server.h"
#include "esp_timer.h"

extern const char dashboard[] asm("_binary_dashboard_html_start");

esp_err_t web_home(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, dashboard);
}

#if !CONFIG_APHELION_TOOLBOX || CONFIG_APHELION_TOOLBOX_DISPLAY
static void number(char out[16], bool valid, float value)
{
    if (valid && isfinite(value)) {
        snprintf(out, 16, "%.1f", value);
    } else {
        strcpy(out, "null");
    }
}
#endif

esp_err_t web_status(httpd_req_t *req)
{
    network_info_t net = network_snapshot();
#if !CONFIG_APHELION_TOOLBOX || CONFIG_APHELION_TOOLBOX_DISPLAY
    environment_t env;
    sound_t sound;
    sensor_snapshot(&env, &sound);
    char temp[16], rh[16], dbfs[16], spl[16];
    bool audio_valid = sound.status == SAMPLE_OK || sound.status == SAMPLE_CLIPPING;
    number(temp, env.status == SAMPLE_OK, env.celsius);
    number(rh, env.status == SAMPLE_OK, env.humidity);
    number(dbfs, audio_valid, sound.dbfs);
    number(spl, audio_valid, sound.spl);
    const bool oled = oled_is_ready();
    const char *view = net.ip[0] ? terminal_view_name(oled_selected_view()) : "setup";
    const char *environment = sample_status_label(env.status);
    const char *microphone = sample_status_label(sound.status);
#else
    const char *temp = "null", *rh = "null", *dbfs = "null", *spl = "null";
    const bool oled = false;
    const char *view = "", *environment = "未启用", *microphone = "未启用";
#endif
#if CONFIG_APHELION_TOOLBOX
    const char *build = "工具箱";
    const char *tools = "true";
#else
    const char *build = "传感器终端";
    const char *tools = "false";
#endif
    static const char *const phases[] = {"starting", "setup", "connecting", "online", "failed"};
    char connected[65];
    web_wifi_ssid_hex(net.connected_ssid, connected);
    static uint32_t flash_physical, flash_configured;
    if (!flash_physical) esp_flash_get_physical_size(NULL, &flash_physical);
    if (!flash_configured) esp_flash_get_size(NULL, &flash_configured);
    const esp_partition_t *app = esp_ota_get_running_partition();
    size_t psram_size = 0;
#if CONFIG_SPIRAM
    if (esp_psram_is_initialized()) psram_size = esp_psram_get_size();
#endif
    char json[860];
    snprintf(json, sizeof(json),
             "{\"temperature\":%s,\"humidity\":%s,\"dbfs\":%s,\"spl\":%s,"
             "\"environment\":\"%s\",\"sound\":\"%s\",\"uptime\":%" PRId64 ","
             "\"network\":\"%s\",\"ap\":\"%s\",\"ip\":\"%s\",\"setup\":%s,\"saved\":%s,\"ssid\":\"%s\","
             "\"oled_view\":\"%s\",\"oled\":%s,\"tools\":%s,\"build\":\"%s\",\"heap\":%" PRIu32 ","
             "\"heap_internal\":%zu,\"flash_physical\":%" PRIu32 ",\"flash_configured\":%" PRIu32 ","
             "\"app_partition\":%" PRIu32 ",\"psram\":%zu}",
             temp, rh, dbfs, spl, environment, microphone,
             (int64_t)(esp_timer_get_time() / 1000000), phases[net.phase], net.ap, net.ip,
             web_wifi_on_hotspot(req) ? "true" : "false", net.saved ? "true" : "false", connected,
             view, oled ? "true" : "false", tools, build, esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             flash_physical, flash_configured, app ? app->size : 0, psram_size);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

esp_err_t web_oled_button(httpd_req_t *req)
{
    char marker[8];
    if (httpd_req_get_hdr_value_str(req, "X-Aphelion", marker, sizeof(marker)) != ESP_OK ||
        strcmp(marker, "button") != 0) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Use the device WebUI");
    }
    if (req->content_len) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected body");
    }
#if !CONFIG_APHELION_TOOLBOX || CONFIG_APHELION_TOOLBOX_DISPLAY
    if (!oled_is_ready() || network_snapshot().phase != NET_ONLINE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "OLED unavailable or showing Wi-Fi setup");
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"oled_view\":\"%s\"}", terminal_view_name(oled_press()));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
#else
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "OLED is unavailable");
#endif
}
