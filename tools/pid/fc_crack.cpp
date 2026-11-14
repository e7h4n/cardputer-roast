// First crack on the Syrina D 100g curve (key points only): how the Cardputer PID's RoR behaves after F.
// The crack absorbs heat from crack onset (BT 200 C): E * exp(-s / 40) degC/s. F 10 s after onset, drop 45 s
// after F (the plan for this batch). Uses the firmware's roast_control.cpp; one step per second.
//   tools/pid/run.sh  (builds and runs both tools)
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
    float gain = 2.2f, rate = 0.0175f, ambient = 26.0f, endo = 0.08f, lateRate = 1.0f;
    int dead = 5;
    float bt = 25.0f, air = 26.0f;
    int crackAt = -1, t = 0;
    std::vector<float> hist;
    void step(float heat)
    {
        hist.push_back(heat);
        float late = hist.size() > (size_t)dead ? hist[hist.size() - 1 - dead] : 0;
        air += (ambient + gain * late - air) * (1 - std::exp(-1.0f));
        if (crackAt < 0 && bt >= 200) crackAt = t;
        float sink = crackAt >= 0 ? endo * std::exp(-(t - crackAt) / 40.0f) : 0;
        bt += (air - bt) * rate * (t >= 380 ? lateRate : 1.0f) - sink;
        t++;
    }
};

static Curve syrina()
{
    Curve c;
    c.pts    = {{0, 50, PointTag::Charge}, {260, 150, PointTag::Dry}, {365, 185, PointTag::None},
                {425, 200.1f, PointTag::FCs}, {465, 206.4f, PointTag::None}, {540, 213.2f, PointTag::Drop}};
    c.totalT = 540;
    c.build();
    return c;
}

enum Mode { Hold, Anchor };

struct Out {
    int f = -1, drop = -1;
    float ror[4] = {}, plan[4] = {}, rorMin = 1e9f, planMin = 1e9f, btDrop = 0, curveDrop = 0, heatF = 0, heatMin = 1e9f,
          heatMax = 0, devDrop = 0, rorErr = 0;
    int rorN = 0;
};

static Out run(Mode m, Plant pl, const char* csv, float td)
{
    srand(1);
    Curve c = syrina();
    SvConfig sc;  // lookahead 8, floor lead 20
    PidConfig pc;
    pc.fcTd = td;
    PidState ps;
    PidReset(ps, 0);
    FILE* f = csv ? fopen(csv, "w") : nullptr;
    if (f) fprintf(f, "t,bt,curve,target,heat,ror,plan\n");
    std::deque<float> bts;
    Out o;
    float offset = 0;
    bool anchored = false;
    int winStart = FcFloorStart(c.tagTime(PointTag::FCs), -1, sc.floorLead);
    for (int t = 0; t < 700; t++) {
        float bt = std::round((pl.bt + ((rand() % 100) - 50) / 2000.0f) * 10) / 10;
        if (o.f < 0 && pl.crackAt >= 0 && t >= pl.crackAt + 10) o.f = t;  // F, 2nd-3rd crack
        if (o.f >= 0 && o.drop < 0 && t > o.f + 45) o.drop = t;
        if (o.drop >= 0) break;
        PidAim a;
        if (m == Anchor) {
            if (o.f >= 0 && !anchored) {
                offset   = FcAnchorOffset(c, t, bt, sc.floorCap);
                anchored = true;
                PidAim na = CardputerAim(c, t, bt, winStart, true, offset, sc.lookahead, sc.floorCap);
                PidPrime(ps, ps.out, na, bt, pc);  // bumpless at F
            }
            a = CardputerAim(c, t, bt, winStart, o.f >= 0, offset, sc.lookahead, sc.floorCap);
        } else {  // the firmware before this change: hold from the window on (BT above the curve = on it)
            SvConfig now   = sc;
            now.lookahead  = 0;
            now.floorDelta = 0;
            bool in        = t >= FcFloorStart(c.tagTime(PointTag::FCs), o.f, sc.floorLead);
            a.target = ComputeSv(c, t, bt, in, now).sv;
            a.ffTemp = c.at(t + sc.lookahead);
            a.ffRate = CurveRate(c, t + sc.lookahead);
            a.rate   = 0;  // the firmware before this change: D on the measurement
        }
        float heat = std::round(PidStep(ps, a, bt, 1.0f, pc).heat * 10) / 10;
        pl.step(heat);
        bts.push_back(pl.bt);
        if (bts.size() > 31) bts.pop_front();
        float ror  = bts.size() > 30 ? (bts.back() - bts.front()) * 2 : NAN;
        float plan = (c.at(t + 1) - c.at(t - 29)) * 2;
        if (o.f >= 0) {
            int k = (t - o.f) / 15;  // F+0, +15, +30, +45
            if ((t - o.f) % 15 == 0 && k < 4) { o.ror[k] = ror; o.plan[k] = plan; }
            o.rorMin  = std::min(o.rorMin, ror);
            if (!std::isnan(ror)) { o.rorErr += std::fabs(ror - plan); o.rorN++; }
            o.planMin = std::min(o.planMin, plan);
            o.heatMin = std::min(o.heatMin, heat);
            o.heatMax = std::max(o.heatMax, heat);
            if (t == o.f) o.heatF = heat;
        }
        o.btDrop    = pl.bt;
        o.curveDrop = c.at(t + 1);
        o.devDrop   = pl.bt - (c.at(t + 1) + (m == Anchor ? offset : 0));
        if (f) fprintf(f, "%d,%.2f,%.2f,%.2f,%.1f,%s,%.2f\n", t + 1, pl.bt, c.at(t + 1), a.target, heat,
                       std::isnan(ror) ? "" : std::to_string(ror).c_str(), plan);
    }
    if (f) fclose(f);
    return o;
}

int main()
{
    const char* out = getenv("OUT") ? getenv("OUT") : ".";
    Curve c = syrina();
    printf("Syrina D 100g, crack sink E*exp(-s/40) from BT 200, F 10 s after onset, drop at F+45\n");
    printf("%-30s %5s %5s | %-23s | %-23s | %6s %6s %6s | %5s %5s %5s\n", "run", "F", "drop", "RoR at F/+15/+30/+45",
           "plan at F/+15/+30/+45", "|dRoR|", "minRoR", "BT-crv", "heatF", "hmin", "hmax");
    struct S { const char* name; float endo, gain, late; } scen[] = {
        {"crack 0.08", 0.08f, 2.2f, 1.0f}, {"crack 0.12", 0.12f, 2.2f, 1.0f},
        {"beans ahead (k+25% from 6:20)", 0.08f, 2.2f, 1.25f}, {"heater weak -15%", 0.08f, 1.87f, 1.0f}};
    struct V { const char* name; Mode m; float td; } vars[] = {
        {"now: hold, D off", Hold, 0}, {"anchor at F, D off", Anchor, 0}, {"anchor + D 5 s after F", Anchor, 5},
        {"anchor + D 10 s after F", Anchor, 10}, {"anchor + D 15 s after F", Anchor, 15}, {"anchor + D 20 s after F", Anchor, 20}};
    for (auto& sc : scen) {
        printf("-- %s\n", sc.name);
        for (auto& v : vars) {
            Plant p;
            p.endo = sc.endo; p.gain = sc.gain; p.lateRate = sc.late;
            std::string file = std::string(out) + "/fc_" + std::to_string(&sc - scen) + "_" + std::to_string(&v - vars) + ".csv";
            Out o = run(v.m, p, file.c_str(), v.td);
            printf("%-30s %5d %5d | %5.1f %5.1f %5.1f %5.1f | %5.1f %5.1f %5.1f %5.1f | %6.2f %6.1f %+6.1f | %5.1f %5.1f %5.1f\n", v.name,
                   o.f, o.drop, o.ror[0], o.ror[1], o.ror[2], o.ror[3], o.plan[0], o.plan[1], o.plan[2], o.plan[3], o.rorErr / std::max(1, o.rorN), o.rorMin,
                   o.btDrop - o.curveDrop, o.heatF, o.heatMin, o.heatMax);
        }
    }
    printf("\nRoR: degC/min over 30 s. plan: the curve's own RoR there. BT-crv: BT minus the (unshifted) curve at the drop.\n");
    return 0;
}
