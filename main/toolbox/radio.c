#include "toolbox.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

enum { SCAN_COMPLETE = BIT0, SCAN_CANCEL = BIT1, SCAN_REQUEST = BIT2, SCAN_FAILED = BIT3 };
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static toolbox_scan_t scan;
static TaskHandle_t worker;
static uint8_t local_address_type;
static bool ble_ready;
static bool cancel_requested;
static bool scan_in_driver;
void ble_store_config_init(void);

static void ble_sync(void)
{
    uint8_t type;
    int err = ble_hs_util_ensure_addr(0);
    if (err == 0) err = ble_hs_id_infer_auto(0, &type);
    portENTER_CRITICAL(&lock);
    if (err == 0) local_address_type = type;
    ble_ready = err == 0;
    portEXIT_CRITICAL(&lock);
    if (err == 0) {
        ESP_LOGI("tool_radio", "BLE scanner ready");
    } else {
        ESP_LOGW("tool_radio", "BLE scanner address unavailable: %d", err);
    }
}

static void ble_reset(int reason)
{
    portENTER_CRITICAL(&lock);
    ble_ready = false;
    portEXIT_CRITICAL(&lock);
}

static void ble_host(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    portENTER_CRITICAL(&lock);
    bool scanning = scan.kind == RADIO_WIFI && scan_in_driver;
    portEXIT_CRITICAL(&lock);
    if (scanning) {
        const wifi_event_sta_scan_done_t *result = data;
        xTaskNotify(worker, SCAN_COMPLETE | (result && result->status ? SCAN_FAILED : 0), eSetBits);
    }
}

static int ble_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        const int reason = event->disc_complete.reason;
        portENTER_CRITICAL(&lock);
        scan.driver_error = reason;
        portEXIT_CRITICAL(&lock);
        xTaskNotify(worker, SCAN_COMPLETE | (reason ? SCAN_FAILED : 0), eSetBits);
        return 0;
    }
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    const struct ble_gap_disc_desc *adv = &event->disc;
    portENTER_CRITICAL(&lock);
    if (scan.kind == RADIO_BLE && scan.phase == RADIO_RUNNING) {
        unsigned i;
        for (i = 0; i < scan.ble_count; ++i) {
            const toolbox_ble_record_t *record = &scan.ble[i];
            if (record->address_type == adv->addr.type &&
                !memcmp(record->address, adv->addr.val, sizeof(record->address))) break;
        }
        if (i < TOOLBOX_BLE_RESULTS) {
            toolbox_ble_record_t *record = &scan.ble[i];
            if (i == scan.ble_count) {
                *record = (toolbox_ble_record_t){ .address_type = adv->addr.type };
                memcpy(record->address, adv->addr.val, sizeof(record->address));
                ++scan.ble_count;
            }
            const size_t size = adv->length_data > sizeof(record->adv) ? sizeof(record->adv) : adv->length_data;
            record->rssi = adv->rssi;
            if (adv->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
                memcpy(record->response, adv->data, size);
                record->response_size = size;
            } else {
                memcpy(record->adv, adv->data, size);
                record->adv_size = size;
            }
        }
    }
    portEXIT_CRITICAL(&lock);
    return 0;
}

bool toolbox_scan_start(radio_kind_t kind)
{
    portENTER_CRITICAL(&lock);
    bool allowed = scan.phase != RADIO_RUNNING;
    if (allowed) {
        scan.kind = kind;
        scan.phase = RADIO_RUNNING;
        scan.detail = "";
        scan.driver_error = 0;
        cancel_requested = false;
        scan.wifi.count = 0;
        scan.ble_count = 0;
        ++scan.revision;
    }
    portEXIT_CRITICAL(&lock);
    if (allowed) xTaskNotify(worker, SCAN_REQUEST, eSetBits);
    return allowed;
}

bool toolbox_scan_cancel(void)
{
    portENTER_CRITICAL(&lock);
    bool running = scan.phase == RADIO_RUNNING;
    if (running) cancel_requested = true;
    portEXIT_CRITICAL(&lock);
    if (running) xTaskNotify(worker, SCAN_CANCEL, eSetBits);
    return running;
}

bool toolbox_scan_snapshot(unsigned after_revision, toolbox_scan_t *snapshot)
{
    portENTER_CRITICAL(&lock);
    bool changed = after_revision != scan.revision;
    if (changed) *snapshot = scan;
    portEXIT_CRITICAL(&lock);
    return changed;
}

static int start_scan(radio_kind_t kind)
{
    if (kind == RADIO_WIFI) {
        const wifi_scan_config_t options = {
            .show_hidden = true,
            .scan_time.active = {.min = 30, .max = 100},
            .home_chan_dwell_time = 30,
        };
        return esp_wifi_scan_start(&options, false);
    }
    portENTER_CRITICAL(&lock);
    bool ready = ble_ready;
    uint8_t address = local_address_type;
    portEXIT_CRITICAL(&lock);
    const struct ble_gap_disc_params options = {
        .itvl = 80, .window = 32, .filter_duplicates = 1,
    };
    if (!ready) return BLE_HS_ENOTSYNCED;
    // Observer-only NimBLE does not run the GAP duration timer in this SDK.
    // The radio worker owns the deadline and explicitly stops discovery.
    const int err = ble_gap_disc(address, BLE_HS_FOREVER, &options, ble_event, NULL);
    if (err != 0) ESP_LOGW("tool_radio", "BLE discovery did not start: %d", err);
    return err;
}

static bool cancelled(void)
{
    portENTER_CRITICAL(&lock);
    bool result = cancel_requested;
    portEXIT_CRITICAL(&lock);
    return result;
}

static uint32_t wait_for_scan(radio_kind_t kind)
{
    const int64_t expires = esp_timer_get_time() + (kind == RADIO_BLE ? 4000000 : 9000000);
    for (;;) {
        uint32_t events = 0;
        const int64_t remaining_ms = (expires - esp_timer_get_time()) / 1000;
        if (remaining_ms <= 0) return 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, pdMS_TO_TICKS(remaining_ms) + 1);
        if (cancelled()) return events | SCAN_CANCEL;
        if (events & SCAN_COMPLETE) return events & ~SCAN_CANCEL;
    }
}

static void radio_worker(void *arg)
{
    static wifi_scan_list_t wifi;
    for (;;) {
        uint32_t events = 0;
        xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
        if (!(events & SCAN_REQUEST)) continue;

        portENTER_CRITICAL(&lock);
        radio_kind_t kind = scan.kind;
        portEXIT_CRITICAL(&lock);
        radio_phase_t phase = RADIO_FAILED;
        const char *detail = "";
        int error = 0;
        bool reserved = false;
        for (unsigned attempts = 0; attempts < 30 && !cancelled(); ++attempts) {
            reserved = network_radio_acquire();
            if (reserved) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        portENTER_CRITICAL(&lock);
        scan_in_driver = reserved;
        portEXIT_CRITICAL(&lock);
        if (cancelled()) {
            phase = RADIO_CANCELLED;
        } else if (!reserved) {
            detail = "radio_busy";
        } else if ((error = start_scan(kind)) == 0) {
            ESP_LOGI("tool_radio", "%s scan started", kind == RADIO_BLE ? "BLE" : "Wi-Fi");
            events = wait_for_scan(kind);
            if (events & SCAN_CANCEL || !(events & SCAN_COMPLETE)) {
                phase = events & SCAN_CANCEL ? RADIO_CANCELLED : RADIO_FAILED;
                if (!(events & SCAN_COMPLETE)) {
                    if (kind == RADIO_WIFI) {
                        esp_wifi_scan_stop();
                        uint32_t late;
                        xTaskNotifyWait(0, UINT32_MAX, &late, pdMS_TO_TICKS(500));
                    } else {
                        const int stop = ble_gap_disc_cancel();
                        if (stop == 0 || stop == BLE_HS_EALREADY) {
                            if (phase != RADIO_CANCELLED) phase = RADIO_DONE;
                        } else {
                            error = stop;
                            phase = RADIO_FAILED;
                            detail = "driver";
                        }
                    }
                }
                if (kind == RADIO_WIFI) esp_wifi_clear_ap_list();
                if (phase == RADIO_FAILED && !error) detail = "timeout";
            } else if (events & SCAN_FAILED) {
                detail = "driver";
                if (kind == RADIO_WIFI) esp_wifi_clear_ap_list();
            } else {
                phase = RADIO_DONE;
                if (kind == RADIO_WIFI && network_collect_wifi(&wifi, false) != ESP_OK) {
                    phase = RADIO_FAILED;
                }
            }
        } else {
            detail = "driver";
        }
        portENTER_CRITICAL(&lock);
        scan.detail = detail;
        if (error) scan.driver_error = error;
        if (phase == RADIO_DONE && kind == RADIO_WIFI) scan.wifi = wifi;
        if (phase != RADIO_DONE) scan.ble_count = 0;
        scan_in_driver = false;
        scan.phase = phase;
        unsigned count = kind == RADIO_BLE ? scan.ble_count : scan.wifi.count;
        int driver_error = scan.driver_error;
        ++scan.revision;
        portEXIT_CRITICAL(&lock);
        if (reserved) network_radio_release();
        ESP_LOGI("tool_radio", "%s scan phase=%u, results=%u, error=%d %s",
                 kind == RADIO_BLE ? "BLE" : "Wi-Fi", phase, count, driver_error, detail);
    }
}

esp_err_t toolbox_radio_init(void)
{
    if (xTaskCreate(radio_worker, "tool_radio", 4096, NULL, 4, &worker) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_event, NULL);
    if (err != ESP_OK) return err;
    err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.sync_cb = ble_sync;
    ble_hs_cfg.reset_cb = ble_reset;
    ble_store_config_init();
    nimble_port_freertos_init(ble_host);
    return ESP_OK;
}
