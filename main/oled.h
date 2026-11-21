#pragma once

#include "driver/i2c_master.h"
#include "terminal.h"

esp_err_t oled_open(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t *device);
esp_err_t oled_write(i2c_master_dev_handle_t device, const uint8_t frame[OLED_FRAME_BYTES],
                    unsigned height);

void oled_task(void *arg);
terminal_view_t oled_selected_view(void);
terminal_view_t oled_press(void);
