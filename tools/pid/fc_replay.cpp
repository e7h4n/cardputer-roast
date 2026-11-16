// The 2026-11-14/15 competition roasts replayed, one plant per roast: least squares on each log up to the drop (heater
// -> air -> beans, 5 s dead time, the beans' own heating above a fitted temperature; RMS 0.23-0.50 degC from 90 s
// before first crack to the drop), plus the log's residual (what those beans did beyond the plant), so writing the
// logged heat retraces the log. F is pressed when BT reaches what it was when the crack was heard. Measures what went
// wrong on those roasts: BT ahead of the curve into the crack and the development faster or slower than planned (or
// cut short by the drop temperature). Exits 1 unless this firmware keeps all five within 0.3 degC (mean BT - curve
// over the 30 s before F) and 10 % of the planned development speed. `fc_replay <logs> sweep` adds a parameter grid;
// TRACE=<log id> prints that roast's run every 3 s. Logs: tools/pid/logs (t, BT, heat of each roast).
#include <apps/app_roast/roast_control.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

using namespace roast;

// What the beans did beyond the fitted plant: residual(t) = smoothed BT(t + 1) - plant step from smoothed BT(t) under
// the heat that was written. Up to 150 degC indexed by time, then by BT (the beans heat with their temperature),
// from F on by the seconds since F (the crack does what it did, when it did): a replay that writes the same heat
// retraces the log.
struct Residual {
    std::vector<float> byT;           // degC per s at second t
    std::vector<float> env, envR;     // above 150: running-max BT and the residual there
    int tF = -1, tDrop = -1;          // F and drop in the log
    float at(int t, double bt, int simF) const
    {
        if (simF >= 0 && tF >= 0) {
            int k = tF + (t - simF);
            if (k >= tDrop) {  // past the logged drop: its last 5 s on average
                float s = 0;
                for (int j = tDrop - 5; j < tDrop; j++) s += byT[j];
                return s / 5;
            }
            return byT[std::max(0, k)];
        }
        if (!env.empty() && bt >= env.front()) {
            size_t i = std::lower_bound(env.begin(), env.end(), (float)bt) - env.begin();
            if (i >= env.size()) {  // beyond the drop: the last 5 s on average
                float s = 0;
                int n   = 0;
                for (size_t k = envR.size() > 5 ? envR.size() - 5 : 0; k < envR.size(); k++, n++) s += envR[k];
                return n ? s / n : 0;
            }
            return envR[i];
        }
        return t < (int)byT.size() ? byT[t] : 0;
    }
};

struct Plant {
    double gain, rate, extra, from, bt, air;
    int dead = 5;
    std::vector<double> hist;
    const Residual* res = nullptr;
    int simF = -1;  // second F was pressed in the replay
    double step(int t, double heat)  // returns BT after the second
    {
        hist.push_back(heat);
        double late = hist.size() > (size_t)dead ? hist[hist.size() - 1 - dead] : 0;
        air += (26 + gain * late - air) * (1 - std::exp(-1.0));
        double r = res ? res->at(t, bt, simF) : 0;
        bt += rate * (air - bt) + (bt > from ? extra * (bt - from) : 0) + r;
        return bt;
    }
};

struct RoastCase {
    const char* name;
    const char* log;
    double gain, rate, extra, from, bt0;  // fitted plant (extra: degC/s per degC above `from`)
    float fcBt;                           // BT when F was pressed
    int fcT, dropT;                       // curve: FC point, drop point
    float fcC, dropC;
    // as roasted (firmware 1cb0df1): BT - curve over the last 30 s before F, RoR at F (30 s), development rate
    float realLead, realRor, realDev;
    int realDevS, realF, realDropT;
};

static const RoastCase kCases[] = {
    {"X1 Syrina 150 g", "20261114-143137", 2.1556, 0.01665, 0.004430, 159.2, 25.8, 202.7f, 405, 430, 203, 206.5f,
     0.38f, 10.2f, 7.2f, 26, 396, 422},
    {"X2 Syrina 100 g", "20261114-144733", 2.1196, 0.01842, 0.006019, 159.3, 28.4, 202.4f, 405, 430, 203, 206.5f,
     0.79f, 10.2f, 9.5f, 24, 397, 421},
    {"Syrina 204.5 g", "20261115-074043", 2.1328, 0.01690, 0.005113, 151.3, 25.6, 202.5f, 405, 430, 203, 206.5f,
     0.58f, 9.2f, 11.7f, 21, 398, 419},
    {"Huakui 11 B 200 g", "20261115-140136", 2.0379, 0.01602, 0.011863, 183.2, 24.3, 202.3f, 420, 470, 204, 209,
     0.87f, 10.2f, 7.1f, 49, 389, 438},
    {"Huakui 11 A 200 g", "20261115-141554", 2.0668, 0.01529, 0.005928, 170.0, 28.0, 203.0f, 420, 450, 204, 208,
     0.90f, 11.6f, 8.7f, 29, 402, 431},
};

// The log (tools/pid/logs/<id>.csv: t,bt,heat) and the residual of its plant.
static bool LoadResidual(const char* dir, const RoastCase& rc, Residual& res, std::vector<float>& realBt)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.csv", dir, rc.log);
    FILE* f = fopen(path, "r");
    if (!f) return false;
    std::vector<float> bt, heat;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int t;
        float b, h;
        if (sscanf(line, "%d,%f,%f", &t, &b, &h) == 3 && t == (int)bt.size()) {
            bt.push_back(b);
            heat.push_back(h);
        }
    }
    fclose(f);
    int n = (int)bt.size();
    if (n < 100) return false;
    std::vector<float> bs(n);  // centred 5 s mean: the 0.1 degC steps out
    for (int t = 0; t < n; t++) {
        double s = 0;
        for (int k = -2; k <= 2; k++) s += bt[std::max(0, std::min(n - 1, t + k))];
        bs[t] = (float)(s / 5);
    }
    realBt = bs;
    res.byT.assign(n, 0);
    double air = bs[0];
    for (int t = 0; t + 1 < n; t++) {
        double late = t >= 5 ? heat[t - 5] : 0;
        air += (26 + rc.gain * late - air) * (1 - std::exp(-1.0));
        double pred = bs[t] + rc.rate * (air - bs[t]) + (bs[t] > rc.from ? rc.extra * (bs[t] - rc.from) : 0);
        res.byT[t] = (float)(bs[t + 1] - pred);
    }
    res.tF    = rc.realF;
    res.tDrop = rc.realDropT;
    float top = 0;
    for (int t = 0; t + 1 < n && t < rc.realF; t++) {
        if (bs[t] < 150 || bs[t] <= top) continue;
        top = bs[t];
        res.env.push_back(top);
        res.envR.push_back(res.byT[t]);
    }
    return true;
}

// Writing the logged heat into the plant (with the residual) must retrace the log: max |BT - log| up to the drop.
static float Retrace(const char* dir, const RoastCase& rc, const Residual& res, const std::vector<float>& bs)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.csv", dir, rc.log);
    FILE* f = fopen(path, "r");
    std::vector<float> heat;
    char line[256];
    while (f && fgets(line, sizeof(line), f)) {
        int t;
        float b, h;
        if (sscanf(line, "%d,%f,%f", &t, &b, &h) == 3 && t == (int)heat.size()) heat.push_back(h);
    }
    if (f) fclose(f);
    Plant pl{rc.gain, rc.rate, rc.extra, rc.from, bs[0], bs[0]};
    pl.res   = &res;
    float worst = 0;
    for (int t = 0; t < rc.realDropT && t + 1 < (int)bs.size(); t++) {
        if (t == rc.realF) pl.simF = t;
        double b = pl.step(t, heat[t]);
        worst    = std::max(worst, (float)std::fabs(b - bs[t + 1]));
    }
    return worst;
}

struct Out {
    int f = -1, drop = -1;
    const char* why = "";
    float btF = 0, btDrop = 0, lead30 = 0, leadF = 0, rorF = 0, curveRorF = 0, dev = 0, heatF = 0, rms = 0, maxObs = 0;
};

static float gNoise       = 0.025f;  // degC, uniform +- on BT before the 0.1 degC rounding
static const char* gTrace = nullptr;

static Out run(const RoastCase& rc, const Residual* res, const PidConfig& pc, unsigned seed)
{
    srand(seed);
    Curve c;
    c.pts = {{0, 50, PointTag::Charge}, {rc.fcT, rc.fcC, PointTag::FCs}, {rc.dropT, rc.dropC, PointTag::Drop}};
    c.build();
    DevPlan plan = c.devPlan();
    PidState ps;
    PidReset(ps, 0);
    Plant pl{rc.gain, rc.rate, rc.extra, rc.from, rc.bt0, rc.bt0};
    pl.res = res;
    DevTrack dev;
    std::vector<float> bts;
    Out o;
    o.maxObs = 0;
    double se = 0;
    int ne = 0;
    for (int t = 0; t < 900; t++) {
        float bt = std::round((pl.bt + ((rand() % 101) - 50) / 50.0 * gNoise) * 10) / 10;
        bts.push_back(bt);
        if (o.f < 0 && bt >= rc.fcBt) {
            o.f   = t;
            o.btF = bt;
            o.leadF = bt - c.at(t);
            double s = 0;
            for (int k = std::max(0, t - 30); k < t; k++) s += bts[k] - c.at(k);
            o.lead30    = (float)(s / std::min(30, t));
            o.rorF      = t >= 30 ? (bt - bts[t - 30]) * 2 : 0;
            o.curveRorF = CurveRate(c, t) * 60;
            dev         = DevStart(c, t, bt);
            PidAim a;
            a.target = dev.target(t);
            a.ffTemp = dev.target(t + 8);
            a.ffRate = dev.rate(t + 8);
            a.rate    = dev.rate(t);
            a.afterF  = true;
            a.observe = true;
            PidPrime(ps, ps.out, a, bt, pc);
            o.heatF = ps.out;
            pl.simF = t;
        }
        if (o.f >= 0) {
            DropWhy w = DevDropDue(dev, plan, t, bt);
            if (w != DropWhy::None) {
                o.drop   = t;
                o.why    = w == DropWhy::Time ? "time" : "temp";
                o.btDrop = bt;
                break;
            }
        }
        PidAim a;
        if (dev.active) {
            a.target = dev.target(t);
            a.ffTemp = dev.target(t + 8);
            a.ffRate = dev.rate(t + 8);
            a.rate    = dev.rate(t);
            a.afterF  = true;
            a.observe = true;
        } else {
            a = CardputerAim(c, t, bt, ObserveStart(c.tagTime(PointTag::FCs), o.f, pc.obsLead), false, 0, 8, 8);
        }
        if (t >= 60 && o.f < 0) {
            se += (bt - a.target) * (bt - a.target);
            ne++;
        }
        PidTerms pt = PidStep(ps, a, bt, 1.0f, pc);
        if (gTrace && !strcmp(gTrace, rc.log) && t >= 240 && t % 3 == 0)  // TRACE=<log id>: the run every 3 s
            printf("    t %3d bt %6.1f target %6.1f heat %5.1f ff %5.1f p %5.1f i %5.1f d %5.1f estimate %+5.1f/min\n", t,
                   bt, a.target, pt.heat, pt.ff, pt.p, pt.i, pt.d, pt.obs * 60);
        o.maxObs    = std::max(o.maxObs, std::fabs(pt.obs) * 60);
        float heat  = std::round(pt.heat * 10) / 10;
        pl.step(t, heat);
    }
    if (o.drop > o.f) o.dev = (o.btDrop - o.btF) / (o.drop - o.f) * 60;
    o.rms = ne ? (float)std::sqrt(se / ne) : 0;
    return o;
}

struct Variant {
    const char* name;
    bool observe = true;   // self-heating estimate
    float fcKp   = 4;      // P after F (0: as before F)
    float fcTd   = 3;      // D after F
    bool exact   = false;  // firmware model = this roast's fitted plant (an upper bound for any model)
    float tau    = 10;
    int lead     = 120;
    float tauF   = 4;      // estimate low-pass after F
};

static PidConfig ConfigOf(const Variant& v, const RoastCase& rc)
{
    PidConfig pc;
    pc.observe = v.observe;
    pc.obsTau  = v.tau;
    pc.obsLead = v.lead;
    pc.fcObsTau = v.tauF;
    pc.fcKp    = v.fcKp;
    pc.fcTd    = v.fcTd;
    if (v.exact) {
        pc.model.gain      = (float)rc.gain;
        pc.model.rate      = (float)rc.rate;
        pc.model.extra     = (float)rc.extra;
        pc.model.extraFrom = (float)rc.from;
    }
    return pc;
}

static Curve CurveOf(const RoastCase& rc)
{
    Curve c;
    c.pts = {{0, 50, PointTag::Charge}, {rc.fcT, rc.fcC, PointTag::FCs}, {rc.dropT, rc.dropC, PointTag::Drop}};
    c.build();
    return c;
}

// Per roast: lead30 / development % of plan (T: dropped at the temperature), averaged over 3 noise seeds; worst of both.
static void Summary(const Variant& v, const Residual* res, float& worstLead, float& worstDev, char* line, size_t cap)
{
    worstLead = worstDev = 0;
    int n = snprintf(line, cap, "%-44s", v.name);
    for (int i = 0; i < 5; i++) {
        const RoastCase& rc = kCases[i];
        float sp = CurveOf(rc).devPlan().speed * 60, l = 0, d = 0;
        bool temp = false;
        for (unsigned seed = 1; seed <= 3; seed++) {
            Out o = run(rc, &res[i], ConfigOf(v, rc), seed);
            l += o.lead30 / 3;
            d += o.dev / sp * 100 / 3;
            temp |= o.why[1] == 'e';
        }
        worstLead = std::max(worstLead, std::fabs(l));
        worstDev  = std::max(worstDev, std::fabs(d - 100));
        n += snprintf(line + n, cap - n, " %+5.2f/%3.0f%s", l, d, temp ? "T" : " ");
    }
    snprintf(line + n, cap - n, " | %.2f %3.0f", worstLead, worstDev);
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "tools/pid/logs";
    bool sweep      = argc > 2 && !strcmp(argv[2], "sweep");
    if (getenv("NOISE")) gNoise = (float)atof(getenv("NOISE"));
    gTrace = getenv("TRACE");
    static Residual res[5];
    printf("Competition roasts 2026-11-14/15 replayed: one plant fitted per roast plus what those beans did beyond it "
           "(the residual), F at the BT the crack was heard\n");
    for (int i = 0; i < 5; i++) {
        const RoastCase& rc = kCases[i];
        std::vector<float> bs;
        if (!LoadResidual(dir, rc, res[i], bs)) {
            fprintf(stderr, "cannot read %s/%s.csv\n", dir, rc.log);
            return 1;
        }
        float worst = Retrace(dir, rc, res[i], bs);
        if (worst > 0.05f) {
            fprintf(stderr, "%s: the logged heat retraces the log only within %.2f degC\n", rc.log, worst);
            return 1;
        }
    }
    std::vector<Variant> vs = {
        {"1cb0df1 (as roasted)", false, 0, 0},
        {"this: estimate + after F P 4, Td 3", true, 4, 3},
        {"  estimate only (after F P 1.5, Td 0)", true, 0, 0},
        {"  after F P 4, Td 3 only (no estimate)", false, 4, 3},
        {"  1cb0df1 with each roast's exact model", false, 0, 0, true},
    };
    vs[2].tauF = 0;
    if (sweep) {
        for (float tau : {8.f, 10.f, 15.f})
            for (float tauF : {3.f, 4.f, 6.f, 0.f})
                for (float kp : {2.f, 3.f, 4.f, 5.f})
                    for (float td : {0.f, 3.f, 5.f}) {
                        int lead = 120;
                        char* nm = new char[64];
                        snprintf(nm, 64, "est %2.0f, after F %2.0f s, P %.1f Td %2.0f", tau, tauF, kp, td);
                        Variant v{nm, true, kp, td};
                        v.tau  = tau;
                        v.lead = lead;
                        v.tauF = tauF;
                        vs.push_back(v);
                    }
    }
    if (!sweep) {
        for (int i = 0; i < 5; i++) {
            const RoastCase& rc = kCases[i];
            Curve c      = CurveOf(rc);
            DevPlan plan = c.devPlan();
            printf("\n%s (%s): crack heard at %.1f; plan %.1f/min for %d s, at most %.1f. Roasted: lead %+.2f, RoR at F "
                   "%.1f, development %.1f/min for %d s (%.0f %%)\n",
                   rc.name, rc.log, rc.fcBt, plan.speed * 60, plan.dur, plan.dropBT, rc.realLead, rc.realRor,
                   rc.realDev, rc.realDevS, rc.realDev / (plan.speed * 60) * 100);
            printf("  %-42s %5s %6s %6s %6s %5s | %5s %-4s %4s %6s %6s %5s | %5s %5s\n", "firmware", "F", "lead30",
                   "leadF", "RoR F", "curve", "drop", "why", "dev", "BT", "rate", "%plan", "RMS", "est");
            for (auto& v : vs) {
                Out o = run(rc, &res[i], ConfigOf(v, rc), 1);
                printf("  %-42s %5s %+6.2f %+6.2f %6.1f %5.1f | %5s %-4s %4d %6.1f %6.1f %5.0f | %5.2f %5.1f\n", v.name,
                       FormatMmSs(o.f).c_str(), o.lead30, o.leadF, o.rorF, o.curveRorF, FormatMmSs(o.drop).c_str(),
                       o.why, o.drop - o.f, o.btDrop, o.dev, o.dev / (plan.speed * 60) * 100, o.rms, o.maxObs);
            }
        }
        printf("\nlead30: BT - curve over the 30 s before F; leadF: at F. RoR F: 30 s RoR at F; curve: the curve's slope "
               "there. rate: development rate F -> drop (%%plan: of the planned speed). RMS: BT - target from 1:00 to F. "
               "est: largest correction of the self-heating estimate, degC/min.\n");
    }
    printf("\nAll five, 3 noise seeds (+-%.3f degC before rounding): lead30 / development %% (T: dropped at the "
           "temperature) | worst |lead30|, worst |development - 100 %%|\n",
           gNoise);
    int fails = 0;
    for (size_t k = 0; k < vs.size(); k++) {
        char line[512];
        float wl, wd;
        Summary(vs[k], res, wl, wd, line, sizeof(line));
        printf("%s\n", line);
        if (k == 1 && (wl > 0.3f || wd > 10.5f)) fails++;  // this firmware: within 0.3 degC and 10 %
    }
    if (fails) fprintf(stderr, "this firmware misses lead30 <= 0.3 degC or development within 10 %%\n");
    return fails ? 1 : 0;
}
