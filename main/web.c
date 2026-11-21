#include "network.h"
#include "oled.h"
#include "sensors.h"
#include "terminal.h"
#include "web_wifi.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_timer.h"

static const char *TAG = "web";
extern const char dashboard[] asm("_binary_dashboard_html_start");

static esp_err_t home(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, dashboard);
}

static void number(char out[16], bool valid, float value)
{
    if (valid && isfinite(value)) {
        snprintf(out, 16, "%.1f", value);
    } else {
        strcpy(out, "null");
    }
}

static esp_err_t status(httpd_req_t *req)
{
    environment_t env;
    sound_t sound;
    sensor_snapshot(&env, &sound);
    char temp[16], rh[16], dbfs[16], spl[16];
    bool audio_valid = sound.status == SAMPLE_OK || sound.status == SAMPLE_CLIPPING;
    number(temp, env.status == SAMPLE_OK, env.celsius);
    number(rh, env.status == SAMPLE_OK, env.humidity);
    number(dbfs, audio_valid, sound.dbfs);
    number(spl, audio_valid, sound.spl);
    static const char *const phases[] = {"starting", "setup", "connecting", "online", "failed"};
    network_info_t net = network_snapshot();
    char connected[65];
    web_wifi_ssid_hex(net.connected_ssid, connected);
    char json[700];
    snprintf(json, sizeof(json),
             "{\"temperature\":%s,\"humidity\":%s,\"dbfs\":%s,\"spl\":%s,"
             "\"environment\":\"%s\",\"sound\":\"%s\",\"uptime\":%" PRId64 ","
             "\"network\":\"%s\",\"ap\":\"%s\",\"ip\":\"%s\",\"setup\":%s,\"saved\":%s,\"ssid\":\"%s\","
             "\"oled_view\":\"%s\"}",
             temp, rh, dbfs, spl, sample_status_label(env.status), sample_status_label(sound.status),
             (int64_t)(esp_timer_get_time() / 1000000), phases[net.phase], net.ap, net.ip,
             web_wifi_on_hotspot(req) ? "true" : "false", net.saved ? "true" : "false", connected,
             net.ip[0] ? terminal_view_name(oled_selected_view()) : "setup");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t virtual_button(httpd_req_t *req)
{
    char marker[8];
    if (httpd_req_get_hdr_value_str(req, "X-Aphelion", marker, sizeof(marker)) != ESP_OK ||
        strcmp(marker, "button") != 0) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Use the device WebUI");
    }
    if (req->content_len) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected body");
    }
    if (network_snapshot().phase != NET_ONLINE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "OLED is showing Wi-Fi setup");
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"oled_view\":\"%s\"}", terminal_view_name(oled_press()));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

esp_err_t web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "server");
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = home},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status},
        {.uri = "/api/button", .method = HTTP_POST, .handler = virtual_button},
        {.uri = "/api/scan", .method = HTTP_POST, .handler = web_wifi_scan_start},
        {.uri = "/api/networks", .method = HTTP_GET, .handler = web_wifi_scan_results},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = web_wifi_join},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]), TAG, "route");
    }
    return ESP_OK;
}
