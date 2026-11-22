#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { BLE_EXPLORER_SERVICES = 24, BLE_EXPLORER_CHARS = 48, BLE_EXPLORER_VALUE = 512 };

typedef enum { BLE_IDLE, BLE_CONNECTING, BLE_DISCOVERING, BLE_READY, BLE_CLOSING } explorer_phase_t;
typedef enum { BLE_READ, BLE_WRITE, BLE_SUBSCRIBE, BLE_NOTIFY, BLE_INDICATE } explorer_event_t;

typedef struct {
    uint16_t start, end;
    char uuid[37];
} explorer_service_t;

typedef struct {
    uint16_t service, definition, handle, cccd;
    uint8_t properties, subscription;
    char uuid[37];
} explorer_characteristic_t;

typedef struct {
    uint32_t revision, dropped;
    explorer_phase_t phase;
    int error;
    bool busy, limited;
    uint16_t mtu;
    uint8_t address[6], address_type, service_count, characteristic_count;
    explorer_service_t services[BLE_EXPLORER_SERVICES];
    explorer_characteristic_t chars[BLE_EXPLORER_CHARS];
} explorer_snapshot_t;

typedef struct {
    int64_t elapsed_ms;
    explorer_event_t event;
    uint16_t handle, length;
    int status;
    bool truncated, unacknowledged;
    uint8_t value[BLE_EXPLORER_VALUE];
} explorer_value_t;

bool explorer_active(void);
bool explorer_snapshot(uint32_t after, explorer_snapshot_t *out);
bool explorer_next_value(explorer_value_t *out);
void explorer_reset(int reason);
int explorer_connect(uint8_t own_address_type, uint8_t type, const uint8_t address[6]);
int explorer_disconnect(void);
void explorer_release(void);
int explorer_read(uint16_t handle);
int explorer_write(uint16_t handle, const uint8_t *value, size_t length, bool without_response);
int explorer_subscribe(uint16_t handle, uint8_t mode);
