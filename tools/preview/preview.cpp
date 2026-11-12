// Desktop preview: runs the real Roast app (app_roast*.cpp, curves, session, simulator) against a fake HAL and a
// fake clock, drives it with scripted key presses through a whole roast, and writes every page as a 240x135 PNG.
//
//   tools/preview/build.sh && build_preview/preview build_preview/shots
//
// The "BLE" link here is the built-in thermal simulator under the real roaster's name, so pages look like the
// real thing; nothing talks to hardware.
#include <apps/app_roast/app_roast.h>
#include <apps/app_roast/roast_debug.h>
#include <apps/app_roast/roast_link.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <sys/stat.h>

// ------------------------------------------------------------------ fake platform
static uint32_t g_ms = 1000;
static Hal g_hal;
FakeM5 M5;
Hal& GetHAL() { return g_hal; }
uint32_t Hal::millis() { return g_ms; }
int64_t esp_timer_get_time() { return (int64_t)g_ms * 1000; }

namespace roast {
bool g_debugShift          = false;
RoasterLink* g_activeLink  = nullptr;
bool g_debugQuiet          = true;
void DebugStart() {}
void DebugPollMain() {}
}  // namespace roast

static bool g_bleLost        = false;
static uint32_t g_bleReadyIn = 2500;

struct PreviewBle : roast::RoasterLink {
    std::unique_ptr<roast::RoasterLink> sim{roast::CreateSimLink()};
    uint32_t readyAt = 0;
    roast::RegSnapshot last;
    bool begin() override
    {
        readyAt = g_ms + g_bleReadyIn;
        return sim->begin();
    }
    void end() override { sim->end(); }
    roast::LinkState state() override
    {
        if (g_ms < readyAt) return roast::LinkState::Scanning;
        return g_bleLost ? roast::LinkState::Lost : roast::LinkState::Ready;
    }
    std::string deviceName() override { return "E-Meter621 BLE"; }
    std::string lastError() override { return ""; }
    std::vector<roast::ScanEntry> scanResults() override { return {{"E-Meter621 BLE", -58}}; }
    bool isSimulated() const override { return false; }
    roast::RegSnapshot snapshot() override
    {
        if (g_bleLost) return last;  // frozen at the moment the link dropped
        last          = sim->snapshot();
        last.lastRxMs = g_ms;
        return last;
    }
    uint32_t command(const std::vector<roast::RegWrite>& w) override { return sim->command(w); }
    roast::TxStatus txStatus(uint32_t id) override { return sim->txStatus(id); }
    void setTarget(uint16_t raw) override { sim->setTarget(raw); }
    void clearQueue() override { sim->clearQueue(); }
};
roast::RoasterLink* roast::CreateBleLink() { return new PreviewBle(); }

// ------------------------------------------------------------------ driving the app
static AppRoast* g_app = nullptr;
static std::string g_out;
static int g_shot = 0;

static void step(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 50) {
        g_ms += 50;
        g_app->onRunning();
    }
}

struct KeyDef {
    const char* n;
    KeScanCode_t code;
    const char* name;
};
static const KeyDef kKeys[] = {
    {"enter", KEY_ENTER, ""}, {"esc", KEY_ESC, ""},     {"tab", KEY_TAB, ""},       {"space", KEY_SPACE, " "},
    {"bksp", KEY_BACKSPACE, ""}, {"up", KEY_UP, ""},    {"down", KEY_DOWN, ""},     {"left", KEY_LEFT, ""},
    {"right", KEY_RIGHT, ""}, {".", KEY_DOT, "."},      {"-", KEY_MINUS, "-"},
};
static const char* kLetters[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                                 "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
static const char* kDigits[]  = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};

static Keyboard::KeyEvent_t ev(const std::string& k, bool state)
{
    Keyboard::KeyEvent_t e;
    e.state = state;
    for (auto& d : kKeys)
        if (k == d.n) {
            e.keyCode = d.code;
            e.keyName = d.name;
            return e;
        }
    if (k.size() == 1 && k[0] >= 'a' && k[0] <= 'z') {
        e.keyCode = (KeScanCode_t)(KEY_A + (k[0] - 'a'));
        e.keyName = kLetters[k[0] - 'a'];
    } else if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') {
        e.keyCode = k[0] == '0' ? KEY_0 : (KeScanCode_t)(KEY_1 + (k[0] - '1'));
        e.keyName = kDigits[k[0] - '0'];
    } else {
        fprintf(stderr, "unknown key %s\n", k.c_str());
        exit(2);
    }
    return e;
}

static void press(const std::string& k, uint32_t after = 120)
{
    GetHAL().keyboard.onKeyEvent.emit(ev(k, true));
    step(50);
    GetHAL().keyboard.onKeyEvent.emit(ev(k, false));
    step(after);
}

static void type(const std::string& s)
{
    for (char c : s) press(c == ' ' ? std::string("space") : std::string(1, c), 60);
}

static void shot(const char* name)
{
    step(260);  // the app renders every 250 ms
    size_t len = 0;
    void* png  = GetHAL().canvas.createPng(&len, 0, 0, 240, 135);
    char path[512];
    snprintf(path, sizeof(path), "%s/%02d_%s.png", g_out.c_str(), ++g_shot, name);
    FILE* f = fopen(path, "wb");
    if (!f || !png) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fwrite(png, 1, len, f);
    fclose(f);
    free(png);
    printf("%s\n", path);
}

// advance the simulated roast one second at a time until cond() or the timeout
static void until(const std::function<bool()>& cond, int maxSec)
{
    for (int i = 0; i < maxSec && !cond(); i++) step(1000);
}

static float bt()
{
    auto* l = roast::g_activeLink;
    return l ? l->snapshot().bt() : 0;
}

int main(int argc, char** argv)
{
    g_out = argc > 1 ? argv[1] : "shots";
    mkdir(g_out.c_str(), 0755);
#ifdef ROAST_SD_ROOT
    mkdir(ROAST_SD_ROOT, 0755);
#endif
    auto& cv = GetHAL().canvas;
    cv.setColorDepth(16);
    if (!cv.createSprite(240, 135)) return 1;

    AppRoast app;
    g_app = &app;
    app.onOpen();
    step(600);
    shot("connect");
    step(2500);  // link comes up
    shot("prep");

    press("w");
    type("0");
    press("tab");
    type("guji g1");
    shot("batch_sheet");
    press("enter");
    step(2500);  // let the toast fade so the next pages are clean

    press("c");
    shot("curves");
    press("up");
    press("enter");
    shot("prep_no_curve");
    press("c");
    press("down");
    press("enter");
    step(2500);

    press("e");
    shot("editor");
    press("enter");
    type("4");
    shot("editor_minutes");
    press("esc");
    press("tab");
    shot("editor_preview");
    press("tab");
    press("esc");

    press("o");
    press("down");
    press("down");
    shot("rules");
    press("enter");
    press("right");
    shot("rules_edit");
    press("esc");
    for (int i = 0; i < 12; i++) press("down", 60);
    shot("rules_bottom");
    press("esc");

    press("h");
    shot("keys");
    press("esc");

    // Wi-Fi & Dropbox setup: the rules row, then a whole (simulated) pairing
    press("o");
    for (int i = 0; i < 12; i++) press("down", 60);
    shot("rules_sync");
    press("enter");
    step(600);
    shot("network_code");
    step(500);
    shot("network_pairing");
    step(2700);
    shot("network_wifi");
    step(1200);
    shot("network_dropbox_busy");
    step(3000);
    shot("network_done");
    press("esc");
    step(3000);  // back on Connect, then Prep once the (preview) roaster answers
    press("o");
    for (int i = 0; i < 12; i++) press("down", 60);
    shot("rules_sync_saved");
    press("esc");

    press("s");
    shot("fan_on");
    step(4000);
    press("enter");  // charge
    step(3000);
    shot("roast_start");

    until([] { return bt() >= 150; }, 400);
    press("y");
    shot("roast_dry_marked");
    step(60 * 1000);
    shot("roast_maillard");
    press("tab");
    shot("roast_chart");
    press("tab");

    press("p");
    step(400);
    GetHAL().keyboard.onKeyEvent.emit(ev("right", true));  // hold → repeats
    step(1200);
    GetHAL().keyboard.onKeyEvent.emit(ev("right", false));
    shot("roast_manual");
    press("p");

    until([] { return bt() >= 198; }, 400);
    press("f");
    shot("roast_fc");
    step(20 * 1000);
    shot("roast_dev");
    press("d");
    shot("roast_confirm_drop");
    step(2600);
    until([] { return false; }, 52);  // ~75 s after first crack
    shot("roast_window");
    g_bleLost = true;
    step(5000);
    shot("roast_ble_lost");
    g_bleLost = false;
    step(2000);

    press("d", 50);
    press("d");
    step(1500);
    shot("cool");
    step(60 * 1000);
    shot("cool_later");
    press("tab");
    shot("cool_chart");
    press("tab");
    press("enter");
    shot("summary_fan_on");
    press("x");
    shot("summary_confirm_stop");
    press("x");
    shot("summary");
    press("tab");
    shot("summary_chart");
    press("tab");
    press("enter");
    shot("prep_after");

    press("o");
    for (int i = 0; i < 20; i++) press("down", 60);
    press("enter");
    shot("prep_simulator");
    press("esc");
    shot("exit_confirm");
    return 0;
}
