/*
 * Roast app logic: connection, roast state machine, keys, alerts.
 * Rendering lives in app_roast_ui.cpp.
 */
#include "app_roast.h"
#include "roast_debug.h"
#include "roast_audio.h"
#include "net/roast_net.h"
#include <esp_system.h>
#if __has_include(<esp_attr.h>)
#include <esp_attr.h>
#endif
#ifndef RTC_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#endif
#include <apps/utils/audio/audio.h>
#include <hal/utils/settings/settings.h>
#include <mooncake_log.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <cmath>
#include <climits>
#include <cstring>
#include <cctype>
#include <algorithm>

using namespace mooncake;
using namespace roast;
using namespace roast::ui;

static const char* TAG = "Roast";

/* --------------------------------- rules --------------------------------- */
void RoastRules::load()
{
    Settings s("roast", false);
    devMin    = s.GetInt("devMin", devMin);
    devMax    = s.GetInt("devMax", devMax);
    dtMin     = s.GetInt("dtMin10", (int)lroundf(dtMin * 10)) / 10.0f;
    dtMax     = s.GetInt("dtMax10", (int)lroundf(dtMax * 10)) / 10.0f;
    maxDropBT = s.GetInt("maxDrop10", (int)lroundf(maxDropBT * 10)) / 10.0f;
    safetyBT  = s.GetInt("safety10", (int)lroundf(safetyBT * 10)) / 10.0f;
    afterEnd  = s.GetInt("afterEnd", afterEnd);
    rorLow    = s.GetInt("rorLow10", (int)lroundf(rorLow * 10)) / 10.0f;
    coldBT    = s.GetInt("coldBT10", (int)lroundf(coldBT * 10)) / 10.0f;
    startFan  = s.GetInt("fan10", (int)lroundf(startFan * 10)) / 10.0f;
    greenG    = s.GetInt("green10", (int)lroundf(greenG * 10)) / 10.0f;
    heatDrop  = s.GetInt("heatDrop10", (int)lroundf(heatDrop * 10)) / 10.0f;
    curveOffset = s.GetInt("curveOff10", (int)lroundf(curveOffset * 10)) / 10.0f;
    micGain     = std::max(0, std::min(rec::kMaxGainDb, (int)s.GetInt("micGain", micGain)));
    svLookahead = std::max(0, std::min(20, (int)s.GetInt("svLook", svLookahead)));
    fcFloor     = s.GetInt("fcFloor", fcFloor ? 1 : 0) != 0;
    floorDelta  = std::max(0, std::min(50, (int)s.GetInt("floorD10", (int)lroundf(floorDelta * 10)))) / 10.0f;
    floorCap    = std::max(10, std::min(80, (int)s.GetInt("floorCap10", (int)lroundf(floorCap * 10)))) / 10.0f;
}

void RoastRules::save() const
{
    Settings s("roast", true);
    s.SetInt("devMin", devMin);
    s.SetInt("devMax", devMax);
    s.SetInt("dtMin10", (int)lroundf(dtMin * 10));
    s.SetInt("dtMax10", (int)lroundf(dtMax * 10));
    s.SetInt("maxDrop10", (int)lroundf(maxDropBT * 10));
    s.SetInt("safety10", (int)lroundf(safetyBT * 10));
    s.SetInt("afterEnd", afterEnd);
    s.SetInt("rorLow10", (int)lroundf(rorLow * 10));
    s.SetInt("coldBT10", (int)lroundf(coldBT * 10));
    s.SetInt("fan10", (int)lroundf(startFan * 10));
    s.SetInt("green10", (int)lroundf(greenG * 10));
    s.SetInt("heatDrop10", (int)lroundf(heatDrop * 10));
    s.SetInt("curveOff10", (int)lroundf(curveOffset * 10));
    s.SetInt("micGain", micGain);
    s.SetInt("svLook", svLookahead);
    s.SetInt("fcFloor", fcFloor ? 1 : 0);
    s.SetInt("floorD10", (int)lroundf(floorDelta * 10));
    s.SetInt("floorCap10", (int)lroundf(floorCap * 10));
}

/* ---------------------------------- app ---------------------------------- */
AppRoast::AppRoast()
{
    setAppInfo().name = "Roast";
    for (auto& m : _markMs) m = -1;
}

AppRoast::~AppRoast() {}

void AppRoast::onOpen()
{
    mclog::tagInfo(TAG, "on open");
    GetHAL().canvas.setBaseColor(ui::BG);
    GetHAL().canvas.setTextScroll(false);

    _rules.load();
    _sdOk = GetHAL().sdCardMount();
    reloadCurves();
    {
        Settings s("roast", false);
        std::string last = s.GetString("curve", "");
        int idx          = 0;
        for (size_t i = 0; i < _curves.size(); i++)
            if (_curves[i].name == last) idx = (int)i;
        selectCurve(_curves.empty() ? -1 : idx);
        _bean = s.GetString("bean", "");
    }
    _phase    = Phase::Idle;
    _screen   = Screen::Connect;
    _view     = 0;
    _manual   = false;
    _keySlot  = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if (e.isModifier) return;
        if (e.state) {
            if (e.keyCode != KEY_COMMA && e.keyCode != KEY_LEFT && e.keyCode != KEY_SLASH && e.keyCode != KEY_RIGHT)
                _heatHeld = 0;  // any other key ends a heater repeat
            _keys.push_back(e);
        } else if (e.keyCode == KEY_DOT || e.keyCode == KEY_DOWN) {
            _downHeld  = false;
            _barrierAt = -1;
        } else if (e.keyCode == KEY_COMMA || e.keyCode == KEY_LEFT || e.keyCode == KEY_SLASH || e.keyCode == KEY_RIGHT) {
            _heatHeld = 0;
        }
    });
    _blockedByKeyboardApp = GetHAL().bleKeyboardInited();
    // Wi-Fi and Dropbox are required. Boot syncs the curves from Dropbox (must succeed) and restarts once with a
    // clean heap; that second boot (flagged in RTC memory) skips the sync and connects to the roaster.
    char ssid[33];
    // Read the boot marker once and clear it, whichever way this boot goes (a stale "skip sync" must not survive
    // into the boot after setup).
    bool bypass = consumeBypass();
    bool skip   = consumeSkipSync();
    if (bypass) {  // test only: USB console `boot roast`
        if (!_blockedByKeyboardApp) startLink(false);
        toast("Test boot: no Dropbox sync", WARN, 4000);
    } else if (!roast_net_saved_wifi(ssid) || !roast_net_saved_dropbox()) {
        _netRequired = true;
        openNetwork();
    } else if (skip) {
        if (!_blockedByKeyboardApp) startLink(false);
    } else {
        startJob(sync::Job::Curves, "");
    }
    _dirty = true;
}

void AppRoast::onClose()
{
    mclog::tagInfo(TAG, "on close");
    if (_keySlot >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_keySlot);
    _keySlot = -1;
    stopRecording();
    if (_netOpen) roast_net_close();
    _netOpen = false;
    if (_session.active()) {
        std::string err;
        _session.finalize(&err);
        _session.end();
    }
    if (_link) {
        _link->clearQueue();
        _link->end();
        _link.reset();
    }
    g_activeLink = nullptr;
    GetHAL().setRoastBleActive(false);
    _keys.clear();
    // Back to the community Launcher. Its bootloader starts the Launcher only after a power-on or a deep-sleep
    // wake-up (a plain esp_restart() boots this app again), so take a 0.2 s deep-sleep nap.
    GetHAL().display.fillScreen(0);
    GetHAL().delay(100);
    esp_sleep_enable_timer_wakeup(200 * 1000);
    esp_deep_sleep_start();
}

void AppRoast::startLink(bool sim)
{
    if (_link) {
        g_activeLink = nullptr;
        _link->end();
        _link.reset();
    }
    GetHAL().setRoastBleActive(!sim);
    _link.reset(sim ? CreateSimLink() : CreateBleLink());
    g_activeLink = _link.get();
    if (!_link->begin()) toast("BLE init failed", ui::DROP);
    _linkState = _link->state();
}

void AppRoast::onRunning()
{
    pollLink();
    handleKeys();
    pollFanBarrier();
    // ←/→ held in manual mode: repeat the heater step every 150 ms after 450 ms. A hold repeats at most 8 times,
    // so a lost key-release event can never run the heater up on its own.
    if (_heatHeld && _manual && _phase == Phase::Roasting) {
        uint32_t now = GetHAL().millis();
        if (now - _heatHeldMs > 450 && now - _heatRepMs > 150) {
            adjustHeat(_heatHeld * (_heatHeldShift ? 1.0f : 5.0f));
            _heatRepMs = now;
            _dirty     = true;
            if (++_heatReps >= 8) _heatHeld = 0;
        }
    } else {
        _heatHeld = 0;
    }
    pumpBeep();
    pollRecording();
    pollSync();
    pollSave();
    if (_phase == Phase::Roasting || _phase == Phase::Cooling) tickRoast();

    // Leaving the app is only allowed when the machine is not heating.
    if (GetHAL().homeButton.wasClicked()) {
        if ((_screen == Screen::Sync && !sync::Get().finished) || saveBusy()) {
            // leaving now would cut an upload in half (or skip the roast's save altogether)
            toast(_fanRunning ? "Stop the fan (X X): the roast is saved to Dropbox" : "Saving to Dropbox: wait", WARN);
        } else if (_phase == Phase::Roasting) {
            toast("Roasting: drop before exit", ui::DROP);
        } else {
            if (!quietRoast()) audio::play_random_tone();
            close();
            return;
        }
    }

    uint32_t now = GetHAL().millis();
    if (_batMs == 0 || now - _batMs > 5000) {
        int b   = M5.Power.getBatteryLevel();
        _batPct = (b >= 0 && b <= 100) ? b : -1;
        _batMs  = now;
    }
    if (_dirty || now - _lastRender > 250) {
        render();
        _lastRender = now;
        _dirty      = false;
    }
}

/* --------------------------------- link ---------------------------------- */
void AppRoast::pollLink()
{
    if (!_link) return;
    _linkState = _link->state();
    _snap      = _link->snapshot();
    if (_linkState != _prevLinkState) {
        if (_linkState == LinkState::Ready) {
            if (_screen == Screen::Connect) _screen = Screen::Prep;
            if (_prevLinkState == LinkState::Lost) toast("Reconnected", ui::GO);
            beep(Tone::Good);
        } else if (_linkState == LinkState::Lost) {
            toast("BLE lost, reconnecting", ui::DROP);
        }
        _prevLinkState = _linkState;
        _dirty         = true;
    }
}

int AppRoast::roastT() const
{
    if (_chargeMs == 0) return 0;
    return (int)((GetHAL().millis() - _chargeMs) / 1000);
}

float AppRoast::curveTargetAt(int t) const
{
    return _hasCurve ? _curve.at(t) : NAN;
}

int AppRoast::curveT() const
{
    // The phone app indexes the curve with the machine timer (reg 2) on every poll; do the same while it is sane.
    int t = roastT();
    if (_phase == Phase::Roasting && _snap.has(2)) {
        int m = _snap.reg[2];
        if (m >= 0 && std::abs(m - t) <= 5) return m;
    }
    return t;
}

/* -------------------------------- curves --------------------------------- */
void AppRoast::reloadCurves()
{
    _curves = ListCurves(_sdOk);
}

void AppRoast::selectCurve(int idx)
{
    if (idx < 0 || idx >= (int)_curves.size()) {
        _hasCurve = false;
        _curve    = Curve();
        return;
    }
    _curve        = _curves[idx];
    _curve.offset = _rules.curveOffset;
    _hasCurve     = _curve.valid();
    if (_curve.batchG > 0) _rules.greenG = (float)_curve.batchG;
    Settings s("roast", true);
    s.SetString("curve", _curve.name);
}

/* ------------------------------- commands -------------------------------- */
void AppRoast::startFan()
{
    if (!linkReady()) {
        toast("Roaster not connected", ui::DROP);
        return;
    }
    uint16_t fan = (uint16_t)lroundf(_rules.startFan * 10);
    _link->command({{12, fan}, {13, 1}});
    _fanSet     = _rules.startFan;
    _fanSetMs   = GetHAL().millis();
    _phase      = Phase::FanOn;
    _fanRunning = true;
    toast("Fan on (no heat): add beans", ui::GO, 3500);
    beep(Tone::Info);
}

void AppRoast::charge()
{
    if (!linkReady()) {
        toast("Roaster not connected", ui::DROP);
        return;
    }
    if (_phase != Phase::FanOn) {
        toast("Press S (fan) before charging", ui::WARN);
        return;
    }
    if (_snap.has(4) && _snap.bt() >= _rules.coldBT &&
        !confirm(1000, "BT " + std::to_string((int)_snap.bt()) + "°C is warm: Enter again"))
        return;

    std::vector<RegWrite> w;
    if (_hasCurve) {
        uint16_t t0 = (uint16_t)lroundf(ComputeSv(_curve, 0, NAN, false, svConfig()).sv * 10);
        w.push_back({9, t0});
        w.push_back({2, 0});
        w.push_back({15, 1});
        _manual        = false;
        _lastTargetRaw = t0;
    } else {
        w.push_back({2, 0});
        _manual  = true;
        _heatSet = 0;
    }
    _link->command(w);
    _chargeMs     = GetHAL().millis();
    _lastSampleT  = -1;
    _dropMs       = 0;
    _satRun       = 0;
    _satWarned = _heatDropWarned = _rorWarned = _autoDropped = false;
    _svFloored = _floorLogged = false;
    _lastSv                   = NAN;
    _autoDropWhy.clear();
    _session.reset(_hasCurve ? &_curve : nullptr, _rules.greenG, _bean);
    _session.openLog(NextLogId());
    for (auto& m : _markMs) m = -1;
    {
        char b[40];
        snprintf(b, sizeof(b), "charge fan %.1f%% BT %.1f", _snap.has(12) ? _snap.fan() : _fanSet, _snap.bt());
        _session.addAction(0, b);
    }
    _phase  = Phase::Roasting;
    _screen = Screen::Roast;
    // quiet roast (see quietRoast()): no key clicks either, until the log ends
    audio::set_keyboard_sfx_enable(false);
    _beepQueue.clear();
    toast(_hasCurve ? "Charged: PID follows curve" : "Charged: manual heat (←→)", ui::GO);
    startRecording();  // last, so a failure toast is not covered by the charge toast
}

void AppRoast::drop(const char* why)
{
    if (_phase != Phase::Roasting) return;
    if (_link) _link->command({{14, 2}});
    int t    = roastT();
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    _session.setMark(Mark::Drop, t, bt);
    _markMs[(int)Mark::Drop] = (int32_t)(GetHAL().millis() - _chargeMs);
    _session.addAction(t, std::string("drop") + (why ? why : ""));
    _session.flush();
    _dropMs = GetHAL().millis();
    _phase  = Phase::Cooling;
    _screen = Screen::Cool;
}

void AppRoast::stopMachine()
{
    if (_link && linkReady()) _link->command({{14, 1}, {15, 0}});
    if (_session.active()) _session.addAction(roastT(), "stop");
    _fanRunning = false;
    _phase      = Phase::Done;
}

void AppRoast::adjustFan(float delta)
{
    if (_phase == Phase::Idle) {
        // Machine not running: only change the preset used by S. Never write to an idle machine.
        float nv = std::max(0.0f, std::min(100.0f, std::round((_rules.startFan + delta) * 10) / 10.0f));
        char b[48];
        snprintf(b, sizeof(b), "Start fan preset %.1f%%", nv);
        _rules.startFan = nv;
        toast(b, ui::FAN, 1200);
        return;
    }
    if (!linkReady()) return;
    float base = _fanSet;
    if (GetHAL().millis() - _fanSetMs > 2500 && _snap.has(12)) base = _snap.fan();
    float nv = std::round((base + delta) * 10) / 10.0f;
    nv       = std::max(0.0f, std::min(100.0f, nv));
    _link->command({{12, (uint16_t)lroundf(nv * 10)}});
    char b[48];
    snprintf(b, sizeof(b), "fan %.1f→%.1f%%", base, nv);
    if (_phase == Phase::Roasting || _phase == Phase::Cooling) _session.addAction(roastT(), b);
    if (_phase == Phase::FanOn || _phase == Phase::Idle) _rules.startFan = nv;
    _fanSet   = nv;
    _fanSetMs = GetHAL().millis();
    toast(b, ui::FAN, 1200);
}

// ↓ with a floor at 50 % and 40 %: a short press stops at the floor, a long press (≥0.6 s) at the floor crosses it.
static const float kFanFloors[2] = {50.0f, 40.0f};

void AppRoast::fanDown(float step)
{
    _downHeld    = true;
    _downPressMs = GetHAL().millis();
    float cur    = _phase == Phase::Idle ? _rules.startFan : _fanSet;
    if (_phase != Phase::Idle && (GetHAL().millis() - _fanSetMs > 2500) && _snap.has(12)) cur = _snap.fan();
    for (float f : kFanFloors) {
        if (std::fabs(cur - f) < 0.05f) {  // sitting on a floor: wait for a long press
            _barrierAt   = f;
            _barrierStep = step;
            char b[48];
            snprintf(b, sizeof(b), "%.0f%% floor: hold ↓ to go lower", f);
            toast(b, ui::WARN, 1500);
            return;
        }
        if (cur > f && cur - step < f) {  // a short press never crosses a floor
            adjustFan(f - cur);
            return;
        }
    }
    adjustFan(-step);
}

void AppRoast::pollFanBarrier()
{
    if (_barrierAt < 0 || !_downHeld) return;
    if (GetHAL().millis() - _downPressMs < 600) return;
    float f      = _barrierAt;
    _barrierAt   = -1;
    adjustFan(-_barrierStep);  // crosses the floor by one step
    char b[48];
    snprintf(b, sizeof(b), "Below the %.0f%% floor", f);
    toast(b, ui::HEAT, 1500);
    beep(Tone::Warn);
}

void AppRoast::adjustHeat(float delta)
{
    if (!linkReady() || _phase != Phase::Roasting) return;
    if (!_manual) {
        toast("PID on curve: P for manual heat", ui::WARN);
        return;
    }
    float nv = std::max(0.0f, std::min(100.0f, _heatSet + delta));
    _link->command({{11, (uint16_t)lroundf(nv * 10)}});
    char b[40];
    snprintf(b, sizeof(b), "heat %.0f→%.0f%%", _heatSet, nv);
    _session.addAction(roastT(), b);
    _heatSet = nv;
    toast(b, ui::HEAT, 1200);
}

void AppRoast::toggleManual()
{
    if (!linkReady() || _phase != Phase::Roasting) return;
    if (!_hasCurve) {
        toast("No curve: heat is manual", ui::WARN);
        return;
    }
    int t = roastT();
    if (!_manual) {
        // PID off, then hold the heater at its average over the last 30 s: one reading can catch the PID mid-swing.
        float nowHeat = _snap.has(11) ? _snap.heat() : 80.0f;
        _heatSet      = HoldHeat(_session.samples(), t, 30, nowHeat);
        _link->command({{15, 0}, {11, (uint16_t)lroundf(_heatSet * 10)}});
        _manual = true;
        char b[64];
        snprintf(b, sizeof(b), "manual heat %.1f%% (30 s avg, now %.1f%%)", _heatSet, nowHeat);
        _session.addAction(t, b);
        snprintf(b, sizeof(b), "manual %.1f%% (30 s avg)", _heatSet);
        toast(std::string(b) + "  ←→", ui::HEAT, 3000);
    } else {
        uint16_t tgt = (uint16_t)lroundf(svAt(curveT()).sv * 10);
        _link->command({{9, tgt}, {15, 1}});
        _lastTargetRaw = tgt;
        _manual = false;
        _session.addAction(t, "PID on curve");
        toast("PID on curve", ui::GO);
    }
}

void AppRoast::toggleMark(Mark m)
{
    if (_phase != Phase::Roasting) return;
    int t    = roastT();
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    if (_session.markT(m) >= 0) {
        if (!confirm(2000 + (int)m, std::string("Again to clear ") + MarkName(m))) return;
        _session.clearMark(m);
        _markMs[(int)m] = -1;
        toast(std::string("Cleared ") + MarkName(m), ui::WARN);
        if (m == Mark::FCs) _heatDropWarned = _rorWarned = false;
        return;
    }
    _session.setMark(m, t, bt);
    _markMs[(int)m] = (int32_t)(GetHAL().millis() - _chargeMs);
    char b[48];
    snprintf(b, sizeof(b), "%s %s · %.1f°", MarkName(m), FormatMmSs(t).c_str(), bt);
    toast(b, m == Mark::Dry ? ui::DRY : (m == Mark::FCs ? ui::FC : ui::FCE));
}

bool AppRoast::confirm(int key, const std::string& msg, bool quiet)
{
    uint32_t now = GetHAL().millis();
    if (_pendingKey == key && now < _pendingUntil) {
        _pendingKey = 0;
        return true;
    }
    _pendingKey   = key;
    _pendingUntil = now + 2000;
    if (!quiet) toast(msg, ui::WARN, 2000);
    beep(Tone::Key);
    return false;
}

/* ------------------------------ curve follow ------------------------------ */
SvConfig AppRoast::svConfig() const
{
    SvConfig c;
    c.lookahead  = _rules.svLookahead;
    c.fcFloor    = _rules.fcFloor;
    c.floorDelta = _rules.floorDelta;
    c.floorCap   = _rules.floorCap;
    return c;
}

SvResult AppRoast::svAt(int t) const
{
    SvConfig c = svConfig();
    int start  = FcFloorStart(_curve.tagTime(PointTag::FCs), _session.markT(Mark::FCs), c.floorLead);
    bool in    = _phase == Phase::Roasting && start >= 0 && t >= start;
    float bt   = _snap.has(4) ? _snap.bt() : NAN;
    return ComputeSv(_curve, t, bt, in, c);
}

/* ------------------------------ roast ticks ------------------------------ */
void AppRoast::tickRoast()
{
    int t = roastT();
    if (t == _lastSampleT) return;
    _lastSampleT = t;
    _dirty       = true;

    // 1) target (SV) for the machine PID: curve with lookahead, floored at BT + delta around first crack
    float sv = NAN;
    if (_hasCurve) {
        SvResult r = svAt(curveT());
        sv         = r.sv;
        _svFloored = _phase == Phase::Roasting && r.floored;
        if (_svFloored && !_floorLogged) {
            char b[64];
            snprintf(b, sizeof(b), "FC floor on: SV %.1f (BT %+.1f, curve %.1f)", r.sv, _rules.floorDelta, r.ahead);
            _session.addAction(t, b);
            _floorLogged = true;
        }
    }
    _lastSv = sv;
    if (_phase == Phase::Roasting && _hasCurve && linkReady()) {
        int raw = (int)lroundf(sv * 10);
        if (raw != _lastTargetRaw) {
            _link->setTarget((uint16_t)raw);
            _lastTargetRaw = raw;
        }
    }

    // 2) sample
    Sample s;
    s.t      = (int16_t)t;
    s.bt10   = _snap.has(4) ? _snap.reg[4] : (_session.samples().empty() ? 0 : _session.samples().back().bt10);
    s.tgt10  = _hasCurve ? (int16_t)lroundf(sv * 10) : 0;  // the SV actually written (lookahead, floor)
    s.heat10 = _snap.reg[11];
    s.fan10  = _snap.reg[12];
    s.mtime  = _snap.reg[2];
    s.pid    = _snap.pidOn() ? 1 : 0;
    s.ror10  = INT16_MIN;
    bool cap = _phase == Phase::Cooling && _dropMs && GetHAL().millis() - _dropMs > 240000;
    if (!cap) _session.addSample(s);
    if (cap) stopRecording();  // the recording ends with the curve log

    // 3) link loss is the most dangerous state (the machine keeps chasing its last target): the red bar says so,
    //    and the alerts below would only be guesses without fresh readings
    if (_phase == Phase::Roasting && !linkReady()) return;
    if (_phase != Phase::Roasting) return;

    float bt   = s.bt10 / 10.0f;
    float heat = s.heat10 / 10.0f;

    // 4) safety auto-drop
    if (bt >= _rules.safetyBT) {
        _autoDropped = true;
        _autoDropWhy = "BT above safety limit";
        drop(" (auto: over temp)");
        return;
    }
    if (_hasCurve && t >= _curve.endTime() + _rules.afterEnd) {
        _autoDropped = true;
        _autoDropWhy = "Curve ended long ago";
        drop(" (auto: timeout)");
        return;
    }

    // 5) heater saturation
    if (heat >= 95.0f && !_manual) {
        if (++_satRun >= 20) _satWarned = true;
    } else {
        _satRun = 0;
    }

    int fc = _session.markT(Mark::FCs);
    if (fc >= 0) {
        int dev = t - fc;
        // heater falling right after first crack (PID sees BT above target)
        if (!_heatDropWarned && !_manual && dev <= 60) {
            double sum = 0;
            int n      = 0;
            for (auto& x : _session.samples())
                if (x.t >= fc - 30 && x.t < fc) {
                    sum += x.heat10 / 10.0;
                    n++;
                }
            if (n > 5 && heat < sum / n - _rules.heatDrop) _heatDropWarned = true;
        }
        float ror = _session.rorNow();
        if (!_rorWarned && dev >= 30 && !std::isnan(ror) && ror < _rules.rorLow) _rorWarned = true;
        // the drop window and "drop now" are drawn from the live values (bannerText); nothing to latch here
    }
}

void AppRoast::enterSummary()
{
    stopRecording();
    audio::set_keyboard_sfx_enable(true);  // the roast is over: sounds are back
    std::string err;
    bool ok   = _session.finalize(&err);
    _saveMsg  = ok ? "Saved: roast/logs/" + _session.logId() : err;
    _screen   = Screen::Summary;
    _phase    = Phase::Done;
    _dirty    = true;
    _saveStarted = _saveReported = false;
    _saveAt                      = 0;
    if (!_fanRunning) planSave();  // X X on the cooling page: the fan is already off
}

void AppRoast::finishRoast()
{
    _session.end();
    _chargeMs = 0;
    _phase    = _fanRunning ? Phase::FanOn : Phase::Idle;
    _screen   = Screen::Prep;
    _manual   = false;
    _view     = 0;
    if (_link) _link->clearQueue();
    _rules.save();
    Settings s("roast", true);
    s.SetString("bean", _bean);
}

/* ---------------------------------- keys --------------------------------- */
static int norm_code(const Keyboard::KeyEvent_t& e)
{
    switch (e.keyCode) {
        case KEY_SEMICOLON:
            return KEY_UP;
        case KEY_DOT:
            return KEY_DOWN;
        case KEY_COMMA:
            return KEY_LEFT;
        case KEY_SLASH:
            return KEY_RIGHT;
        case KEY_GRAVE:
            return KEY_ESC;
        default:
            return e.keyCode;
    }
}

void AppRoast::handleKeys()
{
    while (!_keys.empty()) {
        auto e = _keys.front();
        _keys.pop_front();
        onKey(e);
        _dirty = true;
    }
}

void AppRoast::onKey(const Keyboard::KeyEvent_t& e)
{
    bool shift = g_debugShift || (GetHAL().keyboard.getModifierMask() & KEY_MOD_LSHIFT);
    if (_input.active) {
        keyInput(e, e.keyCode);
        return;
    }
    if (_batch.active) {
        keyBatch(e);
        return;
    }
    int code = norm_code(e);
    // typing a value in the editor or the rules list: every key belongs to that field
    bool typing = (_screen == Screen::Editor && _edStage != 0) || (_screen == Screen::Settings && _setEditing);
    if (!typing && code == KEY_H && _screen != Screen::Help) {
        _backScreen = _screen;
        _screen     = Screen::Help;
        return;
    }
    if (!typing && code == KEY_M) {
        _muted = !_muted;
        toast(_muted ? "Muted" : (quietRoast() ? "Sound on after the roast" : "Sound on"), FAINT);
        return;
    }
    switch (_screen) {
        case Screen::Connect:
            keyConnect(code);
            break;
        case Screen::Prep:
            keyPrep(code, shift);
            break;
        case Screen::Roast:
            keyRoast(code, shift);
            break;
        case Screen::Cool:
            keyCool(code);
            break;
        case Screen::Summary:
            keySummary(code);
            break;
        case Screen::Curves:
            keyCurves(code);
            break;
        case Screen::Editor:
            keyEditor(e, code);
            break;
        case Screen::Settings:
            keySettings(e, code);
            break;
        case Screen::Help:
            if (code == KEY_ESC || code == KEY_ENTER || code == KEY_H) _screen = _backScreen;
            break;
        case Screen::Network:
            keyNetwork(code);
            break;
        case Screen::Sync:
            keySync(code);
            break;
    }
}

bool AppRoast::keyConnect(int code)
{
    if (code == KEY_D) {
        setSimulator(true);
        return true;
    }
    if (code == KEY_O) {  // rules, including Wi-Fi & Dropbox setup, also while the roaster is off
        openSettings();
        return true;
    }
    if (code == KEY_ENTER || code == KEY_R) {
        if (_blockedByKeyboardApp) {
            toast("BLE busy: restart the device", DROP, 3000);
            return true;
        }
        startLink(false);
        return true;
    }
    if (code == KEY_ESC) {
        close();
        return true;
    }
    return false;
}

// Simulator on/off. Only from Connect and Rules, and never while the machine runs: on the Prep page D D used to
// mean "simulator" while during a roast it means "drop", one gesture with two meanings.
void AppRoast::setSimulator(bool on)
{
    if (_fanRunning || _phase != Phase::Idle) {
        toast("Stop the machine first", WARN, 2500);
        return;
    }
    if (on) {
        startLink(true);
        if (_screen == Screen::Connect) _screen = Screen::Prep;
        toast("Simulator: no roaster", SIM, 3000);
    } else {
        if (_blockedByKeyboardApp) {
            toast("BLE busy: restart the device", DROP, 3000);
            return;
        }
        startLink(false);
        _screen = Screen::Connect;
    }
}

void AppRoast::openBatch(int field)
{
    _batch.active = true;
    _batch.field  = field;
    _batch.weight = _rules.greenG > 0 ? std::to_string((int)lroundf(_rules.greenG)) : "";
    _batch.bean   = _bean;
}

void AppRoast::keyBatch(const Keyboard::KeyEvent_t& e)
{
    int code = e.keyCode;
    if (code == KEY_ESC || (code == KEY_GRAVE && _batch.field == 0)) {
        _batch.active = false;
        return;
    }
    if (code == KEY_TAB || code == KEY_UP || code == KEY_DOWN) {
        _batch.field ^= 1;
        return;
    }
    if (code == KEY_ENTER) {
        float g = strtof(_batch.weight.c_str(), nullptr);
        if (g > 0 && g < 2000) _rules.greenG = g;
        _bean         = _batch.bean;
        _batch.active = false;
        _rules.save();
        Settings s("roast", true);
        s.SetString("bean", _bean);
        toast("Batch " + std::to_string((int)lroundf(_rules.greenG)) + " g" + (_bean.empty() ? "" : " · " + _bean), INK);
        return;
    }
    std::string& buf = _batch.field == 0 ? _batch.weight : _batch.bean;
    if (code == KEY_BACKSPACE) {
        if (!buf.empty()) buf.pop_back();
        return;
    }
    if (!e.keyName || strlen(e.keyName) != 1 || buf.size() >= 24) return;
    char c = e.keyName[0];
    if (_batch.field == 0) {
        if (isdigit((unsigned char)c) || c == '.') buf += c;
    } else if (c >= 32 && c < 127) {
        buf += c;
    }
}

bool AppRoast::keyPrep(int code, bool shift)
{
    switch (code) {
        case KEY_S:
            if (_phase == Phase::FanOn) {
                toast("Fan is on: Enter to charge", FAN);
            } else {
                startFan();
            }
            return true;
        case KEY_ENTER:
            charge();
            return true;
        case KEY_X:
            if (_fanRunning && confirm(KEY_X, "", true)) {
                stopMachine();
                _phase = Phase::Idle;
                toast("Fan stopped", INK);
            }
            return true;
        case KEY_UP:
            adjustFan(shift ? 0.1f : 1.0f);
            return true;
        case KEY_DOWN:
            fanDown(shift ? 0.1f : 1.0f);
            return true;
        case KEY_C: {
            reloadCurves();
            _listSel = 0;
            for (size_t i = 0; i < _curves.size(); i++)
                if (_hasCurve && _curves[i].name == _curve.name) _listSel = (int)i + 1;
            _listTop = std::max(0, std::min(_listSel - 1, (int)_curves.size() + 1 - 3));
            _screen  = Screen::Curves;
            return true;
        }
        case KEY_E:
            if (!_hasCurve) {  // new curve = the phone app's dark-roast preset marks, nothing edited yet
                _curve        = Curve();
                _curve.name   = "new";
                _curve.totalT = 880;
                _curve.pts    = {{301, NAN, PointTag::Dry}, {480, NAN, PointTag::FCs}, {556, NAN, PointTag::FCe},
                                 {644, NAN, PointTag::SCs}, {880, NAN, PointTag::Drop}};
                _curve.offset = _rules.curveOffset;
                _curve.build();
                _hasCurve = true;
            }
            if (_curve.readOnly) {
                toast(".alog curves are read-only", WARN);
                return true;
            }
            edLoad();
            _view   = 0;
            _screen = Screen::Editor;
            return true;
        case KEY_W:
            openBatch(0);
            return true;
        case KEY_B:
            openBatch(1);
            return true;
        case KEY_O:
            openSettings();
            return true;
        case KEY_TAB:
            if (_hasCurve)
                _view ^= 1;
            else
                toast("No curve to preview", FAINT);
            return true;
        case KEY_ESC:
            if (_phase == Phase::FanOn) {
                toast("Fan is on: X X to stop it first", WARN, 3000);
            } else if (confirm(KEY_ESC, "Esc again to exit")) {
                close();
            }
            return true;
        default:
            return false;
    }
}

bool AppRoast::keyRoast(int code, bool shift)
{
    switch (code) {
        case KEY_Y:
            toggleMark(Mark::Dry);
            return true;
        case KEY_F:
            toggleMark(Mark::FCs);
            return true;
        case KEY_G:
            toggleMark(Mark::FCe);
            return true;
        case KEY_D:
        case KEY_SPACE:
            if (confirm(KEY_D, "", true)) drop("");
            return true;
        case KEY_UP:
            adjustFan(shift ? 0.1f : 1.0f);
            return true;
        case KEY_DOWN:
            fanDown(shift ? 0.1f : 1.0f);
            return true;
        case KEY_LEFT:
        case KEY_RIGHT: {
            int dir = code == KEY_LEFT ? -1 : 1;
            adjustHeat(dir * (shift ? 1.0f : 5.0f));
            _heatHeld      = dir;
            _heatHeldShift = shift;
            _heatReps      = 0;
            _heatHeldMs = _heatRepMs = GetHAL().millis();
            return true;
        }
        case KEY_P:
            toggleManual();
            return true;
        case KEY_TAB:
            _view ^= 1;
            return true;
        case KEY_ESC:
            toast("Roasting: drop before exit", WARN);
            return true;
        default:
            return false;
    }
}

bool AppRoast::keyCool(int code)
{
    bool shift = g_debugShift || (GetHAL().keyboard.getModifierMask() & KEY_MOD_LSHIFT);
    switch (code) {
        case KEY_X:
            if (confirm(KEY_X, "", true)) {
                stopMachine();
                enterSummary();
            }
            return true;
        case KEY_ENTER:  // finish the log; the fan keeps running and can still be stopped from the summary
            enterSummary();
            return true;
        case KEY_UP:
            adjustFan(shift ? 0.1f : 1.0f);
            return true;
        case KEY_DOWN:
            fanDown(shift ? 0.1f : 1.0f);
            return true;
        case KEY_W:
            openInput("Roasted weight", "", true, "grams, can also be entered later", [this](const std::string& v) {
                float g = strtof(v.c_str(), nullptr);
                if (g > 0) _session.setRoastedWeight(g);
            });
            return true;
        case KEY_TAB:
            _view ^= 1;
            return true;
        default:
            return false;
    }
}

bool AppRoast::keySummary(int code)
{
    switch (code) {
        case KEY_X:
            if (_fanRunning && confirm(KEY_X, "", true)) {
                stopMachine();
                _phase = Phase::Done;
                toast("Fan stopped", INK);
                planSave();
            }
            return true;
        case KEY_W:
            openInput("Roasted weight", "", true, "grams", [this](const std::string& v) {
                float g = strtof(v.c_str(), nullptr);
                if (g > 0) {
                    _session.setRoastedWeight(g);
                    std::string err;
                    _saveMsg = _session.finalize(&err) ? "Updated: roast/logs/" + _session.logId() : err;
                    resaveEdited();
                }
            });
            return true;
        case KEY_N:
            openInput("Note", "", false, "stored in the log", [this](const std::string& v) {
                _session.setNote(v);
                std::string err;
                _saveMsg = _session.finalize(&err) ? "Updated: roast/logs/" + _session.logId() : err;
                resaveEdited();
            });
            return true;
        case KEY_TAB:
            _view ^= 1;
            return true;
        case KEY_ENTER:
        case KEY_ESC: {
            // The roast is saved to Dropbox while the summary stays open; it can only be closed once that is done
            // (or has failed: nothing is retried). Then Roast restarts with a clean heap.
            bool needsSave = _sdOk && !_session.logId().empty();
            if (!needsSave) {
                finishRoast();
                return true;
            }
            if (_fanRunning) {
                toast("Stop the fan (X X): the roast is saved to Dropbox", WARN, 3000);
                return true;
            }
            if (saveBusy()) {
                toast("Saving to Dropbox: wait", WARN, 2500);
                return true;
            }
            sync::Finish();
            for (int i = 0; i < 300 && !sync::Get().finished; i++) GetHAL().delay(10);  // idle: ends at once
            finishRoast();
            restartApp(true);
            return true;
        }
        default:
            return false;
    }
}

bool AppRoast::keyCurves(int code)
{
    int n = (int)_curves.size() + 1;  // row 0 = "No curve"
    switch (code) {
        case KEY_UP:
            if (_listSel > 0) _listSel--;
            break;
        case KEY_DOWN:
            if (_listSel < n - 1) _listSel++;
            break;
        case KEY_ENTER:
            if (_listSel == 0) {
                _hasCurve = false;
                _noCurve  = true;
                _curve    = Curve();
                toast("No curve: heat by hand", HEAT);
            } else {
                selectCurve(_listSel - 1);
                _noCurve = false;
                toast("Curve: " + _curve.name, CURVE);
            }
            _view   = 0;
            _screen = Screen::Prep;
            return true;
        case KEY_E:
            if (_listSel == 0) {
                toast("Nothing to edit: N for a new curve", FAINT);
            } else if (_curves[_listSel - 1].readOnly) {
                toast(".alog curves are read-only", WARN);
            } else {
                selectCurve(_listSel - 1);
                _noCurve = false;
                edLoad();
                _view   = 0;
                _screen = Screen::Editor;
            }
            return true;
        case KEY_N:  // new curve from the phone app's dark-roast preset
            _curve        = Curve();
            _curve.name   = "new";
            _curve.totalT = 880;
            _curve.pts    = {{301, NAN, PointTag::Dry}, {480, NAN, PointTag::FCs}, {556, NAN, PointTag::FCe},
                             {644, NAN, PointTag::SCs}, {880, NAN, PointTag::Drop}};
            _curve.offset = _rules.curveOffset;
            _curve.build();
            _hasCurve = true;
            _noCurve  = false;
            edLoad();
            _edDirty = true;
            _view    = 0;
            _screen  = Screen::Editor;
            return true;
        case KEY_S:  // sync now: same as at boot (Dropbox's curve folder replaces the card's)
            if (networkIdle("sync")) restartApp(false);
            return true;
        case KEY_BACKSPACE: {  // delete: on Dropbox, then on the card
            if (_listSel == 0) return true;
            const Curve& c = _curves[_listSel - 1];
            if (c.builtin) {
                toast("Built-in curves cannot be deleted", WARN);
                return true;
            }
            if (!networkIdle("delete")) return true;
            if (!confirm(3000, "Del again to delete " + c.name + " (also on Dropbox)")) return true;
            startJob(sync::Job::DeleteCurve, c.path);
            return true;
        }
        case KEY_ESC:
            _screen = Screen::Prep;
            return true;
        default:
            return false;
    }
    if (_listSel < _listTop) _listTop = _listSel;
    if (_listSel > _listTop + 2) _listTop = _listSel - 2;
    return true;
}

/* ------------------------------ curve editor ------------------------------ */
// rows: 0 Start (temperature only) 1 Dry 2 FC start 3 FC end 4 SC start 5 SC end 6 Drop (always on) 7 Total (time only)
static const PointTag kEdTag[8] = {PointTag::Charge, PointTag::Dry, PointTag::FCs, PointTag::FCe,
                                   PointTag::SCs,    PointTag::SCe, PointTag::Drop, PointTag::None};

void AppRoast::edLoad()
{
    for (auto& r : _ed) r = EdRow();
    for (auto& p : _curve.pts) {
        for (int i = 0; i < 7; i++) {
            if (p.tag == kEdTag[i] || (i == 0 && p.t == 0 && p.tag == PointTag::None)) {
                _ed[i].on = true;
                _ed[i].t  = i == 0 ? 0 : p.t;
                _ed[i].c  = p.c;
            }
        }
    }
    _ed[0].t = 0;
    if (!_ed[6].on) {
        _ed[6].on = true;
        _ed[6].t  = _curve.endTime();
    }
    _ed[7].on = true;
    _ed[7].t  = _curve.totalT > 0 ? _curve.totalT : _curve.endTime();
    _edRow = 1;
    _edCol = 0;
    _edStage = 0;
    _edDirty = false;
}

void AppRoast::edApply()
{
    std::vector<CurvePoint> pts;
    for (auto& p : _curve.pts)  // keep free (untagged) points from files
        if (p.tag == PointTag::None && p.t > 0) pts.push_back(p);
    if (_ed[0].on && !std::isnan(_ed[0].c)) pts.push_back({0, _ed[0].c, PointTag::Charge});
    for (int i = 1; i < 7; i++)
        if (_ed[i].on && _ed[i].t >= 0) pts.push_back({_ed[i].t, _ed[i].c, kEdTag[i]});
    _curve.pts    = pts;
    _curve.totalT = _ed[7].t;
    _curve.build();
    _hasCurve = _curve.valid();
    _edDirty  = true;
}

// Same rule as the phone editor: a node sits strictly between its enabled neighbours; Drop <= Total <= 1800 s.
bool AppRoast::edCheckTime(int row, int t, std::string& why)
{
    if (row == 7) {
        if (t < _ed[6].t) {
            why = "Total must be >= Drop " + FormatMmSs(_ed[6].t);
            return false;
        }
        if (t > 1800) {
            why = "Total must be <= 30:00";
            return false;
        }
        return true;
    }
    int lo = 0, hi = _ed[7].t;
    for (int i = row - 1; i >= 1; i--)
        if (_ed[i].on) {
            lo = _ed[i].t;
            break;
        }
    for (int i = row + 1; i <= 6; i++)
        if (_ed[i].on) {
            hi = _ed[i].t;
            break;
        }
    bool ok = t > lo && (row == 6 ? t <= hi : t < hi);
    if (!ok) why = "Must be between " + FormatMmSs(lo) + " and " + FormatMmSs(hi);
    return ok;
}

bool AppRoast::keyEditor(const Keyboard::KeyEvent_t& e, int code)
{
    auto digit = [&](char& out) {
        if (e.keyName && strlen(e.keyName) == 1 && isdigit((unsigned char)e.keyName[0])) {
            out = e.keyName[0];
            return true;
        }
        return false;
    };
    char d;
    EdRow& r = _ed[_edRow];
    if (_view == 1) {  // preview: Tab/Esc back to the table, S still saves
        if (code == KEY_TAB || code == KEY_ESC) _view = 0;
        if (code != KEY_S) return true;
    }
    // ---- typing a value: minutes -> seconds -> temperature
    if (_edStage != 0) {
        if (code == KEY_ESC) {
            _edStage = 0;
            return true;
        }
        if (code == KEY_BACKSPACE) {
            if (!_edBuf.empty()) _edBuf.pop_back();
            return true;
        }
        if (_edStage == 3 && e.keyName && strcmp(e.keyName, ".") == 0 && _edBuf.find('.') == std::string::npos) {
            _edBuf += '.';
            return true;
        }
        if (digit(d)) {
            if (_edBuf.size() < (_edStage == 3 ? 5u : 2u)) _edBuf += d;
            return true;
        }
        if (code != KEY_ENTER && code != KEY_RIGHT && code != KEY_TAB) return true;
        if (_edStage == 1) {
            if (code == KEY_TAB && _edRow != 7) {  // keep the time, go straight to the temperature
                _edStage = 3;
                _edBuf.clear();
                return true;
            }
            _edMin   = _edBuf.empty() ? std::to_string(std::max(0, r.t) / 60) : _edBuf;
            _edBuf.clear();
            _edStage = 2;
            return true;
        }
        if (_edStage == 2) {
            int m   = atoi(_edMin.c_str());
            int sec = _edBuf.empty() ? std::max(0, r.t) % 60 : atoi(_edBuf.c_str());
            if (sec > 59) {
                toast("Seconds must be 0-59", DROP);
                _edStage = 0;
                return true;
            }
            int t = m * 60 + sec;
            if (t != r.t) {
                std::string why;
                if (!edCheckTime(_edRow, t, why)) {
                    toast(why, DROP, 2500);
                    _edStage = 0;
                    return true;
                }
                r.t = t;
                if (_edRow == 6 && _ed[7].t < t) _ed[7].t = t;
                edApply();
            }
            if (_edRow == 7) {
                _edStage = 0;
            } else {
                _edStage = 3;
                _edBuf.clear();
            }
            return true;
        }
        if (_edStage == 3) {
            _edStage = 0;
            if (_edBuf.empty()) return true;
            float c = strtof(_edBuf.c_str(), nullptr);
            if (c < 0 || c > 300) {
                toast("Temperature 0-300°C", DROP);
                return true;
            }
            r.c = std::round(c * 10) / 10.0f;
            edApply();
        }
        return true;
    }
    // ---- browsing
    auto toggle = [&]() -> bool {  // returns true when the node is (now) on
        if (_edRow == 6 || _edRow == 7) {
            toast("Drop and Total are always on", WARN);
            return true;
        }
        if (!r.on && _edRow > 0 && r.t < 0) {  // first enable: middle of the free gap
            int lo = 0, hi = _ed[7].t;
            for (int i = _edRow - 1; i >= 1; i--)
                if (_ed[i].on) {
                    lo = _ed[i].t;
                    break;
                }
            for (int i = _edRow + 1; i <= 6; i++)
                if (_ed[i].on) {
                    hi = _ed[i].t;
                    break;
                }
            r.t = (lo + hi) / 2;
        }
        if (!r.on && _edRow > 0) {
            std::string why;
            if (!edCheckTime(_edRow, r.t, why)) {
                toast(std::string("Fix the time: ") + why, DROP, 2500);
                r.on     = true;  // enable so the time can be fixed right away
                _edStage = 1;
                _edBuf.clear();
                return true;
            }
        }
        if (_edRow == 0 && !r.on && std::isnan(r.c)) r.c = _curve.at(0);
        r.on = !r.on;
        edApply();
        return r.on;
    };
    auto startEdit = [&](const std::string& first) {
        if (!r.on && !toggle()) return;
        if (_edStage != 0) return;  // toggle() already opened the time field
        _edStage = _edRow == 0 ? 3 : 1;
        _edBuf   = first;
    };
    switch (code) {
        case KEY_UP:
            if (_edRow > 0) _edRow--;
            break;
        case KEY_DOWN:
            if (_edRow < kEdRows - 1) _edRow++;
            break;
        case KEY_SPACE:
            toggle();
            break;
        case KEY_ENTER:
            startEdit("");
            break;
        case KEY_BACKSPACE:  // temperature back to "not edited" (follows the preset)
            if (r.on && _edRow > 0 && _edRow < 7) {
                r.c = NAN;
                edApply();
                toast("Temperature follows the preset", CURVE);
            }
            break;
        case KEY_N:
            openInput("Curve name", _curve.name, false, "", [this](const std::string& v) {
                if (!v.empty()) {
                    _curve.name = v;
                    _curve.path.clear();
                    _edDirty = true;
                }
            });
            break;
        case KEY_B:
            openInput("Batch size", std::to_string(_curve.batchG), true, "grams this curve is made for",
                      [this](const std::string& v) {
                          _curve.batchG = atoi(v.c_str());
                          _edDirty      = true;
                      });
            break;
        case KEY_S: {
            if (!networkIdle("save")) break;  // saving uploads the curve: the roaster link closes meanwhile
            if (!_sdOk) _sdOk = GetHAL().sdCardMount();
            if (!_sdOk) {
                toast("No SD: curve kept until reboot", WARN, 3000);
                _hasCurve = _curve.valid();
                _view     = 0;
                _screen   = Screen::Prep;
                break;
            }
            std::string err;
            if (_curve.builtin) _curve.name += " copy";
            if (SaveCurveJson(_curve, &err)) {
                reloadCurves();
                for (size_t i = 0; i < _curves.size(); i++)
                    if (_curves[i].path == _curve.path) selectCurve((int)i);
                _edDirty = false;
                _noCurve = false;
                _view    = 0;
                startJob(sync::Job::PushCurve, _curve.path);  // overwrite on Dropbox, then restart
            } else {
                toast(err, DROP, 3000);
            }
            break;
        }
        case KEY_TAB:
            _view = 1;
            break;
        case KEY_ESC:
            if (_edDirty && !confirm(KEY_ESC, "Unsaved: Esc again to use as is")) break;
            _hasCurve = _curve.valid();
            _screen   = Screen::Prep;
            _view     = 0;
            break;
        default:
            if (digit(d)) startEdit(std::string(1, d));
            break;
    }
    return true;
}

std::vector<AppRoast::RuleRow> AppRoast::ruleRows()
{
    std::vector<RuleRow> v;
    auto group = [&](const char* g, uint32_t c) {
        RuleRow r;
        r.group      = g;
        r.groupColor = c;
        v.push_back(r);
    };
    auto rowF = [&](const char* l, float* a, float* b, float lo, float hi, float step, const char* unit) {
        RuleRow r;
        r.label = l;
        r.f[0]  = a;
        r.f[1]  = b;
        r.lo    = lo;
        r.hi    = hi;
        r.step  = step;
        r.unit  = unit;
        v.push_back(r);
    };
    auto rowI = [&](const char* l, int* a, int* b, float lo, float hi, float step, const char* unit) {
        RuleRow r;
        r.label = l;
        r.i[0]  = a;
        r.i[1]  = b;
        r.lo    = lo;
        r.hi    = hi;
        r.step  = step;
        r.unit  = unit;
        v.push_back(r);
    };
    group("DROP WINDOW", GO);
    rowI("dev after FC", &_rules.devMin, &_rules.devMax, 20, 400, 1, " s");
    rowF("rise after FC", &_rules.dtMin, &_rules.dtMax, 0, 30, 0.5f, "°");
    rowF("drop now at", &_rules.maxDropBT, nullptr, 150, 250, 1, "°");
    group("SAFETY", DROP);
    rowF("auto-drop at", &_rules.safetyBT, nullptr, 150, 260, 1, "°");
    rowI("after curve end", &_rules.afterEnd, nullptr, 0, 900, 10, " s");
    group("ALERTS", WARN);
    rowF("low RoR below", &_rules.rorLow, nullptr, 0, 30, 0.5f, "");
    rowF("heat drop after FC", &_rules.heatDrop, nullptr, 1, 50, 1, "%");
    group("MACHINE", FAN);
    rowF("start fan", &_rules.startFan, nullptr, 20, 100, 1, "%");
    rowF("cold below", &_rules.coldBT, nullptr, 20, 80, 1, "°");
    rowF("curve offset", &_rules.curveOffset, nullptr, -30, 30, 0.5f, "°");
    group("CURVE FOLLOW", CURVE);
    rowI("SV lookahead", &_rules.svLookahead, nullptr, 0, 20, 1, " s");
    RuleRow floorRow;
    floorRow.label  = "FC floor";
    floorRow.toggle = &_rules.fcFloor;
    v.push_back(floorRow);
    rowF("floor above BT", &_rules.floorDelta, nullptr, 0, 5, 0.5f, "°");
    rowF("floor cap", &_rules.floorCap, nullptr, 1, 8, 0.5f, "°");
    group("AUDIO", DROP);  // every roast is recorded (when there is an SD card); only the gain is a setting
    rowI("mic gain", &_rules.micGain, nullptr, 0, rec::kMaxGainDb, 3, " dB");
    group("SYNC", CURVE);
    RuleRow netRow;
    netRow.label   = "network";
    netRow.network = true;
    v.push_back(netRow);
    group("MODE", SIM);
    RuleRow sim;
    sim.label     = "simulator";
    sim.simToggle = true;
    v.push_back(sim);
    return v;
}

bool AppRoast::keySettings(const Keyboard::KeyEvent_t& e, int code)
{
    auto rows = ruleRows();
    std::vector<int> sel;
    for (int i = 0; i < (int)rows.size(); i++)
        if (rows[i].label) sel.push_back(i);
    int n   = (int)sel.size();
    _setSel = std::max(0, std::min(n - 1, _setSel));
    RuleRow r  = rows[sel[_setSel]];
    bool range = r.f[1] || r.i[1];
    auto cur   = [&](int k) { return r.f[k] ? *r.f[k] : (float)*r.i[k]; };
    if (!_setEditing) {
        switch (code) {
            case KEY_UP:
                if (_setSel > 0) _setSel--;
                return true;
            case KEY_DOWN:
                if (_setSel < n - 1) _setSel++;
                return true;
            case KEY_ENTER:
                if (r.simToggle) {
                    setSimulator(!(_link && _link->isSimulated()));
                    return true;
                }
                if (r.network) {
                    openNetwork();
                    return true;
                }
                if (r.toggle) {
                    *r.toggle = !*r.toggle;
                    _rules.save();
                    return true;
                }
                _setEditing = true;
                _setStage   = 0;
                _setBuf.clear();
                _setVal[0] = cur(0);
                _setVal[1] = range ? cur(1) : 0;
                return true;
            case KEY_ESC:
                _rules.save();
                _screen = _setBack;
                return true;
            default:
                return false;
        }
    }
    float& v = _setVal[_setStage];
    // typing: digits, '.' and '-' come from the raw key (',' '.' ';' '/' double as arrows elsewhere)
    if (e.keyName && strlen(e.keyName) == 1) {
        char c = e.keyName[0];
        if ((isdigit((unsigned char)c) || (c == '.' && _setBuf.find('.') == std::string::npos) ||
             (c == '-' && _setBuf.empty() && r.lo < 0)) &&
            _setBuf.size() < 6) {
            _setBuf += c;
            return true;
        }
    }
    switch (code) {
        case KEY_LEFT:
        case KEY_RIGHT: {
            _setBuf.clear();
            float nv = v + (code == KEY_RIGHT ? r.step : -r.step);
            nv       = std::round(nv / r.step) * r.step;
            v        = std::max(r.lo, std::min(r.hi, nv));
            return true;
        }
        case KEY_BACKSPACE:
            if (!_setBuf.empty()) _setBuf.pop_back();
            return true;
        case KEY_ESC:
            _setEditing = false;
            _setBuf.clear();
            return true;
        case KEY_ENTER: {
            if (!_setBuf.empty()) {
                float x = strtof(_setBuf.c_str(), nullptr);
                _setBuf.clear();
                if (x < r.lo || x > r.hi) {
                    char b[48];
                    snprintf(b, sizeof(b), "Out of range: %g to %g", r.lo, r.hi);
                    toast(b, DROP, 2500);
                    return true;
                }
                v = x;
            }
            if (range && _setStage == 0) {
                _setStage = 1;
                if (_setVal[1] < _setVal[0]) _setVal[1] = _setVal[0];
                return true;
            }
            if (range && _setVal[1] < _setVal[0]) std::swap(_setVal[0], _setVal[1]);
            for (int k = 0; k < (range ? 2 : 1); k++) {
                if (r.f[k])
                    *r.f[k] = _setVal[k];
                else
                    *r.i[k] = (int)lroundf(_setVal[k]);
            }
            _curve.offset = _rules.curveOffset;
            _rules.save();
            _setEditing = false;
            return true;
        }
        default:
            return true;
    }
}

void AppRoast::openInput(const std::string& title, const std::string& init, bool numeric, const std::string& hint,
                         std::function<void(const std::string&)> done)
{
    _input.active  = true;
    _input.title   = title;
    _input.buf     = init;
    _input.numeric = numeric;
    _input.hint    = hint;
    _input.done    = std::move(done);
}

void AppRoast::keyInput(const Keyboard::KeyEvent_t& e, int code)
{
    if (code == KEY_ENTER) {
        _input.active = false;
        if (_input.done) _input.done(_input.buf);
        return;
    }
    if (code == KEY_ESC || (code == KEY_GRAVE && _input.numeric)) {
        _input.active = false;
        return;
    }
    if (code == KEY_BACKSPACE) {
        if (!_input.buf.empty()) _input.buf.pop_back();
        return;
    }
    if (!e.keyName || strlen(e.keyName) != 1 || _input.buf.size() >= 24) return;
    char c = e.keyName[0];
    if (_input.numeric) {
        if (isdigit((unsigned char)c) || c == '.' || c == '-' || c == ':') _input.buf += c;
    } else if (c >= 32 && c < 127) {
        _input.buf += c;
    }
}

/* ------------------------------ Dropbox sync ------------------------------ */
// RTC memory survives esp_restart(): a marker here makes the next boot skip the curve sync (we just synced or
// just pushed a change).
static RTC_NOINIT_ATTR uint32_t s_bootFlag;
static constexpr uint32_t kSkipSync = 0x53594E43;  // "SYNC"

static constexpr uint32_t kBypass = 0x42595053;  // "BYPS": test boot from the USB console

void roast::DebugBootRoast()
{
    s_bootFlag = kBypass;
    esp_restart();
}

bool AppRoast::consumeBypass()
{
    if (s_bootFlag != kBypass) return false;
    s_bootFlag = 0;
    return true;
}

bool AppRoast::consumeSkipSync()
{
    bool skip  = s_bootFlag == kSkipSync;
    s_bootFlag = 0;
    return skip;
}

void AppRoast::restartApp(bool skipBootSync)
{
    s_bootFlag = skipBootSync ? kSkipSync : 0;
    _rules.save();
    toast("Restarting…", INK, 5000);
    render();
    GetHAL().delay(150);
    esp_restart();
    // Desktop preview only (esp_restart returns there): carry on as the next boot would.
    s_bootFlag  = 0;
    _toastUntil = 0;
    if (skipBootSync) {
        startLink(false);
        _screen = Screen::Connect;
    } else {
        startJob(sync::Job::Curves, "");
    }
}

bool AppRoast::networkIdle(const char* what)
{
    if (_phase != Phase::Idle || _fanRunning) {
        toast(std::string("Stop the fan to ") + what + ": it needs Dropbox", WARN, 3000);
        return false;
    }
    return true;
}

void AppRoast::planSave()
{
    if (!_sdOk || _session.logId().empty() || _saveStarted) return;
    _saveAt = GetHAL().millis() + 1500;  // the fan-stop command reaches the roaster before the link closes
}

void AppRoast::pollSave()
{
    if (_screen != Screen::Summary && _screen != Screen::Help) return;
    if (!_saveStarted && _saveAt && GetHAL().millis() >= _saveAt && !_fanRunning) {
        _saveAt = 0;
        // Close the roaster link (BLE + Wi-Fi + TLS do not fit together); keep the roast's data for the summary.
        if (_link) {
            g_activeLink = nullptr;
            _link->clearQueue();
            _link->end();
            _link.reset();
        }
        GetHAL().setRoastBleActive(false);
        _linkState = _prevLinkState = LinkState::Off;
        _saveStarted = sync::Start(sync::Job::SaveRoast, _session.logId());
        if (!_saveStarted) toast("Dropbox save could not start", DROP, 3000);
        _dirty = true;
    }
    if (_saveStarted && !_saveReported) {
        sync::Status st = sync::Get();
        if (st.finished && !st.ok) {  // only a failure is worth a word: success is the normal case
            _saveReported = true;
            toast("Not saved: " + st.error, DROP, 6000);
        }
    }
}

bool AppRoast::saveBusy()
{
    if (_phase != Phase::Done || !_sdOk || _session.logId().empty()) return false;
    if (!_saveStarted) return true;  // not started yet (fan still on, or about to start)
    sync::Status st = sync::Get();
    return !(st.finished || st.idle);
}

void AppRoast::resaveEdited()
{
    if (!_saveStarted) return;  // not started yet: the first upload takes the edited files anyway
    sync::Resave(".json");
    sync::Resave(".alog");
    _saveReported = false;  // report again when this lands
}

bool AppRoast::startJob(sync::Job job, const std::string& arg)
{
    // BLE + Wi-Fi + TLS do not fit together: close the roaster link first (the app restarts afterwards).
    if (_link) {
        g_activeLink = nullptr;
        _link->clearQueue();
        _link->end();
        _link.reset();
    }
    GetHAL().setRoastBleActive(false);
    _linkState = _prevLinkState = LinkState::Off;
    _session.release();  // the roast's samples are saved; free them for TLS
    _screen = Screen::Sync;
    _dirty  = true;
    if (!sync::Start(job, arg)) {
        toast("Sync could not start", DROP, 3000);
        return false;
    }
    return true;
}

void AppRoast::pollSync()
{
    if (_screen != Screen::Sync) return;
    sync::Status st = sync::Get();
    if (st.finished && st.ok) restartApp(true);  // done: clean heap, no second sync
}

bool AppRoast::keySync(int code)
{
    sync::Status st = sync::Get();
    if (!st.finished) return true;  // nothing to do while it runs: no cancel, no retry
    bool boot = st.job == sync::Job::Curves;
    if (code == KEY_ENTER) {  // boot sync must succeed: Enter tries again; other jobs: carry on without it
        restartApp(!boot);
        return true;
    }
    if (code == KEY_O) {
        openNetwork();
        return true;
    }
    if (code == KEY_ESC) {
        if (boot)
            close();
        else
            restartApp(true);
        return true;
    }
    return false;
}

/* ------------------------- Wi-Fi & Dropbox setup ------------------------- */
void AppRoast::openSettings()
{
    _setBack    = _screen == Screen::Connect ? Screen::Connect : Screen::Prep;
    _setSel     = 0;
    _setEditing = false;
    refreshNetSummary();
    _screen = Screen::Settings;
}

void AppRoast::refreshNetSummary()
{
    char ssid[33];
    bool wifi    = roast_net_saved_wifi(ssid);
    bool dropbox = roast_net_saved_dropbox();
    if (!wifi && !dropbox)
        _netSummary = "set up Wi-Fi & Dropbox";
    else
        _netSummary = std::string(wifi ? ssid : "no Wi-Fi") + (dropbox ? " · Dropbox" : " · no Dropbox");
}

// Setup needs NimBLE for itself (and the memory BLE + Wi-Fi + TLS take), so the roaster link closes meanwhile.
void AppRoast::openNetwork()
{
    if (_phase != Phase::Idle || _fanRunning) {
        toast("Stop the fan before network setup", WARN);
        return;
    }
    _netWasSim = _link && _link->isSimulated();
    if (_link) {
        g_activeLink = nullptr;
        _link->clearQueue();
        _link->end();
        _link.reset();
    }
    GetHAL().setRoastBleActive(false);
    _linkState = _prevLinkState = LinkState::Off;
    _netOpen = roast_net_open();
    if (!_netOpen) toast("Setup could not start", DROP, 3000);
    _screen = Screen::Network;
}

void AppRoast::closeNetwork()
{
    bool started = _netOpen;
    if (_netOpen) roast_net_close();
    _netOpen = false;
    if (!started && _netRequired) {  // nothing saved yet and setup did not start: boot again (it reopens setup)
        restartApp(false);
        return;
    }
    if (started) {
        // BLE + Wi-Fi + TLS leave the heap fragmented (measured: largest block 15 KB afterwards), too little for the
        // roast log (19 KB) and a recording. Restart into Roast with a clean heap; esp_restart() boots this app again,
        // not the Launcher. Rules and the network setup are already saved in NVS.
        _rules.save();
        toast("Restarting…", INK, 5000);
        render();
        GetHAL().delay(150);
        esp_restart();
    }
    refreshNetSummary();
    startLink(_netWasSim);
    _screen = Screen::Connect;  // pollLink() moves on to Prep when the roaster answers
}

bool AppRoast::keyNetwork(int code)
{
    if (code != KEY_ESC) return false;
    if (_netOpen && roast_net_busy()) {
        toast("Dropbox is busy: wait a moment", WARN);
        return true;
    }
    closeNetwork();
    return true;
}

/* ------------------------------- recording ------------------------------- */
void AppRoast::startRecording()
{
    if (_recording || !_sdOk || !_session.logging()) return;  // always on: there is no switch
    std::string err;
    if (!rec::Start(LogPath(_session.logId(), ".wav"), _rules.micGain, &err)) {
        _session.addAction(roastT(), "no audio: " + err);
        toast("No recording: " + err, WARN, 3500);
        return;
    }
    _recording  = true;
    _recStartMs = rec::StartMs();
}

void AppRoast::stopRecording()
{
    if (!_recording) return;
    _recording = false;
    AudioStats st;
    std::vector<AudioGap> gaps;
    bool ok = rec::Stop(&st, &gaps);
    AudioInfo a;
    a.valid              = true;
    a.file               = _session.logId() + ".wav";
    a.sampleRate         = st.sampleRate;
    a.gainDb             = st.gainDb;
    a.startMsAfterCharge = (int)(_recStartMs - _chargeMs);
    a.durationS          = st.sampleRate ? (double)st.samples / st.sampleRate : 0;
    a.peakDbfs           = PeakDbfs(st.peak);
    a.clipped            = st.clipped;
    a.bufferMs           = st.bufferMs;
    a.slot               = st.slot;
    a.failed             = !ok || st.failed;
    a.gaps               = gaps;
    a.gapCount           = st.gapCount;
    a.eventMs[0]         = 0;  // charge: the recording starts a few ms after it
    for (int i = 0; i < (int)Mark::Count; i++)
        if (_markMs[i] >= 0) a.eventMs[i + 1] = std::max(0, (int)_markMs[i] - a.startMsAfterCharge);
    _session.setAudio(a);
    char b[48];
    snprintf(b, sizeof(b), "audio %.0f s%s", a.durationS, a.failed ? " (write failed)" : "");
    _session.addAction(roastT(), b);
}

void AppRoast::pollRecording()
{
    if (!_recording) return;
    if (rec::Stats().failed) {
        stopRecording();
        toast("Recording stopped: SD write failed", DROP, 4000);
    }
}

/* --------------------------------- misc ---------------------------------- */
void AppRoast::toast(const std::string& msg, uint32_t color, uint32_t ms)
{
    _toast      = msg;
    _toastColor = color;
    _toastUntil = GetHAL().millis() + ms;
    _dirty      = true;
}

void AppRoast::beep(Tone t)
{
    // Silent during a roast (quietRoast()). Notes are played one by one with Speaker.tone(); the launcher's
    // play_melody() renders the whole melody into one large buffer, which runs out of heap while BLE is up.
    if (g_debugQuiet || quietRoast() || _muted || audio::is_quiet_mode()) return;
    auto midi = [](int m) { return (int)lround(440.0 * pow(2.0, (m - 69) / 12.0)); };
    std::vector<std::pair<int, int>> notes;  // freq (0 = rest), ms
    if (GetHAL().speaker.getVolume() < audio::DEFAULT_VOLUME) GetHAL().speaker.setVolume(audio::DEFAULT_VOLUME);
    switch (t) {
        case Tone::Key:
            notes = {{1800, 30}};
            break;
        case Tone::Info:
            notes = {{midi(76), 60}, {midi(81), 60}};
            break;
        case Tone::Good:
            notes = {{midi(72), 70}, {midi(76), 70}, {midi(79), 70}, {midi(84), 90}};
            break;
        case Tone::Warn:
            notes = {{midi(84), 90}, {midi(77), 90}, {midi(84), 90}, {midi(77), 90}};
            break;
    }
    _beepQueue.clear();
    for (auto& n : notes) _beepQueue.push_back(n);
    _beepNextMs = 0;
}

void AppRoast::pumpBeep()
{
    uint32_t now = GetHAL().millis();
    if (quietRoast()) {
        _beepQueue.clear();
        return;
    }
    if (_beepQueue.empty()) return;
    if (now < _beepNextMs) return;
    auto n = _beepQueue.front();
    _beepQueue.pop_front();
    if (n.first > 0) GetHAL().speaker.tone((float)n.first, n.second, 1, true);
    _beepNextMs = now + n.second;
}
