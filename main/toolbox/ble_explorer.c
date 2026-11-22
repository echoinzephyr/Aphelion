#include "ble_explorer.h"

#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "nimble/hci_common.h"
#include "os/os_mbuf.h"

enum { EVENT_CAPACITY = 8, NO_CONNECTION = UINT16_MAX };
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static explorer_snapshot_t session;
static explorer_value_t queue[EVENT_CAPACITY];
static unsigned queue_start, queue_count;
static uint16_t connection = NO_CONNECTION;
static unsigned discover_index;
static int64_t started_us;

static struct {
    explorer_value_t value;
    uint16_t cccd;
    uint8_t mode;
    bool inside_characteristic;
} operation;

static int gap_event(struct ble_gap_event *event, void *arg);
static int discover_characteristics(uint16_t conn);

static void push(const explorer_value_t *value)
{
    if (queue_count == EVENT_CAPACITY) {
        queue_start = (queue_start + 1) % EVENT_CAPACITY;
        --queue_count;
        ++session.dropped;
        ++session.revision;
    }
    queue[(queue_start + queue_count) % EVENT_CAPACITY] = *value;
    ++queue_count;
}

static explorer_characteristic_t *characteristic(uint16_t handle)
{
    for (unsigned i = 0; i < session.characteristic_count; ++i) {
        if (session.chars[i].handle == handle) return &session.chars[i];
    }
    return NULL;
}

bool explorer_active(void)
{
    portENTER_CRITICAL(&lock);
    const bool active = session.phase != BLE_IDLE;
    portEXIT_CRITICAL(&lock);
    return active;
}

bool explorer_snapshot(uint32_t after, explorer_snapshot_t *out)
{
    portENTER_CRITICAL(&lock);
    const bool changed = after != session.revision;
    if (changed) *out = session;
    portEXIT_CRITICAL(&lock);
    return changed;
}

bool explorer_next_value(explorer_value_t *out)
{
    portENTER_CRITICAL(&lock);
    const bool present = queue_count != 0;
    if (present) {
        *out = queue[queue_start];
        queue_start = (queue_start + 1) % EVENT_CAPACITY;
        --queue_count;
    }
    portEXIT_CRITICAL(&lock);
    return present;
}

static void disconnected(int reason)
{
    session.phase = BLE_IDLE;
    session.busy = false;
    session.error = reason;
    session.service_count = session.characteristic_count = 0;
    connection = NO_CONNECTION;
    ++session.revision;
}

void explorer_reset(int reason)
{
    portENTER_CRITICAL(&lock);
    if (session.phase != BLE_IDLE) disconnected(reason);
    portEXIT_CRITICAL(&lock);
}

static int discovery_done(uint16_t conn, int error)
{
    portENTER_CRITICAL(&lock);
    if (conn == connection && session.phase == BLE_DISCOVERING) {
        session.phase = BLE_READY;
        session.error = error;
        ++session.revision;
    }
    portEXIT_CRITICAL(&lock);
    return 0;
}

static int characteristic_discovered(uint16_t conn, const struct ble_gatt_error *error,
                                    const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == BLE_HS_EDONE) {
        portENTER_CRITICAL(&lock);
        if (conn == connection && session.phase == BLE_DISCOVERING) ++discover_index;
        portEXIT_CRITICAL(&lock);
        return discover_characteristics(conn);
    }
    if (error->status) return discovery_done(conn, error->status);
    char uuid[37];
    ble_uuid_to_str(&chr->uuid.u, uuid);
    portENTER_CRITICAL(&lock);
    if (conn == connection && session.phase == BLE_DISCOVERING) {
        if (session.characteristic_count < BLE_EXPLORER_CHARS) {
            explorer_characteristic_t *out = &session.chars[session.characteristic_count++];
            *out = (explorer_characteristic_t){
                .service = session.services[discover_index].start, .definition = chr->def_handle,
                .handle = chr->val_handle, .properties = chr->properties,
            };
            strcpy(out->uuid, uuid);
        } else {
            session.limited = true;
        }
    }
    portEXIT_CRITICAL(&lock);
    return 0;
}

static int discover_characteristics(uint16_t conn)
{
    portENTER_CRITICAL(&lock);
    if (conn != connection || session.phase != BLE_DISCOVERING) {
        portEXIT_CRITICAL(&lock);
        return 0;
    }
    const bool finished = discover_index >= session.service_count;
    explorer_service_t service = finished ? (explorer_service_t){0} : session.services[discover_index];
    portEXIT_CRITICAL(&lock);
    if (finished) return discovery_done(conn, 0);
    const int rc = ble_gattc_disc_all_chrs(conn, service.start, service.end,
                                         characteristic_discovered, NULL);
    return rc ? discovery_done(conn, rc) : 0;
}

static int service_discovered(uint16_t conn, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *arg)
{
    if (error->status == BLE_HS_EDONE) return discover_characteristics(conn);
    if (error->status) return discovery_done(conn, error->status);
    char uuid[37];
    ble_uuid_to_str(&service->uuid.u, uuid);
    portENTER_CRITICAL(&lock);
    if (conn == connection && session.phase == BLE_DISCOVERING) {
        if (session.service_count < BLE_EXPLORER_SERVICES) {
            explorer_service_t *out = &session.services[session.service_count++];
            out->start = service->start_handle;
            out->end = service->end_handle;
            strcpy(out->uuid, uuid);
        } else {
            session.limited = true;
        }
    }
    portEXIT_CRITICAL(&lock);
    return 0;
}

static int discover_services(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    portENTER_CRITICAL(&lock);
    const bool active = conn == connection && session.phase == BLE_DISCOVERING;
    if (active && error && error->status == 0) session.mtu = mtu;
    portEXIT_CRITICAL(&lock);
    if (active) {
        const int rc = ble_gattc_disc_all_svcs(conn, service_discovered, NULL);
        if (rc) discovery_done(conn, rc);
    }
    return 0;
}

static void receive_notification(struct ble_gap_event *event)
{
    const struct os_mbuf *mbuf = event->notify_rx.om;
    const unsigned size = OS_MBUF_PKTLEN(mbuf);
    explorer_value_t value = {
        .event = event->notify_rx.indication ? BLE_INDICATE : BLE_NOTIFY,
        .handle = event->notify_rx.attr_handle,
        .length = size > BLE_EXPLORER_VALUE ? BLE_EXPLORER_VALUE : size,
        .truncated = size > BLE_EXPLORER_VALUE,
    };
    if (os_mbuf_copydata(mbuf, 0, value.length, value.value)) return;
    portENTER_CRITICAL(&lock);
    if (event->notify_rx.conn_handle == connection && session.phase == BLE_READY) {
        value.elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
        push(&value);
    }
    portEXIT_CRITICAL(&lock);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        const int status = event->connect.status;
        const uint16_t conn = event->connect.conn_handle;
        portENTER_CRITICAL(&lock);
        const bool wanted = session.phase == BLE_CONNECTING;
        if (status) {
            if (wanted || session.phase == BLE_CLOSING) disconnected(wanted ? status : 0);
        } else if (wanted) {
            connection = conn;
            session.phase = BLE_DISCOVERING;
            session.mtu = 23;
            ++session.revision;
        } else {
            connection = conn;
            session.phase = BLE_CLOSING;
        }
        portEXIT_CRITICAL(&lock);
        if (!status && wanted) {
            if (ble_gattc_exchange_mtu(conn, discover_services, NULL)) discover_services(conn, NULL, 23, NULL);
        } else if (!status) {
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        portENTER_CRITICAL(&lock);
        if (event->disconnect.conn.conn_handle == connection) {
            disconnected(session.phase == BLE_CLOSING ? 0 : event->disconnect.reason);
        }
        portEXIT_CRITICAL(&lock);
    } else if (event->type == BLE_GAP_EVENT_NOTIFY_RX) {
        receive_notification(event);
    }
    return 0;
}

int explorer_connect(uint8_t own_address_type, uint8_t type, const uint8_t address[6])
{
    portENTER_CRITICAL(&lock);
    if (session.phase != BLE_IDLE) {
        portEXIT_CRITICAL(&lock);
        return BLE_HS_EBUSY;
    }
    const uint32_t revision = session.revision + 1;
    // Counts are reset; storage is reused as discovery callbacks populate it.
    session.revision = revision;
    session.dropped = 0;
    session.phase = BLE_CONNECTING;
    session.error = 0;
    session.limited = false;
    session.mtu = 23;
    session.address_type = type;
    memcpy(session.address, address, sizeof(session.address));
    queue_start = queue_count = discover_index = 0;
    started_us = esp_timer_get_time();
    portEXIT_CRITICAL(&lock);

    ble_addr_t peer = {.type = type};
    memcpy(peer.val, address, sizeof(peer.val));
    const int rc = ble_gap_connect(own_address_type, &peer, 9000, NULL, gap_event, NULL);
    if (rc) explorer_reset(rc);
    return rc;
}

int explorer_disconnect(void)
{
    portENTER_CRITICAL(&lock);
    const explorer_phase_t previous = session.phase;
    const uint16_t conn = connection;
    if (previous == BLE_IDLE || previous == BLE_CLOSING) {
        portEXIT_CRITICAL(&lock);
        return 0;
    }
    session.phase = BLE_CLOSING;
    ++session.revision;
    portEXIT_CRITICAL(&lock);
    const int rc = conn == NO_CONNECTION ? ble_gap_conn_cancel() :
                                         ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc && rc != BLE_HS_EALREADY) {
        portENTER_CRITICAL(&lock);
        if (session.phase == BLE_CLOSING && connection == conn) {
            if (rc == BLE_HS_ENOTCONN) disconnected(0);
            else {
                session.phase = previous;
                session.error = rc;
                ++session.revision;
            }
        }
        portEXIT_CRITICAL(&lock);
    }
    return rc == BLE_HS_ENOTCONN || rc == BLE_HS_EALREADY ? 0 : rc;
}

void explorer_release(void)
{
    explorer_disconnect();
    portENTER_CRITICAL(&lock);
    queue_start = queue_count = 0;
    portEXIT_CRITICAL(&lock);
}

static bool matches_operation(uint16_t conn)
{
    return conn == connection && session.phase == BLE_READY && session.busy;
}

static void finish_operation(uint16_t conn, int status, bool unacknowledged)
{
    portENTER_CRITICAL(&lock);
    if (matches_operation(conn)) {
        operation.value.status = status;
        operation.value.unacknowledged = unacknowledged;
        operation.value.elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
        if (!status && operation.value.event == BLE_SUBSCRIBE) {
            explorer_characteristic_t *chr = characteristic(operation.value.handle);
            if (chr) {
                chr->cccd = operation.cccd;
                chr->subscription = operation.mode;
            }
        }
        push(&operation.value);
        session.busy = false;
        ++session.revision;
    }
    portEXIT_CRITICAL(&lock);
}

static int written(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    finish_operation(conn, error->status, false);
    return 0;
}

static int read_fragment(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    if (error->status || !attr) {
        finish_operation(conn, error->status == BLE_HS_EDONE ? 0 : error->status, false);
        return 0;
    }
    const unsigned size = OS_MBUF_PKTLEN(attr->om);
    bool full = false;
    portENTER_CRITICAL(&lock);
    if (matches_operation(conn)) {
        const unsigned room = BLE_EXPLORER_VALUE - operation.value.length;
        const unsigned copied = size < room ? size : room;
        if (!os_mbuf_copydata(attr->om, 0, copied, operation.value.value + operation.value.length)) {
            operation.value.length += copied;
            operation.value.truncated |= copied < size;
            full = operation.value.length == BLE_EXPLORER_VALUE;
        }
    }
    portEXIT_CRITICAL(&lock);
    if (full) {
        finish_operation(conn, 0, false);
        return BLE_HS_EDONE;
    }
    return 0;
}

static int begin_operation(uint16_t handle, explorer_event_t type, uint8_t property,
                           explorer_characteristic_t *chr, uint16_t *conn, uint16_t *mtu)
{
    portENTER_CRITICAL(&lock);
    if (session.phase != BLE_READY || session.busy) {
        portEXIT_CRITICAL(&lock);
        return BLE_HS_EBUSY;
    }
    const explorer_characteristic_t *known = characteristic(handle);
    if (!known || !(known->properties & property)) {
        portEXIT_CRITICAL(&lock);
        return BLE_HS_ENOTSUP;
    }
    *chr = *known;
    *conn = connection;
    *mtu = session.mtu;
    operation.value = (explorer_value_t){.event = type, .handle = handle};
    session.busy = true;
    ++session.revision;
    portEXIT_CRITICAL(&lock);
    return 0;
}

int explorer_read(uint16_t handle)
{
    explorer_characteristic_t chr;
    uint16_t conn, mtu;
    int rc = begin_operation(handle, BLE_READ, BLE_GATT_CHR_PROP_READ, &chr, &conn, &mtu);
    if (rc) return rc;
    rc = ble_gattc_read_long(conn, handle, 0, read_fragment, NULL);
    if (rc) finish_operation(conn, rc, false);
    return 0;
}

int explorer_write(uint16_t handle, const uint8_t *value, size_t length, bool without_response)
{
    if (length > BLE_EXPLORER_VALUE) return BLE_HS_EMSGSIZE;
    explorer_characteristic_t chr;
    uint16_t conn, mtu;
    const uint8_t property = without_response ? BLE_GATT_CHR_PROP_WRITE_NO_RSP : BLE_GATT_CHR_PROP_WRITE;
    int rc = begin_operation(handle, BLE_WRITE, property, &chr, &conn, &mtu);
    if (rc) return rc;
    portENTER_CRITICAL(&lock);
    if (matches_operation(conn)) {
        operation.value.length = length;
        memcpy(operation.value.value, value, length);
    }
    portEXIT_CRITICAL(&lock);
    if (without_response) {
        rc = length > (unsigned)(mtu - 3) ? BLE_HS_EMSGSIZE : ble_gattc_write_no_rsp_flat(conn, handle, value, length);
        finish_operation(conn, rc, true);
    } else {
        if (length <= (unsigned)(mtu - 3)) {
            rc = ble_gattc_write_flat(conn, handle, value, length, written, NULL);
        } else {
            struct os_mbuf *buf = ble_hs_mbuf_from_flat(value, length);
            rc = buf ? ble_gattc_write_long(conn, handle, 0, buf, written, NULL) : BLE_HS_ENOMEM;
        }
        if (rc) finish_operation(conn, rc, false);
    }
    return 0;
}

static void write_cccd(uint16_t conn, uint16_t handle, uint8_t mode)
{
    const uint8_t bytes[] = {mode, 0};
    const int rc = handle ? ble_gattc_write_flat(conn, handle, bytes, sizeof(bytes), written, NULL) : BLE_HS_ENOENT;
    if (rc) finish_operation(conn, rc, false);
}

static int descriptor_discovered(uint16_t conn, const struct ble_gatt_error *error,
                                 uint16_t characteristic_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    portENTER_CRITICAL(&lock);
    const bool active = matches_operation(conn) && operation.value.event == BLE_SUBSCRIBE;
    if (active && !error->status) {
        const uint16_t uuid = ble_uuid_u16(&dsc->uuid.u);
        if (uuid == BLE_ATT_UUID_CHARACTERISTIC) operation.inside_characteristic = false;
        if (uuid == BLE_GATT_DSC_CLT_CFG_UUID16 && operation.inside_characteristic && !operation.cccd) {
            operation.cccd = dsc->handle;
        }
    }
    const uint16_t handle = operation.cccd;
    const uint8_t mode = operation.mode;
    portEXIT_CRITICAL(&lock);
    if (active && error->status) {
        if (error->status == BLE_HS_EDONE) write_cccd(conn, handle, mode);
        else finish_operation(conn, error->status, false);
    }
    return 0;
}

int explorer_subscribe(uint16_t handle, uint8_t mode)
{
    explorer_characteristic_t chr;
    uint16_t conn, mtu;
    const uint8_t property = mode == 1 ? BLE_GATT_CHR_PROP_NOTIFY :
        mode == 2 ? BLE_GATT_CHR_PROP_INDICATE : BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE;
    int rc = begin_operation(handle, BLE_SUBSCRIBE, property, &chr, &conn, &mtu);
    if (rc) return rc;
    uint16_t end = handle;
    portENTER_CRITICAL(&lock);
    if (matches_operation(conn)) {
        operation.mode = mode;
        operation.cccd = chr.cccd;
        operation.inside_characteristic = true;
        operation.value.value[0] = mode;
        operation.value.length = 1;
        for (unsigned i = 0; i < session.service_count; ++i) {
            if (session.services[i].start == chr.service) end = session.services[i].end;
        }
        for (unsigned i = 0; i < session.characteristic_count; ++i) {
            const explorer_characteristic_t *next = &session.chars[i];
            if (next->service == chr.service && next->definition > handle && next->definition <= end) {
                end = next->definition - 1;
            }
        }
    }
    portEXIT_CRITICAL(&lock);
    if (chr.cccd || end == handle) {
        write_cccd(conn, chr.cccd, mode);
    } else {
        rc = ble_gattc_disc_all_dscs(conn, handle, end, descriptor_discovered, NULL);
        if (rc) finish_operation(conn, rc, false);
    }
    return 0;
}
