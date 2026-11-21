#include "sensors.h"
#include "board.h"

#include <math.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { MIC_RATE = 16000, MIC_BLOCK = 256 };
static const char *const ENV = "SHT40";
static const char *const MIC = "INMP441";

static esp_err_t open_sht40(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t *device)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x44,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_probe(bus, config.device_address, 50);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_bus_add_device(bus, &config, device);
}

static esp_err_t read_sht40(i2c_master_dev_handle_t device, environment_t *reading)
{
    const uint8_t measure_high_precision = 0xfd;
    esp_err_t err = i2c_master_transmit(device, &measure_high_precision, 1, 100);
    if (err != ESP_OK) {
        return err;
    }
    // Include a tick for scheduling phase so the conversion always gets >= 9 ms.
    vTaskDelay(pdMS_TO_TICKS(10) + 1);
    uint8_t frame[6];
    err = i2c_master_receive(device, frame, sizeof(frame), 100);
    if (err == ESP_OK && !sht40_decode(frame, reading)) {
        return ESP_ERR_INVALID_CRC;
    }
    return err;
}

static sample_status_t transfer_status(esp_err_t err)
{
    if (err == ESP_ERR_TIMEOUT) {
        return SAMPLE_TIMEOUT;
    }
    if (err == ESP_ERR_INVALID_CRC) {
        return SAMPLE_BAD_CRC;
    }
    return err == ESP_OK ? SAMPLE_OK : SAMPLE_IO_ERROR;
}

void environment_task(void *arg)
{
    i2c_master_bus_handle_t bus = arg;
    i2c_master_dev_handle_t device = NULL;
    environment_t reading = {0};
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        esp_err_t err = device ? ESP_OK : open_sht40(bus, &device);
        if (err == ESP_OK) {
            err = read_sht40(device, &reading);
        }
        sample_status_t previous = reading.status;
        reading.status = transfer_status(err);
        if (err == ESP_OK) {
            reading.last_good_us = esp_timer_get_time();
            ESP_LOGI(ENV, "%.1f C  %.1f %%RH%s", reading.celsius, reading.humidity,
                     previous == SAMPLE_OK ? "" : " (online)");
        } else if (reading.status != previous) {
            ESP_LOGW(ENV, "%s; retrying every 5 s", esp_err_to_name(err));
        }
        publish_environment(&reading);
        xTaskDelayUntil(&next, pdMS_TO_TICKS(5000));
    }
}

static esp_err_t open_microphone(i2s_chan_handle_t *rx)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel, NULL, rx), MIC, "RX allocation");
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = MIC_SCK, .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED, .din = MIC_SD,
        },
    };
    config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; // INMP441 L/R is wired to GND
    esp_err_t err = i2s_channel_init_std_mode(*rx, &config);
    if (err == ESP_OK) {
        // An unplugged or tri-stated data line should settle to zero.
        err = gpio_set_pull_mode(MIC_SD, GPIO_PULLDOWN_ONLY);
    }
    if (err == ESP_OK) {
        err = i2s_channel_enable(*rx);
    }
    if (err != ESP_OK) {
        i2s_del_channel(*rx);
        *rx = NULL;
    }
    return err;
}

static esp_err_t read_sound_window(i2s_chan_handle_t rx, sound_t *reading)
{
    int32_t slots[MIC_BLOCK];
    sound_window_t window = {0};
    while (window.count < MIC_RATE) {
        size_t remaining = MIC_RATE - window.count;
        size_t requested = remaining < MIC_BLOCK ? remaining : MIC_BLOCK;
        size_t received = 0;
        esp_err_t err = i2s_channel_read(rx, slots, requested * sizeof(*slots), &received, 150);
        if (err != ESP_OK) {
            return err;
        }
        if (received == 0 || received % sizeof(*slots) != 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        sound_accumulate(&window, slots, received / sizeof(*slots));
    }
    *reading = sound_measure(&window);
    reading->sampled_us = esp_timer_get_time();
    return ESP_OK;
}

void sound_task(void *arg)
{
    i2s_chan_handle_t rx = NULL;
    sample_status_t previous = SAMPLE_WAITING;
    int64_t last_log = 0;
    for (;;) {
        sound_t reading = {0};
        esp_err_t err = rx ? ESP_OK : open_microphone(&rx);
        if (err == ESP_OK) {
            err = read_sound_window(rx, &reading);
        }
        if (err != ESP_OK) {
            reading.status = transfer_status(err);
            if (rx) {
                i2s_channel_disable(rx);
                i2s_del_channel(rx);
                rx = NULL;
            }
        }
        publish_sound(&reading);
        bool transition = reading.status != previous;
        if (reading.status == SAMPLE_OK || reading.status == SAMPLE_CLIPPING) {
            if (transition || reading.sampled_us - last_log >= 5000000) {
                ESP_LOGI(MIC, "%.1f dBFS  %.1f dB SPL (estimated)%s", reading.dbfs, reading.spl,
                         reading.status == SAMPLE_CLIPPING ? " [clipping]" : "");
                last_log = reading.sampled_us;
            }
        } else if (transition) {
            if (err != ESP_OK) {
                ESP_LOGW(MIC, "%s; restarting RX", esp_err_to_name(err));
            } else {
                ESP_LOGW(MIC, "%s", reading.status == SAMPLE_ALL_ZERO ? "all samples zero" : "DC only");
            }
        }
        previous = reading.status;
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}
