#include "toolbox.h"

#include <math.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_continuous.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

enum { ADC_BYTES = TOOLBOX_WAVE_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES };
static const char *const TAG = "tool_signal";
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t lease_expiry_us;
static toolbox_signal_status_t desired;
static toolbox_signal_status_t actual;
static toolbox_wave_t latest;
static bool waveform_ready;
static bool pwm_initialized;

static void clear_requests(void)
{
    desired.pwm = false;
    desired.capturing = false;
    waveform_ready = false;
}

void toolbox_signal_keepalive(void)
{
    portENTER_CRITICAL(&lock);
    lease_expiry_us = esp_timer_get_time() + TOOLBOX_LEASE_MS * 1000;
    portEXIT_CRITICAL(&lock);
}

void toolbox_signal_release(void)
{
    portENTER_CRITICAL(&lock);
    lease_expiry_us = 0;
    clear_requests();
    portEXIT_CRITICAL(&lock);
}

bool toolbox_pwm_request(bool on, unsigned hz, unsigned duty)
{
    portENTER_CRITICAL(&lock);
    bool allowed = esp_timer_get_time() < lease_expiry_us;
    if (allowed) {
        desired.pwm = on;
        desired.hz = hz;
        desired.duty = duty;
        desired.pwm_error = ESP_OK;
    }
    portEXIT_CRITICAL(&lock);
    return allowed;
}

bool toolbox_scope_request(bool on, bool demo)
{
    portENTER_CRITICAL(&lock);
    bool allowed = esp_timer_get_time() < lease_expiry_us;
    if (allowed) {
        desired.capturing = on;
        desired.demo = demo;
        desired.adc_error = ESP_OK;
        waveform_ready = false;
    }
    portEXIT_CRITICAL(&lock);
    return allowed;
}

toolbox_signal_status_t toolbox_signal_status(void)
{
    portENTER_CRITICAL(&lock);
    toolbox_signal_status_t copy = actual;
    portEXIT_CRITICAL(&lock);
    return copy;
}

bool toolbox_wave_snapshot(uint32_t after_sequence, toolbox_wave_t *wave)
{
    portENTER_CRITICAL(&lock);
    bool available = waveform_ready && latest.sequence != after_sequence;
    if (available) *wave = latest;
    portEXIT_CRITICAL(&lock);
    return available;
}

static esp_err_t apply_pwm(const toolbox_signal_status_t *config)
{
    if (!config->pwm) {
        return pwm_initialized ? ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0) : ESP_OK;
    }
    // More duty bits keep the timer divisor representable at low frequencies.
    ledc_timer_bit_t resolution = config->hz < 100 ? LEDC_TIMER_13_BIT : LEDC_TIMER_10_BIT;
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = resolution,
        .timer_num = LEDC_TIMER_0, .freq_hz = config->hz, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "PWM timer");
    if (!pwm_initialized) {
        const ledc_channel_config_t channel = {
            .gpio_num = TOOLBOX_PWM_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0,
        };
        ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "PWM channel");
        pwm_initialized = true;
    }
    if (config->duty == 100) {
        return ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1);
    }
    const uint32_t duty = ((1u << resolution) * config->duty + 50) / 100;
    // This worker exclusively owns the channel, so the fade-service lock is unnecessary.
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty), TAG, "PWM duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t adc_open(adc_continuous_handle_t *adc, adc_cali_handle_t *calibration)
{
    adc_unit_t unit;
    adc_channel_t channel;
    ESP_RETURN_ON_ERROR(adc_continuous_io_to_channel(TOOLBOX_ADC_GPIO, &unit, &channel), TAG, "ADC pin");
    const adc_continuous_handle_cfg_t buffers = {
        .max_store_buf_size = ADC_BYTES * 3, .conv_frame_size = ADC_BYTES,
        .flags.flush_pool = 1,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&buffers, adc), TAG, "ADC allocation");
    adc_digi_pattern_config_t pattern = {
        .atten = ADC_ATTEN_DB_12, .channel = channel, .unit = unit, .bit_width = ADC_BITWIDTH_12,
    };
    const adc_continuous_config_t config = {
        .pattern_num = 1, .adc_pattern = &pattern, .sample_freq_hz = TOOLBOX_SAMPLE_RATE,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1, .format = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    esp_err_t err = adc_continuous_config(*adc, &config);
    if (err != ESP_OK) {
        adc_continuous_deinit(*adc);
        *adc = NULL;
        return err;
    }
    const adc_cali_curve_fitting_config_t curve = {
        .unit_id = unit, .chan = channel, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&curve, calibration) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable; reporting raw counts");
    }
    return ESP_OK;
}

static esp_err_t read_wave(adc_continuous_handle_t adc, adc_cali_handle_t calibration, toolbox_wave_t *wave)
{
    static _Alignas(4) uint8_t samples[ADC_BYTES];
    uint32_t bytes = 0;
    esp_err_t err = adc_continuous_read(adc, samples, sizeof(samples), &bytes, 40);
    if (err != ESP_OK) return err;
    if (bytes != sizeof(samples)) return ESP_ERR_INVALID_SIZE;

    wave->calibrated = calibration != NULL;
    for (unsigned i = 0; i < TOOLBOX_WAVE_SAMPLES; ++i) {
        const adc_digi_output_data_t *datum = (const void *)(samples + i * SOC_ADC_DIGI_RESULT_BYTES);
        const unsigned raw = datum->type2.data;
        int mv = 0;
        if (calibration && adc_cali_raw_to_voltage(calibration, raw, &mv) != ESP_OK) {
            return ESP_FAIL;
        }
        wave->samples[i] = calibration ? (mv > 0 ? mv : 0) : raw;
        wave->clipped |= raw >= 4090;
    }
    return ESP_OK;
}

static void synthetic_wave(toolbox_wave_t *wave)
{
    for (unsigned i = 0; i < TOOLBOX_WAVE_SAMPLES; ++i) {
        wave->samples[i] = 1400 + 1000 * sinf((float)(i % 200) * (6.2831853f / 200));
    }
    wave->calibrated = true;
    wave->demo = true;
}

static void signal_worker(void *arg)
{
    static toolbox_wave_t wave;
    adc_continuous_handle_t adc = NULL;
    adc_cali_handle_t calibration = NULL;
    toolbox_signal_status_t applied = {0};
    uint32_t sequence = 0;
    int64_t next_publish_us = 0;
    bool adc_running = false;
    unsigned incomplete_reads = 0;

    for (;;) {
        portENTER_CRITICAL(&lock);
        if (esp_timer_get_time() >= lease_expiry_us) clear_requests();
        toolbox_signal_status_t request = desired;
        portEXIT_CRITICAL(&lock);

        if (request.pwm != applied.pwm ||
            (request.pwm && (request.hz != applied.hz || request.duty != applied.duty))) {
            esp_err_t err = apply_pwm(&request);
            if (err != ESP_OK) {
                const toolbox_signal_status_t stopped = {0};
                apply_pwm(&stopped);
                request.pwm = false;
                portENTER_CRITICAL(&lock);
                desired.pwm = false;
                desired.pwm_error = err;
                portEXIT_CRITICAL(&lock);
            }
        }

        bool want_adc = request.capturing && !request.demo;
        esp_err_t adc_error = ESP_OK;
        if (want_adc && !adc_running) {
            adc_error = adc ? ESP_OK : adc_open(&adc, &calibration);
            if (adc_error == ESP_OK) adc_error = adc_continuous_start(adc);
            adc_running = adc_error == ESP_OK;
            incomplete_reads = 0;
        } else if (!want_adc && adc_running) {
            adc_continuous_stop(adc);
            adc_continuous_flush_pool(adc);
            adc_running = false;
        }

        const int64_t now = esp_timer_get_time();
        bool frame = false;
        if (adc_running || (request.capturing && now >= next_publish_us)) {
            wave = (toolbox_wave_t){0};
            if (request.demo) {
                synthetic_wave(&wave);
                frame = true;
            } else if (adc_running) {
                const esp_err_t read_status = read_wave(adc, calibration, &wave);
                if (read_status == ESP_OK) {
                    incomplete_reads = 0;
                    frame = true;
                } else if ((read_status != ESP_ERR_TIMEOUT && read_status != ESP_ERR_INVALID_SIZE) ||
                           ++incomplete_reads >= 8) {
                    adc_error = read_status;
                }
            }
        }

        portENTER_CRITICAL(&lock);
        if (adc_error != ESP_OK) {
            desired.capturing = false;
            desired.adc_error = adc_error;
            request.capturing = false;
            waveform_ready = false;
        }
        if (frame && now >= next_publish_us && desired.capturing &&
            desired.demo == request.demo && esp_timer_get_time() < lease_expiry_us) {
            wave.sequence = ++sequence;
            latest = wave;
            next_publish_us = now + 100000;
            waveform_ready = true;
        }
        request.pwm_error = desired.pwm_error;
        request.adc_error = desired.adc_error;
        request.calibrated = request.demo || calibration != NULL;
        actual = applied = request;
        portEXIT_CRITICAL(&lock);

        if (!adc_running) vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t toolbox_signal_init(void)
{
    ESP_RETURN_ON_ERROR(gpio_set_level(TOOLBOX_PWM_GPIO, 0), TAG, "initial PWM level");
    ESP_RETURN_ON_ERROR(gpio_set_direction(TOOLBOX_PWM_GPIO, GPIO_MODE_OUTPUT), TAG, "initial PWM direction");
    return xTaskCreate(signal_worker, "tool_signal", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
