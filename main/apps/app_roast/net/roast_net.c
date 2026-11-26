// SPDX-License-Identifier: MIT
/* Dropbox-only Bluetooth setup. Networking is owned by ESP Hub. */
#include "roast_net.h"
#include "roast_dropbox.h"
#include "eh_agent.h"
#include "eh_agent_ext.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
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


static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static rn_view_t view;
static atomic_bool ble_ready, requested_open, opened;
static bool ble_running;
static uint8_t own_type;
static uint16_t browser = BLE_HS_CONN_HANDLE_NONE, signal_handle;
static struct ble_npl_event control_event, notify_event;
static char status_json[384];
static int status_length;
static char dropbox_json[192];
static int dropbox_length;

// c9a81001-9a9d-4f55-a621-5b4e7699b647 and siblings (byte order reversed for NimBLE)
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(0x47, 0xb6, 0x99, 0x76, 0x4e, 0x5b, 0x21, 0xa6, 0x55, 0x4f, 0x9d, 0x9a, 0x01, 0x10, 0xa8, 0xc9);
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

/* Read-only network status from ESP Hub; no Wi-Fi credentials or driver lifecycle here. */
bool roast_net_esphub_network(char ssid[33])
{
    ssid[0] = 0;
    if (eh_wifi_current(ssid, 33)) return true;
    cJSON *networks = cJSON_CreateArray();
    if (!networks) return false;
    eh_wifi_saved_json(networks);
    cJSON *first = cJSON_GetArrayItem(networks, 0);
    const cJSON *name = first ? cJSON_GetObjectItem(first, "ssid") : NULL;
    if (cJSON_IsString(name)) snprintf(ssid, 33, "%s", name->valuestring);
    bool saved = ssid[0] != 0;
    cJSON_Delete(networks);
    return saved;
}

bool roast_net_saved_dropbox(void) { return rn_dropbox_saved(); }

static void refresh_network_view(void)
{
    char ssid[33], ip[16] = "";
    bool saved = roast_net_esphub_network(ssid);
    bool up = eh_net_has_ip();
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (up && sta && esp_netif_get_ip_info(sta, &info) == ESP_OK)
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&info.ip));
    portENTER_CRITICAL(&lock);
    view.saved = saved;
    view.state = up ? RN_NET_CONNECTED : (saved ? RN_NET_CONNECTING : RN_NET_UNCONFIGURED);
    snprintf(view.ssid, sizeof view.ssid, "%s", ssid);
    snprintf(view.ip, sizeof view.ip, "%s", ip);
    portEXIT_CRITICAL(&lock);
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

static int access_status(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    if (!atomic_load(&requested_open) || conn != browser) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    static const char *states[] = {"unconfigured", "connecting", "connected", "failed", "disconnected"};
    if (ctx->offset && !status_length) return BLE_ATT_ERR_INVALID_OFFSET;
    // One coherent JSON snapshot across all Read Blob fragments.
    if (ctx->offset == 0) {
        refresh_network_view();
        rn_view_t v = snapshot();
        char ssid[193];
        json_escape(ssid, v.ssid);
        status_length = snprintf(status_json, sizeof status_json,
                                 "{\"v\":1,\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"reason\":%d,\"request\":%lu,\"saved\":%s}",
                                 states[v.state], ssid, v.ip, v.reason, 0UL, v.saved ? "true" : "false");
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
    view.connecting  = false;
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
                // Connected: no longer advertising, and the passkey stays on screen while the OS dialog asks for it.
                portENTER_CRITICAL(&lock);
                view.advertising = false;
                view.connecting  = true;
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

// The BLE controller asserts (and the chip resets) when its allocations fail, so it starts first, and only with room
// to spare; Wi-Fi after it fails cleanly instead. Measured need is unknown yet: these floors are conservative.
#define BLE_MIN_FREE (40 * 1024)

static bool ble_start(void)
{
    size_t free_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "heap before BLE: %u free, %u largest", (unsigned)free_now,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (free_now < BLE_MIN_FREE) return false;
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
bool roast_net_open(void)
{
    if (atomic_load(&opened)) return true;
    portENTER_CRITICAL(&lock);
    memset(&view, 0, sizeof view);
    snprintf(view.dropbox, sizeof view.dropbox, "unconfigured");
    portEXIT_CRITICAL(&lock);
    atomic_store(&requested_open, true);
    bool ok = ble_start();  // Only Dropbox BLE; ESP Hub networking stays up.
    if (ok) ok = rn_dropbox_start();
    ESP_LOGI(TAG, "setup %s; heap %u free, %u largest", ok ? "running" : "FAILED",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
    refresh_network_view();
    return snapshot();
}

/* Wait for the SDK-managed connection. Never stop it after Dropbox sync. */
bool roast_net_wait_for_network(int timeout_ms, char *err, size_t errlen)
{
    for (int waited = 0; waited <= timeout_ms; waited += 100) {
        if (eh_net_has_ip()) return true;
        if (waited < timeout_ms) vTaskDelay(pdMS_TO_TICKS(100));
    }
    snprintf(err, errlen, "ESP Hub Wi-Fi offline; configure it through ESP Hub");
    return false;
}
