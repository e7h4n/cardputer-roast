// SPDX-License-Identifier: MIT
/*
 * Bluetooth Wi-Fi/Dropbox setup (see roast_net.h). Ported from e7h4n/coffee-watch firmware/demo/main/wifi_setup.c:
 * the GATT service, LE Secure Connections with a displayed passkey, the Wi-Fi state machine and the status JSON are
 * the same, so https://e7h4n.github.io/coffee-watch/ works unchanged. Differences: everything starts in
 * roast_net_open() and stops in roast_net_close() (the Cardputer only needs the network while this screen is open),
 * the UI polls roast_net_view() instead of receiving events, and the Wi-Fi and Dropbox NVS namespaces are rn_*.
 */
#include "roast_net.h"
#include "roast_dropbox.h"
#include "wifi_packet.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "mbedtls/platform_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);

#define TAG "rn-setup"
#define WIFI_NS "rn_wifi"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static rn_view_t view;
static atomic_bool ble_ready, requested_open, opened;
static bool ble_running, wifi_running, network_task_running;
static uint8_t own_type;
static uint16_t browser = BLE_HS_CONN_HANDLE_NONE, signal_handle;
static struct ble_npl_event control_event, notify_event;
static QueueHandle_t network_queue;
static SemaphoreHandle_t network_done;
static esp_netif_t *station_interface;
static esp_event_handler_instance_t wifi_handler, ip_handler;
static char status_json[384];
static int status_length;
static char dropbox_json[192];
static int dropbox_length;

enum { OP_JOIN = 1, OP_FORGET = 2, OP_START = 10, OP_DOWN, OP_IP, OP_QUIT };
typedef struct {
    cw_wifi_packet_t packet;
    int op, reason;
    uint32_t ip_address;
    char ip[16];
} network_command_t;

// c9a81001-9a9d-4f55-a621-5b4e7699b647 and siblings (byte order reversed for NimBLE)
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(0x47, 0xb6, 0x99, 0x76, 0x4e, 0x5b, 0x21, 0xa6, 0x55, 0x4f, 0x9d, 0x9a, 0x01, 0x10, 0xa8, 0xc9);
static const ble_uuid128_t config_uuid = BLE_UUID128_INIT(0x47, 0xb6, 0x99, 0x76, 0x4e, 0x5b, 0x21, 0xa6, 0x55, 0x4f, 0x9d, 0x9a, 0x02, 0x10, 0xa8, 0xc9);
static const ble_uuid128_t status_uuid = BLE_UUID128_INIT(0x47, 0xb6, 0x99, 0x76, 0x4e, 0x5b, 0x21, 0xa6, 0x55, 0x4f, 0x9d, 0x9a, 0x03, 0x10, 0xa8, 0xc9);
static const ble_uuid128_t signal_uuid = BLE_UUID128_INIT(0x47, 0xb6, 0x99, 0x76, 0x4e, 0x5b, 0x21, 0xa6, 0x55, 0x4f, 0x9d, 0x9a, 0x04, 0x10, 0xa8, 0xc9);
static const ble_uuid128_t dropbox_config_uuid = BLE_UUID128_INIT(0x47, 0xb6, 0x99, 0x76, 0x4e, 0x5b, 0x21, 0xa6, 0x55, 0x4f, 0x9d, 0x9a, 0x05, 0x10, 0xa8, 0xc9);
static const ble_uuid128_t dropbox_status_uuid = BLE_UUID128_INIT(0x47, 0xb6, 0x99, 0x76, 0x4e, 0x5b, 0x21, 0xa6, 0x55, 0x4f, 0x9d, 0x9a, 0x06, 0x10, 0xa8, 0xc9);

static rn_view_t snapshot(void)
{
    rn_view_t v;
    portENTER_CRITICAL(&lock);
    v = view;
    portEXIT_CRITICAL(&lock);
    return v;
}

// Every state change invalidates the browser's cached status: one notification, then it reads fresh JSON.
static void publish(void)
{
    if (atomic_load(&ble_ready)) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &notify_event);
}

void rn_dropbox_changed(const char *state, const char *error)
{
    portENTER_CRITICAL(&lock);
    snprintf(view.dropbox, sizeof view.dropbox, "%s", state);
    snprintf(view.dropbox_error, sizeof view.dropbox_error, "%s", error ? error : "");
    portEXIT_CRITICAL(&lock);
    publish();
}

static void network_state(rn_net_state_t state, int reason, const char *ssid, const char *ip)
{
    portENTER_CRITICAL(&lock);
    view.state  = state;
    view.reason = reason;
    if (ssid) snprintf(view.ssid, sizeof view.ssid, "%s", ssid);
    snprintf(view.ip, sizeof view.ip, "%s", ip ? ip : "");
    portEXIT_CRITICAL(&lock);
    publish();
}

static uint32_t request_id;
static void request_state(uint32_t request, rn_net_state_t state, int reason, const char *ssid)
{
    portENTER_CRITICAL(&lock);
    request_id  = request;
    view.state  = state;
    view.reason = reason;
    view.ip[0]  = 0;
    if (ssid) snprintf(view.ssid, sizeof view.ssid, "%s", ssid);
    portEXIT_CRITICAL(&lock);
    publish();
}

/* ------------------------------------------------------------------ credentials */
static bool load_credentials(cw_wifi_packet_t *p)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(WIFI_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t a = sizeof p->ssid, b = sizeof p->password;
        ok = nvs_get_str(h, "ssid", p->ssid, &a) == ESP_OK && nvs_get_str(h, "pass", p->password, &b) == ESP_OK && p->ssid[0];
        nvs_close(h);
    }
    p->op = OP_JOIN;
    return ok;
}

bool roast_net_saved_wifi(char ssid[33])
{
    nvs_handle_t h;
    bool ok = false;
    ssid[0] = 0;
    if (nvs_open(WIFI_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t a = 33;
        ok       = nvs_get_str(h, "ssid", ssid, &a) == ESP_OK && ssid[0];
        nvs_close(h);
    }
    return ok;
}

bool roast_net_saved_dropbox(void)
{
    return rn_dropbox_saved();
}

static esp_err_t save_credentials(const cw_wifi_packet_t *p)
{
    nvs_handle_t h;
    esp_err_t rc = nvs_open(WIFI_NS, NVS_READWRITE, &h);
    if (rc != ESP_OK) return rc;
    rc = nvs_set_str(h, "ssid", p->ssid);
    if (rc == ESP_OK) rc = nvs_set_str(h, "pass", p->password);
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h);
    return rc;
}

static esp_err_t erase_credentials(void)
{
    nvs_handle_t h;
    esp_err_t rc = nvs_open(WIFI_NS, NVS_READWRITE, &h);
    if (rc != ESP_OK) return rc;
    rc = nvs_erase_all(h);
    if (rc == ESP_OK) rc = nvs_commit(h);
    nvs_close(h);
    return rc;
}

/* -------------------------------------------------------------------- Wi-Fi */
static bool connect_once(cw_wifi_packet_t *p)
{
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, p->ssid, strlen(p->ssid));
    memcpy(config.sta.password, p->password, strlen(p->password));
    config.sta.threshold.authmode = p->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.sae_pwe_h2e        = WPA3_SAE_PWE_BOTH;
    config.sta.pmf_cfg.capable    = true;
    config.sta.pmf_cfg.required   = false;
    esp_err_t rc = esp_wifi_set_config(WIFI_IF_STA, &config);
    mbedtls_platform_zeroize(&config, sizeof config);
    if (rc == ESP_OK) rc = esp_wifi_connect();
    network_state(rc == ESP_OK ? RN_NET_CONNECTING : RN_NET_FAILED, rc, p->ssid, NULL);
    if (rc != ESP_OK) mbedtls_platform_zeroize(p->password, sizeof p->password);
    return rc == ESP_OK;
}

// One task serializes every Wi-Fi operation. Only a real IP event confirms a connection and commits credentials; a
// failure leaves the previously saved network untouched. No retry loop, no fallback network.
static void network_task(void *arg)
{
    cw_wifi_packet_t active = {0}, pending = {0};
    bool attempt = false, changing = false, forgetting = false, credential_pending = false, disconnect_pending = false;
    network_command_t c;
    for (;;) {
        if (xQueueReceive(network_queue, &c, portMAX_DELAY) != pdTRUE) continue;
        if (c.op == OP_QUIT) break;
        if (c.op == OP_START) {
            if (load_credentials(&active)) {
                portENTER_CRITICAL(&lock);
                view.saved = true;
                portEXIT_CRITICAL(&lock);
                attempt            = connect_once(&active);
                credential_pending = false;  // already saved
            }
        } else if (c.op == OP_JOIN) {
            pending  = c.packet;
            changing = true;
            forgetting = false;
            request_state(c.packet.request_id, RN_NET_CONNECTING, 0, pending.ssid);
            if (attempt && !disconnect_pending) {
                esp_err_t rc = esp_wifi_disconnect();
                if (rc == ESP_OK) disconnect_pending = true;
                if (rc == ESP_ERR_WIFI_NOT_CONNECT)
                    attempt = false;
                else if (rc != ESP_OK) {
                    changing = false;
                    network_state(RN_NET_FAILED, rc, pending.ssid, NULL);
                }
            }
            if (changing && !attempt && !disconnect_pending) {
                active = pending;
                mbedtls_platform_zeroize(&pending, sizeof pending);
                changing           = false;
                attempt            = connect_once(&active);
                credential_pending = attempt;
            }
        } else if (c.op == OP_FORGET) {
            esp_err_t rc = erase_credentials();
            if (rc != ESP_OK) {
                request_state(c.packet.request_id, RN_NET_FAILED, rc, NULL);
            } else {
                forgetting = true;
                changing   = false;
                mbedtls_platform_zeroize(&active, sizeof active);
                mbedtls_platform_zeroize(&pending, sizeof pending);
                if (attempt && !disconnect_pending) disconnect_pending = esp_wifi_disconnect() == ESP_OK;
                attempt            = false;
                credential_pending = false;
                portENTER_CRITICAL(&lock);
                view.saved = false;
                portEXIT_CRITICAL(&lock);
                request_state(c.packet.request_id, RN_NET_UNCONFIGURED, 0, "");
            }
        } else if (c.op == OP_DOWN) {
            bool was_connecting = snapshot().state == RN_NET_CONNECTING;
            attempt = credential_pending = disconnect_pending = false;
            if (changing) {
                active = pending;
                mbedtls_platform_zeroize(&pending, sizeof pending);
                changing           = false;
                attempt            = connect_once(&active);
                credential_pending = attempt;
            } else if (forgetting) {
                forgetting = false;
            } else {
                mbedtls_platform_zeroize(active.password, sizeof active.password);
                network_state(was_connecting ? RN_NET_FAILED : RN_NET_DISCONNECTED, c.reason, NULL, NULL);
            }
        } else if (c.op == OP_IP && !changing && !forgetting && attempt) {
            // Ignore an old network's queued IP event while changing credentials.
            wifi_ap_record_t ap;
            esp_netif_ip_info_t current;
            if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK || strcmp((const char *)ap.ssid, active.ssid) ||
                esp_netif_get_ip_info(station_interface, &current) != ESP_OK || !current.ip.addr ||
                current.ip.addr != c.ip_address) {
                mbedtls_platform_zeroize(&c, sizeof c);
                continue;
            }
            esp_err_t rc = ESP_OK;
            if (credential_pending) {
                rc = save_credentials(&active);
                portENTER_CRITICAL(&lock);
                view.saved = rc == ESP_OK;
                portEXIT_CRITICAL(&lock);
                credential_pending = false;
            }
            network_state(RN_NET_CONNECTED, rc, active.ssid, c.ip);
            mbedtls_platform_zeroize(active.password, sizeof active.password);
        }
        mbedtls_platform_zeroize(&c, sizeof c);
    }
    mbedtls_platform_zeroize(&active, sizeof active);
    mbedtls_platform_zeroize(&pending, sizeof pending);
    xSemaphoreGive(network_done);
    vTaskDelete(NULL);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    network_command_t c = {0};
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        c.op = OP_START;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        c.op     = OP_DOWN;
        c.reason = ((wifi_event_sta_disconnected_t *)data)->reason;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        c.op                 = OP_IP;
        c.ip_address         = e->ip_info.ip.addr;
        snprintf(c.ip, sizeof c.ip, IPSTR, IP2STR(&e->ip_info.ip));
    } else {
        return;
    }
    if (network_queue) xQueueSend(network_queue, &c, 0);
}

/* --------------------------------------------------------------------- GATT */
static void json_escape(char *out, const char *s)
{
    while (*s) {
        unsigned char c = *s++;
        if (c == '"' || c == '\\') {
            *out++ = '\\';
            *out++ = c;
        } else if (c < 32) {
            sprintf(out, "\\u%04x", c);
            out += 6;
        } else {
            *out++ = c;
        }
    }
    *out = 0;
}

// Writes need the browser's authenticated (passkey) and encrypted link; reads are limited to the same connection.
static bool authenticated(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    return atomic_load(&requested_open) && conn == browser && !ble_gap_conn_find(conn, &desc) &&
           desc.sec_state.authenticated && desc.sec_state.encrypted;
}

static int access_config(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    if (!authenticated(conn)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    uint8_t bytes[104];
    uint16_t size       = 0;
    network_command_t c = {0};
    if (ble_hs_mbuf_to_flat(ctx->om, bytes, sizeof bytes, &size)) {
        mbedtls_platform_zeroize(bytes, sizeof bytes);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    bool valid = cw_wifi_packet_decode(bytes, size, &c.packet);
    mbedtls_platform_zeroize(bytes, sizeof bytes);
    if (!valid) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    c.op        = c.packet.op;
    bool queued = xQueueSend(network_queue, &c, 0) == pdTRUE;
    mbedtls_platform_zeroize(&c, sizeof c);
    return queued ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_status(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    if (!atomic_load(&requested_open) || conn != browser) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    static const char *states[] = {"unconfigured", "connecting", "connected", "failed", "disconnected"};
    if (ctx->offset && !status_length) return BLE_ATT_ERR_INVALID_OFFSET;
    // One coherent JSON snapshot across all Read Blob fragments.
    if (ctx->offset == 0) {
        rn_view_t v = snapshot();
        char ssid[193];
        json_escape(ssid, v.ssid);
        status_length = snprintf(status_json, sizeof status_json,
                                 "{\"v\":1,\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"reason\":%d,\"request\":%lu,\"saved\":%s}",
                                 states[v.state], ssid, v.ip, v.reason, (unsigned long)request_id, v.saved ? "true" : "false");
    }
    return os_mbuf_append(ctx->om, status_json, status_length) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_dropbox_config(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    if (!authenticated(conn)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    uint8_t bytes[CW_DROPBOX_PACKET_MAX];
    uint16_t size                = 0;
    cw_dropbox_command_t command = {0};
    if (ble_hs_mbuf_to_flat(ctx->om, bytes, sizeof bytes, &size)) {
        mbedtls_platform_zeroize(bytes, sizeof bytes);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    bool valid = cw_dropbox_packet_decode(bytes, size, &command);
    mbedtls_platform_zeroize(bytes, sizeof bytes);
    if (!valid) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    bool queued = rn_dropbox_submit(&command);
    mbedtls_platform_zeroize(&command, sizeof command);
    return queued ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_dropbox_status(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    if (!atomic_load(&requested_open) || conn != browser) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (ctx->offset && !dropbox_length) return BLE_ATT_ERR_INVALID_OFFSET;
    if (!ctx->offset) dropbox_length = rn_dropbox_status(dropbox_json, sizeof dropbox_json);
    if (dropbox_length <= 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
    return os_mbuf_append(ctx->om, dropbox_json, dropbox_length) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_signal(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def services[] = {
    {.type            = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid            = &service_uuid.u,
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = &config_uuid.u, .access_cb = access_config,
          .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN},
         {.uuid = &status_uuid.u, .access_cb = access_status,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN},
         {.uuid = &signal_uuid.u, .access_cb = access_signal, .val_handle = &signal_handle, .flags = BLE_GATT_CHR_F_NOTIFY},
         {.uuid = &dropbox_config_uuid.u, .access_cb = access_dropbox_config,
          .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN},
         {.uuid = &dropbox_status_uuid.u, .access_cb = access_dropbox_status,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN},
         {0}}},
    {0}};

static void notify(struct ble_npl_event *e)
{
    rn_view_t v = snapshot();
    if (browser != BLE_HS_CONN_HANDLE_NONE && v.paired) {
        uint8_t signal    = (uint8_t)v.state;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&signal, 1);
        if (om) ble_gatts_notify_custom(browser, signal_handle, om);
    }
}

/* ---------------------------------------------------------------------- GAP */
static int setup_gap(struct ble_gap_event *e, void *arg);

static int advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags                    = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128                 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128             = 1;
    fields.uuids128_is_complete     = 1;
    int rc                          = ble_gap_adv_set_fields(&fields);
    if (rc) return rc;
    char name[24];
    snprintf(name, sizeof name, "%s", snapshot().device_name);
    struct ble_hs_adv_fields response = {0};
    response.name                     = (uint8_t *)name;
    response.name_len                 = strlen(name);
    response.name_is_complete         = 1;
    rc                                = ble_gap_adv_rsp_set_fields(&response);
    if (rc) return rc;
    struct ble_gap_adv_params params = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    return ble_gap_adv_start(own_type, NULL, BLE_HS_FOREVER, &params, setup_gap, NULL);
}

static void setup_state(bool advertising, bool paired, bool new_code)
{
    portENTER_CRITICAL(&lock);
    view.advertising = advertising;
    view.paired      = paired;
    if (new_code) {
        view.passkey     = esp_random() % 1000000;
        view.setup_error = 0;
    }
    portEXIT_CRITICAL(&lock);
    publish();
}

static int setup_gap(struct ble_gap_event *e, void *arg)
{
    if (e->type == BLE_GAP_EVENT_CONNECT) {
        ESP_LOGI(TAG, "BLE connect status=%d", e->connect.status);
        if (!e->connect.status) {
            if (!atomic_load(&requested_open) || browser != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(e->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            } else {
                status_length = dropbox_length = 0;
                browser                        = e->connect.conn_handle;
                portENTER_CRITICAL(&lock);
                view.advertising = false;  // connected: no longer advertising
                portEXIT_CRITICAL(&lock);
                ble_gap_security_initiate(browser);
            }
        } else if (atomic_load(&requested_open)) {
            advertise();
        }
    } else if (e->type == BLE_GAP_EVENT_PASSKEY_ACTION && e->passkey.params.action == BLE_SM_IOACT_DISP) {
        struct ble_sm_io key = {.action = BLE_SM_IOACT_DISP, .passkey = snapshot().passkey};
        ble_sm_inject_io(e->passkey.conn_handle, &key);
    } else if (e->type == BLE_GAP_EVENT_ENC_CHANGE) {
        struct ble_gap_conn_desc desc;
        bool paired = atomic_load(&requested_open) && !e->enc_change.status &&
                      !ble_gap_conn_find(e->enc_change.conn_handle, &desc) && desc.sec_state.encrypted &&
                      desc.sec_state.authenticated;
        ESP_LOGI(TAG, "encryption status=%d authenticated=%u", e->enc_change.status, paired);
        setup_state(false, paired, false);
        if (!paired) ble_gap_terminate(e->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else if (e->type == BLE_GAP_EVENT_DISCONNECT) {
        ESP_LOGI(TAG, "BLE disconnected reason=%d", e->disconnect.reason);
        if (e->disconnect.conn.conn_handle != browser) return 0;
        browser       = BLE_HS_CONN_HANDLE_NONE;
        status_length = dropbox_length = 0;
        if (atomic_load(&requested_open)) {  // a new code for every browser session
            setup_state(true, false, true);
            int rc = advertise();
            if (rc) {
                portENTER_CRITICAL(&lock);
                view.setup_error = rc;
                portEXIT_CRITICAL(&lock);
                setup_state(false, false, false);
            }
        }
    }
    return 0;
}

static void control(struct ble_npl_event *e)
{
    if (!atomic_load(&ble_ready) || !atomic_load(&requested_open) || browser != BLE_HS_CONN_HANDLE_NONE) return;
    if (snapshot().advertising) return;
    setup_state(true, false, true);
    int rc = advertise();
    if (rc) {
        portENTER_CRITICAL(&lock);
        view.setup_error = rc;
        portEXIT_CRITICAL(&lock);
        setup_state(false, false, false);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_type);
    uint8_t addr[6] = {0};
    ble_hs_id_copy_addr(own_type, addr, NULL);
    portENTER_CRITICAL(&lock);
    snprintf(view.device_name, sizeof view.device_name, "CardputerRoast-%02X%02X", addr[1], addr[0]);
    portEXIT_CRITICAL(&lock);
    atomic_store(&ble_ready, true);
    control(NULL);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset, reason %d", reason);
    atomic_store(&ble_ready, false);
    browser = BLE_HS_CONN_HANDLE_NONE;
    portENTER_CRITICAL(&lock);
    view.setup_error = reason;
    portEXIT_CRITICAL(&lock);
    setup_state(false, false, false);
}

static void host_task(void *param)
{
    nimble_port_run();  // returns after nimble_port_stop()
    nimble_port_freertos_deinit();
}

static bool ble_start(void)
{
    if (nimble_port_init() != ESP_OK) return false;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // SMP checks key-store capacity even for a session without bonding.
    ble_store_config_init();
    ble_npl_event_init(&control_event, control, NULL);
    ble_npl_event_init(&notify_event, notify, NULL);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) {
        nimble_port_deinit();
        return false;
    }
    ble_svc_gap_device_name_set("Cardputer Roast");
    // LE Secure Connections, MITM protection by the passkey on our screen, no bonds kept.
    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding        = 0;
    ble_hs_cfg.sm_mitm           = 1;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_sc_only        = 1;
    ble_hs_cfg.sm_our_key_dist   = 0;
    ble_hs_cfg.sm_their_key_dist = 0;
    nimble_port_freertos_init(host_task);
    ble_running = true;
    return true;
}

static void ble_stop(void)
{
    if (!ble_running) return;
    atomic_store(&ble_ready, false);
    if (browser != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(browser, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ble_gap_adv_stop();
    nimble_port_stop();  // ends the host task
    nimble_port_deinit();
    ble_gatts_free_svcs();  // the roaster link initializes NimBLE again without our service
    ble_hs_cfg.sm_sc_only = 0;
    ble_hs_cfg.sm_mitm    = 0;
    browser               = BLE_HS_CONN_HANDLE_NONE;
    ble_running           = false;
}

/* ------------------------------------------------------------------ lifecycle */
static bool wifi_start(void)
{
    static bool netif_ready = false;
    if (!netif_ready) {  // process-wide, once
        if (esp_netif_init() != ESP_OK) return false;
        esp_err_t rc = esp_event_loop_create_default();
        if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) return false;
        netif_ready = true;
    }
    station_interface = esp_netif_create_default_wifi_sta();  // destroyed again in wifi_stop()
    if (!station_interface) return false;
    network_queue = xQueueCreate(8, sizeof(network_command_t));
    network_done  = xSemaphoreCreateBinary();
    if (!network_queue || !network_done) return false;
    if (xTaskCreate(network_task, "rn_wifi", 4096, NULL, 4, NULL) != pdPASS) return false;
    network_task_running      = true;
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&config) != ESP_OK) return false;
    wifi_running = true;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  // credentials live in our own NVS namespace
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, &wifi_handler);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL, &ip_handler);
    return esp_wifi_start() == ESP_OK;  // STA_START -> join the saved network once
}

static void wifi_stop(void)
{
    if (wifi_handler) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    if (ip_handler) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);
    wifi_handler = ip_handler = NULL;
    if (wifi_running) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        wifi_running = false;
    }
    if (station_interface) {
        esp_netif_destroy_default_wifi(station_interface);
        station_interface = NULL;
    }
    if (network_task_running) {
        network_command_t quit = {.op = OP_QUIT};
        xQueueSend(network_queue, &quit, portMAX_DELAY);
        xSemaphoreTake(network_done, portMAX_DELAY);
        network_task_running = false;
    }
    if (network_queue) vQueueDelete(network_queue);
    if (network_done) vSemaphoreDelete(network_done);
    network_queue = NULL;
    network_done  = NULL;
}

bool roast_net_open(void)
{
    if (atomic_load(&opened)) return true;
    portENTER_CRITICAL(&lock);
    memset(&view, 0, sizeof view);
    snprintf(view.dropbox, sizeof view.dropbox, "unconfigured");
    portEXIT_CRITICAL(&lock);
    request_id = 0;
    atomic_store(&requested_open, true);
    bool ok = wifi_start();
    if (ok) ok = rn_dropbox_start();
    if (ok) ok = ble_start();
    if (!ok) {
        ESP_LOGE(TAG, "setup could not start");
        atomic_store(&opened, true);  // let close() release what did start
        roast_net_close();
        portENTER_CRITICAL(&lock);
        view.setup_error = -1;
        portEXIT_CRITICAL(&lock);
        return false;
    }
    atomic_store(&opened, true);
    portENTER_CRITICAL(&lock);
    view.open = true;
    portEXIT_CRITICAL(&lock);
    return true;
}

void roast_net_close(void)
{
    if (!atomic_load(&opened)) return;
    atomic_store(&requested_open, false);
    ble_stop();
    rn_dropbox_stop();
    wifi_stop();
    atomic_store(&opened, false);
    portENTER_CRITICAL(&lock);
    view.open = view.advertising = view.paired = false;
    portEXIT_CRITICAL(&lock);
}

bool roast_net_busy(void)
{
    return rn_dropbox_busy();
}

rn_view_t roast_net_view(void)
{
    return snapshot();
}
