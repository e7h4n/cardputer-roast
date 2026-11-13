// Headless sweep of the curve-follow settings on the simulator, replaying batch A (R0012): F at 8:01, drop at 8:56.
// Prints BT at drop, RoR after first crack and the heater, per setting.   build: see tools/preview/build_sweep.sh
#include <apps/app_roast/roast_control.h>
#include <apps/app_roast/roast_link.h>
#include <hal/hal.h>
#include <cmath>
#include <cstdio>
#include <deque>

static uint32_t g_ms = 1000;
static Hal g_hal;
FakeM5 M5;
Hal& GetHAL() { return g_hal; }
uint32_t Hal::millis() { return g_ms; }
int64_t esp_timer_get_time() { return (int64_t)g_ms * 1000; }
namespace roast {
bool g_debugShift = false;
RoasterLink* g_activeLink = nullptr;
bool g_debugQuiet = true;
void DebugStart() {}
void DebugPollMain() {}
RoasterLink* CreateBleLink() { return nullptr; }
}  // namespace roast
using namespace roast;

struct Out { float btFc30, rorFc30, rorMin, rorMax, btDrop, heatFc, heatMin; };

static Out run(const Curve& c, SvConfig cfg)
{
    srand(1);
    g_ms = 1000;
    RoasterLink* l = CreateSimLink();
    l->begin();
    for (int i = 0; i < 40; i++) { g_ms += 50; l->snapshot(); }
    l->command({{12, 530}, {13, 1}});
    for (int i = 0; i < 200; i++) { g_ms += 50; l->snapshot(); }  // 10 s fan only
    l->command({{9, (uint16_t)lroundf(ComputeSv(c, 0, NAN, false, cfg).sv * 10)}, {2, 0}, {15, 1}});
    std::deque<float> bts;
    Out o{0, 0, 1e9, -1e9, 0, 0, 1e9};
    float bt = 25;
    for (int t = 0; t <= 536; t++) {
        int start = FcFloorStart(c.tagTime(PointTag::FCs), t >= 481 ? 481 : -1, cfg.floorLead);
        bool in   = start >= 0 && t >= start;
        SvResult r = ComputeSv(c, t, bt, in, cfg);
        l->setTarget((uint16_t)lroundf(r.sv * 10));
        for (int i = 0; i < 20; i++) { g_ms += 50; l->snapshot(); }
        RegSnapshot s = l->snapshot();
        bt = s.bt();
        float heat = s.heat();
        bts.push_back(bt);
        if (bts.size() > 31) bts.pop_front();
        float ror = bts.size() > 30 ? (bts.back() - bts.front()) * 2 : NAN;  // degC/min over 30 s
        if (t == 481) o.heatFc = heat;
        if (t == 511) { o.btFc30 = bt; o.rorFc30 = ror; }
        if (t > 481 && !std::isnan(ror)) { o.rorMin = std::min(o.rorMin, ror); o.rorMax = std::max(o.rorMax, ror); o.heatMin = std::min(o.heatMin, heat); }
        o.btDrop = bt;
    }
    l->end();
    delete l;
    return o;
}

int main()
{
    Curve c;
    c.pts    = {{0, 50, PointTag::Charge}, {270, 150, PointTag::Dry}, {450, 199, PointTag::FCs}, {600, 210, PointTag::Drop}};
    c.totalT = 600;
    c.build();
    printf("curve at FC 8:01 %.1f, at drop 8:56 %.1f\n", c.at(481), c.at(536));
    printf("%-34s %8s %8s %8s %8s %8s %8s\n", "setting", "BT drop", "RoR+30", "RoR min", "RoR max", "heat@FC", "heat min");
    struct V { const char* name; int la; bool floor; float delta, cap; } vs[] = {
        {"before: L0, floor off", 0, false, 1, 8},     {"L8, floor off", 8, false, 1, 8},
        {"L8, floor d1 cap8 (requested)", 8, true, 1, 8}, {"L8, floor d1 cap5", 8, true, 1, 5},
        {"L8, floor d1 cap3", 8, true, 1, 3},          {"L8, floor d1 cap2", 8, true, 1, 2},
        {"L8, floor d0.5 cap3", 8, true, 0.5f, 3},     {"L8, floor d0 cap3", 8, true, 0, 3},
    };
    for (auto& v : vs) {
        SvConfig cfg;
        cfg.lookahead = v.la; cfg.fcFloor = v.floor; cfg.floorDelta = v.delta; cfg.floorCap = v.cap;
        Out o = run(c, cfg);
        printf("%-34s %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f\n", v.name, o.btDrop, o.rorFc30, o.rorMin, o.rorMax, o.heatFc, o.heatMin);
    }
}
