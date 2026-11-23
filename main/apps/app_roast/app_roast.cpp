/*
 * Roast app logic: connection, roast state machine, keys, alerts.
 * Rendering lives in app_roast_ui.cpp.
 */
#include "app_roast.h"
#ifndef LGFX_LINUX_FB
#include <esp_log.h>
#endif
#include "roast_debug.h"
#include "net/roast_net.h"
#include <esp_system.h>
#if __has_include(<esp_attr.h>)
#include <esp_attr.h>
#endif
#ifndef RTC_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#endif
#include <hal/utils/settings/settings.h>
#include <mooncake_log.h>
#include <esp_system.h>
#include "okou_agent.h"
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
    safetyBT  = s.GetInt("safety10", (int)lroundf(safetyBT * 10)) / 10.0f;
    afterEnd  = s.GetInt("afterEnd", afterEnd);
    rorLow    = s.GetInt("rorLow10", (int)lroundf(rorLow * 10)) / 10.0f;
    coldBT    = s.GetInt("coldBT10", (int)lroundf(coldBT * 10)) / 10.0f;
    startFan  = s.GetInt("fan10", (int)lroundf(startFan * 10)) / 10.0f;
    greenG    = s.GetInt("green10", (int)lroundf(greenG * 10)) / 10.0f;
    curveOffset = s.GetInt("curveOff10", (int)lroundf(curveOffset * 10)) / 10.0f;
    svLookahead = std::max(0, std::min(20, (int)s.GetInt("svLook", svLookahead)));
    floorCap    = std::max(10, std::min(80, (int)s.GetInt("floorCap10", (int)lroundf(floorCap * 10)))) / 10.0f;
    pidKp       = std::max(1, std::min(100, (int)s.GetInt("pidKp10", (int)lroundf(pidKp * 10)))) / 10.0f;
    pidTi       = std::max(0, std::min(600, (int)s.GetInt("pidTi", pidTi)));
    pidTd       = std::max(0, std::min(30, (int)s.GetInt("pidTd", pidTd)));
    // "pidFcTd" held 0, the default until the self-heating estimate; its new default needs a new key.
    pidFcKp     = std::max(1, std::min(100, (int)s.GetInt("pidFcKp10", (int)lroundf(pidFcKp * 10)))) / 10.0f;
    pidFcTd     = std::max(0, std::min(30, (int)s.GetInt("pidFcTd2", pidFcTd)));
    pidFF       = s.GetInt("pidFF", pidFF ? 1 : 0) != 0;
    pidObs      = s.GetInt("pidObs", pidObs ? 1 : 0) != 0;
}

void RoastRules::save() const
{
    Settings s("roast", true);
    s.SetInt("safety10", (int)lroundf(safetyBT * 10));
    s.SetInt("afterEnd", afterEnd);
    s.SetInt("rorLow10", (int)lroundf(rorLow * 10));
    s.SetInt("coldBT10", (int)lroundf(coldBT * 10));
    s.SetInt("fan10", (int)lroundf(startFan * 10));
    s.SetInt("green10", (int)lroundf(greenG * 10));
    s.SetInt("curveOff10", (int)lroundf(curveOffset * 10));
    s.SetInt("svLook", svLookahead);
    s.SetInt("floorCap10", (int)lroundf(floorCap * 10));
    s.SetInt("pidKp10", (int)lroundf(pidKp * 10));
    s.SetInt("pidTi", pidTi);
    s.SetInt("pidTd", pidTd);
    s.SetInt("pidFcKp10", (int)lroundf(pidFcKp * 10));
    s.SetInt("pidFcTd2", pidFcTd);
    s.SetInt("pidFF", pidFF ? 1 : 0);
    s.SetInt("pidObs", pidObs ? 1 : 0);
}

/* ---------------------------------- app ---------------------------------- */
AppRoast::AppRoast()
{
    setAppInfo().name = "Roast";
}

AppRoast::~AppRoast() {}

void AppRoast::onOpen()
{
    mclog::tagInfo(TAG, "on open");
    GetHAL().canvas.setBaseColor(ui::BG);
    GetHAL().canvas.setTextScroll(false);

    _rules.load();
    _sdOk = GetHAL().sdCardMount();
    char ssid[33];
    const bool bypass = consumeBypass();
    const bool skip = consumeSkipSync();
    const bool configured = roast_net_esphub_network(ssid) && roast_net_saved_dropbox();
    const bool bootSync = !bypass && !skip && configured && _sdOk;
    // Boot sync restarts the app. Load the curve tables on the boot that uses them.
    if (!bootSync) {
        reloadCurves();
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
    _keySlot  = GetHAL().keyboard.onKeyEvent.connect([this](const Keyboard::KeyEvent_t& e) {
        if (e.isModifier) return;
        if (e.state) {
            _keys.push_back(e);
        } else if (e.keyCode == KEY_DOT || e.keyCode == KEY_DOWN) {
            _downHeld  = false;
            _barrierAt = -1;
        }
    });
    // Wi-Fi and Dropbox are required. Boot syncs the curves from Dropbox (must succeed) and restarts once with a
    // clean heap; that second boot (flagged in RTC memory) skips the sync and connects to the roaster.
    if (bypass) {  // test only: USB console `boot roast`
        startLink();
        toast("Test boot: no Dropbox sync", WARN, 4000);
    } else if (!configured) {
        _netRequired = true;
        openNetwork();
    } else if (skip || !_sdOk) {
        startLink();
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
    _keys.clear();
    // The SDK selects the preserved ESP Hub launcher, without altering NVS or the app image.
    blankExternal();
    GetHAL().display.fillScreen(0);
    GetHAL().delay(100);
    okou_enter_setup();
}

void AppRoast::startLink(bool sim)
{
    if (_link) {
        g_activeLink = nullptr;
        _link->end();
        _link.reset();
    }
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
    pollSync();
    pollSave();
    if (_phase == Phase::Roasting || _phase == Phase::Cooling) tickRoast();

    // Leaving the app is only allowed when the machine is not heating.
    if (GetHAL().homeButton.wasClicked()) {
        if ((_screen == Screen::Sync && !sync::Get().finished) || saveBusy()) {
            // leaving now would cut an upload in half (or skip the roast's save altogether)
            toast(_fanRunning ? "Stop the fan first (X X)" : "Syncing", WARN);
        } else if (_phase == Phase::Roasting) {
            toast("Roasting: drop before exit", ui::DROP);
        } else {
            close();
            return;
        }
    }

    uint32_t now = GetHAL().millis();
    if (_batMs == 0 || now - _batMs > 5000) {
        const int level = M5.Power.getBatteryLevel();
        _batPct = (level >= 0 && level <= 100) ? level : -1;
#ifndef LGFX_LINUX_FB
        static int lastLevel = -2;
        if (_batPct != lastLevel) {
            ESP_LOGI("roast-battery", "M5Unified percent=%d", _batPct);
            lastLevel = _batPct;
        }
#endif
        _batMs  = now;
    }
    if (_dirty || now - _lastRender > 250) {
        render();
        _lastRender = now;
        _dirty      = false;
    }
    pumpExternal();
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

bool AppRoast::selectCurve(int idx)
{
    if (idx < 0 || idx >= (int)_curves.size()) {
        _hasCurve = false;
        _curve    = Curve();
        return false;
    }
    if (!OpenLibraryCurve(_curves[idx], _curve)) {
        toast("Cannot load the selected curve", WARN);
        return false;
    }
    _curve.offset = _rules.curveOffset;
    _hasCurve     = _curve.valid();
    if (_curve.batchG > 0) _rules.greenG = (float)_curve.batchG;
    Settings s("roast", true);
    s.SetString("curve", _curve.name);
    return true;
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
    if (!_hasCurve) {
        toast("Pick a curve first (C)", ui::WARN);
        return;
    }
    if (_snap.has(4) && _snap.bt() >= _rules.coldBT &&
        !confirm(1000, "BT " + std::to_string((int)_snap.bt()) + "°C is warm: Enter again"))
        return;

    // The Cardputer PID sets the heater: the roaster's own PID stays off, pidTick() writes register 11 from the first
    // second on. Register 9 only shows the curve's start on the roaster.
    _link->command({{9, (uint16_t)lroundf(_curve.at(0) * 10)}, {2, 0}, {15, 0}});
    _dev         = DevTrack();
    _fcOffset    = 0;
    _lastHeatRaw = -1;
    _pidMs       = 0;
    PidReset(_pid, 0);
    _chargeMs     = GetHAL().millis();
    _lastSampleT  = -1;
    _dropMs       = 0;
    _satRun       = 0;
    _satWarned = _rorWarned = _autoDropped = false;
    _lastSv = NAN;
    _autoDropWhy.clear();
    _session.reset(&_curve, _rules.greenG, _bean);
    _session.openLog(NextLogId());
    {
        char b[40];
        snprintf(b, sizeof(b), "charge fan %.1f%% BT %.1f", _snap.has(12) ? _snap.fan() : _fanSet, _snap.bt());
        _session.addAction(0, b);
        char c[128];
        snprintf(c, sizeof(c), "control: Cardputer PID P %.1f Ti %d s Td %d s, after F P %.1f Td %d s, feedforward %s%s",
                 _rules.pidKp, _rules.pidTi, _rules.pidTd, _rules.pidFcKp, _rules.pidFcTd, _rules.pidFF ? "on" : "off",
                 _rules.pidFF && _rules.pidObs ? " + self-heat estimate" : "");
        _session.addAction(0, c);
    }
    _phase  = Phase::Roasting;
    _screen = Screen::Roast;
    toast("Charged: Cardputer PID", ui::GO);
}

void AppRoast::drop(const char* why)
{
    if (_phase != Phase::Roasting) return;
    if (_link) {
        _link->cancelHeat();  // a heater write queued before the drop must not land after it
        _link->command({{14, 2}});
    }
    int t    = roastT();
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    _session.setMark(Mark::Drop, t, bt);
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
}

void AppRoast::toggleMark(Mark m)
{
    if (_phase != Phase::Roasting) return;
    int t    = roastT();
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    if (_session.markT(m) >= 0) {
        if (!confirm(2000 + (int)m, std::string("Again to clear ") + MarkName(m))) return;
        _session.clearMark(m);
        toast(std::string("Cleared ") + MarkName(m), ui::WARN);
        if (m == Mark::FCs) {
            _rorWarned = false;
            fcAnchor(false);
        }
        return;
    }
    _session.setMark(m, t, bt);
    if (m == Mark::FCs) fcAnchor(true);
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
    return false;
}

/* ------------------------------ curve follow ------------------------------ */
PidConfig AppRoast::pidConfig() const
{
    PidConfig c;
    c.kp          = _rules.pidKp;
    c.ti          = (float)_rules.pidTi;
    c.td          = (float)_rules.pidTd;
    c.fcKp        = _rules.pidFcKp;
    c.fcTd        = (float)_rules.pidFcTd;
    c.feedforward = _rules.pidFF;
    c.observe     = _rules.pidObs;
    return c;
}

// What the Cardputer PID aims at, at curve second t: error against the curve now, feedforward from the curve
// `lookahead` seconds ahead (roast_control.h PidAim); after F the development, or the curve shifted to BT.
PidAim AppRoast::pidAim(int t) const
{
    int fcT    = _session.markT(Mark::FCs);
    int start  = ObserveStart(_curve.tagTime(PointTag::FCs), fcT, pidConfig().obsLead);
    bool roast = _phase == Phase::Roasting;
    if (roast && _dev.active) return DevAim(_dev, t, _rules.svLookahead);
    return CardputerAim(_curve, t, roast ? start : -1, roast && fcT >= 0, _fcOffset, _rules.svLookahead);
}

// F set or cleared. With a curve that has first crack and drop points, development starts here: from where the
// beans actually cracked, at the curve's development speed (DevStart); Roast drops when the development time is over
// or the drop temperature is reached (tickRoast). Without such a curve the rest of the curve is shifted to BT.
// The Cardputer PID is re-primed so the heater does not jump.
void AppRoast::fcAnchor(bool marked)
{
    if (!_hasCurve || _phase != Phase::Roasting) return;
    float bt  = _snap.has(4) ? _snap.bt() : _session.lastBT();
    int t     = curveT();
    _dev      = marked ? DevStart(_curve, t, bt) : DevTrack();
    _fcOffset = marked && !_dev.active ? FcAnchorOffset(_curve, t, bt, _rules.floorCap) : 0;
    PidPrime(_pid, _pid.out, pidAim(t), bt, pidConfig());
    char b[96];
    if (marked && _dev.active)
        snprintf(b, sizeof(b), "development from %.1f: %.1f/min for %s, at most %.1f", bt, _dev.plan.speed * 60,
                 FormatMmSs(_dev.plan.dur).c_str(), _dev.plan.dropBT);
    else if (marked)
        snprintf(b, sizeof(b), "FC anchor: curve %+.1f from here (BT %.1f)", _fcOffset, bt);
    else
        snprintf(b, sizeof(b), "FC cleared: back to the curve");
    _session.addAction(roastT(), b);
}

// Cardputer PID, once a roast second: heater output from the curve and BT.
void AppRoast::pidTick(const PidAim& aim)
{
    uint32_t now = GetHAL().millis();
    float dt     = _pidMs ? (now - _pidMs) / 1000.0f : 1.0f;
    _pidMs       = now;
    // A reading older than 3 s is not fresh: hold the output (PidStep with NAN) rather than act on it.
    bool fresh = _snap.has(4) && _snap.lastRxMs && now - _snap.lastRxMs < 3000;
    float bt   = fresh ? _snap.bt() : NAN;
    PidTerms o = PidStep(_pid, aim, bt, dt, pidConfig());
    int raw    = (int)lroundf(o.heat * 10);
    // Also re-send an unchanged value every 5 s: the roaster cut its heater ~17-19 s into a Bluetooth loss (empty
    // run, 2026-11-13), and if that timeout counts writes rather than the connection, a steady output must not
    // trip it.
    if (raw != _lastHeatRaw || now - _heatSentMs >= 5000) {
        _link->setHeat((uint16_t)raw);
        _lastHeatRaw = raw;
        _heatSentMs  = now;
    }
    _session.setControl(o.heat, o.ff, o.p, o.i, o.d, o.obs * 60);
}

/* ------------------------------ roast ticks ------------------------------ */
void AppRoast::tickRoast()
{
    int t = roastT();
    if (t == _lastSampleT) return;
    _lastSampleT = t;
    _dirty       = true;

    // 1) what the Cardputer PID aims at: the curve now, or after F the development
    float sv = NAN;
    PidAim aim;
    if (_hasCurve) {
        aim = pidAim(curveT());
        sv  = aim.target;
    }
    _lastSv = sv;
    if (_phase == Phase::Roasting && linkReady()) pidTick(aim);

    // 2) sample
    Sample s;
    s.t      = (int16_t)t;
    s.bt10   = _snap.has(4) ? _snap.reg[4] : (_session.samples().empty() ? 0 : _session.samples().back().bt10);
    s.tgt10  = _hasCurve ? (int16_t)lroundf(sv * 10) : 0;  // the Cardputer PID's target
    s.heat10 = _snap.reg[11];
    s.fan10  = _snap.reg[12];
    s.mtime  = _snap.reg[2];
    s.pid    = _phase == Phase::Roasting ? 2 : (_snap.pidOn() ? 1 : 0);
    s.ror10  = INT16_MIN;
    bool cap = _phase == Phase::Cooling && _dropMs && GetHAL().millis() - _dropMs > 240000;
    if (!cap) _session.addSample(s);

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
    // the curve's development: drop when its time is over (from F) or its drop temperature is reached; before F at
    // the drop temperature or the curve's drop time
    if (_hasCurve) {
        DevPlan plan = _curve.devPlan();
        DropWhy why  = DevDropDue(_dev, plan, curveT(), bt);
        if (why != DropWhy::None) {
            char b[64];
            if (why == DropWhy::Time && _dev.active)
                snprintf(b, sizeof(b), " (auto: development %s, %+.1f)", FormatMmSs(plan.dur).c_str(), bt - _dev.btF);
            else if (why == DropWhy::Time)
                snprintf(b, sizeof(b), " (auto: curve drop time %s)", FormatMmSs(plan.dropT).c_str());
            else
                snprintf(b, sizeof(b), " (auto: %.1f reached)", plan.dropBT);
            _autoDropped = true;
            _autoDropWhy = why == DropWhy::Temp ? "Drop temperature reached" : "Development time reached";
            drop(b);
            return;
        }
    }

    // 5) heater saturation
    if (heat >= 95.0f) {
        if (++_satRun >= 20) _satWarned = true;
    } else {
        _satRun = 0;
    }

    int fc = _session.markT(Mark::FCs);
    if (fc >= 0) {
        int dev   = t - fc;
        float ror = _session.rorNow();
        if (!_rorWarned && dev >= 30 && !std::isnan(ror) && ror < _rules.rorLow) _rorWarned = true;
        // the drop window and "drop now" are drawn from the live values (bannerText); nothing to latch here
    }
}

void AppRoast::enterSummary()
{
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
    if (code == KEY_O) {  // rules, including Dropbox setup, also while the roaster is off
        openSettings();
        return true;
    }
    if (code == KEY_ENTER || code == KEY_R) {
        startLink();
        return true;
    }
    if (code == KEY_ESC) {
        close();
        return true;
    }
    return false;
}

// Simulator on/off. Only from Connect and Rules, and never while the machine runs: during a roast D means "drop".
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
                if (_hasCurve && _curves[i].name == _curve.name) _listSel = (int)i;
            _listTop = std::max(0, std::min(_listSel - 1, (int)_curves.size() - 3));
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
                toast("Stop the fan first (X X)", WARN, 3000);
                return true;
            }
            if (saveBusy()) {
                toast("Syncing", WARN, 1500);
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
    int n = (int)_curves.size();
    bool any = _listSel >= 0 && _listSel < n;
    switch (code) {
        case KEY_UP:
            if (_listSel > 0) _listSel--;
            break;
        case KEY_DOWN:
            if (_listSel < n - 1) _listSel++;
            break;
        case KEY_ENTER:
            if (!any) {
                toast("No curves: N for a new one", FAINT);
                return true;
            }
            if (!selectCurve(_listSel)) return true;
            toast("Curve: " + _curve.name, CURVE);
            _view   = 0;
            _screen = Screen::Prep;
            return true;
        case KEY_E:
            if (!any) {
                toast("Nothing to edit: N for a new curve", FAINT);
            } else if (_curves[_listSel].readOnly) {
                toast(".alog curves are read-only", WARN);
            } else {
                if (!selectCurve(_listSel)) return true;
                edLoad();
                _view   = 0;
                _screen = Screen::Editor;
            }
            return true;
        case KEY_N:  // new curve from the phone app's dark-roast preset
            _curve        = Curve();
            _curve.name   = "new";
            _curve.totalT = 540;
            _curve.pts    = {{0, 50, PointTag::Charge}, {450, 200, PointTag::FCs}, {540, 210, PointTag::Drop}};
            _curve.offset = _rules.curveOffset;
            _curve.build();
            _hasCurve = true;
            edLoad();
            _edDirty = true;
            _view    = 0;
            _screen  = Screen::Editor;
            return true;
        case KEY_S:  // sync now: same as at boot (Dropbox's curve folder replaces the card's)
            if (networkIdle("sync")) restartApp(false);
            return true;
        case KEY_BACKSPACE: {  // delete: on Dropbox, then on the card
            if (!any) return true;
            const Curve& c = _curves[_listSel];
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
// rows: 0 Start (temperature only) 1 first crack 2 drop (both always on, time and temperature). Yellowing is not a
// curve point: Y marks it during the roast.
static const PointTag kEdTag[3] = {PointTag::Charge, PointTag::FCs, PointTag::Drop};

void AppRoast::edLoad()
{
    for (auto& r : _ed) r = EdRow();
    for (auto& p : _curve.pts)
        for (int i = 0; i < kEdRows; i++)
            if (p.tag == kEdTag[i] || (i == 0 && p.t == 0 && p.tag == PointTag::None)) {
                _ed[i].on = true;
                _ed[i].t  = i == 0 ? 0 : p.t;
                _ed[i].c  = p.c;
            }
    _ed[0].t = 0;
    int end  = std::max(60, _curve.endTime());
    if (_ed[2].t <= 0) _ed[2].t = end;
    if (_ed[1].t <= 0 || _ed[1].t >= _ed[2].t) _ed[1].t = std::max(1, _ed[2].t * 5 / 6);
    _ed[1].on = _ed[2].on = true;
    for (int i = 1; i < kEdRows; i++)
        if (std::isnan(_ed[i].c)) _ed[i].c = std::round(_curve.at(_ed[i].t) * 10) / 10.0f;
    if (!(_ed[2].c > _ed[1].c)) _ed[2].c = _ed[1].c + 5;
    _edRow   = 1;
    _edCol   = 0;
    _edStage = 0;
    _edDirty = false;
}

void AppRoast::edApply()
{
    std::vector<CurvePoint> pts;
    if (_ed[0].on && !std::isnan(_ed[0].c)) pts.push_back({0, _ed[0].c, PointTag::Charge});
    pts.push_back({_ed[1].t, _ed[1].c, PointTag::FCs});
    pts.push_back({_ed[2].t, _ed[2].c, PointTag::Drop});
    _curve.pts    = pts;
    _curve.totalT = _ed[2].t;
    _curve.build();
    _hasCurve = _curve.valid();
    _edDirty  = true;
}

// First crack strictly before the drop; the drop at most 30:00.
bool AppRoast::edCheckTime(int row, int t, std::string& why)
{
    if (row == 1 && !(t > 0 && t < _ed[2].t)) {
        why = "First crack must be before the drop " + FormatMmSs(_ed[2].t);
        return false;
    }
    if (row == 2 && !(t > _ed[1].t && t <= 1800)) {
        why = "Drop must be after first crack " + FormatMmSs(_ed[1].t) + " and by 30:00";
        return false;
    }
    return true;
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
            if (code == KEY_TAB) {  // keep the time, go straight to the temperature
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
                edApply();
            }
            _edStage = 3;
            _edBuf.clear();
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
            if ((_edRow == 1 && c >= _ed[2].c) || (_edRow == 2 && c <= _ed[1].c)) {
                toast("Drop must be hotter than first crack", DROP, 2500);
                return true;
            }
            r.c = std::round(c * 10) / 10.0f;
            edApply();
        }
        return true;
    }
    // ---- browsing
    auto toggle = [&]() -> bool {  // returns true when the node is (now) on
        if (_edRow != 0) {
            toast("First crack and drop are always on", WARN);
            return true;
        }
        if (!r.on && std::isnan(r.c)) r.c = _curve.at(0);
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
        case KEY_BACKSPACE:  // start temperature back to the preset's
            if (_edRow == 0 && r.on) {
                r.on = false;
                edApply();
                toast("Start follows the preset", CURVE);
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
    group("SAFETY", DROP);
    rowF("auto-drop at", &_rules.safetyBT, nullptr, 150, 260, 1, "°");
    rowI("after curve end", &_rules.afterEnd, nullptr, 0, 900, 10, " s");
    group("ALERTS", WARN);
    rowF("low RoR below", &_rules.rorLow, nullptr, 0, 30, 0.5f, "");
    group("MACHINE", FAN);
    rowF("start fan", &_rules.startFan, nullptr, 20, 100, 1, "%");
    rowF("cold below", &_rules.coldBT, nullptr, 20, 80, 1, "°");
    rowF("curve offset", &_rules.curveOffset, nullptr, -30, 30, 0.5f, "°");
    group("CURVE FOLLOW", CURVE);
    rowF("P", &_rules.pidKp, nullptr, 0.1f, 10, 0.1f, "%/°");
    rowI("I time", &_rules.pidTi, nullptr, 0, 600, 5, " s");
    rowI("D time", &_rules.pidTd, nullptr, 0, 30, 1, " s");
    rowF("P after F", &_rules.pidFcKp, nullptr, 0.1f, 10, 0.1f, "%/°");
    rowI("D after F", &_rules.pidFcTd, nullptr, 0, 30, 1, " s");
    RuleRow ffRow;
    ffRow.label  = "feedforward";
    ffRow.toggle = &_rules.pidFF;
    v.push_back(ffRow);
    RuleRow obsRow;
    obsRow.label  = "self-heat estimate";
    obsRow.toggle = &_rules.pidObs;
    v.push_back(obsRow);
    rowI("SV lookahead", &_rules.svLookahead, nullptr, 0, 20, 1, " s");
    rowF("floor cap", &_rules.floorCap, nullptr, 1, 8, 0.5f, "°");
    group("SYNC", CURVE);
    RuleRow netRow;
    netRow.label   = "Dropbox";
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
        startLink();
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
        _linkState = _prevLinkState = LinkState::Off;
        // The summary cannot open the curve library, and Roast restarts after the save.
        std::vector<Curve>().swap(_curves);
        releaseExternal();
        GetHAL().releaseCanvas();
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

bool AppRoast::syncingNow()
{
    if (_saveAt) return true;  // the roast's save starts in a moment
    sync::Status st = sync::Get();
    return st.running && !st.idle;
}

bool AppRoast::saveFailed()
{
    if (_screen != Screen::Summary && _screen != Screen::Help) return false;
    if (!_saveStarted) return false;
    sync::Status st = sync::Get();
    return st.finished && !st.ok;
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
    if (job == sync::Job::Curves && !(_sdOk = GetHAL().sdCardMount())) {
        toast("No SD card: sync skipped", WARN);
        return false;
    }
    // BLE + Wi-Fi + TLS do not fit together: close the roaster link first (the app restarts afterwards).
    releaseExternal();
    if (_link) {
        g_activeLink = nullptr;
        _link->clearQueue();
        _link->end();
        _link.reset();
    }
    _linkState = _prevLinkState = LinkState::Off;
    GetHAL().releaseCanvas();  // draw directly to the TFT while Dropbox TLS needs a contiguous block
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
    // done: clean heap, no second sync. A firmware check stays on screen: what was found, what to do in the Launcher.
    if (st.finished && st.ok) restartApp(true);
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
    bool wifi    = roast_net_esphub_network(ssid);
    bool dropbox = roast_net_saved_dropbox();
    if (!wifi && !dropbox)
        _netSummary = "ESP Hub Wi-Fi / Dropbox setup";
    else
        _netSummary = std::string(wifi ? ssid : "ESP Hub offline") + (dropbox ? " · Dropbox" : " · no Dropbox");
}

// Setup needs NimBLE for itself (and the memory BLE + Wi-Fi + TLS take), so the roaster link closes meanwhile.
void AppRoast::openNetwork()
{
    if (_phase != Phase::Idle || _fanRunning) {
        toast("Stop the fan before Dropbox setup", WARN);
        return;
    }
    releaseExternal();
    _netWasSim = _link && _link->isSimulated();
    if (_link) {
        g_activeLink = nullptr;
        _link->clearQueue();
        _link->end();
        _link.reset();
    }
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
        // roast log (19 KB). Restart into Roast with a clean heap; esp_restart() boots this app again,
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

/* --------------------------------- misc ---------------------------------- */
void AppRoast::toast(const std::string& msg, uint32_t color, uint32_t ms)
{
    _toast      = msg;
    _toastColor = color;
    _toastUntil = GetHAL().millis() + ms;
    _dirty      = true;
}
