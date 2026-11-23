// Host unit tests for roast_control (the Cardputer PID, its self-heating estimate, the development) and the curve
// lookup they use.
//   tools/tests/run.sh
#include <apps/app_roast/roast_control.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>

using namespace roast;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (cond) {                                                                 \
            g_pass++;                                                               \
        } else {                                                                    \
            g_fail++;                                                               \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                  \
        }                                                                           \
    } while (0)
#define NEAR(a, b) CHECK(std::fabs((a) - (b)) < 0.051)

// A straight test curve: 100 + t/6 degC (10 degC/min), 0..600 s.
static Curve line()
{
    Curve c;
    c.table.resize(601);
    for (int i = 0; i <= 600; i++) c.table[i] = 100.0f + i / 6.0f;
    c.pts = {{0, 100, PointTag::Charge}, {450, 175, PointTag::FCs}, {600, 200, PointTag::Drop}};
    return c;
}

static PidAim Aim(float target, float rate)
{
    PidAim a;
    a.target = target;
    a.ffTemp = target;
    a.ffRate = rate;
    return a;
}



// Heater -> bean plant as fitted on R0012-R0014 (roast_control.h PlantModel), with a dead time; one step per second.
struct Plant {  // as fitted (roast_control.h PlantModel), self-heating included
    float gain = 2.133f, rate = 0.0204f, ambient = 26.0f, extra = 0.253f / 60, from = 156.2f;
    int dead   = 5;
    float bt = 100, air = 100;
    std::vector<float> hist;
    void step(float heat)
    {
        hist.push_back(heat);
        float late = hist.size() > (size_t)dead ? hist[hist.size() - 1 - dead] : 0;
        air += (ambient + gain * late - air) * (1 - std::exp(-1.0f));
        bt += (air - bt) * rate + (bt > from ? extra * (bt - from) : 0);
    }
};

// Follows line() for 600 s from BT 100 (air already at 100) with lookahead 8; worst |BT - curve| after 120 s, and
// the largest heater change between two seconds over the last 300 s.
static void follow(const PidConfig& cfg, Plant pl, float* worstErr, float* maxJump)
{
    Curve c = line();
    PidState st;
    pl.bt = pl.air = 100;
    auto aim = [&](int t) {
        PidAim a;
        a.target = c.at(t);
        a.ffTemp = c.at(t + 8);
        a.ffRate = CurveRate(c, t + 8);
        return a;
    };
    PidPrime(st, (100 + (1 / 6.0f) / pl.rate - pl.ambient) / pl.gain, aim(0), 100, cfg);
    *worstErr = 0;
    *maxJump  = 0;
    float last = st.out;
    for (int t = 0; t < 560; t++) {
        PidTerms o = PidStep(st, aim(t), pl.bt, 1.0f, cfg);
        pl.step(o.heat);
        if (t > 120) *worstErr = std::max(*worstErr, std::fabs(pl.bt - c.at(t + 1)));
        if (t > 260) *maxJump = std::max(*maxJump, std::fabs(o.heat - last));
        last = o.heat;
    }
}

int main()
{
    Curve c = line();
    // past the end of the curve: hold the last value, never extrapolate
    NEAR(c.at(900), 200.0f);
    NEAR(c.at(-5), 100.0f);

    // --- Cardputer PID: feedforward from the model
    PlantModel pm;
    // at first crack, RoR 5: the beans' own heating (~11 degC/min at 200) leaves ~79 % (the real roast held ~81 %)
    NEAR(FeedForwardHeat(200, 5 / 60.0f, pm), (200 + (5 / 60.0f - pm.selfHeat(200)) / pm.rate - 26) / pm.gain);
    CHECK(FeedForwardHeat(200, 5 / 60.0f, pm) > 75 && FeedForwardHeat(200, 5 / 60.0f, pm) < 83);
    NEAR(pm.selfHeat(150), 0.0f);
    NEAR(FeedForwardHeat(26, 0, pm), 0.0f);
    NEAR(FeedForwardHeat(500, 0, pm), 100.0f);                     // clamped
    NEAR(FeedForwardHeat(150, NAN, pm), (150 - 26) / pm.gain);     // no slope: hold (no self-heating at 150)
    NEAR(CurveRate(line(), 300), 1 / 6.0f);
    NEAR(CurveRate(line(), 0), 1 / 6.0f);                          // one-sided at the start

    PidConfig pc;  // P 1.5, Ti 60, Td 0, feedforward, 3 %/s
    {  // bumpless: primed at 60 %, the first step stays at 60 % plus one second of integral
        PidState st;
        PidPrime(st, 60, Aim(180, 0.1f), 178, pc);
        PidTerms o = PidStep(st, Aim(180, 0.1f), 178, 1.0f, pc);
        NEAR(o.heat, 60 + 1.5f / 60 * 2);
        NEAR(o.ff + o.p + o.i + o.d, o.heat);
    }
    {  // charge: a fresh start has no integral, and ramps from 0 % at the slew limit
        PidState st;
        PidReset(st, 0);
        NEAR(st.integ, 0.0f);
        PidTerms o = PidStep(st, Aim(60, 0.4f), 25, 1.0f, pc);
        NEAR(o.heat, 3.0f);
        NEAR(st.integ, 0.0f);  // slew-limited: no integration while ramping (anti-windup)
    }
    {  // slew limit: from 0 % with a large error, at most 3 % per second
        PidState st;
        PidPrime(st, 0, Aim(150, 0), 100, pc);
        PidTerms o = PidStep(st, Aim(150, 0), 100, 1.0f, pc);
        NEAR(o.heat, 3.0f);
        CHECK(o.limited);
        for (int i = 0; i < 9; i++) o = PidStep(st, Aim(150, 0), 100, 1.0f, pc);
        NEAR(o.heat, 30.0f);
    }
    {  // anti-windup: 200 s pinned at 100 % with BT below SV, then BT above SV: off the limit within 3 s
        PidState st;
        PidPrime(st, 100, Aim(220, 0), 150, pc);
        for (int i = 0; i < 200; i++) PidStep(st, Aim(220, 0), 150, 1.0f, pc);
        CHECK(st.integ <= pc.iLimit + 1e-3f);
        PidTerms o{};
        int n = 0;
        for (; n < 3; n++) {
            o = PidStep(st, Aim(220, 0), 225, 1.0f, pc);
            if (o.heat < 100) break;
        }
        CHECK(n < 3);
    }
    {  // no fresh reading: hold the output, do not integrate
        PidState st;
        PidPrime(st, 70, Aim(190, 0.1f), 185, pc);
        float i0   = st.integ;
        PidTerms o = PidStep(st, Aim(190, 0.1f), NAN, 1.0f, pc);
        NEAR(o.heat, 70.0f);
        NEAR(st.integ, i0);
    }
    {  // no integral (Ti 0): P + feedforward only
        PidConfig p0 = pc;
        p0.ti        = 0;
        PidState st;
        PidPrime(st, 50, Aim(180, 0), 179, p0);
        PidTerms o = PidStep(st, Aim(180, 0), 179, 1.0f, p0);
        NEAR(o.i, 0.0f);
    }
    {  // derivative on BT: rising BT lowers the output (Td 10 s)
        PidConfig pd = pc;
        pd.td        = 10;
        pd.maxStep   = 100;
        PidState a, b;
        PidPrime(a, 60, Aim(180, 0), 170, pd);
        PidPrime(b, 60, Aim(180, 0), 170, pd);
        PidStep(a, Aim(180, 0), 170, 1.0f, pd);
        PidStep(b, Aim(180, 0), 170, 1.0f, pd);
        PidTerms flat = PidStep(a, Aim(180, 0), 170, 1.0f, pd);
        PidTerms rise = PidStep(b, Aim(180, 0), 172, 1.0f, pd);
        CHECK(rise.d < 0);
        CHECK(rise.heat < flat.heat);
    }
    {  // closed loop on the fitted plant: follows a 10 degC/min ramp within 1 degC, smooth heater
        float err, jump;
        follow(pc, Plant(), &err, &jump);
        printf("  PID on fitted plant: worst |BT - curve| %.2f degC, largest heater step %.2f %%\n", err, jump);
        CHECK(err < 0.5f);
        CHECK(jump < 1.0f);
        // model wrong by 20 % (gain and rate, self-heating 50 % stronger) and 8 s dead time: within 3 degC, no oscillation
        Plant off;
        off.gain  = 2.56f;
        off.rate  = 0.0163f;
        off.extra = 0.38f / 60;
        off.dead  = 8;
        follow(pc, off, &err, &jump);
        printf("  PID on mismatched plant: worst |BT - curve| %.2f degC, largest heater step %.2f %%\n", err, jump);
        CHECK(err < 3.0f);
        CHECK(jump < 1.5f);
        // without feedforward the PI alone lags the ramp
        PidConfig noff = pc;
        noff.feedforward = false;
        noff.iLimit      = 100;
        float errNo;
        follow(noff, Plant(), &errNo, &jump);
        printf("  PI without feedforward: worst |BT - curve| %.2f degC\n", errNo);
        CHECK(errNo > err);
    }


    // --- first crack, Cardputer PID: the curve until F, then the curve shifted to where the beans are
    {
        Curve c = line();  // 100 + t/6
        int win = 430;
        PidAim a = CardputerAim(c, 300, win, false, 0, 8);  // the curve now, feedforward 8 s ahead
        NEAR(a.target, c.at(300));
        NEAR(a.ffTemp, c.at(308));
        NEAR(a.rate, 1 / 6.0f);
        CHECK(!a.afterF);
        float off = FcAnchorOffset(c, 450, c.at(450) + 2.5f, 8);  // F with BT 2.5 above the curve
        NEAR(off, 2.5f);
        NEAR(FcAnchorOffset(c, 450, c.at(450) + 20, 8), 8.0f);    // capped
        NEAR(FcAnchorOffset(c, 450, c.at(450) - 3, 8), -3.0f);    // late crack: below the curve too
        NEAR(FcAnchorOffset(c, 450, NAN, 8), 0.0f);
        a = CardputerAim(c, 470, win, true, off, 8);  // after F: shifted
        NEAR(a.target, c.at(470) + 2.5f);
        NEAR(a.ffTemp, c.at(478) + 2.5f);
        CHECK(a.afterF);
    }
    {  // re-prime at F keeps the heater where it was (no jump), and D after F answers a rate-of-rise dip
        Curve c = line();
        PidConfig pf;
        pf.maxStep = 100;
        pf.fcTd    = 10;  // 3 by default; 10 here to make it plain
        PidState st;
        PidAim before = CardputerAim(c, 449, 430, false, 0, 8);
        PidPrime(st, 80, before, c.at(449) - 1.0f, pf);
        float bt  = c.at(450) - 1.0f;
        PidAim at = CardputerAim(c, 450, 430, true, FcAnchorOffset(c, 450, bt, 8), 8);
        PidPrime(st, st.out, at, bt, pf);
        NEAR(st.out, 80.0f);
        PidState flat = st, dip = st;
        PidStep(flat, at, bt, 1.0f, pf);
        PidStep(dip, at, bt, 1.0f, pf);
        PidTerms onPlan = PidStep(flat, at, bt + 1 / 6.0f, 1.0f, pf);  // rising at the plan's rate
        PidTerms slow   = PidStep(dip, at, bt, 1.0f, pf);              // stalled: the crack
        CHECK(slow.d > onPlan.d);
        CHECK(slow.heat > onPlan.heat);
        PidConfig noD = pf;
        noD.fcTd      = 0;
        PidState z    = st;
        NEAR(PidStep(z, at, bt, 1.0f, noD).d, 0.0f);
        PidAim pre = CardputerAim(c, 440, 430, false, 0, 8);  // before F: td (0 by default) applies
        PidState y = st;
        NEAR(PidStep(y, pre, bt, 1.0f, pf).d, 0.0f);
    }


    // --- self-heating estimate (Observer) and the after-F gains
    {
        CHECK(ObserveStart(420, -1, 120) == 300);
        CHECK(ObserveStart(420, 290, 120) == 290);  // F before the window: from F
        CHECK(ObserveStart(-1, 390, 120) == 390);   // curve without an FC point: from F
        CHECK(ObserveStart(-1, -1, 120) == -1);
        Curve c;
        c.pts = {{0, 50, PointTag::Charge}, {420, 204, PointTag::FCs}, {470, 209, PointTag::Drop}};
        c.build();
        CHECK(!CardputerAim(c, 299, 300, false, 0, 8).observe);
        CHECK(CardputerAim(c, 300, 300, false, 0, 8).observe);
        CHECK(!CardputerAim(c, 300, -1, false, 0, 8).observe);
        DevTrack d = DevStart(c, 400, 202.5f);
        PidAim da  = DevAim(d, 410, 8);
        NEAR(da.target, 202.5f + d.plan.speed * 10);
        NEAR(da.ffTemp, 202.5f + d.plan.speed * 18);
        NEAR(da.rate * 60, 6.0f);
        CHECK(da.afterF && da.observe);
    }
    {  // the estimate: ~0 on the plant the model describes, the extra slope on one that heats itself harder
        PidConfig po;
        Plant same, hot;
        hot.extra = 0.4f / 60;  // degC/s per degC above 156.2 (the model: 0.253)
        same.bt = same.air = hot.bt = hot.air = 190;
        Observer a, b;
        for (int t = 0; t < 120; t++) {  // heat held at 75 %, BT read to 0.1 degC
            ObserverStep(a, std::round(same.bt * 10) / 10, 75, 1.0f, po);
            ObserverStep(b, std::round(hot.bt * 10) / 10, 75, 1.0f, po);
            same.step(75);
            hot.step(75);
        }
        CHECK(std::fabs(a.dist * 60) < 0.5f);
        float extra = (0.4f - 0.253f) * (hot.bt - 156.2f);  // degC/min the model misses at this BT
        CHECK(std::fabs(b.dist * 60 - extra) < 1.0f);
        NEAR(a.air, 26 + 2.133f * 75);  // the modelled air settles on the heat written
    }
    {  // the correction: what the estimate has changed by since the window opened, capped; off outside it
        PidConfig po;
        po.maxStep = 100;
        PidState st;
        PidAim a  = Aim(195, 0.15f);
        a.ffTemp  = 196;
        a.observe = true;
        st.obs.dist = 0.05f;
        PidPrime(st, 70, a, 195, po);  // opens the window: dist0 = 0.05
        CHECK(st.obs.on);
        NEAR(st.obs.dist0, 0.05f);
        st.obs.dist   = 0.15f;  // the beans now rise 6 degC/min faster than at the start of the window
        st.obs.lastBt = NAN;    // (no slope this step: the estimate stays put)
        PidTerms o = PidStep(st, a, 195, 1.0f, po);
        NEAR(o.obs * 60, 6.0f);
        NEAR(o.ff, FeedForwardHeat(196, 0.15f - 0.1f, po.model));
        st.obs.dist   = 2.0f;
        st.obs.lastBt = NAN;
        NEAR(PidStep(st, a, 195, 1.0f, po).obs, po.obsLimit);
        a.observe     = false;
        st.obs.lastBt = NAN;
        o             = PidStep(st, a, 195, 1.0f, po);
        NEAR(o.obs, 0.0f);
        CHECK(!st.obs.on);
        NEAR(o.ff, FeedForwardHeat(196, 0.15f, po.model));
        PidConfig off = po;
        off.observe   = false;
        a.observe     = true;
        st.obs.lastBt = NAN;
        NEAR(PidStep(st, a, 195, 1.0f, off).obs, 0.0f);
    }
    {  // P after F: fcKp from the F mark on, kp before
        PidConfig pf;
        pf.maxStep = 100;
        PidState st;
        PidAim a = Aim(200, 0.1f);
        PidPrime(st, 70, a, 199, pf);
        NEAR(PidStep(st, a, 199, 1.0f, pf).p, 1.5f);
        a.afterF = true;
        NEAR(PidStep(st, a, 199, 1.0f, pf).p, 4.0f);
    }
    {  // closed loop on beans that heat themselves like Huakui 11 B (2026-11-15): BT into the crack on the curve
        Curve c;
        c.pts = {{0, 50, PointTag::Charge}, {420, 204, PointTag::FCs}, {470, 209, PointTag::Drop}};
        c.build();
        auto lead = [&](bool observe) {
            PidConfig pc;
            pc.observe = observe;
            Plant pl;
            pl.gain = 2.038f, pl.rate = 0.0160f, pl.extra = 0.0119f, pl.from = 183.2f;
            pl.bt = pl.air = 25;
            PidState st;
            PidReset(st, 0);
            double sum = 0;
            for (int t = 0; t < 400; t++) {
                float bt  = std::round(pl.bt * 10) / 10;
                PidAim a  = CardputerAim(c, t, ObserveStart(420, -1, pc.obsLead), false, 0, 8);
                if (t >= 370) sum += bt - c.at(t);
                pl.step(std::round(PidStep(st, a, bt, 1.0f, pc).heat * 10) / 10);
            }
            return (float)(sum / 30);
        };
        float without = lead(false), with = lead(true);
        printf("  beans heating like Huakui 11 B: BT - curve over 6:10-6:40 %+.2f without the estimate, %+.2f with\n",
               without, with);
        CHECK(without > 0.5f);
        CHECK(std::fabs(with) < 0.3f);
    }

    {
        auto previews = ListCurves(false);
        auto full = BuiltinCurves();
        CHECK(previews.size() == full.size());
        for (size_t i = 0; i < previews.size(); ++i) {
            auto& preview = previews[i];
            CHECK(preview.table.empty());
            CHECK(LoadPreview(preview));
            CHECK(preview.table.size() < full[i].table.size() / 4);
            CHECK(preview.endTime() == full[i].endTime());
            NEAR(preview.at(preview.endTime()), full[i].at(full[i].endTime()));
            Curve selected;
            CHECK(OpenLibraryCurve(preview, selected));
            CHECK(selected.table == full[i].table);
            CHECK(selected.samplePeriod == 1);
        }
    }

    // --- curves from first crack and drop: through both points, no corners, development at one speed
    {
        Curve c;
        c.pts = {{0, 50, PointTag::Charge}, {260, 150, PointTag::Dry}, {365, 185, PointTag::None},
                 {425, 200.1f, PointTag::FCs}, {470, 208.5f, PointTag::Drop}};  // Syrina D: Dry and free points ignored
        c.build();
        CHECK(c.valid());
        CHECK(c.endTime() == 470);
        NEAR(c.at(0), 50.0f);
        NEAR(c.at(425), 200.1f);
        NEAR(c.at(470), 208.5f);
        auto ror = [&](int a, int b) { return (c.at(b) - c.at(a)) / (b - a) * 60; };
        float worst = 0;
        for (int t = 10; t + 10 <= 470; t++) worst = std::max(worst, std::fabs(ror(t, t + 10) - ror(t - 10, t)));
        CHECK(worst < 1.5f);  // the old builder stepped -4.5 degC/min at first crack
        NEAR(ror(425, 470), 8.4f / 45 * 60);  // development: one speed
        NEAR(ror(440, 450), 8.4f / 45 * 60);
        CHECK(std::fabs(ror(415, 425) - ror(425, 435)) < 0.3f);  // no step at first crack
        DevPlan p = c.devPlan();
        CHECK(p.valid && p.dur == 45 && p.dropT == 470);
        NEAR(p.rise, 8.4f);
        NEAR(p.speed * 60, 11.2f);
        NEAR(p.dropBT, 208.5f);
    }
    {  // a slow development: the rate of rise falls into first crack over the last minute, no corner either
        Curve c;
        c.pts = {{0, 50, PointTag::Charge}, {450, 199, PointTag::FCs}, {600, 210, PointTag::Drop}};  // 4.4 degC/min
        c.build();
        NEAR(c.at(450), 199.0f);
        NEAR(c.at(600), 210.0f);
        auto ror = [&](int a, int b) { return (c.at(b) - c.at(a)) / (b - a) * 60; };
        float worst = 0, up = 0;
        for (int t = 10; t + 10 <= 600; t++) {
            worst = std::max(worst, std::fabs(ror(t, t + 10) - ror(t - 10, t)));
            up    = std::max(up, ror(t, t + 10) - ror(t - 10, t));
        }
        CHECK(worst < 1.5f);
        CHECK(up < 0.3f);  // never rises
    }
    {  // development from F: the curve's speed from where the beans cracked; drop at the time or the temperature
        Curve c;
        c.pts = {{0, 50, PointTag::Charge}, {425, 200, PointTag::FCs}, {470, 210, PointTag::Drop}};  // 45 s at 13.3/min
        c.build();
        DevPlan plan = c.devPlan();
        DevTrack late = DevStart(c, 440, 203.0f);  // 15 s late, 3 over the curve
        CHECK(late.active);
        NEAR(late.target(440), 203.0f);
        NEAR(late.target(455), 203.0f + 10.0f / 45 * 15);
        NEAR(late.rate(450) * 60, 13.33f);
        CHECK(DevDropDue(late, plan, 470, 207.0f) == DropWhy::None);
        CHECK(DevDropDue(late, plan, 485, 209.0f) == DropWhy::Time);  // 45 s after F
        CHECK(DevDropDue(late, plan, 470, 210.0f) == DropWhy::Temp);  // 210 first
        CHECK(DevDropDue(DevTrack(), plan, 300, 209.9f) == DropWhy::None);  // before F: the drop temperature
        CHECK(DevDropDue(DevTrack(), plan, 300, 210.0f) == DropWhy::Temp);
        CHECK(DevDropDue(DevTrack(), plan, 470, 205.0f) == DropWhy::Time);  // or the curve's drop time
        CHECK(!DevStart(c, 425, NAN).active);
        Curve none;  // a drop point but no first crack: no plan
        none.pts = {{0, 50, PointTag::Charge}, {600, 210, PointTag::Drop}};
        none.build();
        CHECK(none.valid());
        NEAR(none.at(600), 210.0f);
        CHECK(!DevStart(none, 425, 200).active);
        CHECK(DevDropDue(DevStart(none, 425, 200), none.devPlan(), 600, 260) == DropWhy::None);
    }
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
