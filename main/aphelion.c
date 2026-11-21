#include "board.h"
#include "network.h"
#include "oled.h"
#include "sensors.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static environment_t environment;
static sound_t sound;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;

void publish_environment(const environment_t *reading)
{
    portENTER_CRITICAL(&state_lock);
    environment = *reading;
    portEXIT_CRITICAL(&state_lock);
}

void publish_sound(const sound_t *reading)
{
    portENTER_CRITICAL(&state_lock);
    sound = *reading;
    portEXIT_CRITICAL(&state_lock);
}

void sensor_snapshot(environment_t *env, sound_t *audio)
{
    portENTER_CRITICAL(&state_lock);
    *env = environment;
    *audio = sound;
    portEXIT_CRITICAL(&state_lock);
}

void app_main(void)
{
    ESP_LOGI("aphelion", "I2C SDA=%d SCL=%d; INMP441 BCLK=%d WS=%d DIN=%d (left, L/R grounded)",
             I2C_SDA, I2C_SCL, MIC_SCK, MIC_WS, MIC_SD);
    ESP_ERROR_CHECK(xTaskCreate(sound_task, "inmp441", 6144, NULL, 4, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(network_task, "network", 6144, NULL, 4, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    const i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    esp_err_t err = i2c_new_master_bus(&config, &i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE("aphelion", "I2C init failed: %s", esp_err_to_name(err));
        publish_environment(&(environment_t){.status = SAMPLE_IO_ERROR});
        return;
    }
    ESP_ERROR_CHECK(xTaskCreate(oled_task, "oled", 6144, i2c_bus, 3, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(environment_task, "sht40", 4096, i2c_bus, 4, NULL) == pdPASS
                    ? ESP_OK : ESP_ERR_NO_MEM);
}
