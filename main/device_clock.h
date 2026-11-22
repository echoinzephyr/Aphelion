#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_http_server.h"

void device_clock_sync(void);
bool device_clock_set(int64_t epoch_ms);
bool device_clock_read(struct tm *local);
esp_err_t device_clock_status(httpd_req_t *req);
