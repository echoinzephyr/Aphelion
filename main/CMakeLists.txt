if(CONFIG_APHELION_TOOLBOX)
    set(app_sources "toolbox/app.c" "toolbox/radio.c" "toolbox/ble_explorer.c" "toolbox/signal.c" "toolbox/web.c")
else()
    set(app_sources "aphelion.c" "web.c")
endif()

if(NOT CONFIG_APHELION_TOOLBOX OR CONFIG_APHELION_TOOLBOX_DISPLAY)
    list(APPEND app_sources "peripherals.c" "oled.c" "oled_ui.c" "oled_layout.c"
                            "fonts/oled_labels_12.c" "sensors.c"
                            "measurements.c" "terminal.c" "terminal_draw.c")
endif()

idf_component_register(
    SRCS "network.c" "web_wifi.c" "device_clock.c" "device_settings.c" "web_status.c" ${app_sources}
    EMBED_TXTFILES "dashboard.html"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc
                  esp_adc esp_timer esp_wifi esp_event esp_netif esp_http_server
                  esp_hw_support nvs_flash lwip bt spi_flash app_update esp_psram
)
