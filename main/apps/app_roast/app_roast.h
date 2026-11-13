/*
 * Roast: drive the E-Roast fluid-bed roaster from the Cardputer ADV over BLE.
 */
#pragma once
#include <mooncake.h>
#include <hal/hal.h>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include "roast_link.h"
#include "roast_curve.h"
#include "roast_session.h"
#include "roast_control.h"
#include "roast_sync.h"
#include "roast_theme.h"

struct RoastRules {
    int devMin       = 75;    // s after first crack: drop window opens
    int devMax       = 85;    // s after first crack: drop now
    float dtMin      = 5.0f;  // degC rise since first crack to open the window
    float dtMax      = 6.0f;  // degC rise that already counts as enough
    float maxDropBT  = 206.f; // drop immediately at this bean temperature
    float safetyBT   = 225.f; // app drops by itself above this
    int afterEnd     = 120;   // app drops by itself this long after the curve ends
    float rorLow     = 5.0f;  // RoR warning 30 s after first crack
    float coldBT     = 40.f;  // charge / stop threshold
    float startFan   = 53.0f; // fan when pressing S
    float greenG     = 200.f; // default batch
    float heatDrop   = 5.0f;  // heater drop around first crack that triggers a hint
    float curveOffset = 0.0f; // shift the whole curve (myERoast tempCompensation), -30..30
    int micGain      = 18;    // microphone PGA gain, 0..30 dB in 3 dB steps
    int svLookahead  = 8;     // SV = curve(t + this), 0..20 s (roast_control.h)
    bool fcFloor     = true;  // around first crack, SV >= BT + floorDelta (capped at curve + 8)
    float floorDelta = 1.0f;  // 0..5 degC
    float floorCap   = 8.0f;  // the floor never exceeds curve(t) + this, 1..8 degC
    bool cardPid     = true;  // the Cardputer computes the heater output; off: the roaster's own PID follows SV
    float pidKp      = 1.5f;  // % heat per degC, 0.1..10 (roast_control.h PidConfig)
    int pidTi        = 60;    // integral time, s (0: no integral)
    int pidTd        = 0;     // derivative time, s
    bool pidFF       = true;  // model feedforward
    void load();
    void save() const;
};

class AppRoast : public mooncake::AppAbility {
public:
    AppRoast();
    ~AppRoast();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    enum class Screen : uint8_t { Connect, Prep, Roast, Cool, Summary, Curves, Editor, Settings, Help, Network, Sync };
    enum class Phase : uint8_t { Idle, FanOn, Roasting, Cooling, Done };
    enum class Tone : uint8_t { Key, Info, Good, Warn };

    struct Input {
        bool active = false;
        bool numeric = true;
        std::string title;
        std::string hint;
        std::string buf;
        std::function<void(const std::string&)> done;
    };
    // W/B: one sheet for the batch (green weight + bean name)
    struct BatchSheet {
        bool active = false;
        int field   = 0;  // 0 weight, 1 bean
        std::string weight, bean;
    };
    // O: rules list. Rows are built on demand so they always point at the live values.
    struct RuleRow {
        const char* label = nullptr;  // nullptr: group header
        const char* group = nullptr;
        uint32_t groupColor = 0;
        float* f[2]  = {nullptr, nullptr};  // float value(s); second = upper end of a range
        int* i[2]    = {nullptr, nullptr};  // int value(s)
        float lo = 0, hi = 0, step = 1;
        const char* unit = "";
        bool simToggle = false;
        bool* toggle   = nullptr;  // on/off row
        bool network   = false;    // opens the Wi-Fi & Dropbox setup screen
    };
    std::vector<RuleRow> ruleRows();

    // ---- core
    std::unique_ptr<roast::RoasterLink> _link;
    roast::RegSnapshot _snap;
    roast::LinkState _linkState = roast::LinkState::Off;
    roast::LinkState _prevLinkState = roast::LinkState::Off;
    RoastRules _rules;
    roast::Session _session;
    std::vector<roast::Curve> _curves;
    roast::Curve _curve;  // working copy of the selected curve
    bool _hasCurve = false;
    bool _sdOk     = false;
    std::string _bean;

    Screen _screen     = Screen::Connect;
    Screen _backScreen = Screen::Prep;
    Phase _phase       = Phase::Idle;
    int _view          = 0;  // 0 numbers, 1 chart
    bool _muted        = false;
    bool _blockedByKeyboardApp = false;
    bool _fanRunning   = false;  // machine fan on (S/charge) until X X stop, independent of the log
    bool _noCurve      = false;  // "No curve" picked explicitly: manual heat

    // ---- roast timing / control
    uint32_t _chargeMs   = 0;
    int _lastSampleT     = -1;
    uint32_t _dropMs     = 0;
    bool _manual         = false;  // PID off: heater set by hand
    float _fanSet        = 0;
    uint32_t _fanSetMs   = 0;
    float _heatSet       = 0;
    int _lastTargetRaw   = -1;
    bool _svFloored      = false;  // last SV came from the first-crack floor
    bool _floorLogged    = false;  // the floor's first use is in the log
    float _lastSv        = NAN;
    roast::SvConfig svConfig() const;
    roast::PidConfig pidConfig() const;
    roast::PidState _pid;
    bool _cardPid      = false;  // this roast: heater from the Cardputer PID (decided at charge)
    int _lastHeatRaw   = -1;
    uint32_t _pidMs    = 0;
    void pidTick(const roast::PidAim& aim);
    roast::PidAim pidAim(int t) const;
    roast::SvResult svAt(int t) const;  // what to write as SV at curve second t (lookahead, first-crack floor)

    // ---- alerts
    int _satRun          = 0;
    bool _satWarned      = false;
    bool _heatDropWarned = false;
    bool _rorWarned      = false;
    bool _autoDropped    = false;
    std::string _autoDropWhy;

    // ---- ui helpers
    std::deque<Keyboard::KeyEvent_t> _keys;
    int _keySlot         = -1;
    uint32_t _lastRender = 0;
    bool _dirty          = true;
    std::string _toast;
    uint32_t _toastUntil = 0;
    uint32_t _toastColor = 0xFFFFFF;
    int _batPct          = -1;  // 0..100, -1 = unknown
    uint32_t _batMs      = 0;
    int _pendingKey      = 0;
    uint32_t _pendingUntil = 0;
    Input _input;
    BatchSheet _batch;
    bool _setEditing = false;
    int _setStage    = 0;      // range rows: 0 lower end, 1 upper end
    std::string _setBuf;       // digits typed while editing
    float _setVal[2] = {0, 0};
    // ←/→ held in manual mode repeats the heater step
    int _heatHeld        = 0;
    uint32_t _heatHeldMs = 0;
    uint32_t _heatRepMs  = 0;
    bool _heatHeldShift  = false;
    int _heatReps        = 0;
    int _listSel = 0, _listTop = 0;
    int _setSel  = 0;
    // curve editor: fixed nodes like the phone editor (Start, Dry, FC start, FC end, SC start, SC end, Drop, Total)
    struct EdRow {
        bool on = false;
        int t   = -1;      // seconds
        float c = NAN;     // NAN = temperature not edited (node only pins the curve)
    };
    static constexpr int kEdRows = 8;
    EdRow _ed[kEdRows];
    int _edRow = 0, _edCol = 0;  // col 0 enabled, 1 time, 2 temperature
    int _edStage = 0;            // 0 browse, 1 minutes, 2 seconds, 3 temperature
    bool _edDirty = false;
    std::string _edBuf, _edMin;
    void edLoad();
    void edApply();
    bool edCheckTime(int row, int t, std::string& why);

    // fan floor protection: crossing 50 % and 40 % downwards needs a long press of ↓
    bool _downHeld          = false;
    uint32_t _downPressMs   = 0;
    float _barrierAt        = -1;
    float _barrierStep      = 1.0f;
    void fanDown(float step);
    void pollFanBarrier();
    std::string _saveMsg;

    // ---- Wi-Fi & Dropbox setup (net/roast_net.h): pairs with the Coffee Watch setup page over Bluetooth
    bool _netOpen    = false;  // setup running: the roaster link is closed meanwhile
    bool _netWasSim  = false;  // reopen the simulator rather than BLE when setup closes
    Screen _setBack  = Screen::Prep;  // where Esc on the rules page returns
    std::string _netSummary;          // rules page value: saved network and Dropbox state
    void openNetwork();
    void closeNetwork();

    // ---- Dropbox sync (roast_sync.h): runs with the roaster link closed, then Roast restarts
    bool _netRequired = false;  // Wi-Fi or Dropbox not set up yet: setup is the only way forward
    // The roast is saved to Dropbox from the summary, in the background, once the fan is off (the link closes then).
    uint32_t _saveAt   = 0;      // when to start (lets the fan-stop command reach the roaster first); 0 = not planned
    bool _saveStarted  = false;
    bool _saveReported = false;  // the outcome was toasted
    void planSave();
    void pollSave();
    void resaveEdited();         // the summary's edits rewrote the .json and .alog
    bool saveBusy();             // the roast on the summary is not in Dropbox yet (and has not failed)
    bool syncingNow();           // a Dropbox job is moving data: the status corner says "Syncing" instead of battery
    bool saveFailed();           // the summary's roast save ended with an error (status corner: "Sync failed")
    bool startJob(roast::sync::Job job, const std::string& arg);
    void pollSync();
    bool keySync(int code);
    void renderSync();
    void restartApp(bool skipBootSync);
    bool consumeSkipSync();
    bool consumeBypass();  // esp_restart(); boot sync runs again unless skipBootSync
    bool networkIdle(const char* what);  // fan off and nothing running: a network job may close the link
    void refreshNetSummary();
    bool keyNetwork(int code);
    void renderNetwork();
    void openSettings();

    // ---- audio recording (roast_audio.h): charge -> end of the log
    bool _recording      = false;
    uint32_t _recStartMs = 0;
    int32_t _markMs[(int)roast::Mark::Count];  // ms after charge, -1 = not set (for the recording's event times)
    void startRecording();
    void stopRecording();
    void pollRecording();

    // ---- logic (app_roast.cpp)
    void startLink(bool sim);
    void pollLink();
    void handleKeys();
    void onKey(const Keyboard::KeyEvent_t& e);
    bool keyConnect(int code);
    bool keyPrep(int code, bool shift);
    bool keyRoast(int code, bool shift);
    bool keyCool(int code);
    bool keySummary(int code);
    bool keyCurves(int code);
    bool keyEditor(const Keyboard::KeyEvent_t& e, int code);
    bool keySettings(const Keyboard::KeyEvent_t& e, int code);
    void keyInput(const Keyboard::KeyEvent_t& e, int code);
    void keyBatch(const Keyboard::KeyEvent_t& e);
    void openBatch(int field);
    void setSimulator(bool on);
    bool confirm(int key, const std::string& msg, bool quiet = false);
    bool confirmPending(int key) const;

    void selectCurve(int idx);
    void reloadCurves();
    void startFan();
    void charge();
    void drop(const char* why);
    void stopMachine();
    void adjustFan(float delta);
    void adjustHeat(float delta);
    void toggleManual();
    void toggleMark(roast::Mark m);
    void tickRoast();
    void finishRoast();
    void enterSummary();

    int roastT() const;
    int curveT() const;  // machine timer (reg 2) like the phone app, local clock as fallback
    float curveTargetAt(int t) const;
    bool linkReady() const
    {
        return _linkState == roast::LinkState::Ready;
    }
    void toast(const std::string& msg, uint32_t color = 0xFFFFFF, uint32_t ms = 2200);
    /// From charge until the log ends the speaker stays off: no key clicks, chimes, warnings or alarms. Alerts are
    /// shown on screen only, and the microphone can record (it shares the codec and its clocks with the speaker).
    bool quietRoast() const
    {
        return _phase == Phase::Roasting || _phase == Phase::Cooling;
    }
    void beep(Tone t);
    void pumpBeep();
    std::deque<std::pair<int, int>> _beepQueue;
    uint32_t _beepNextMs = 0;
    void openInput(const std::string& title, const std::string& init, bool numeric, const std::string& hint,
                   std::function<void(const std::string&)> done);

    // ---- rendering (app_roast_ui.cpp)
    void render();
    void renderConnect();
    void renderPrep();
    void renderRoast();
    void renderDash();
    void renderChart(int x, int y, int w, int h, bool withText);
    void renderCool();
    void renderSummary();
    void renderCurves();
    void renderEditor();
    void renderSettings();
    void renderHelp();
    void renderInput();
    void renderBanner();
    void renderToast();
    int renderStatus(bool draw = true);  // returns the left edge of the status corner
    int statusLeft()
    {
        return renderStatus(false);
    }
    void renderBatch();
    void renderKeysBar();
    bool renderAlertBar();
    bool bannerText(std::string& text, uint32_t& bg, uint32_t& fg);
};
