#include "network.h"
#include "oled.h"
#include "sensors.h"
#include "terminal.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "web";
extern const char dashboard[] asm("_binary_dashboard_html_start");

static bool on_setup_ap(httpd_req_t *req)
{
    struct sockaddr_storage local = {0};
    socklen_t size = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &size) != 0) {
        return false;
    }
    uint32_t ap_ip = inet_addr("192.168.4.1");
    if (local.ss_family == AF_INET) {
        return ((struct sockaddr_in *)&local)->sin_addr.s_addr == ap_ip;
    }
#if CONFIG_LWIP_IPV6
    if (local.ss_family == AF_INET6) {
        const struct in6_addr *addr = &((struct sockaddr_in6 *)&local)->sin6_addr;
        return IN6_IS_ADDR_V4MAPPED(addr) && !memcmp(&addr->s6_addr[12], &ap_ip, 4);
    }
#endif
    return false;
}

static esp_err_t home(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, dashboard);
}

static void number(char out[16], bool valid, float value)
{
    if (valid && isfinite(value)) {
        snprintf(out, 16, "%.1f", value);
    } else {
        strcpy(out, "null");
    }
}

// SSIDs are byte strings. Hex keeps the JSON valid even for legacy encodings.
static void ssid_hex(const char *ssid, char out[65])
{
    static const char digits[] = "0123456789abcdef";
    size_t n = strnlen(ssid, 32);
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = (uint8_t)ssid[i];
        out[2*i] = digits[b >> 4];
        out[2*i+1] = digits[b & 15];
    }
    out[2*n] = 0;
}

static esp_err_t status(httpd_req_t *req)
{
    environment_t env;
    sound_t sound;
    sensor_snapshot(&env, &sound);
    char temp[16], rh[16], dbfs[16], spl[16];
    bool audio_valid = sound.status == SAMPLE_OK || sound.status == SAMPLE_CLIPPING;
    number(temp, env.status == SAMPLE_OK, env.celsius);
    number(rh, env.status == SAMPLE_OK, env.humidity);
    number(dbfs, audio_valid, sound.dbfs);
    number(spl, audio_valid, sound.spl);
    static const char *const phases[] = {"starting", "setup", "connecting", "online", "failed"};
    network_info_t net = network_snapshot();
    char connected[65];
    ssid_hex(net.connected_ssid, connected);
    char json[700];
    snprintf(json, sizeof(json),
             "{\"temperature\":%s,\"humidity\":%s,\"dbfs\":%s,\"spl\":%s,"
             "\"environment\":\"%s\",\"sound\":\"%s\",\"uptime\":%" PRId64 ","
             "\"network\":\"%s\",\"ap\":\"%s\",\"ip\":\"%s\",\"setup\":%s,\"saved\":%s,\"ssid\":\"%s\","
             "\"oled_view\":\"%s\"}",
             temp, rh, dbfs, spl, sample_status_label(env.status), sample_status_label(sound.status),
             (int64_t)(esp_timer_get_time() / 1000000), phases[net.phase], net.ap, net.ip,
             on_setup_ap(req) ? "true" : "false", net.saved ? "true" : "false", connected,
             net.ip[0] ? terminal_view_name(oled_selected_view()) : "setup");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t virtual_button(httpd_req_t *req)
{
    char marker[8];
    if (httpd_req_get_hdr_value_str(req, "X-Aphelion", marker, sizeof(marker)) != ESP_OK ||
        strcmp(marker, "button") != 0) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Use the device WebUI");
    }
    if (req->content_len) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unexpected body");
    }
    if (network_snapshot().phase != NET_ONLINE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "OLED is showing Wi-Fi setup");
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"oled_view\":\"%s\"}", terminal_view_name(oled_press()));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

// Decode one application/x-www-form-urlencoded field, rejecting NULs and overflow.
static bool decode(const char *src, char *dst, size_t capacity)
{
    size_t used = 0;
    while (*src) {
        unsigned char c = *src++;
        if (c == '%') {
            if (!isxdigit((unsigned char)src[0]) || !isxdigit((unsigned char)src[1])) {
                return false;
            }
            char hex[] = {src[0], src[1], 0};
            c = strtoul(hex, NULL, 16);
            src += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (!c || used + 1 >= capacity) {
            return false;
        }
        dst[used++] = c;
    }
    dst[used] = 0;
    return true;
}

static bool decode_ssid_hex(const char *hex, char out[33])
{
    size_t len = strlen(hex);
    if (!len || len > 64 || len % 2 || strspn(hex, "0123456789abcdefABCDEF") != len) return false;
    for (size_t i = 0; i < len; i += 2) {
        char byte[] = {hex[i], hex[i+1], 0};
        out[i/2] = strtoul(byte, NULL, 16);
        if (!out[i/2]) return false;
    }
    out[len/2] = 0;
    return true;
}

static bool parse_join(char *body, wifi_credentials_t *creds)
{
    bool ssid = false, password = false;
    char *field;
    while ((field = strsep(&body, "&")) != NULL) {
        if (!ssid && !strncmp(field, "ssid=", 5)) {
            ssid = decode(field + 5, creds->ssid, sizeof(creds->ssid));
            if (!ssid) return false;
        } else if (!ssid && !strncmp(field, "ssid_hex=", 9)) {
            ssid = decode_ssid_hex(field + 9, creds->ssid);
            if (!ssid) return false;
        } else if (!password && !strncmp(field, "password=", 9)) {
            password = decode(field + 9, creds->password, sizeof(creds->password));
            if (!password) return false;
        } else {
            return false;
        }
    }
    size_t n = strlen(creds->password);
    return ssid && creds->ssid[0] && password &&
           (!n || (n >= 8 && n < 64) || (n == 64 && strspn(creds->password, "0123456789abcdefABCDEF") == 64));
}

static bool setup_action(httpd_req_t *req)
{
    char marker[8];
    return on_setup_ap(req) && httpd_req_get_hdr_value_str(req, "X-Aphelion", marker, sizeof(marker)) == ESP_OK &&
           strcmp(marker, "setup") == 0;
}

static esp_err_t scan_start(httpd_req_t *req)
{
    if (!setup_action(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Use setup hotspot");
    if (!network_scan_request()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Busy");
    }
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "Scanning");
}

static esp_err_t scan_results(httpd_req_t *req)
{
    if (!on_setup_ap(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Use setup hotspot");
    wifi_scan_list_t list;
    network_scan_snapshot(&list);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[192];
    snprintf(buf, sizeof(buf), "{\"busy\":%s,\"failed\":%s,\"fresh\":%s,\"networks\":[",
             list.busy ? "true" : "false", list.failed ? "true" : "false",
             list.scanned_us && esp_timer_get_time() - list.scanned_us < 15000000 ? "true" : "false");
    esp_err_t err = httpd_resp_sendstr_chunk(req, buf);
    for (unsigned i = 0; err == ESP_OK && i < list.count; ++i) {
        const nearby_network_t *item = &list.items[i];
        char hex[65];
        ssid_hex(item->ssid, hex);
        snprintf(buf, sizeof(buf), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"security\":\"%s\",\"supported\":%s}",
                 i ? "," : "", hex, item->rssi, item->security, item->supported ? "true" : "false");
        err = httpd_resp_sendstr_chunk(req, buf);
    }
    if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, "]}");
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t join(httpd_req_t *req)
{
    if (!setup_action(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Use setup hotspot");
    char body[320];
    if (!req->content_len || req->content_len >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form length");
    }
    size_t read = 0;
    while (read < req->content_len) {
        int n = httpd_req_recv(req, body + read, req->content_len - read);
        if (n <= 0) return ESP_FAIL;
        read += n;
    }
    body[read] = 0;
    wifi_credentials_t creds = {0};
    if (memchr(body, 0, read) || !parse_join(body, &creds)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid network name or password length");
    }
    if (!network_join(&creds)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "A connection request is pending");
    }
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "Connecting");
}

esp_err_t web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "server");
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = home},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status},
        {.uri = "/api/button", .method = HTTP_POST, .handler = virtual_button},
        {.uri = "/api/scan", .method = HTTP_POST, .handler = scan_start},
        {.uri = "/api/networks", .method = HTTP_GET, .handler = scan_results},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = join},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]), TAG, "route");
    }
    return ESP_OK;
}
