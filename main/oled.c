#include "oled.h"
#include "sdkconfig.h"

#include <string.h>
#include "esp_check.h"

#ifdef CONFIG_APHELION_OLED_SH1106
enum { OLED_COLUMN_OFFSET = 2 };
#else
enum { OLED_COLUMN_OFFSET = 0 };
#endif

static const char *TAG = "oled";

esp_err_t oled_open(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t *device)
{
    const uint8_t addresses[] = {0x3c, 0x3d};
    for (size_t i = 0; i < sizeof(addresses); ++i) {
        uint8_t address = addresses[i];
        if (i2c_master_probe(bus, address, 50) != ESP_OK) {
            continue;
        }
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = 400000,
        };
        esp_err_t err = i2c_master_bus_add_device(bus, &config, device);
        if (err != ESP_OK) {
            return err;
        }
        const uint8_t init[] = {
            0x00,                     // I2C control byte: command stream
            0xae,                     // display off
            0xd5, 0x80,               // clock divide and oscillator
#ifdef CONFIG_APHELION_OLED_SSD1306_32
            0xa8, 31, 0xda, 0x02,     // multiplex and COM pins for 32 rows
#else
            0xa8, 63, 0xda, 0x12,     // multiplex and COM pins for 64 rows
#endif
            0xd3, 0x00, 0x40,         // display offset and start line
#ifdef CONFIG_APHELION_OLED_SH1106
            0xad, 0x8b, 0xd9, 0x1f,   // SH1106 internal DC/DC and precharge
#else
            0x8d, 0x14, 0x20, 0x02,   // SSD1306 charge pump and page addressing
            0xd9, 0xf1,               // internal charge pump precharge
#endif
            0xa1, 0xc8,               // landscape module orientation
            0x81, 0x7f,               // contrast
            0xa4, 0xa6, 0xaf,         // RAM output, normal polarity, display on
        };
        err = i2c_master_transmit(*device, init, sizeof(init), 100);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "I2C display at 0x%02x", address);
        } else {
            i2c_master_bus_rm_device(*device);
            *device = NULL;
        }
        return err;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t oled_write(i2c_master_dev_handle_t device, const uint8_t frame[OLED_FRAME_BYTES],
                    unsigned height)
{
    uint8_t data[OLED_WIDTH + 1] = {0x40}; // I2C control byte: display RAM data
    for (unsigned page = 0; page < height / 8; ++page) {
        const uint8_t cursor[] = {0x00, 0xb0 | page, OLED_COLUMN_OFFSET, 0x10};
        ESP_RETURN_ON_ERROR(i2c_master_transmit(device, cursor, sizeof(cursor), 100), TAG, "set RAM cursor");
        memcpy(data + 1, frame + page * OLED_WIDTH, OLED_WIDTH);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(device, data, sizeof(data), 100), TAG, "write RAM page");
    }
    return ESP_OK;
}
