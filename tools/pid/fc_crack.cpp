// First crack and development on Syrina D 100g, as planned for the competition: first crack 7:05 / 200.1, drop
// 45 s later at most 208.5 (curve FCs 425/200.1, Drop 470/208.5).
//   main:  the curve as the myERoast-style builder made it (syrina_myeroast_build.csv, a -4.5 degC/min corner at
//          first crack), the Cardputer PID holding through the crack, drop by hand at F + 45 s;
//   new:   the smooth two-point curve; from F the development speed (11.2 degC/min), drop at 45 s or 208.5.
// The crack starts when BT reaches `onset`; it then absorbs (E > 0) or releases (E < 0) E * exp(-s / 40) degC/s.
// Neither is measured: the literature disputes where roasting turns exothermic, and R0012-R0014 show no heat sink at
// the crack. F 2 s after onset (reaction time). One step per second; uses the firmware's roast_curve.cpp and roast_control.cpp.
#include <apps/app_roast/roast_control.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

using namespace roast;

struct Plant {
    float gain = 2.2f, rate = 0.0175f, ambient = 26.0f, endo = 0, onset = 200;
    int dead = 5;
    float bt = 25.0f, air = 26.0f;
    int crackAt = -1, t = 0;
    std::vector<float> hist;
    void step(float heat)
    {
        hist.push_back(heat);
        float late = hist.size() > (size_t)dead ? hist[hist.size() - 1 - dead] : 0;
        air += (ambient + gain * late - air) * (1 - std::exp(-1.0f));
        if (crackAt < 0 && bt >= onset) crackAt = t;
        float sink = crackAt >= 0 ? endo * std::exp(-(t - crackAt) / 40.0f) : 0;
        bt += (air - bt) * rate - sink;
        t++;
    }
};

static Curve oldCurve(const char* dir)
{
    Curve c;
    c.pts = {{0, 50, PointTag::Charge}, {425, 200.1f, PointTag::FCs}, {470, 208.5f, PointTag::Drop}};
    std::string p = std::string(dir) + "/syrina_myeroast_build.csv";
    FILE* f       = fopen(p.c_str(), "r");
    char line[64];
    while (f && fgets(line, sizeof line, f)) {
        int t;
        float v;
        if (sscanf(line, "%d,%f", &t, &v) == 2) c.table.push_back(v);
    }
    if (f) fclose(f);
    return c;
}

static Curve newCurve()
{
    Curve c;
    c.pts = {{0, 50, PointTag::Charge}, {425, 200.1f, PointTag::FCs}, {470, 208.5f, PointTag::Drop}};
    c.build();
    return c;
}

struct Out {
    int f = -1, drop = -1;
    float btF = 0, btDrop = 0, rorMinAfter = 1e9f, flick = 0, err = 0;
    int n = 0;
    const char* why = "";
};

static Out run(bool isNew, Plant pl, const Curve& c)
{
    srand(1);
    PidConfig pc;
    PidState ps;
    PidReset(ps, 0);
    std::deque<float> bts;
    std::vector<float> rors;
    DevPlan plan = c.devPlan();
    DevTrack dev;
    Out o;
    int winStart = FcFloorStart(c.tagTime(PointTag::FCs), -1, 20);
    for (int t = 0; t < 900; t++) {
        float bt = std::round((pl.bt + ((rand() % 100) - 50) / 2000.0f) * 10) / 10;
        if (o.f < 0 && pl.crackAt >= 0 && t >= pl.crackAt + 2) {
            o.f   = t;
            o.btF = bt;
            if (isNew) {
                dev = DevStart(c, t, bt);
                PidAim da;
                da.target = dev.target(t);
                da.ffTemp = dev.target(t + 8);
                da.ffRate = dev.rate(t + 8);
                da.rate   = dev.rate(t);
                da.afterF = true;
                PidPrime(ps, ps.out, da, bt, pc);  // as fcAnchor
            }
        }
        if (o.f >= 0 && o.drop < 0) {
            if (isNew) {
                DropWhy w = DevDropDue(dev, plan, t, bt);
                if (w != DropWhy::None) {
                    o.drop = t;
                    o.why  = w == DropWhy::Time ? "time" : "temp";
                }
            } else if (t >= o.f + 45 || (bt >= 208.5f && t >= o.f + 35)) {  // the plan's manual drop
                o.drop = t;
                o.why  = t >= o.f + 45 ? "F+45 (hand)" : "208.5 (hand)";
            }
        }
        if (o.drop >= 0) {
            o.btDrop = bt;
            break;
        }
        PidAim a;
        if (isNew && dev.active) {
            a.target = dev.target(t);
            a.ffTemp = dev.target(t + 8);
            a.ffRate = dev.rate(t + 8);
            a.rate   = dev.rate(t);
            a.afterF = true;
        } else if (isNew) {
            a = CardputerAim(c, t, bt, winStart, false, 0, 8, 8);
        } else {  // main: hold from the window on, through the crack
            bool in  = t >= FcFloorStart(c.tagTime(PointTag::FCs), o.f, 20);
            a        = CardputerAim(c, t, bt, in ? 0 : -1, false, 0, 8, 8);
            a.rate   = 0;
        }
        float heat = std::round(PidStep(ps, a, bt, 1.0f, pc).heat * 10) / 10;
        pl.step(heat);
        bts.push_back(pl.bt);
        if (bts.size() > 31) bts.pop_front();
        float ror = bts.size() > 30 ? (bts.back() - bts.front()) * 2 : NAN;
        if (o.f >= 0 && !std::isnan(ror)) {
            float plan30 = isNew && dev.active ? (dev.target(t + 1) - dev.target(t - 29)) * 2
                                               : (c.at(t + 1) - c.at(t - 29)) * 2;
            if (t >= o.f + 30) {  // once the 30 s window has left the time before F
                o.err += std::fabs(ror - plan30);
                o.n++;
            }
            o.rorMinAfter = std::min(o.rorMinAfter, ror);
            rors.push_back(ror);
            if (rors.size() > 10) o.flick = std::max(o.flick, ror - rors[rors.size() - 11]);
        }
    }
    return o;
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "tools/pid";
    Curve oc = oldCurve(dir), nc = newCurve();
    if (oc.table.empty()) {
        printf("missing %s/syrina_myeroast_build.csv\n", dir);
        return 1;
    }
    printf("Syrina D 100g as planned for the competition: FC 7:05 / 200.1, 45 s at 11.2 C/min, at most 208.5\n");
    printf("%-36s %-5s %5s %6s | %5s %-12s %5s %6s %6s | %6s %6s %6s\n", "scenario", "fw", "F", "BT F", "drop", "why", "dev",
           "rise", "BT", "minRoR", "flick", "|dRoR|");
    struct S { const char* name; float onset, endo; } sc[] = {
        {"crack on time (200), no heat effect", 200, 0},  {"crack on time, absorbs 0.08", 200, 0.08f}, {"crack on time, releases 0.05", 200, -0.05f},
        {"crack 15 s early (196), no effect", 196, 0}, {"crack 15 s early, absorbs 0.08", 196, 0.08f}, {"crack 15 s early, releases 0.05", 196, -0.05f},
        {"crack 15 s late (204), no effect", 204, 0},  {"crack 15 s late, absorbs 0.08", 204, 0.08f}, {"crack 15 s late, releases 0.05", 204, -0.05f},
    };
    for (auto& s : sc) {
        for (int k = 0; k < 2; k++) {
            Plant p;
            p.onset = s.onset;
            p.endo  = s.endo;
            Out o   = run(k == 1, p, k == 1 ? nc : oc);
            printf("%-36s %-5s %5s %6.1f | %5s %-12s %5d %+6.1f %6.1f | %6.1f %6.1f %6.2f\n", k == 0 ? s.name : "", k == 0 ? "main" : "new",
                   FormatMmSs(o.f).c_str(), o.btF, FormatMmSs(o.drop).c_str(), o.why, o.drop - o.f, o.btDrop - o.btF, o.btDrop,
                   o.rorMinAfter, o.flick, o.err / std::max(1, o.n));
        }
    }
    printf("\nflick: largest RoR increase over 10 s after F (degC/min). |dRoR|: mean |RoR - plan| from F + 30 s to the\n"
           "drop (main: the curve's RoR on the clock; new: the development plan from F). RoR over 30 s.\n");
    return 0;
}
