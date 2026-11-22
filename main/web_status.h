#pragma once

#include "esp_http_server.h"

esp_err_t web_home(httpd_req_t *req);
esp_err_t web_status(httpd_req_t *req);
esp_err_t web_oled_button(httpd_req_t *req);
