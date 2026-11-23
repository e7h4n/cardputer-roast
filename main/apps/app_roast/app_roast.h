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
    float safetyBT   = 225.f; // app drops by itself above this
    int afterEnd     = 120;   // app drops by itself this long after the curve ends
    float rorLow     = 5.0f;  // RoR warning 30 s after first crack
    float coldBT     = 40.f;  // charge / stop threshold
    float startFan   = 53.0f; // fan when pressing S
    float greenG     = 200.f; // default batch
    float curveOffset = 0.0f; // shift the whole curve (myERoast tempCompensation), -30..30
    int svLookahead  = 8;     // the feedforward aims at curve(t + this), 0..20 s (roast_control.h PidAim)
    float floorCap   = 8.0f;  // after F on a curve without FC/DROP points: shift to BT by at most this, 1..8 degC
    float pidKp      = 1.5f;  // % heat per degC, 0.1..10 (roast_control.h PidConfig)
    int pidTi        = 60;    // integral time, s (0: no integral)
    int pidTd        = 0;     // derivative time before F, s
    float pidFcKp    = 4.0f;  // % heat per degC from F on, 0.1..10
    int pidFcTd      = 3;     // derivative time from F on, s (rate-of-rise shortfall against the development speed)
    bool pidFF       = true;  // model feedforward
    bool pidObs      = true;  // self-heating estimate in the feedforward near first crack (roast_control.h Observer)
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
    enum class Screen : uint8_t { Storage, Connect, Prep, Roast, Cool, Summary, Curves, Editor, Settings, Help, Network, Sync };
    enum class Phase : uint8_t { Idle, FanOn, Roasting, Cooling, Done };

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
        bool* toggle   = nullptr;  // on/off row
        bool network   = false;    // opens the Wi-Fi & Dropbox setup screen
        bool simToggle = false;    // simulator on/off
    };
    std::vector<RuleRow> ruleRows();

    // ---- core
    std::unique_ptr<roast::RoasterLink> _link;
    roast::RegSnapshot _snap;
    roast::LinkState _linkState = roast::LinkState::Off;
    roast::LinkState _prevLinkState = roast::LinkState::Off;
    RoastRules _rules;
    roast::Session _session;
    std::vector<roast::CurveEntry> _curves;
    roast::Curve _curve;  // working copy of the selected curve
    bool _hasCurve = false;
    std::string _bean;

    Screen _screen     = Screen::Connect;
    Screen _backScreen = Screen::Prep;
    Phase _phase       = Phase::Idle;
    int _view          = 0;  // 0 numbers, 1 chart
    bool _fanRunning   = false;  // machine fan on (S/charge) until X X stop, independent of the log

    // ---- roast timing / control
    uint32_t _chargeMs   = 0;
    int _lastSampleT     = -1;
    uint32_t _dropMs     = 0;
    float _fanSet        = 0;
    uint32_t _fanSetMs   = 0;
    float _lastSv        = NAN;
    roast::PidConfig pidConfig() const;
    roast::PidState _pid;
    int _lastHeatRaw   = -1;
    uint32_t _pidMs    = 0;
    uint32_t _heatSentMs = 0;
    float _fcOffset      = 0;   // curve shift from the F mark on, for curves without a development plan
    roast::DevTrack _dev;           // development from the F mark (curve with first crack and drop points)
    void fcAnchor(bool marked);
    void pidTick(const roast::PidAim& aim);
    roast::PidAim pidAim(int t) const;

    // ---- alerts
    int _satRun          = 0;
    bool _satWarned      = false;
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
    int _listSel = 0, _listTop = 0;
    int _setSel  = 0;
    // curve editor: Start (temperature), first crack and drop (time and temperature); roast_curve.cpp builds the rest
    struct EdRow {
        bool on = false;
        int t   = -1;      // seconds
        float c = NAN;     // NAN = temperature not edited (node only pins the curve)
    };
    static constexpr int kEdRows = 3;
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
    Screen _setBack  = Screen::Prep;  // where Esc on the rules page returns
    std::string _netSummary;          // rules page value: saved network and Dropbox state
    void openNetwork();
    void closeNetwork();

    // ---- Dropbox sync (roast_sync.h): runs with the roaster link closed, then Roast restarts
    bool _netRequired = false;  // Wi-Fi or Dropbox not set up yet: setup is the only way forward
    // The roast is saved to Dropbox from the summary, in the background, once the fan is off (the link closes then).
    uint32_t _saveAt   = 0;      // when to start (lets the fan-stop command reach the roaster first); 0 = not planned
    bool _exportReady = false;  // complete local files available for upload
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

    // ---- logic (app_roast.cpp)
    void startLink(bool sim = false);
    void setSimulator(bool on);
    bool _netWasSim = false;  // reopen the simulator rather than BLE when network setup closes
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
    bool confirm(int key, const std::string& msg, bool quiet = false);
    bool confirmPending(int key) const;

    bool selectCurve(int idx);
    static constexpr int kCurveRows = 5;
    void rememberCurve();
    void startWithCard();
    void loadSelectedCurve();
    void startFan();
    void charge();
    void drop(const char* why);
    void stopMachine();
    void adjustFan(float delta);
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
    void openInput(const std::string& title, const std::string& init, bool numeric, const std::string& hint,
                   std::function<void(const std::string&)> done);

    // ---- rendering (app_roast_ui.cpp)
    void render();
    void drawFrame();
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

    // ---- external screen (hal/ext_display.h, 320x240): the chart while roasting and on the summary, the selected
    // curve otherwise. No room for a 150 KB frame: each frame is drawn in 320x16 strips, one strip per loop pass.
    LGFX_Sprite _extStrip;
    int _extRow         = -1;  // next strip of the frame being drawn; -1: no frame in progress
    uint32_t _extKey    = 0;   // what the screen shows now (externalKey())
    uint32_t _extDrawMs = 0;   // when the last frame started
    bool _extVisible = false;
    uint32_t externalKey();
    void pumpExternal();
    void renderExternalPlan(int y);
    void renderExternal(int top);  // the whole frame, shifted up by `top` (the strip's first row)
    void blankExternal();
    void releaseExternal();        // free the strip (before Wi-Fi + TLS); the next frame allocates it again
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
