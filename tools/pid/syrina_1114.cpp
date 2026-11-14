// The 2026-11-14 Syrina D roast (100 g), replayed: the plant fitted on that roast alone (its beans heat themselves
// harder than the model from R0012-R0014 says), the curve as roasted (FC 7:05 / 200.1, 45 s at 11.2 degC/min, at
// most 208.5), F when BT reaches 202.9 as it did (7:09). The firmware that roasted it (PR #5 at 3b08be9: model
// without self-heating, hold before F) against this one. Real roast: drop at 24 s by temperature (209.1), RoR after F
// 15.5 on average, 17 at the drop.
#include <apps/app_roast/roast_control.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

using namespace roast;

struct Plant {  // least squares on 20261114-020724.csv up to the drop: RMS 0.75 degC, 0.47 from 6:00
    double gain = 2.1405, rate = 0.0211, extra = 0.00619, from = 150, bt = 25.8, air = 26;
    int dead = 5;
    std::vector<double> hist;
    void step(double heat)
    {
        hist.push_back(heat);
        double late = hist.size() > (size_t)dead ? hist[hist.size() - 1 - dead] : 0;
        air += (26 + gain * late - air) * (1 - std::exp(-1.0));
        bt += rate * (air - bt) + (bt > from ? extra * (bt - from) : 0);
    }
};

struct Out {
    int f = -1, drop = -1;
    const char* why = "";
    float btF = 0, btDrop = 0, aheadAtF = 0, ror6 = 0, rorDev = 0, rorMax = 0, heatF = 0, heatDrop = 0;
};

static Out run(bool firmware3b08, float fcTd)
{
    srand(1);
    Curve c;
    c.pts = {{0, 50, PointTag::Charge}, {425, 200.1f, PointTag::FCs}, {470, 208.5f, PointTag::Drop}};
    c.build();
    DevPlan plan = c.devPlan();
    PidConfig pc;
    pc.fcTd = fcTd;
    if (firmware3b08) {  // the model before the self-heating term
        pc.model.gain  = 2.2f;
        pc.model.rate  = 0.0175f;
        pc.model.extra = 0;
    }
    PidState ps;
    PidReset(ps, 0);
    Plant pl;
    DevTrack dev;
    std::deque<double> bts;
    Out o;
    int win = FcFloorStart(c.tagTime(PointTag::FCs), -1, 20);
    for (int t = 0; t < 900; t++) {
        float bt = std::round((pl.bt + ((rand() % 100) - 50) / 2000.0) * 10) / 10;
        if (o.f < 0 && bt >= 202.9f) {
            o.f        = t;
            o.btF      = bt;
            o.aheadAtF = bt - c.at(t);
            dev        = DevStart(c, t, bt);
            PidAim a;
            a.target = dev.target(t);
            a.ffTemp = dev.target(t + 8);
            a.ffRate = dev.rate(t + 8);
            a.rate   = dev.rate(t);
            a.afterF = true;
            PidPrime(ps, ps.out, a, bt, pc);
            o.heatF = ps.out;
        }
        if (o.f >= 0) {
            DropWhy w = DevDropDue(dev, plan, t, bt);
            if (w != DropWhy::None) {
                o.drop     = t;
                o.why      = w == DropWhy::Time ? "time" : "temp";
                o.btDrop   = bt;
                o.heatDrop = ps.out;
                break;
            }
        }
        PidAim a;
        if (dev.active) {
            a.target = dev.target(t);
            a.ffTemp = dev.target(t + 8);
            a.ffRate = dev.rate(t + 8);
            a.rate   = dev.rate(t);
            a.afterF = true;
        } else {
            a = CardputerAim(c, t, bt, win, false, 0, 8, 8);
            if (firmware3b08 && t >= win && bt > a.target) a.target = std::min(bt, a.target + 8.0f);  // its hold
        }
        float heat = std::round(PidStep(ps, a, bt, 1.0f, pc).heat * 10) / 10;
        pl.step(heat);
        bts.push_back(pl.bt);
        if (bts.size() > 31) bts.pop_front();
        double ror = bts.size() > 30 ? (bts.back() - bts.front()) * 2 : 0;
        if (t == 360) o.ror6 = (float)ror;
        if (o.f >= 0) o.rorMax = std::max(o.rorMax, (float)ror);
    }
    if (o.drop > o.f) o.rorDev = (o.btDrop - o.btF) / (o.drop - o.f) * 60;
    return o;
}

int main()
{
    printf("2026-11-14 Syrina D replayed (plan: 6:00 RoR 15.0; after F 11.2 degC/min for 45 s, at most 208.5)\n");
    printf("%-40s %5s %6s %7s | %5s %-4s %4s %6s | %7s %7s | %6s %6s %6s\n", "firmware", "F", "BT F", "ahead", "drop", "why",
           "dev", "BT", "RoR dev", "RoR max", "RoR6", "heatF", "heatD");
    struct V { const char* name; bool old; float td; } vs[] = {
        {"PR #5 as roasted (3b08be9)", true, 0},
        {"this: self-heating model, no hold", false, 0},
        {"this + D after F 10 s", false, 10},
        {"this + D after F 20 s", false, 20},
    };
    for (auto& v : vs) {
        Out o = run(v.old, v.td);
        printf("%-40s %5s %6.1f %+7.1f | %5s %-4s %4d %6.1f | %7.1f %7.1f | %6.1f %6.1f %6.1f\n", v.name, FormatMmSs(o.f).c_str(),
               o.btF, o.aheadAtF, FormatMmSs(o.drop).c_str(), o.why, o.drop - o.f, o.btDrop, o.rorDev, o.rorMax, o.ror6, o.heatF,
               o.heatDrop);
    }
    printf("\nahead: BT - curve at F. RoR dev: mean rate from F to the drop. RoR max: largest 30 s RoR after F. RoR6: at 6:00.\n");
    return 0;
}
