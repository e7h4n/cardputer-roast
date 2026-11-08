/*
 * Roast app logic: connection, roast state machine, keys, alerts.
 * Rendering lives in app_roast_ui.cpp.
 */
#include "app_roast.h"
#include "assets/roast_big.h"
#include "assets/roast_small.h"
#include "roast_debug.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
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
}

/* ---------------------------------- app ---------------------------------- */
AppRoast::AppRoast()
{
    setAppInfo().name     = "Roast";
    setAppInfo().userData = new AppIcon_t(image_data_roast_big, image_data_roast_small);
}

AppRoast::~AppRoast()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppRoast::onOpen()
{
    mclog::tagInfo(TAG, "on open");
    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
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
            _keys.push_back(e);
        } else if (e.keyCode == KEY_DOT || e.keyCode == KEY_DOWN) {
            _downHeld  = false;
            _barrierAt = -1;
        }
    });
    _blockedByKeyboardApp = GetHAL().bleKeyboardInited();
    if (!_blockedByKeyboardApp) startLink(false);
    _dirty = true;
}

void AppRoast::onClose()
{
    mclog::tagInfo(TAG, "on close");
    if (_keySlot >= 0) GetHAL().keyboard.onKeyEvent.disconnect(_keySlot);
    _keySlot = -1;
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
    GetHAL().display.fillScreen(TFT_BLACK);
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
    if (!_link->begin()) toast("BLE init failed", 0xFF5252);
    _linkState = _link->state();
}

void AppRoast::onRunning()
{
    pollLink();
    handleKeys();
    pollFanBarrier();
    pumpBeep();
    if (_phase == Phase::Roasting || _phase == Phase::Cooling) tickRoast();

    // Leaving the app is only allowed when the machine is not heating.
    if (GetHAL().homeButton.wasClicked()) {
        if (_phase == Phase::Roasting) {
            toast("Roasting: drop before exit", 0xFF5252);
            beep(Tone::Warn);
        } else {
            audio::play_random_tone();
            close();
            return;
        }
    }

    uint32_t now = GetHAL().millis();
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
            if (_prevLinkState == LinkState::Lost) toast("Reconnected", 0x99FF00);
            beep(Tone::Good);
        } else if (_linkState == LinkState::Lost) {
            toast("BLE lost, reconnecting", 0xFF5252);
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
        toast("Roaster not connected", 0xFF5252);
        return;
    }
    uint16_t fan = (uint16_t)lroundf(_rules.startFan * 10);
    _link->command({{12, fan}, {13, 1}});
    _fanSet   = _rules.startFan;
    _fanSetMs = GetHAL().millis();
    _phase    = Phase::FanOn;
    toast("Fan on (no heat): add beans", 0x99FF00, 3500);
    beep(Tone::Info);
}

void AppRoast::charge()
{
    if (!linkReady()) {
        toast("Roaster not connected", 0xFF5252);
        return;
    }
    if (_phase != Phase::FanOn) {
        toast("Press S (fan) before charging", 0xFFD54F);
        return;
    }
    if (_snap.has(4) && _snap.bt() >= _rules.coldBT &&
        !confirm(1000, "BT " + std::to_string((int)_snap.bt()) + "°C is warm: Enter again"))
        return;

    std::vector<RegWrite> w;
    if (_hasCurve) {
        uint16_t t0 = (uint16_t)lroundf(_curve.at(0) * 10);
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
    _satWarned = _heatDropWarned = _rorWarned = _windowBeeped = _autoDropped = false;
    _autoDropWhy.clear();
    _session.reset(_hasCurve ? &_curve : nullptr, _rules.greenG, _bean);
    _session.openLog(NextLogId());
    {
        char b[40];
        snprintf(b, sizeof(b), "charge fan %.1f%% BT %.1f", _snap.has(12) ? _snap.fan() : _fanSet, _snap.bt());
        _session.addAction(0, b);
    }
    _phase  = Phase::Roasting;
    _screen = Screen::Roast;
    toast(_hasCurve ? "Charged: PID follows curve" : "Charged: manual heat (←→)", 0x99FF00);
    beep(Tone::Good);
}

void AppRoast::drop(const char* why)
{
    if (_phase != Phase::Roasting) return;
    if (_link) _link->command({{14, 2}});
    int t    = roastT();
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    _session.setMark(Mark::Drop, t, bt);
    _session.addAction(t, std::string("drop") + (why ? why : ""));
    _session.flush();
    _dropMs = GetHAL().millis();
    _phase  = Phase::Cooling;
    _screen = Screen::Cool;
    beep(Tone::Good);
}

void AppRoast::stopMachine()
{
    if (_link && linkReady()) _link->command({{14, 1}, {15, 0}});
    if (_session.active()) _session.addAction(roastT(), "stop");
    _phase = Phase::Done;
}

void AppRoast::adjustFan(float delta)
{
    if (_phase == Phase::Idle) {
        // Machine not running: only change the preset used by S. Never write to an idle machine.
        float nv = std::max(0.0f, std::min(100.0f, std::round((_rules.startFan + delta) * 10) / 10.0f));
        char b[48];
        snprintf(b, sizeof(b), "Start fan preset %.1f%%", nv);
        _rules.startFan = nv;
        toast(b, 0x4FC3F7, 1200);
        return;
    }
    if (!linkReady()) return;
    float base = _fanSet;
    if (GetHAL().millis() - _fanSetMs > 2500 && _snap.has(12)) base = _snap.fan();
    float nv = std::round((base + delta) * 10) / 10.0f;
    nv       = std::max(0.0f, std::min(100.0f, nv));
    _link->command({{12, (uint16_t)lroundf(nv * 10)}});
    char b[48];
    snprintf(b, sizeof(b), "fan %.1f->%.1f%%", base, nv);
    if (_phase == Phase::Roasting || _phase == Phase::Cooling) _session.addAction(roastT(), b);
    if (_phase == Phase::FanOn || _phase == Phase::Idle) _rules.startFan = nv;
    _fanSet   = nv;
    _fanSetMs = GetHAL().millis();
    toast(b, 0x4FC3F7, 1200);
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
            toast(b, 0xFFD54F, 1500);
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
    toast(b, 0xFF9A3C, 1500);
    beep(Tone::Warn);
}

void AppRoast::adjustHeat(float delta)
{
    if (!linkReady() || _phase != Phase::Roasting) return;
    if (!_manual) {
        toast("PID on curve: P for manual heat", 0xFFD54F);
        return;
    }
    float nv = std::max(0.0f, std::min(100.0f, _heatSet + delta));
    _link->command({{11, (uint16_t)lroundf(nv * 10)}});
    char b[40];
    snprintf(b, sizeof(b), "heat %.0f->%.0f%%", _heatSet, nv);
    _session.addAction(roastT(), b);
    _heatSet = nv;
    toast(b, 0xFF9A3C, 1200);
}

void AppRoast::toggleManual()
{
    if (!linkReady() || _phase != Phase::Roasting) return;
    if (!_hasCurve) {
        toast("No curve: heat is manual", 0xFFD54F);
        return;
    }
    int t = roastT();
    if (!_manual) {
        // PID off, then hold the heater at its current output.
        _heatSet = _snap.has(11) ? std::round(_snap.heat()) : 80.0f;
        _link->command({{15, 0}, {11, (uint16_t)lroundf(_heatSet * 10)}});
        _manual = true;
        char b[48];
        snprintf(b, sizeof(b), "manual heat %.0f%%", _heatSet);
        _session.addAction(t, b);
        toast(std::string(b) + "  ←→ adjust", 0xFF9A3C, 3000);
    } else {
        uint16_t tgt = (uint16_t)lroundf(curveTargetAt(t) * 10);
        _link->command({{9, tgt}, {15, 1}});
        _manual = false;
        _session.addAction(t, "PID on curve");
        toast("PID on curve", 0x99FF00);
    }
    beep(Tone::Info);
}

void AppRoast::toggleMark(Mark m)
{
    if (_phase != Phase::Roasting) return;
    int t    = roastT();
    float bt = _snap.has(4) ? _snap.bt() : _session.lastBT();
    if (_session.markT(m) >= 0) {
        if (!confirm(2000 + (int)m, std::string("Again to clear ") + MarkName(m))) return;
        _session.clearMark(m);
        toast(std::string("Cleared ") + MarkName(m), 0xFFD54F);
        if (m == Mark::FCs) _heatDropWarned = _rorWarned = _windowBeeped = false;
        return;
    }
    _session.setMark(m, t, bt);
    char b[48];
    snprintf(b, sizeof(b), "%s %s %.1f°C", MarkName(m), FormatMmSs(t).c_str(), bt);
    toast(b, 0x99FF00);
    beep(Tone::Info);
}

bool AppRoast::confirm(int key, const std::string& msg)
{
    uint32_t now = GetHAL().millis();
    if (_pendingKey == key && now < _pendingUntil) {
        _pendingKey = 0;
        return true;
    }
    _pendingKey   = key;
    _pendingUntil = now + 2000;
    toast(msg, 0xFFD54F, 2000);
    beep(Tone::Key);
    return false;
}

/* ------------------------------ roast ticks ------------------------------ */
void AppRoast::tickRoast()
{
    int t = roastT();
    if (t == _lastSampleT) return;
    _lastSampleT = t;
    _dirty       = true;

    // 1) target for the machine PID
    if (_phase == Phase::Roasting && _hasCurve && linkReady()) {
        int raw = (int)lroundf(curveTargetAt(curveT()) * 10);
        if (raw != _lastTargetRaw) {
            _link->setTarget((uint16_t)raw);
            _lastTargetRaw = raw;
        }
    }

    // 2) sample
    Sample s;
    s.t      = (int16_t)t;
    s.bt10   = _snap.has(4) ? _snap.reg[4] : (_session.samples().empty() ? 0 : _session.samples().back().bt10);
    s.tgt10  = _hasCurve ? (int16_t)lroundf(curveTargetAt(curveT()) * 10) : 0;
    s.heat10 = _snap.reg[11];
    s.fan10  = _snap.reg[12];
    s.mtime  = _snap.reg[2];
    s.pid    = _snap.pidOn() ? 1 : 0;
    s.ror10  = INT16_MIN;
    bool cap = _phase == Phase::Cooling && _dropMs && GetHAL().millis() - _dropMs > 240000;
    if (!cap) _session.addSample(s);

    uint32_t now = GetHAL().millis();

    // 3) link loss is the most dangerous state: the machine keeps chasing its last target
    if (_phase == Phase::Roasting && !linkReady()) {
        if (now - _lastAlarmMs > 3000) {
            beep(Tone::Alarm);
            _lastAlarmMs = now;
        }
        return;
    }
    if (_phase != Phase::Roasting) return;

    float bt   = s.bt10 / 10.0f;
    float heat = s.heat10 / 10.0f;

    // 4) safety auto-drop
    if (bt >= _rules.safetyBT) {
        _autoDropped = true;
        _autoDropWhy = "BT above safety limit";
        drop(" (auto: over temp)");
        beep(Tone::Alarm);
        return;
    }
    if (_hasCurve && t >= _curve.endTime() + _rules.afterEnd) {
        _autoDropped = true;
        _autoDropWhy = "Curve ended long ago";
        drop(" (auto: timeout)");
        beep(Tone::Alarm);
        return;
    }

    // 5) heater saturation
    if (heat >= 95.0f && !_manual) {
        if (++_satRun >= 20 && !_satWarned) {
            _satWarned = true;
            beep(Tone::Warn);
        }
    } else {
        _satRun = 0;
    }

    int fc = _session.markT(Mark::FCs);
    if (fc >= 0) {
        int dev  = t - fc;
        float dT = bt - _session.markBT(Mark::FCs);
        // heater falling right after first crack (PID sees BT above target)
        if (!_heatDropWarned && !_manual && dev <= 60) {
            double sum = 0;
            int n      = 0;
            for (auto& x : _session.samples())
                if (x.t >= fc - 30 && x.t < fc) {
                    sum += x.heat10 / 10.0;
                    n++;
                }
            if (n > 5 && heat < sum / n - _rules.heatDrop) {
                _heatDropWarned = true;
                beep(Tone::Warn);
            }
        }
        float ror = _session.rorNow();
        if (!_rorWarned && dev >= 30 && !std::isnan(ror) && ror < _rules.rorLow) {
            _rorWarned = true;
            beep(Tone::Warn);
        }
        bool mustDrop = bt >= _rules.maxDropBT || dev >= _rules.devMax;
        bool window   = dev >= _rules.devMin && dT >= _rules.dtMin;
        if (mustDrop) {
            if (now - _lastAlarmMs > 4000) {
                beep(Tone::Alarm);
                _lastAlarmMs = now;
            }
        } else if (window && !_windowBeeped) {
            _windowBeeped = true;
            beep(Tone::Good);
        }
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
}

void AppRoast::finishRoast()
{
    _session.end();
    _chargeMs = 0;
    _phase    = Phase::Idle;
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
    if (_screen == Screen::Editor) {
        keyEditor(e, norm_code(e));
        return;
    }
    int code = norm_code(e);
    if (code == KEY_M && _screen != Screen::Curves) {
        _muted = !_muted;
        toast(_muted ? "Muted (BLE alarm stays on)" : "Sound on");
        return;
    }
    if (code == KEY_H && _screen != Screen::Help && _screen != Screen::Connect) {
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
        case Screen::Settings:
            keySettings(code);
            break;
        case Screen::Help:
            if (code == KEY_ESC || code == KEY_ENTER || code == KEY_H) _screen = _backScreen;
            break;
        default:
            break;
    }
}

bool AppRoast::keyConnect(int code)
{
    if (code == KEY_D) {
        startLink(true);
        toast("Simulator: no roaster", 0xFFD54F, 3000);
        return true;
    }
    if (code == KEY_ENTER || code == KEY_R) {
        if (_blockedByKeyboardApp) {
            toast("BLE busy: restart the device", 0xFF5252, 3000);
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

bool AppRoast::keyPrep(int code, bool shift)
{
    switch (code) {
        case KEY_S:
            if (_phase == Phase::FanOn) {
                toast("Fan on: Enter charge, XX stop", 0xFFD54F);
            } else {
                startFan();
            }
            return true;
        case KEY_ENTER:
            charge();
            return true;
        case KEY_X:
            if (_phase == Phase::FanOn && confirm(KEY_X, "X again to stop the fan")) {
                stopMachine();
                _phase = Phase::Idle;
                toast("Stopped");
            }
            return true;
        case KEY_UP:
            adjustFan(shift ? 0.1f : 1.0f);
            return true;
        case KEY_DOWN:
            fanDown(shift ? 0.1f : 1.0f);
            return true;
        case KEY_C:
            reloadCurves();
            _listSel = 0;
            for (size_t i = 0; i < _curves.size(); i++)
                if (_hasCurve && _curves[i].name == _curve.name) _listSel = (int)i;
            _listTop = std::max(0, _listSel - 2);
            _screen  = Screen::Curves;
            return true;
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
                toast(".alog curves are read-only", 0xFFD54F);
                return true;
            }
            edLoad();
            _screen = Screen::Editor;
            return true;
        case KEY_W:
            openInput("Green weight g", "", true, "e.g. 200", [this](const std::string& v) {
                float g = strtof(v.c_str(), nullptr);
                if (g > 0 && g < 2000) _rules.greenG = g;
            });
            return true;
        case KEY_B:
            openInput("Bean name", _bean, false, "Stored in the log", [this](const std::string& v) { _bean = v; });
            return true;
        case KEY_O:
            _setSel = 0;
            _screen = Screen::Settings;
            return true;
        case KEY_TAB:
            _view ^= 1;
            return true;
        case KEY_D:  // practise without the machine
            if (_phase == Phase::Idle && _link && !_link->isSimulated() && confirm(KEY_D, "D again: use simulator")) {
                startLink(true);
                toast("Simulator: no roaster", 0xFFD54F, 3000);
            }
            return true;
        case KEY_R:
            if (_phase == Phase::Idle && _link && _link->isSimulated()) {
                if (_blockedByKeyboardApp) {
                    toast("BLE busy: restart the device", 0xFF5252, 3000);
                } else {
                    startLink(false);
                    _screen = Screen::Connect;
                }
            }
            return true;
        case KEY_ESC:
            if (_phase == Phase::FanOn) {
                toast("Fan is on: XX to stop it first", 0xFFD54F, 3000);
            } else {
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
            if (confirm(KEY_D, "Again to DROP")) drop("");
            return true;
        case KEY_UP:
            adjustFan(shift ? 0.1f : 1.0f);
            return true;
        case KEY_DOWN:
            fanDown(shift ? 0.1f : 1.0f);
            return true;
        case KEY_LEFT:
            adjustHeat(-5);
            return true;
        case KEY_RIGHT:
            adjustHeat(5);
            return true;
        case KEY_P:
            toggleManual();
            return true;
        case KEY_TAB:
            _view ^= 1;
            return true;
        default:
            return false;
    }
}

bool AppRoast::keyCool(int code)
{
    switch (code) {
        case KEY_X:
            if (confirm(KEY_X, "X again to stop")) {
                stopMachine();
                enterSummary();
            }
            return true;
        case KEY_ENTER:
            if (confirm(KEY_ENTER, "Fan on: Enter again ends log")) enterSummary();
            return true;
        case KEY_UP:
            adjustFan(1.0f);
            return true;
        case KEY_DOWN:
            fanDown(1.0f);
            return true;
        case KEY_W:
            openInput("Roasted weight g", "", true, "Can be entered later", [this](const std::string& v) {
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
        case KEY_W:
            openInput("Roasted weight g", "", true, "", [this](const std::string& v) {
                float g = strtof(v.c_str(), nullptr);
                if (g > 0) {
                    _session.setRoastedWeight(g);
                    std::string err;
                    _saveMsg = _session.finalize(&err) ? "Updated: roast/logs/" + _session.logId() : err;
                }
            });
            return true;
        case KEY_N:
            openInput("Note", "", false, "Stored in the log", [this](const std::string& v) {
                _session.setNote(v);
                std::string err;
                _saveMsg = _session.finalize(&err) ? "Updated: roast/logs/" + _session.logId() : err;
            });
            return true;
        case KEY_TAB:
            _view ^= 1;
            return true;
        case KEY_ENTER:
        case KEY_ESC:
            finishRoast();
            return true;
        default:
            return false;
    }
}

bool AppRoast::keyCurves(int code)
{
    int n = (int)_curves.size();
    switch (code) {
        case KEY_UP:
            if (_listSel > 0) _listSel--;
            break;
        case KEY_DOWN:
            if (_listSel < n - 1) _listSel++;
            break;
        case KEY_ENTER:
            selectCurve(_listSel);
            toast("Curve: " + _curve.name, 0x99FF00);
            _screen = Screen::Prep;
            return true;
        case KEY_N:  // no curve: manual heater
            _hasCurve = false;
            _curve    = Curve();
            toast("No curve: manual heat", 0xFFD54F);
            _screen = Screen::Prep;
            return true;
        case KEY_E:
            if (n > 0 && !_curves[_listSel].readOnly) {
                selectCurve(_listSel);
                edLoad();
                _screen = Screen::Editor;
            } else {
                toast(".alog curves are read-only", 0xFFD54F);
            }
            return true;
        case KEY_ESC:
            _screen = Screen::Prep;
            return true;
        default:
            return false;
    }
    if (_listSel < _listTop) _listTop = _listSel;
    if (_listSel > _listTop + 3) _listTop = _listSel - 3;
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
    // ---- editing a value ----
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
        if (code == KEY_ENTER || code == KEY_RIGHT || code == KEY_TAB) {
            if (_edStage == 1) {  // minutes done -> seconds
                _edMin   = _edBuf.empty() ? std::to_string(r.t / 60) : _edBuf;
                _edBuf   = "";
                _edStage = 2;
                return true;
            }
            if (_edStage == 2) {
                int m = atoi(_edMin.c_str());
                int sec = _edBuf.empty() ? r.t % 60 : atoi(_edBuf.c_str());
                _edStage = 0;
                if (sec > 59) {
                    toast("Seconds must be 0-59", 0xFF5252);
                    return true;
                }
                int t = m * 60 + sec;
                std::string why;
                if (!edCheckTime(_edRow, t, why)) {
                    toast(why, 0xFF5252, 2500);
                    return true;
                }
                r.t = t;
                if (_edRow == 6 && _ed[7].t < t) _ed[7].t = t;
                edApply();
                if (_edRow != 7) _edCol = 2;  // then the temperature
                return true;
            }
            if (_edStage == 3) {
                _edStage = 0;
                if (_edBuf.empty()) return true;
                float c = strtof(_edBuf.c_str(), nullptr);
                if (c < 0 || c > 300) {
                    toast("Temperature 0-300°C", 0xFF5252);
                    return true;
                }
                r.c = std::round(c * 10) / 10.0f;
                edApply();
                return true;
            }
        }
        return true;
    }
    // ---- browsing ----
    auto startTime = [&](const std::string& first) {
        if (_edRow == 0 || !r.on) return;
        _edStage = 1;
        _edBuf   = first;
    };
    auto startTemp = [&](const std::string& first) {
        if (_edRow == 7 || !r.on) return;
        _edStage = 3;
        _edBuf   = first;
    };
    if (digit(d)) {
        if (_edCol == 1) startTime(std::string(1, d));
        if (_edCol == 2) startTemp(std::string(1, d));
        return true;
    }
    switch (code) {
        case KEY_UP:
            if (_edRow > 0) _edRow--;
            break;
        case KEY_DOWN:
            if (_edRow < kEdRows - 1) _edRow++;
            break;
        case KEY_LEFT:
            if (_edCol > 0) _edCol--;
            break;
        case KEY_RIGHT:
            if (_edCol < 2) _edCol++;
            break;
        case KEY_SPACE:
        case KEY_ENTER:
            if (_edCol == 0 || code == KEY_SPACE) {
                if (_edRow == 6 || _edRow == 7) {
                    toast("Drop and Total are always on", 0xFFD54F);
                    break;
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
                        toast(std::string("Fix the time: ") + why, 0xFF5252, 2500);
                        r.on = true;  // enable so the time can be fixed right away
                        _edCol = 1;
                        startTime("");
                        break;
                    }
                }
                if (_edRow == 0 && !r.on && std::isnan(r.c)) r.c = _curve.at(0);
                r.on = !r.on;
                edApply();
            } else if (_edCol == 1) {
                startTime("");
            } else {
                startTemp("");
            }
            break;
        case KEY_BACKSPACE:  // temperature back to "not edited" (follows the preset)
            if (_edCol == 2 && r.on && _edRow > 0 && _edRow < 7) {
                r.c = NAN;
                edApply();
                toast("Temperature follows the preset", 0x4FC3F7);
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
            openInput("Batch size g", std::to_string(_curve.batchG), true, "", [this](const std::string& v) {
                _curve.batchG = atoi(v.c_str());
                _edDirty      = true;
            });
            break;
        case KEY_S: {
            if (!_sdOk) _sdOk = GetHAL().sdCardMount();
            if (!_sdOk) {
                toast("No SD: curve kept until reboot", 0xFFD54F, 3000);
                _hasCurve = _curve.valid();
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
                toast("Saved and selected", 0x99FF00);
                _screen = Screen::Prep;
            } else {
                toast(err, 0xFF5252, 3000);
            }
            break;
        }
        case KEY_TAB:
            _view ^= 1;
            break;
        case KEY_ESC:
            if (_edDirty && !confirm(KEY_ESC, "Unsaved: Esc again to use as is")) break;
            _hasCurve = _curve.valid();
            _screen   = Screen::Prep;
            _view     = 0;
            break;
        default:
            break;
    }
    return true;
}

bool AppRoast::keySettings(int code)
{
    struct Item {
        const char* name;
        float* f;
        int* i;
        float lo, hi;
    };
    Item items[] = {
        {"Dev min (s)", nullptr, &_rules.devMin, 20, 300},     {"Dev max (s)", nullptr, &_rules.devMax, 20, 400},
        {"FC rise min °C", &_rules.dtMin, nullptr, 0, 30},    {"FC rise max °C", &_rules.dtMax, nullptr, 0, 30},
        {"Drop-now temp", &_rules.maxDropBT, nullptr, 150, 250}, {"Safety drop temp", &_rules.safetyBT, nullptr, 150, 260},
        {"Auto-drop after end s", nullptr, &_rules.afterEnd, 0, 900}, {"RoR low alert", &_rules.rorLow, nullptr, 0, 30},
        {"Cold temp", &_rules.coldBT, nullptr, 20, 80},        {"Start fan %", &_rules.startFan, nullptr, 20, 100},
        {"FC heat-drop alert %", &_rules.heatDrop, nullptr, 1, 50},
        {"Curve offset °C", &_rules.curveOffset, nullptr, -30, 30},
    };
    int n = sizeof(items) / sizeof(items[0]);
    switch (code) {
        case KEY_UP:
            if (_setSel > 0) _setSel--;
            return true;
        case KEY_DOWN:
            if (_setSel < n - 1) _setSel++;
            return true;
        case KEY_ENTER: {
            Item it = items[_setSel];
            char b[16];
            if (it.f)
                snprintf(b, sizeof(b), "%.1f", *it.f);
            else
                snprintf(b, sizeof(b), "%d", *it.i);
            openInput(it.name, b, true, "", [this, it](const std::string& v) {
                float x = strtof(v.c_str(), nullptr);
                if (x < it.lo || x > it.hi) {
                    toast("Out of range", 0xFF5252);
                    return;
                }
                if (it.f)
                    *it.f = x;
                else
                    *it.i = (int)lroundf(x);
                _curve.offset = _rules.curveOffset;
                _rules.save();
            });
            return true;
        }
        case KEY_ESC:
            _rules.save();
            _screen = Screen::Prep;
            return true;
        default:
            return false;
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
    // Notes are played one by one with Speaker.tone(); the launcher's play_melody() renders the whole
    // melody into one large buffer, which runs out of heap while BLE is up.
    auto midi = [](int m) { return (int)lround(440.0 * pow(2.0, (m - 69) / 12.0)); };
    std::vector<std::pair<int, int>> notes;  // freq (0 = rest), ms
    if (g_debugQuiet) return;
    if (t == Tone::Alarm) {
        // Always audible, even when muted or in launcher quiet mode.
        if (GetHAL().speaker.getVolume() < 160) GetHAL().speaker.setVolume(160);
        notes = {{midi(93), 90}, {0, 90}, {midi(93), 90}, {0, 90}, {midi(93), 90}};
    } else {
        if (_muted || audio::is_quiet_mode()) return;
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
            default:
                break;
        }
    }
    _beepQueue.clear();
    for (auto& n : notes) _beepQueue.push_back(n);
    _beepNextMs = 0;
}

void AppRoast::pumpBeep()
{
    if (_beepQueue.empty()) return;
    uint32_t now = GetHAL().millis();
    if (now < _beepNextMs) return;
    auto n = _beepQueue.front();
    _beepQueue.pop_front();
    if (n.first > 0) GetHAL().speaker.tone((float)n.first, n.second, 1, true);
    _beepNextMs = now + n.second;
}
