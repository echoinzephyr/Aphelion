#include "toolbox.h"

#include "esp_log.h"

void app_main(void)
{
    ESP_ERROR_CHECK(network_prepare());
    const network_info_t network = network_snapshot();
    ESP_LOGI("aphelion", "Join %s | WPA2 password: %s | http://192.168.4.1", network.ap, network.key);
    ESP_ERROR_CHECK(toolbox_radio_init());
    ESP_ERROR_CHECK(toolbox_signal_init());
    ESP_ERROR_CHECK(web_start());
    ESP_ERROR_CHECK(network_start_connections());
}
