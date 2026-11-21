#pragma once

#include "esp_http_server.h"

bool web_wifi_on_hotspot(httpd_req_t *req);
void web_wifi_ssid_hex(const char *ssid, char out[65]);
esp_err_t web_wifi_status(httpd_req_t *req);
esp_err_t web_wifi_scan_start(httpd_req_t *req);
esp_err_t web_wifi_scan_results(httpd_req_t *req);
esp_err_t web_wifi_join(httpd_req_t *req);
