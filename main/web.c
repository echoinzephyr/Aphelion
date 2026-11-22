#include "network.h"
#include "device_clock.h"
#include "device_settings.h"
#include "web_status.h"
#include "web_wifi.h"

#include "esp_check.h"

static const char *const TAG = "web";

esp_err_t web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "server");
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = web_home},
        {.uri = "/api/time", .method = HTTP_GET, .handler = device_clock_status},
        {.uri = "/api/time", .method = HTTP_POST, .handler = web_time_set},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = web_settings_get},
        {.uri = "/api/settings", .method = HTTP_PUT, .handler = web_settings_put},
        {.uri = "/api/status", .method = HTTP_GET, .handler = web_status},
        {.uri = "/api/button", .method = HTTP_POST, .handler = web_oled_button},
        {.uri = "/api/scan", .method = HTTP_POST, .handler = web_wifi_scan_start},
        {.uri = "/api/networks", .method = HTTP_GET, .handler = web_wifi_scan_results},
        {.uri = "/api/wifi/scan", .method = HTTP_POST, .handler = web_wifi_scan_start},
        {.uri = "/api/wifi/networks", .method = HTTP_GET, .handler = web_wifi_scan_results},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = web_wifi_join},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]), TAG, "route");
    }
    return ESP_OK;
}
