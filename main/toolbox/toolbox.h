#pragma once

#include "network.h"
#include "esp_err.h"
#include <stdint.h>

enum {
    TOOLBOX_ADC_GPIO = 4, TOOLBOX_PWM_GPIO = 5,
    TOOLBOX_SAMPLE_RATE = 20000, TOOLBOX_WAVE_SAMPLES = 512,
    TOOLBOX_BLE_RESULTS = 24, TOOLBOX_LEASE_MS = 5000,
};

typedef enum { RADIO_NONE, RADIO_WIFI, RADIO_BLE } radio_kind_t;
typedef enum { RADIO_IDLE, RADIO_RUNNING, RADIO_DONE, RADIO_CANCELLED, RADIO_FAILED } radio_phase_t;

typedef struct {
    uint8_t address[6];
    uint8_t address_type;
    int8_t rssi;
    bool connectable;
    uint8_t adv_size;
    uint8_t response_size;
    uint8_t adv[31];
    uint8_t response[31];
} toolbox_ble_record_t;

typedef struct {
    unsigned revision;
    radio_kind_t kind;
    radio_phase_t phase;
    const char *detail;
    int driver_error;
    wifi_scan_list_t wifi;
    unsigned ble_count;
    toolbox_ble_record_t ble[TOOLBOX_BLE_RESULTS];
} toolbox_scan_t;

esp_err_t toolbox_radio_init(void);
bool toolbox_scan_start(radio_kind_t kind);
bool toolbox_scan_cancel(void);
int toolbox_ble_connect(uint8_t type, const uint8_t address[6]);
bool toolbox_scan_snapshot(unsigned after_revision, toolbox_scan_t *snapshot);

typedef struct {
    bool pwm;
    unsigned hz;
    unsigned duty;
    bool capturing;
    bool demo;
    bool calibrated;
    esp_err_t pwm_error;
    esp_err_t adc_error;
} toolbox_signal_status_t;

typedef struct {
    uint32_t sequence;
    bool calibrated;
    bool clipped;
    bool demo;
    uint16_t samples[TOOLBOX_WAVE_SAMPLES];
} toolbox_wave_t;

esp_err_t toolbox_signal_init(void);
void toolbox_signal_keepalive(void);
void toolbox_signal_release(void);
bool toolbox_pwm_request(bool on, unsigned hz, unsigned duty);
bool toolbox_scope_request(bool on, bool demo);
toolbox_signal_status_t toolbox_signal_status(void);
bool toolbox_wave_snapshot(uint32_t after_sequence, toolbox_wave_t *wave);
