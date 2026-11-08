/*
 * BLE central + Modbus RTU master for the E-Roast controller.
 * Timings follow the vendor app: 200 ms between transactions, 300 ms read timeout with two retries,
 * 2 s write-echo timeout with two retries.
 */
#include "roast_link.h"
#include "modbus.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <deque>
#include <map>
#include <cstring>
#include <atomic>

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_sm.h"
#include "store/config/ble_store_config.h"
extern "C" void ble_store_config_init(void);  // not exported by the header in IDF 5.4

namespace roast {

static const char* TAG = "roast-ble";

static uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

class Lock {
public:
    explicit Lock(SemaphoreHandle_t m) : _m(m)
    {
        xSemaphoreTake(_m, portMAX_DELAY);
    }
    ~Lock()
    {
        xSemaphoreGive(_m);
    }

private:
    SemaphoreHandle_t _m;
};

class BleLink : public RoasterLink {
public:
    BleLink();
    ~BleLink() override;

    bool begin() override;
    void end() override;
    LinkState state() override
    {
        return _state.load();
    }
    std::string deviceName() override
    {
        Lock l(_mtx);
        return _devName;
    }
    std::string lastError() override
    {
        Lock l(_mtx);
        return _err;
    }
    std::vector<ScanEntry> scanResults() override
    {
        Lock l(_mtx);
        return _scan;
    }
    bool isSimulated() const override
    {
        return false;
    }
    RegSnapshot snapshot() override
    {
        Lock l(_mtx);
        return _regs;
    }
    uint32_t command(const std::vector<RegWrite>& writes) override;
    TxStatus txStatus(uint32_t id) override;
    void setTarget(uint16_t raw) override
    {
        Lock l(_mtx);
        _targetPending = true;
        _targetRaw     = raw;
    }
    void clearQueue() override
    {
        Lock l(_mtx);
        for (auto& c : _cmds) _txResult[c.id] = TxStatus::Failed;
        _cmds.clear();
        _targetPending = false;
    }

    // NimBLE glue (public for static trampolines)
    void onSync();
    void onReset(int reason);
    int onGap(struct ble_gap_event* ev);
    int onSvc(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_svc* svc);
    int onChr(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_chr* chr);
    int onDsc(uint16_t conn, const struct ble_gatt_error* err, uint16_t chrVal, const struct ble_gatt_dsc* dsc);
    int onCccdWritten(uint16_t conn, const struct ble_gatt_error* err);
    void afterMtu();

private:
    struct Cmd {
        uint32_t id;
        std::vector<RegWrite> writes;
    };

    SemaphoreHandle_t _mtx;
    TaskHandle_t _worker = nullptr;
    std::atomic<bool> _running{false};
    std::atomic<bool> _workerDone{true};
    std::atomic<LinkState> _state{LinkState::Off};
    bool _nimbleUp      = false;
    bool _synced        = false;
    uint8_t _ownAddrType = 0;

    // connection
    uint16_t _conn      = 0xFFFF;
    uint16_t _svcStart  = 0;
    uint16_t _svcEnd    = 0;
    uint16_t _valHandle = 0;   // write characteristic
    uint16_t _notifyHandle = 0;  // notify/indicate characteristic (may differ from the write one)
    bool _notifyIndicate   = false;
    uint16_t _fallbackStart = 0, _fallbackEnd = 0;
    std::vector<uint16_t> _notifyChars, _writeChars, _cccds;
    size_t _cccdNext = 0;
    uint32_t _readyMs = 0, _rxCount = 0, _txLogged = 0;
    bool _switchedWrite = false;
    uint32_t _connectCount = 0;
    uint8_t _savedSecLvl   = 0;  // odd attempts: plain link, even attempts: encrypt first (pairing, no bonding)
    bool _readFff2Done = false;
    uint16_t _cccd      = 0;
    bool _writeNoRsp    = false;
    bool _wantScan      = false;
    std::atomic<bool> _connecting{false};
    uint32_t _scanRestartAt = 0;

    std::string _devName;
    std::string _err;
    std::vector<ScanEntry> _scan;

    // data
    RegSnapshot _regs;
    std::vector<uint8_t> _rx;
    std::deque<Cmd> _cmds;
    std::map<uint32_t, TxStatus> _txResult;
    uint32_t _nextTx    = 1;
    bool _targetPending = false;
    uint16_t _targetRaw = 0;
    uint32_t _cycle     = 0;

    static void workerEntry(void* arg);
    void workerLoop();
    void startScan();
    void setError(const std::string& e);
    void connectionLost(const char* why);

    // Modbus
    bool sendRaw(const uint8_t* data, size_t len);
    bool transact(const uint8_t* req, size_t len, uint8_t expectFn, uint16_t a, uint16_t b, uint32_t timeoutMs,
                  std::vector<uint8_t>* out);
    bool readBatch(uint16_t start, uint16_t qty);
    bool writeReg(uint16_t addr, uint16_t value, int attempts, uint32_t timeoutMs);
    void pumpCommands();
    void waitGap(uint32_t ms);
};

static BleLink* g_link = nullptr;

// C++ cannot take the address of the BLE_UUID16_DECLARE compound literal.
static const ble_uuid16_t kUuidSvc  = BLE_UUID16_INIT(0xFFE0);
static const ble_uuid16_t kUuidChr  = BLE_UUID16_INIT(0xFFE1);
static const ble_uuid16_t kUuidCccd = BLE_UUID16_INIT(0x2902);

/* ------------------------------ trampolines ------------------------------ */
static void host_task(void* param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}
static void on_sync_cb()
{
    if (g_link) g_link->onSync();
}
static void on_reset_cb(int reason)
{
    if (g_link) g_link->onReset(reason);
}
static int gap_cb(struct ble_gap_event* ev, void* arg)
{
    return static_cast<BleLink*>(arg)->onGap(ev);
}
static int svc_cb(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_svc* svc, void* arg)
{
    return static_cast<BleLink*>(arg)->onSvc(conn, err, svc);
}
static int chr_cb(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_chr* chr, void* arg)
{
    return static_cast<BleLink*>(arg)->onChr(conn, err, chr);
}
static int dsc_cb(uint16_t conn, const struct ble_gatt_error* err, uint16_t chrVal, const struct ble_gatt_dsc* dsc,
                  void* arg)
{
    return static_cast<BleLink*>(arg)->onDsc(conn, err, chrVal, dsc);
}
static int cccd_cb(uint16_t conn, const struct ble_gatt_error* err, struct ble_gatt_attr* attr, void* arg)
{
    return static_cast<BleLink*>(arg)->onCccdWritten(conn, err);
}
static int mtu_cb(uint16_t conn, const struct ble_gatt_error* err, uint16_t mtu, void* arg)
{
    ESP_LOGI("roast-ble", "mtu exchange status %d mtu %u", err ? err->status : -1, mtu);
    static_cast<BleLink*>(arg)->afterMtu();
    return 0;
}
static int read_cb(uint16_t conn, const struct ble_gatt_error* err, struct ble_gatt_attr* attr, void* arg)
{
    if (err->status == 0 && attr) {
        uint8_t buf[24];
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(attr->om, 0, len, buf);
        char hx[3 * 24 + 1] = {0};
        for (int i = 0; i < len; i++) snprintf(hx + 3 * i, 4, "%02X ", buf[i]);
        ESP_LOGI(TAG, "read h%u len%u: %s", attr->handle, len, hx);
    } else if (err->status != BLE_HS_EDONE) {
        ESP_LOGI(TAG, "read status %d", err->status);
    }
    return 0;
}
static int write_cb(uint16_t conn, const struct ble_gatt_error* err, struct ble_gatt_attr* attr, void* arg)
{
    return 0;
}

static bool name_matches(const std::string& n)
{
    // Vendor app filter: /^E-Meter.*BLE$/
    return n.size() >= 10 && n.rfind("E-Meter", 0) == 0 && n.compare(n.size() - 3, 3, "BLE") == 0;
}

/* --------------------------------- life ---------------------------------- */
BleLink::BleLink()
{
    _mtx = xSemaphoreCreateMutex();
}

BleLink::~BleLink()
{
    end();
    vSemaphoreDelete(_mtx);
    if (g_link == this) g_link = nullptr;
}

bool BleLink::begin()
{
    if (_running) return true;
    g_link = this;
    if (!_nimbleUp) {
        esp_err_t r = nimble_port_init();
        if (r != ESP_OK) {
            setError("BLE init failed");
            _state = LinkState::Error;
            return false;
        }
        ble_hs_cfg.sync_cb  = on_sync_cb;
        ble_hs_cfg.reset_cb = on_reset_cb;
        // Security needs the store callbacks even without bonding (ble_sm_chk_store_overflow -> ENOTSUP otherwise).
        ble_store_config_init();
        ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
        // Phones (Android 14+, iOS) always run an ATT MTU exchange on connect; some UART modules wait for it.
        ble_att_set_preferred_mtu(247);
        // The demo sdkconfig sets CONFIG_BT_NIMBLE_SM_LVL=2 for the BLE keyboard. With sm_sec_lvl >= 2 NimBLE silently
        // drops every notification on an unencrypted link (ble_att_svr_rx_notify), and the E-Meter621 module cannot
        // pair, so all roaster replies were discarded. Use level 0 while the roast link owns the stack.
        _savedSecLvl         = ble_hs_cfg.sm_sec_lvl;
        ble_hs_cfg.sm_sec_lvl = 0;
        // Just Works pairing without bonding: nothing is stored on either side, so the phone's bond is untouched.
        ble_hs_cfg.sm_io_cap         = BLE_HS_IO_NO_INPUT_OUTPUT;
        ble_hs_cfg.sm_bonding        = 0;
        ble_hs_cfg.sm_mitm           = 0;
        ble_hs_cfg.sm_sc             = 1;
        ble_hs_cfg.sm_our_key_dist   = 0;
        ble_hs_cfg.sm_their_key_dist = 0;
        nimble_port_freertos_init(host_task);
        _nimbleUp = true;
    }
    _running    = true;
    _workerDone = false;
    _state      = LinkState::Scanning;
    {
        Lock l(_mtx);
        _wantScan = true;
    }
    xTaskCreatePinnedToCore(workerEntry, "roast_link", 4096, this, 5, &_worker, 1);
    return true;
}

void BleLink::end()
{
    if (!_nimbleUp && !_running) return;
    _running = false;
    if (_worker) {
        xTaskNotifyGive(_worker);
        for (int i = 0; i < 300 && !_workerDone; i++) vTaskDelay(pdMS_TO_TICKS(10));
        _worker = nullptr;
    }
    if (_nimbleUp) {
        ble_gap_disc_cancel();
        if (_conn != 0xFFFF) {
            ble_gap_terminate(_conn, BLE_ERR_REM_USER_CONN_TERM);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        nimble_port_stop();
        nimble_port_deinit();
        ble_hs_cfg.sm_sec_lvl = _savedSecLvl;
        _nimbleUp = false;
        _synced   = false;
    }
    _conn  = 0xFFFF;
    _state = LinkState::Off;
}

void BleLink::setError(const std::string& e)
{
    Lock l(_mtx);
    _err = e;
    ESP_LOGW(TAG, "%s", e.c_str());
}

/* ---------------------------------- GAP ---------------------------------- */
void BleLink::onSync()
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &_ownAddrType);
    _synced = true;
    startScan();
}

void BleLink::onReset(int reason)
{
    ESP_LOGW(TAG, "host reset %d", reason);
    _synced = false;
}

void BleLink::startScan()
{
    if (!_synced || !_running) return;
    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof(p));
    p.passive           = 0;  // active: names may only be in the scan response
    p.filter_duplicates = 0;
    p.itvl              = 0x50;
    p.window            = 0x30;
    {
        Lock l(_mtx);
        _scan.clear();
        _wantScan = false;
    }
    int rc = ble_gap_disc(_ownAddrType, 5000, &p, gap_cb, this);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        setError("scan failed rc=" + std::to_string(rc));
        Lock l(_mtx);
        _wantScan      = true;
        _scanRestartAt = now_ms() + 1000;
        return;
    }
    if (_state != LinkState::Lost) _state = LinkState::Scanning;
}

int BleLink::onGap(struct ble_gap_event* ev)
{
    switch (ev->type) {
        case BLE_GAP_EVENT_DISC: {
            struct ble_hs_adv_fields f;
            if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) != 0) return 0;
            if (f.name == nullptr || f.name_len == 0) return 0;
            std::string name((const char*)f.name, f.name_len);
            if (!name_matches(name)) return 0;
            if (_connecting.exchange(true)) return 0;  // several adverts can arrive before the cancel lands
            {
                Lock l(_mtx);
                bool found = false;
                for (auto& s : _scan) {
                    if (s.name == name) {
                        s.rssi = ev->disc.rssi;
                        found  = true;
                    }
                }
                if (!found) _scan.push_back({name, ev->disc.rssi});
                _devName = name;
            }
            ESP_LOGI(TAG, "found %s rssi %d, connecting", name.c_str(), ev->disc.rssi);
            ble_gap_disc_cancel();
            ble_addr_t addr = ev->disc.addr;
            if (_state != LinkState::Lost) _state = LinkState::Connecting;
            int rc = ble_gap_connect(_ownAddrType, &addr, 8000, nullptr, gap_cb, this);
            if (rc != 0) {
                _connecting = false;
                setError("connect failed rc=" + std::to_string(rc));
                Lock l(_mtx);
                _wantScan      = true;
                _scanRestartAt = now_ms() + 1000;
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE: {
            if (_conn == 0xFFFF && _running) {
                Lock l(_mtx);
                _wantScan      = true;
                _scanRestartAt = now_ms() + 300;
            }
            return 0;
        }
        case BLE_GAP_EVENT_CONNECT: {
            _connecting = false;
            if (ev->connect.status != 0) {
                setError("connect status " + std::to_string(ev->connect.status));
                Lock l(_mtx);
                _wantScan      = true;
                _scanRestartAt = now_ms() + 500;
                return 0;
            }
            _conn      = ev->connect.conn_handle;
            _valHandle    = 0;
            _notifyHandle = 0;
            _cccd         = 0;
            _svcStart = _svcEnd = _fallbackStart = _fallbackEnd = 0;
            _notifyChars.clear();
            _writeChars.clear();
            _cccds.clear();
            _cccdNext = 0;
            _rxCount = _txLogged = 0;
            _switchedWrite = false;
            _readFff2Done = false;
            _connectCount++;
            ESP_LOGI(TAG, "connected (attempt %lu), exchanging MTU", (unsigned long)_connectCount);
            if (ble_gattc_exchange_mtu(_conn, mtu_cb, this) == 0) return 0;  // continues in afterMtu()
            afterMtu();
            return 0;
        }
        case BLE_GAP_EVENT_DISCONNECT: {
            ESP_LOGW(TAG, "disconnected reason %d", ev->disconnect.reason);
            _conn       = 0xFFFF;
            _connecting = false;
            connectionLost("disconnected");
            return 0;
        }
        case BLE_GAP_EVENT_NOTIFY_RX: {
            if (ev->notify_rx.attr_handle < _svcStart || ev->notify_rx.attr_handle > _svcEnd) return 0;
            uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
            uint8_t buf[64];
            if (len > sizeof(buf)) len = sizeof(buf);
            os_mbuf_copydata(ev->notify_rx.om, 0, len, buf);
            if (_rxCount++ < 1) {
                char hx[3 * 24 + 1] = {0};
                for (int i = 0; i < len && i < 24; i++) snprintf(hx + 3 * i, 4, "%02X ", buf[i]);
                ESP_LOGI(TAG, "rx h%u len%u: %s", ev->notify_rx.attr_handle, len, hx);
            }
            {
                Lock l(_mtx);
                _rx.insert(_rx.end(), buf, buf + len);
                if (_rx.size() > 256) _rx.erase(_rx.begin(), _rx.begin() + (_rx.size() - 256));
            }
            if (_worker) xTaskNotifyGive(_worker);
            return 0;
        }
        case BLE_GAP_EVENT_ENC_CHANGE: {
            ESP_LOGI(TAG, "enc change status %d", ev->enc_change.status);
            if (ev->enc_change.conn_handle == _conn && _valHandle == 0) {
                int rc = ble_gattc_disc_all_svcs(_conn, svc_cb, this);
                if (rc != 0) ble_gap_terminate(_conn, BLE_ERR_REM_USER_CONN_TERM);
            }
            return 0;
        }
        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            ESP_LOGW(TAG, "passkey action %d", ev->passkey.params.action);
            if (ev->passkey.params.action == BLE_SM_IOACT_INPUT || ev->passkey.params.action == BLE_SM_IOACT_DISP) {
                struct ble_sm_io io = {};
                io.action  = ev->passkey.params.action;
                io.passkey = 0;
                ble_sm_inject_io(ev->passkey.conn_handle, &io);
            } else if (ev->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
                struct ble_sm_io io = {};
                io.action    = BLE_SM_IOACT_NUMCMP;
                io.numcmp_accept = 1;
                ble_sm_inject_io(ev->passkey.conn_handle, &io);
            }
            return 0;
        }
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        default:
            return 0;
    }
}

void BleLink::connectionLost(const char* why)
{
    LinkState s = _state.load();
    if (s == LinkState::Ready || s == LinkState::Lost) {
        _state = LinkState::Lost;
    } else if (_running) {
        _state = LinkState::Scanning;
    }
    {
        Lock l(_mtx);
        _err           = why;
        _wantScan      = _running;
        _scanRestartAt = now_ms() + 500;
        _rx.clear();
    }
}

// 16-bit value of a UUID that is either 16-bit or 128-bit on the Bluetooth base; 0 otherwise.
static uint16_t uuid16_of(const ble_uuid_any_t& u)
{
    if (u.u.type == BLE_UUID_TYPE_16) return u.u16.value;
    if (u.u.type == BLE_UUID_TYPE_128) {
        static const uint8_t base[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                                         0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        if (memcmp(u.u128.value, base, 12) == 0 && u.u128.value[14] == 0 && u.u128.value[15] == 0)
            return u.u128.value[12] | (u.u128.value[13] << 8);
    }
    return 0;
}

// Same choice as the vendor app: the first service whose UUID contains "FFE", else the first one;
// inside it the notify/indicate characteristic and the write characteristic (write preferred over no-rsp).
void BleLink::afterMtu()
{
    if (_conn == 0xFFFF) return;
    // The module does not answer SMP pairing (30 s timeout), so stay on a plain link.
    int rc = ble_gattc_disc_all_svcs(_conn, svc_cb, this);
    if (rc != 0) {
        setError("discover rc=" + std::to_string(rc));
        ble_gap_terminate(_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

int BleLink::onSvc(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_svc* svc)
{
    if (err->status == 0 && svc) {
        char buf[BLE_UUID_STR_LEN];
        ESP_LOGI(TAG, "svc %s handles %u-%u", ble_uuid_to_str(&svc->uuid.u, buf), svc->start_handle, svc->end_handle);
        uint16_t v = uuid16_of(svc->uuid);
        if (!_svcStart && (v & 0xFFF0) == 0xFFE0) {
            _svcStart = svc->start_handle;
            _svcEnd   = svc->end_handle;
        }
        if (!_fallbackStart && v != 0x1800 && v != 0x1801) {
            _fallbackStart = svc->start_handle;
            _fallbackEnd   = svc->end_handle;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (!_svcStart) {
            _svcStart = _fallbackStart;
            _svcEnd   = _fallbackEnd;
        }
        if (_svcStart) {
            ble_gattc_disc_all_chrs(conn, _svcStart, _svcEnd, chr_cb, this);
            return 0;
        }
    }
    setError("no usable BLE service (" + std::to_string(err->status) + ")");
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

int BleLink::onChr(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_chr* chr)
{
    if (err->status == 0 && chr) {
        char buf[BLE_UUID_STR_LEN];
        ESP_LOGI(TAG, "chr %s val %u props 0x%02x", ble_uuid_to_str(&chr->uuid.u, buf), chr->val_handle, chr->properties);
        // vendor app keeps the LAST notify-capable characteristic (E-Meter621: write FFF1, notify FFF2)
        if (chr->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) _notifyChars.push_back(chr->val_handle);
        if (chr->properties & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP)) _writeChars.push_back(chr->val_handle);
        if (chr->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) {
            _notifyHandle   = chr->val_handle;
            _notifyIndicate = !(chr->properties & BLE_GATT_CHR_PROP_NOTIFY);
        }
        if (chr->properties & BLE_GATT_CHR_PROP_WRITE) {
            if (!_valHandle || _writeNoRsp) {
                _valHandle  = chr->val_handle;
                _writeNoRsp = false;
            }
        } else if ((chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) && !_valHandle) {
            _valHandle  = chr->val_handle;
            _writeNoRsp = true;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE && _valHandle && _notifyHandle) {
        ESP_LOGI(TAG, "using write %u (%s), notify %u", _valHandle, _writeNoRsp ? "no-rsp" : "rsp", _notifyHandle);
        ble_gattc_disc_all_dscs(conn, _svcStart, _svcEnd, dsc_cb, this);
        return 0;
    }
    setError(_valHandle ? "no notify characteristic" : "no writable characteristic");
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

int BleLink::onDsc(uint16_t conn, const struct ble_gatt_error* err, uint16_t chrVal, const struct ble_gatt_dsc* dsc)
{
    if (err->status == 0 && dsc) {
        if (ble_uuid_cmp(&dsc->uuid.u, &kUuidCccd.u) == 0) {
            ESP_LOGI(TAG, "cccd %u (char %u)", dsc->handle, chrVal);
            _cccds.push_back(dsc->handle);
        }
        return 0;
    }
    if (_cccds.empty()) _cccds.push_back(_notifyHandle + 1);  // common layout for UART bridge modules
    _cccdNext = 0;
    static const uint8_t on[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn, _cccds[0], on, 2, cccd_cb, this);
    if (rc != 0) {
        setError("enable notify rc=" + std::to_string(rc));
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

int BleLink::onCccdWritten(uint16_t conn, const struct ble_gatt_error* err)
{
    if (err->status != 0) ESP_LOGW(TAG, "cccd %u write status %d", _cccds[_cccdNext], err->status);
    if (++_cccdNext < _cccds.size()) {  // subscribe every notify characteristic, the vendor app's pick may not be the data one
        static const uint8_t on[2] = {0x01, 0x00};
        ble_gattc_write_flat(conn, _cccds[_cccdNext], on, 2, cccd_cb, this);
        return 0;
    }
    {
        Lock l(_mtx);
        _rx.clear();
        _regs.lastRxMs = now_ms();
        _err.clear();
    }
    _readyMs = now_ms();
    _state   = LinkState::Ready;
    ESP_LOGI(TAG, "ready (write%s)", _writeNoRsp ? " no-rsp" : " with rsp");
    if (_worker) xTaskNotifyGive(_worker);
    return 0;
}

/* -------------------------------- commands ------------------------------- */
uint32_t BleLink::command(const std::vector<RegWrite>& writes)
{
    Lock l(_mtx);
    uint32_t id    = _nextTx++;
    _txResult[id]  = TxStatus::Pending;
    _cmds.push_back({id, writes});
    while (_txResult.size() > 64) _txResult.erase(_txResult.begin());
    if (_worker) xTaskNotifyGive(_worker);
    return id;
}

TxStatus BleLink::txStatus(uint32_t id)
{
    Lock l(_mtx);
    auto it = _txResult.find(id);
    return it == _txResult.end() ? TxStatus::Failed : it->second;
}

/* --------------------------------- Modbus -------------------------------- */
bool BleLink::sendRaw(const uint8_t* data, size_t len)
{
    if (_conn == 0xFFFF || _valHandle == 0) return false;
    int rc;
    if (_writeNoRsp) {
        rc = ble_gattc_write_no_rsp_flat(_conn, _valHandle, data, len);
    } else {
        rc = ble_gattc_write_flat(_conn, _valHandle, data, len, write_cb, this);
    }
    if (_txLogged++ < 1) ESP_LOGI(TAG, "tx h%u len%u rc=%d", _valHandle, (unsigned)len, rc);

    vTaskDelay(pdMS_TO_TICKS(20));
    return rc == 0;
}

// Wait for a CRC-valid frame with function code `expectFn`.
// For FC06 the echo must match (a = address, b = value). For FC03 `b` is the expected register count.
bool BleLink::transact(const uint8_t* req, size_t len, uint8_t expectFn, uint16_t a, uint16_t b, uint32_t timeoutMs,
                       std::vector<uint8_t>* out)
{
    {
        Lock l(_mtx);
        _rx.clear();
    }
    ulTaskNotifyTake(pdTRUE, 0);
    if (!sendRaw(req, len)) return false;
    uint32_t deadline = now_ms() + timeoutMs;
    while (_running && _state == LinkState::Ready) {
        std::vector<uint8_t> frame;
        {
            Lock l(_mtx);
            while (modbus::extract_frame(_rx, frame)) {
                if (frame[1] == expectFn) break;
                frame.clear();
            }
        }
        if (!frame.empty()) {
            if (expectFn == 0x06) {
                uint16_t ra = (frame[2] << 8) | frame[3];
                uint16_t rv = (frame[4] << 8) | frame[5];
                if (ra == a && rv == b) return true;
            } else if (expectFn == 0x03) {
                if (frame[2] == b * 2) {
                    if (out) *out = frame;
                    return true;
                }
            }
        }
        int32_t left = (int32_t)(deadline - now_ms());
        if (left <= 0) return false;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(left > 50 ? 50 : left));
    }
    return false;
}

bool BleLink::readBatch(uint16_t start, uint16_t qty)
{
    uint8_t req[8];
    modbus::build_read(1, start, qty, req);
    std::vector<uint8_t> frame;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (transact(req, sizeof(req), 0x03, start, qty, 300, &frame)) {
            auto val = [&](int reg) { return (int16_t)((frame[3 + 2 * (reg - start)] << 8) | frame[4 + 2 * (reg - start)]); };
            // A late answer to an earlier batch has the same shape; reject values that cannot belong here.
            bool plausible = true;
            if (start == 2) plausible = val(4) > -500 && val(4) < 4000;
            if (start == 9) plausible = val(11) >= 0 && val(11) <= 1000 && val(12) >= 0 && val(12) <= 1000 &&
                                        (val(15) == 0 || val(15) == 1);
            if (start == 16) plausible = val(16) >= 0 && val(16) <= 1000;
            if (start == 23) plausible = val(29) >= 0 && val(29) <= 1000;
            if (!plausible) {
                ESP_LOGW(TAG, "discard implausible batch @%d", start);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            Lock l(_mtx);
            for (int i = 0; i < qty && start + i < kRegCount; i++) {
                _regs.reg[start + i] = (int16_t)((frame[3 + 2 * i] << 8) | frame[4 + 2 * i]);
                _regs.valid |= 1u << (start + i);
            }
            _regs.lastRxMs = now_ms();
            _regs.frames++;
            return true;
        }
        if (_state != LinkState::Ready) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

bool BleLink::writeReg(uint16_t addr, uint16_t value, int attempts, uint32_t timeoutMs)
{
    uint8_t req[8];
    modbus::build_write_single(1, addr, value, req);
    for (int attempt = 0; attempt < attempts; attempt++) {
        if (transact(req, sizeof(req), 0x06, addr, value, timeoutMs, nullptr)) {
            Lock l(_mtx);
            if (addr < kRegCount) {
                _regs.reg[addr] = (int16_t)value;
                _regs.valid |= 1u << addr;
            }
            _regs.lastRxMs = now_ms();
            return true;
        }
        if (_state != LinkState::Ready) return false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

void BleLink::pumpCommands()
{
    while (_running && _state == LinkState::Ready) {
        Cmd cmd;
        {
            Lock l(_mtx);
            if (_cmds.empty()) break;
            cmd = _cmds.front();
            _cmds.pop_front();
        }
        bool ok = true;
        for (size_t i = 0; i < cmd.writes.size(); i++) {
            if (!writeReg(cmd.writes[i].first, cmd.writes[i].second, 3, 2000)) {
                ok = false;
                break;
            }
            if (i + 1 < cmd.writes.size()) vTaskDelay(pdMS_TO_TICKS(200));
        }
        {
            Lock l(_mtx);
            _txResult[cmd.id] = ok ? TxStatus::Ok : TxStatus::Failed;
        }
        waitGap(200);
    }
}

void BleLink::waitGap(uint32_t ms)
{
    // Inter-transaction gap; a queued command shortens the wait.
    uint32_t until = now_ms() + ms;
    while (_running) {
        int32_t left = (int32_t)(until - now_ms());
        if (left <= 0) return;
        vTaskDelay(pdMS_TO_TICKS(left > 20 ? 20 : left));
    }
}

void BleLink::workerEntry(void* arg)
{
    static_cast<BleLink*>(arg)->workerLoop();
    static_cast<BleLink*>(arg)->_workerDone = true;
    vTaskDelete(nullptr);
}

void BleLink::workerLoop()
{
    static const uint16_t kBatches[4] = {2, 9, 16, 23};
    while (_running) {
        LinkState s = _state.load();
        if (s != LinkState::Ready) {
            bool scan = false;
            {
                Lock l(_mtx);
                scan = _wantScan && (int32_t)(now_ms() - _scanRestartAt) >= 0 && _conn == 0xFFFF;
            }
            if (scan) startScan();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }

        // Liveness: the vendor app treats 20 s of silence as a dead link.
        uint32_t last;
        {
            Lock l(_mtx);
            last = _regs.lastRxMs;
        }
        if (now_ms() - last > 15000) {
            setError("no data for 15 s");
            if (_conn != 0xFFFF) ble_gap_terminate(_conn, BLE_ERR_REM_USER_CONN_TERM);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        pumpCommands();

        // Curve target (latest value wins)
        bool haveTarget = false;
        uint16_t target = 0;
        {
            Lock l(_mtx);
            if (_targetPending) {
                haveTarget     = true;
                target         = _targetRaw;
                _targetPending = false;
            }
        }
        if (haveTarget) {
            writeReg(9, target, 2, 1500);
            waitGap(200);
            pumpCommands();
        }

        // Reads: batches 2..8 and 9..15 every cycle, 16..22 / 23..29 every 5th cycle.
        bool full = (_cycle % 5) == 0;
        for (int b = 0; b < 4 && _running && _state == LinkState::Ready; b++) {
            if (b >= 2 && !full) break;
            readBatch(kBatches[b], 7);
            waitGap(200);
            pumpCommands();
        }
        _cycle++;
    }
}

RoasterLink* CreateBleLink()
{
    return new BleLink();
}

}  // namespace roast
