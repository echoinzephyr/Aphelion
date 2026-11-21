if(CONFIG_APHELION_TOOLBOX)
    set(app_sources "toolbox/app.c" "toolbox/radio.c" "toolbox/signal.c" "toolbox/web.c")
    set(app_page "toolbox/console.html")
else()
    set(app_sources "aphelion.c" "oled.c" "oled_ui.c" "sensors.c" "measurements.c"
                    "terminal.c" "terminal_draw.c" "web.c")
    set(app_page "dashboard.html")
endif()

idf_component_register(
    SRCS "network.c" "web_wifi.c" ${app_sources}
    EMBED_TXTFILES ${app_page}
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc
                  esp_adc esp_timer esp_wifi esp_event esp_netif esp_http_server
                  esp_hw_support nvs_flash lwip bt
)
