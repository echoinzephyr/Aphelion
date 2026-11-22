#pragma once

#include <stdint.h>
#include "esp_http_server.h"
#include <time.h>

esp_err_t device_settings_init(void);
bool device_local_time(time_t utc, struct tm *local, long *offset);
esp_err_t web_settings_get(httpd_req_t *req);
esp_err_t web_settings_put(httpd_req_t *req);
esp_err_t web_time_set(httpd_req_t *req);
