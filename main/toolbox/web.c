#include "toolbox.h"

#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const char toolbox_page[] asm("_binary_console_html_start");
static const char *const TAG = "tool_web";
static const char *const origin = "http://192.168.4.1";
static httpd_handle_t server;
static atomic_bool work_pending;
static int controller = -1;
static int64_t last_heartbeat;
static char token[17];
static unsigned scan_revision;
static uint32_t wave_sequence;
static int64_t next_status;
static char scan_json[8192];

static esp_err_t send_ws(httpd_ws_type_t type, const void *data, size_t size)
{
    httpd_ws_frame_t frame = { .type = type, .payload = (uint8_t *)data, .len = size };
    return httpd_ws_send_frame_async(server, controller, &frame);
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t home(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, toolbox_page);
}

static esp_err_t capabilities(httpd_req_t *req)
{
    char json[224];
    snprintf(json, sizeof(json),
             "{\"name\":\"Aphelion\",\"adc_gpio\":%d,\"pwm_gpio\":%d,\"sample_rate\":%d,"
             "\"wave_samples\":%d,\"lease_ms\":%d,\"tools\":[\"wifi\",\"ble\",\"pwm\",\"scope\"]}",
             TOOLBOX_ADC_GPIO, TOOLBOX_PWM_GPIO, TOOLBOX_SAMPLE_RATE, TOOLBOX_WAVE_SAMPLES, TOOLBOX_LEASE_MS);
    return send_json(req, json);
}

static bool header_matches(httpd_req_t *req, const char *header, const char *expected)
{
    char found[64];
    return httpd_req_get_hdr_value_str(req, header, found, sizeof(found)) == ESP_OK &&
           strcmp(found, expected) == 0;
}

static esp_err_t ws_before(httpd_req_t *req)
{
    if (!header_matches(req, "Origin", origin) || !header_matches(req, "Host", "192.168.4.1")) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Open the Aphelion address directly");
        return ESP_FAIL;
    }
    // A rejected handshake must return ESP_FAIL even if the response itself was sent successfully.
    if (controller != -1) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "Another browser controls Aphelion");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ws_after(httpd_req_t *req)
{
    controller = httpd_req_to_sockfd(req);
    scan_revision = UINT32_MAX;
    wave_sequence = 0;
    next_status = 0;
    last_heartbeat = esp_timer_get_time();
    snprintf(token, sizeof(token), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    toolbox_signal_keepalive();
    char hello[64];
    int size = snprintf(hello, sizeof(hello), "{\"type\":\"hello\",\"token\":\"%s\"}", token);
    return send_ws(HTTPD_WS_TYPE_TEXT, hello, size);
}

static void controller_closed(httpd_handle_t handle, int socket)
{
    if (socket == controller) {
        controller = -1;
        token[0] = '\0';
        toolbox_signal_release();
        toolbox_scan_cancel();
    }
    close(socket);
}

static esp_err_t ws_recv(httpd_req_t *req)
{
    char text[16];
    httpd_ws_frame_t frame = { .payload = (uint8_t *)text };
    if (httpd_req_to_sockfd(req) != controller ||
        httpd_ws_recv_frame(req, &frame, sizeof(text)) != ESP_OK ||
        frame.type != HTTPD_WS_TYPE_TEXT || frame.len != 4 || memcmp(text, "ping", 4) != 0) {
        return ESP_FAIL;
    }
    last_heartbeat = esp_timer_get_time();
    toolbox_signal_keepalive();
    return ESP_OK;
}

static esp_err_t conflict(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, message);
}

static bool valid_control(httpd_req_t *req)
{
    return controller != -1 && esp_timer_get_time() - last_heartbeat < TOOLBOX_LEASE_MS * 1000 &&
           header_matches(req, "X-Aphelion-Control", token);
}

static bool read_unsigned(const char *query, const char *field, unsigned min, unsigned max, unsigned *result)
{
    char text[8];
    if (httpd_query_key_value(query, field, text, sizeof(text)) != ESP_OK || !text[0]) return false;
    unsigned value = 0;
    for (const char *c = text; *c; ++c) {
        if (*c < '0' || *c > '9') return false;
        value = value * 10 + *c - '0';
        if (value > max) return false;
    }
    if (value < min) return false;
    *result = value;
    return true;
}

static esp_err_t control(httpd_req_t *req)
{
    if (!valid_control(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Open the active control session");
    }
    char query[80], mode[8];
    if (req->content_len || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected command parameters");
    }
    bool accepted;
    if (strncmp(req->uri, "/api/pwm", 8) == 0) {
        unsigned on, hz = 0, duty = 0;
        if (!read_unsigned(query, "on", 0, 1, &on) ||
            (on && (!read_unsigned(query, "hz", 10, 20000, &hz) ||
                    !read_unsigned(query, "duty", 0, 100, &duty)))) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Enter 10-20000 Hz and 0-100% duty");
        }
        accepted = toolbox_pwm_request(on, hz, duty);
    } else if (strncmp(req->uri, "/api/scope", 10) == 0) {
        if (httpd_query_key_value(query, "mode", mode, sizeof(mode)) != ESP_OK ||
            (strcmp(mode, "adc") && strcmp(mode, "demo") && strcmp(mode, "stop"))) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown scope mode");
        }
        accepted = toolbox_scope_request(strcmp(mode, "stop") != 0, strcmp(mode, "demo") == 0);
    } else {
        if (httpd_query_key_value(query, "kind", mode, sizeof(mode)) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Scan kind required");
        }
        if (strcmp(mode, "wifi") == 0) {
            accepted = toolbox_scan_start(RADIO_WIFI);
        } else if (strcmp(mode, "ble") == 0) {
            accepted = toolbox_scan_start(RADIO_BLE);
        } else if (strcmp(mode, "stop") == 0) {
            toolbox_scan_cancel();
            accepted = true;
        } else {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown scan kind");
        }
    }
    if (!accepted) return conflict(req, "Tool busy or control session expired");
    next_status = 0;
    httpd_resp_set_status(req, "202 Accepted");
    return send_json(req, "{\"accepted\":true}");
}

static const char *as_bool(bool value)
{
    return value ? "true" : "false";
}

static void hex_text(char *out, const uint8_t *source, unsigned n, bool reverse)
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < n; ++i) {
        unsigned byte = source[reverse ? n - 1 - i : i];
        *out++ = digits[byte >> 4];
        *out++ = digits[byte & 15];
    }
    *out = '\0';
}

static bool append(char **out, size_t *remaining, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int length = vsnprintf(*out, *remaining, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= *remaining) return false;
    *out += length;
    *remaining -= length;
    return true;
}

static esp_err_t send_scan(const toolbox_scan_t *result)
{
    static const char *const kinds[] = {"none", "wifi", "ble"};
    static const char *const phases[] = {"idle", "running", "done", "cancelled", "failed"};
    char *out = scan_json;
    size_t remaining = sizeof(scan_json);
    if (!append(&out, &remaining, "{\"type\":\"scan\",\"kind\":\"%s\",\"phase\":\"%s\",\"results\":[",
                kinds[result->kind], phases[result->phase])) return ESP_ERR_INVALID_SIZE;
    if (result->kind == RADIO_WIFI) {
        for (unsigned i = 0; i < result->wifi.count; ++i) {
            const nearby_network_t *ap = &result->wifi.items[i];
            char ssid[65], bssid[13];
            hex_text(ssid, (const uint8_t *)ap->ssid, strnlen(ap->ssid, 32), false);
            hex_text(bssid, ap->bssid, 6, false);
            if (!append(&out, &remaining,
                        "%s{\"ssid\":\"%s\",\"bssid\":\"%s\",\"rssi\":%d,\"channel\":%u,\"security\":\"%s\"}",
                        i ? "," : "", ssid, bssid, ap->rssi, ap->channel, ap->security)) return ESP_ERR_INVALID_SIZE;
        }
    } else if (result->kind == RADIO_BLE && result->phase == RADIO_DONE) {
        for (unsigned i = 0; i < result->ble_count; ++i) {
            const toolbox_ble_record_t *record = &result->ble[i];
            char address[13], adv[63], response[63];
            hex_text(address, record->address, 6, true);
            hex_text(adv, record->adv, record->adv_size, false);
            hex_text(response, record->response, record->response_size, false);
            if (!append(&out, &remaining,
                        "%s{\"address\":\"%s\",\"address_type\":%u,\"rssi\":%d,\"adv\":\"%s\",\"response\":\"%s\"}",
                        i ? "," : "", address, record->address_type, record->rssi, adv, response)) return ESP_ERR_INVALID_SIZE;
        }
    }
    if (!append(&out, &remaining, "]}")) return ESP_ERR_INVALID_SIZE;
    return send_ws(HTTPD_WS_TYPE_TEXT, scan_json, out - scan_json);
}

static esp_err_t send_status(void)
{
    char json[340];
    const toolbox_signal_status_t state = toolbox_signal_status();
    int size = snprintf(json, sizeof(json),
                        "{\"type\":\"status\",\"pwm\":%s,\"hz\":%u,\"duty\":%u,"
                        "\"capturing\":%s,\"demo\":%s,\"pwm_error\":\"%s\",\"adc_error\":\"%s\",\"heap\":%lu}",
                        as_bool(state.pwm), state.hz, state.duty, as_bool(state.capturing), as_bool(state.demo),
                        esp_err_to_name(state.pwm_error), esp_err_to_name(state.adc_error),
                        (unsigned long)esp_get_free_heap_size());
    return send_ws(HTTPD_WS_TYPE_TEXT, json, size);
}

static void write_le(uint8_t *out, uint32_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i) {
        out[i] = value >> (8 * i);
    }
}

static esp_err_t send_wave(const toolbox_wave_t *wave)
{
    static uint8_t encoded[12 + 2 * TOOLBOX_WAVE_SAMPLES];
    encoded[0] = 1;
    encoded[1] = wave->demo | (wave->calibrated << 1) | (wave->clipped << 2);
    write_le(encoded + 2, TOOLBOX_WAVE_SAMPLES, 2);
    write_le(encoded + 4, wave->sequence, 4);
    write_le(encoded + 8, TOOLBOX_SAMPLE_RATE, 4);
    for (unsigned i = 0; i < TOOLBOX_WAVE_SAMPLES; ++i) {
        write_le(encoded + 12 + 2 * i, wave->samples[i], 2);
    }
    return send_ws(HTTPD_WS_TYPE_BINARY, encoded, sizeof(encoded));
}

// Runs on the HTTP task. There is at most one pending callback and it reads the newest waveform.
static void emit(void *context)
{
    static toolbox_scan_t scan;
    static toolbox_wave_t wave;
    const int64_t now = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    if (controller != -1) {
        if (now - last_heartbeat >= TOOLBOX_LEASE_MS * 1000) {
            err = ESP_ERR_TIMEOUT;
        }
        if (err == ESP_OK && toolbox_scan_snapshot(scan_revision, &scan)) {
            err = send_scan(&scan);
            scan_revision = scan.revision;
        }
        if (err == ESP_OK && now >= next_status) {
            err = send_status();
            next_status = now + 500000;
        }
        if (err == ESP_OK && toolbox_wave_snapshot(wave_sequence, &wave)) {
            err = send_wave(&wave);
            wave_sequence = wave.sequence;
        }
        if (err != ESP_OK) {
            toolbox_signal_release();
            httpd_sess_trigger_close(server, controller);
        }
    }
    atomic_store(&work_pending, false);
}

static void stream_task(void *arg)
{
    for (;;) {
        if (!atomic_exchange(&work_pending, true) && httpd_queue_work(server, emit, NULL) != ESP_OK) {
            atomic_store(&work_pending, false);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 6;
    config.max_uri_handlers = 8;
    config.stack_size = 6144;
    config.close_fn = controller_closed;
    config.recv_wait_timeout = 1;
    config.send_wait_timeout = 1;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "HTTP startup");
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = home},
        {.uri = "/api/capabilities", .method = HTTP_GET, .handler = capabilities},
        {.uri = "/api/scan", .method = HTTP_POST, .handler = control},
        {.uri = "/api/pwm", .method = HTTP_POST, .handler = control},
        {.uri = "/api/scope", .method = HTTP_POST, .handler = control},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_recv, .is_websocket = true,
         .ws_pre_handshake_cb = ws_before, .ws_post_handshake_cb = ws_after},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]), TAG, "HTTP route");
    }
    if (xTaskCreate(stream_task, "tool_stream", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Control console and WebSocket listening on port 80");
    return ESP_OK;
}
