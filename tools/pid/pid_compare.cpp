// Batch A (R0012, curve "new": FC 7:30 / 199, drop 10:00 / 210, 200 g) on the fitted roaster model, the curve as
// roast_curve.cpp builds it now: the roaster's PID (plain, and with the lookahead + first-crack floor) against the
// Cardputer PID (development planned from F at 8:01), also on a model that is 15-20 % off. Drop by hand at 8:56.
// Uses the firmware's own roast_control.cpp. Writes one CSV per run to $OUT and prints a table.
//   tools/pid/run.sh  [logs dir with R0012.csv for the replay check]
#include <apps/app_roast/roast_control.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace roast;

// Fitted on R0012-R0014 (roast_control.h PlantModel); one step per second.
struct Plant {
    float gain = 2.133f, rate = 0.0204f, ambient = 26.0f, airTau = 1.0f, endo = 0;
    int dead = 5;
    float bt = 25.2f, air = 26.0f;
    std::vector<float> hist;
    void step(float heat)
    {
        hist.push_back(heat);
        float late = hist.size() > (size_t)dead ? hist[hist.size() - 1 - dead] : 0;
        air += (ambient + gain * late - air) * (1 - std::exp(-1.0f / airTau));
        float self = bt > 156.2f ? 0.253f / 60 * (bt - 156.2f) : 0;  // the beans' own heating (fitted)
        bt += (air - bt) * rate + self;
    }
};

// The roaster's PID as fitted: heat = 0.50 e + 0.036 integral(e dt) + 12 %.
struct RoasterPid {
    float integ = 0;
    float step(float sv, float bt)
    {
        float e = sv - bt;
        integ   = std::max(-12.0f, std::min(88.0f, integ + 0.036f * e));
        return std::max(0.0f, std::min(100.0f, 0.50f * e + integ + 12));
    }
};

static Curve curveA()
{
    Curve c;
    c.pts    = {{0, 50, PointTag::Charge}, {270, 150, PointTag::Dry}, {450, 199, PointTag::FCs}, {600, 210, PointTag::Drop}};
    c.totalT = 600;
    c.build();
    return c;
}

struct Result {
    float rms = 0, worst = 0, btFc = 0, btDrop = 0, rorFc30 = 0, rorMin = 1e9f, heatFc = 0, heatMinAfter = 1e9f, tv = 0;
};

enum Mode { RoasterAsRoasted, RoasterLookFloor, Cardputer };
static const int kFc = 481, kDrop = 536;

static Result run(Mode m, Plant pl, const char* csv, float floorDelta = 1.0f, float floorCap = 8.0f, int seed = 1)
{
    srand(seed);
    Curve c = curveA();
    SvConfig sc;  // L 8, floor +1, cap 8 (current defaults)
    sc.floorDelta = floorDelta;
    sc.floorCap   = floorCap;
    if (m == RoasterAsRoasted) {
        sc.lookahead = 0;
        sc.fcFloor   = false;
    }
    PidConfig pc;  // P 1.5, Ti 60, Td 0, feedforward
    PidState ps;
    RoasterPid rp;
    FILE* f = csv ? fopen(csv, "w") : nullptr;
    if (f) fprintf(f, "t,bt,curve,heat,ror\n");
    Result r;
    std::deque<float> bts;
    float heat = 0, last = 0, sq = 0;
    DevTrack dev;
    int n = 0;
    for (int t = 0; t <= kDrop; t++) {
        float bt = std::round((pl.bt + ((rand() % 100) - 50) / 2000.0f) * 10) / 10;  // 0.1 degC, like register 4
        int start = FcFloorStart(c.tagTime(PointTag::FCs), t >= kFc ? kFc : -1, sc.floorLead);
        bool in   = start >= 0 && t >= start;
        if (m == Cardputer) {  // as the firmware does it (app_roast.cpp pidAim / fcAnchor): development from F
            int start = FcFloorStart(c.tagTime(PointTag::FCs), t >= kFc ? kFc : -1, sc.floorLead);
            if (t == 0) PidReset(ps, 0);
            auto devAim = [&](int s) {
                PidAim a;
                a.target = dev.target(s);
                a.ffTemp = dev.target(s + sc.lookahead);
                a.ffRate = dev.rate(s + sc.lookahead);
                a.rate   = dev.rate(s);
                a.afterF = true;
                return a;
            };
            if (t == kFc) {
                dev = DevStart(c, t, bt);
                PidPrime(ps, ps.out, devAim(t), bt, pc);
            }
            PidAim a = t >= kFc && dev.active ? devAim(t) : CardputerAim(c, t, bt, start, false, 0, sc.lookahead, sc.floorCap);
            heat = PidStep(ps, a, bt, 1.0f, pc).heat;
        } else {
            heat = rp.step(ComputeSv(c, t, bt, in, sc).sv, bt);
        }
        heat = std::round(heat * 10) / 10;
        pl.step(heat);
        bts.push_back(pl.bt);
        if (bts.size() > 31) bts.pop_front();
        float ror = bts.size() > 30 ? (bts.back() - bts.front()) * 2 : NAN;  // degC/min over 30 s
        float err = pl.bt - c.at(t + 1);
        if (t >= 60) {
            sq += err * err;
            n++;
            r.worst = std::max(r.worst, std::fabs(err));
            r.tv += std::fabs(heat - last);
        }
        last = heat;
        if (t == kFc) { r.btFc = pl.bt; r.heatFc = heat; }
        if (t == kFc + 30) r.rorFc30 = ror;
        if (t > kFc) { r.rorMin = std::min(r.rorMin, ror); r.heatMinAfter = std::min(r.heatMinAfter, heat); }
        r.btDrop = pl.bt;
        if (f) fprintf(f, "%d,%.2f,%.2f,%.1f,%s\n", t + 1, pl.bt, c.at(t + 1), heat,
                       std::isnan(ror) ? "" : std::to_string(ror).c_str());
    }
    r.rms = std::sqrt(sq / n);
    if (f) fclose(f);
    return r;
}

// Replay R0012 through the fitted roaster PID + plant with the SV it actually received: how close is the model?
static void replay(const char* dir, const char* out)
{
    std::string p = std::string(dir) + "/R0012.csv";
    FILE* f       = fopen(p.c_str(), "r");
    if (!f) return;
    char line[256];
    std::vector<float> sv, bt;
    while (fgets(line, sizeof line, f)) {
        if (line[0] < '0' || line[0] > '9') continue;
        int t;
        float b, s;
        if (sscanf(line, "%d,%f,%f", &t, &b, &s) == 3 && t <= kDrop) { bt.push_back(b); sv.push_back(s); }
    }
    fclose(f);
    Plant pl;
    pl.bt = bt[0];
    RoasterPid rp;
    double sq = 0;
    float worst = 0;
    int n = 0;
    FILE* o = fopen((std::string(out) + "/replay_R0012.csv").c_str(), "w");
    if (o) fprintf(o, "t,real,sim,sv\n");
    for (size_t i = 0; i + 1 < bt.size(); i++) {
        pl.step(i >= 2 ? rp.step(sv[i], pl.bt) : 0);
        if (o) fprintf(o, "%d,%.1f,%.2f,%.1f\n", (int)i + 1, bt[i + 1], pl.bt, sv[i + 1]);
        if (i >= 60) {
            float e = pl.bt - bt[i + 1];
            sq += e * e;
            worst = std::max(worst, std::fabs(e));
            n++;
        }
    }
    if (o) fclose(o);
    printf("replay R0012 (logged SV -> fitted roaster PID -> fitted model): BT vs the real roast RMS %.2f degC, worst %.2f degC\n\n",
           std::sqrt(sq / n), worst);
}

int main(int argc, char** argv)
{
    const char* out = getenv("OUT") ? getenv("OUT") : ".";
    if (argc > 1) replay(argv[1], out);
    Plant fit, off;
    off.gain = 2.53f;   // +15 %
    off.rate = 0.0149f; // -15 %
    off.dead = 7;
    struct Run { const char* name; Mode m; Plant pl; const char* file; float delta, cap; } runs[] = {
        {"roaster PID, no lookahead, no floor", RoasterAsRoasted, fit, "roaster_asroasted.csv", 1, 8},
        {"roaster PID, L8 + floor +1 cap 8", RoasterLookFloor, fit, "roaster_floor.csv", 1, 8},
        {"roaster PID, L8 + floor +1 cap 3", RoasterLookFloor, fit, "roaster_floor3.csv", 1, 3},
                {"Cardputer PID (development from F)", Cardputer, fit, "cardputer.csv", 0, 8},
        {"roaster PID, L8 + floor +1 cap 3, model off", RoasterLookFloor, off, "roaster_floor3_off.csv", 1, 3},
        {"Cardputer PID, model off", Cardputer, off, "cardputer_off.csv", 0, 8},
    };
    printf("%-44s %6s %6s %7s %7s %7s %7s %7s %7s %6s\n", "batch A (FC 8:01, drop 8:56)", "rms", "worst", "BT FC",
           "BT drop", "RoR+30", "RoRmin", "heatFC", "heatmin", "moves");
    for (auto& r : runs) {
        std::string p = std::string(out) + "/" + r.file;
        Result x      = run(r.m, r.pl, p.c_str(), r.delta, r.cap);
        printf("%-44s %6.2f %6.2f %7.1f %7.1f %7.1f %7.1f %7.1f %7.1f %6.0f\n", r.name, x.rms, x.worst, x.btFc,
               x.btDrop, x.rorFc30, x.rorMin, x.heatFc, x.heatMinAfter, x.tv);
    }
    Curve c = curveA();
    printf("\ncurve: at FC 8:01 %.1f, at drop 8:56 %.1f. rms/worst: BT - curve from 1:00 to the drop (degC); RoR in degC/min "
           "over 30 s; moves: total heater change (%%)\nmodel off: heater gain +15 %%, bean rate -15 %%, dead time 7 s\n",
           c.at(kFc), c.at(kDrop));
    return 0;
}
