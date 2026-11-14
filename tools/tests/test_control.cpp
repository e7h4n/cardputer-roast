// Host unit tests for roast_control (SV lookahead, first-crack floor, manual hold, Cardputer PID) and the curve
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

static Sample S(int t, float heat)
{
    Sample s{};
    s.t      = (int16_t)t;
    s.heat10 = (int16_t)lroundf(heat * 10);
    return s;
}


// Heater -> bean plant as fitted on R0012-R0014 (roast_control.h PlantModel), with a dead time; one step per second.
struct Plant {
    float gain = 2.2f, rate = 0.0175f, ambient = 26.0f;
    int dead   = 5;
    float bt = 100, air = 100;
    std::vector<float> hist;
    void step(float heat)
    {
        hist.push_back(heat);
        float late = hist.size() > (size_t)dead ? hist[hist.size() - 1 - dead] : 0;
        air += (ambient + gain * late - air) * (1 - std::exp(-1.0f));
        bt += (air - bt) * rate;
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
    SvConfig cfg;  // lookahead 8, floor on, delta 1, cap 8, lead 20

    // --- lookahead
    NEAR(ComputeSv(c, 100, NAN, false, cfg).sv, c.at(108));
    cfg.lookahead = 0;
    NEAR(ComputeSv(c, 100, NAN, false, cfg).sv, c.at(100));
    cfg.lookahead = 50;  // clamped to 20
    NEAR(ComputeSv(c, 100, NAN, false, cfg).sv, c.at(120));
    cfg.lookahead = 8;
    // past the end of the curve: hold the last value, never extrapolate
    NEAR(ComputeSv(c, 597, NAN, false, cfg).sv, 200.0f);
    NEAR(ComputeSv(c, 600, NAN, false, cfg).sv, 200.0f);
    NEAR(ComputeSv(c, 900, NAN, false, cfg).sv, 200.0f);
    NEAR(c.at(-5), 100.0f);

    // --- window start: earlier of curve FC - lead and the F mark
    CHECK(FcFloorStart(450, -1, 20) == 430);
    CHECK(FcFloorStart(450, 400, 20) == 400);  // F pressed early
    CHECK(FcFloorStart(450, 470, 20) == 430);  // F pressed late: the curve's window already started
    CHECK(FcFloorStart(-1, 470, 20) == 470);   // curve without an FC mark: from the F press
    CHECK(FcFloorStart(-1, -1, 20) == -1);
    CHECK(FcFloorStart(10, -1, 20) == 0);

    // --- floor: outside the window nothing changes
    SvResult r = ComputeSv(c, 440, 190.0f, false, cfg);
    CHECK(!r.floored);
    NEAR(r.sv, c.at(448));
    // inside the window, BT below the curve: curve wins
    r = ComputeSv(c, 440, 160.0f, true, cfg);
    CHECK(!r.floored);
    NEAR(r.sv, c.at(448));
    // BT above the curve: SV = BT + delta
    float ahead = c.at(448);  // 174.67
    r           = ComputeSv(c, 440, ahead + 2.0f, true, cfg);
    CHECK(r.floored && !r.capped);
    NEAR(r.sv, ahead + 3.0f);
    // far above: capped at curve(t) + 8, but never below the lookahead curve
    r = ComputeSv(c, 440, ahead + 20.0f, true, cfg);
    CHECK(r.floored && r.capped);
    NEAR(r.sv, c.at(440) + 8.0f);
    CHECK(r.sv >= r.ahead);
    // delta adjustable; floor off disables it
    cfg.floorDelta = 2.5f;
    NEAR(ComputeSv(c, 440, ahead + 2.0f, true, cfg).sv, ahead + 4.5f);
    cfg.fcFloor = false;
    NEAR(ComputeSv(c, 440, ahead + 2.0f, true, cfg).sv, ahead);
    cfg.fcFloor    = true;
    cfg.floorDelta = 1.0f;
    // no BT reading: curve only
    CHECK(!ComputeSv(c, 440, NAN, true, cfg).floored);
    // leaving the window (drop): back to the curve
    r = ComputeSv(c, 560, 200.0f, false, cfg);
    CHECK(!r.floored);
    NEAR(r.sv, c.at(568));

    // --- manual hold: 30 s mean, not the last sample
    std::vector<Sample> s;
    for (int t = 0; t <= 100; t++) s.push_back(S(t, t < 71 ? 50.0f : 80.0f));  // last 30 s at 80, earlier 50
    NEAR(HoldHeat(s, 100, 30, -1), 80.0f);
    s.back().heat10 = 600;  // one low reading at the switch
    NEAR(HoldHeat(s, 100, 30, -1), (29 * 80.0f + 60.0f) / 30.0f);
    NEAR(HoldHeat(s, 80, 30, -1), (20 * 50.0f + 10 * 80.0f) / 30.0f);  // window ends at `now`
    NEAR(HoldHeat({}, 10, 30, 77.0f), 77.0f);                        // no samples: current reading
    NEAR(HoldHeat(s, 5, 30, -1), 50.0f);                              // shorter than 30 s so far


    // --- Cardputer PID: feedforward from the model
    PlantModel pm;
    NEAR(FeedForwardHeat(200, 5 / 60.0f, pm), (200 + (5 / 60.0f) / 0.0175f - 26) / 2.2f);  // ~81 % at FC, RoR 5
    NEAR(FeedForwardHeat(26, 0, pm), 0.0f);
    NEAR(FeedForwardHeat(500, 0, pm), 100.0f);                     // clamped
    NEAR(FeedForwardHeat(150, NAN, pm), (150 - 26) / 2.2f);        // no slope: hold
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
        // model wrong by 20 % (gain and rate) and 8 s dead time: still within 3 degC and no oscillation
        Plant off;
        off.gain = 2.64f;
        off.rate = 0.014f;
        off.dead = 8;
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


    // --- first crack, Cardputer PID: hold until F, then the curve shifted to where the beans are
    {
        Curve c = line();  // 100 + t/6
        int win = 430;
        PidAim a = CardputerAim(c, 300, 140.0f, win, false, 0, 8, 8);  // before the window: the curve now
        NEAR(a.target, c.at(300));
        NEAR(a.ffTemp, c.at(308));
        NEAR(a.rate, 1 / 6.0f);
        CHECK(!a.floored && !a.afterF);
        a = CardputerAim(c, 440, c.at(440) + 2, win, false, 0, 8, 8);  // window, BT 2 above: hold (no error)
        NEAR(a.target, c.at(440) + 2);
        CHECK(a.floored);
        a = CardputerAim(c, 440, c.at(440) + 12, win, false, 0, 8, 8);  // runaway: held at most curve + cap
        NEAR(a.target, c.at(440) + 8);
        a = CardputerAim(c, 440, c.at(440) - 2, win, false, 0, 8, 8);  // below: the curve
        NEAR(a.target, c.at(440));
        CHECK(!a.floored);
        float off = FcAnchorOffset(c, 450, c.at(450) + 2.5f, 8);  // F with BT 2.5 above the curve
        NEAR(off, 2.5f);
        NEAR(FcAnchorOffset(c, 450, c.at(450) + 20, 8), 8.0f);    // capped
        NEAR(FcAnchorOffset(c, 450, c.at(450) - 3, 8), -3.0f);    // late crack: below the curve too
        NEAR(FcAnchorOffset(c, 450, NAN, 8), 0.0f);
        a = CardputerAim(c, 470, c.at(470) + 5, win, true, off, 8, 8);  // after F: shifted, no hold
        NEAR(a.target, c.at(470) + 2.5f);
        NEAR(a.ffTemp, c.at(478) + 2.5f);
        CHECK(a.afterF && !a.floored);
    }
    {  // re-prime at F keeps the heater where it was (no jump), and D after F answers a rate-of-rise dip
        Curve c = line();
        PidConfig pf;
        pf.maxStep = 100;
        pf.fcTd    = 10;  // off by default; on here to check it
        PidState st;
        PidAim before = CardputerAim(c, 449, c.at(449) - 1.0f, 430, false, 0, 8, 8);
        PidPrime(st, 80, before, c.at(449) - 1.0f, pf);
        float bt  = c.at(450) - 1.0f;
        PidAim at = CardputerAim(c, 450, bt, 430, true, FcAnchorOffset(c, 450, bt, 8), 8, 8);
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
        PidAim pre = CardputerAim(c, 440, bt, 430, false, 0, 8, 8);  // before F: td (0 by default) applies
        PidState y = st;
        NEAR(PidStep(y, pre, bt, 1.0f, pf).d, 0.0f);
    }


    // --- curves from first crack and drop: through both points, no corners, rate of rise never rises after FC
    {
        Curve c;
        c.pts = {{0, 50, PointTag::Charge}, {260, 150, PointTag::Dry}, {365, 185, PointTag::None},
                 {425, 200.1f, PointTag::FCs}, {540, 213.2f, PointTag::Drop}};  // Syrina D 100g: Dry and free points ignored
        c.build();
        CHECK(c.valid());
        // 115 s for +13.1 would need the rate to crash (11.8 -> 1.9): it falls to 40 % and reaches +13.1 at ~1:35
        CHECK(c.endTime() > 425 + 80 && c.endTime() < 540);
        NEAR(c.at(0), 50.0f);
        NEAR(c.at(425), 200.1f);
        NEAR(c.at(c.endTime()), 213.2f);
        auto ror = [&](int a, int b) { return (c.at(b) - c.at(a)) / (b - a) * 60; };
        float worst = 0, rise = 0;
        for (int t = 10; t + 10 <= c.endTime(); t++) worst = std::max(worst, std::fabs(ror(t, t + 10) - ror(t - 10, t)));
        for (int t = 435; t + 10 <= c.endTime(); t++) rise = std::max(rise, ror(t, t + 10) - ror(t - 10, t));
        CHECK(worst < 1.5f);  // the old builder stepped -4.5 degC/min at first crack
        CHECK(rise <= 0.01f);
        CHECK(std::fabs(ror(415, 425) - ror(425, 435)) < 0.6f);  // no step at first crack
        DevPlan p = c.devPlan();
        CHECK(p.valid && p.dur == 115);
        NEAR(p.rise, 13.1f);
        NEAR(p.dropBT, 213.2f);
    }
    {  // a development that would need a rising rate stays flat and ends below the drop temperature
        Curve c;
        c.pts = {{0, 50, PointTag::Charge}, {450, 200, PointTag::FCs}, {495, 210, PointTag::Drop}};
        c.build();
        NEAR(c.at(450), 200.0f);
        CHECK(c.at(495) < 210.0f);
        float r0 = (c.at(455) - c.at(450)) / 5 * 60, r1 = (c.at(495) - c.at(490)) / 5 * 60;
        CHECK(std::fabs(r0 - r1) < 0.2f);
        // and one that would fall below 1 degC/min reaches the drop temperature early and holds it
        DevShape d = DevCurve(12 / 60.0, 120, 6);  // a crash would be needed: falls only to 40 % and arrives early
        CHECK(d.full && d.reach < 120);
        NEAR((float)(d.r1 * 60), 4.8f);
        NEAR((float)d.at(d.reach), 6.0f);
        NEAR((float)d.at(120), 6.0f);
        CHECK(d.at(120, true) > 6.5);  // extended: keeps rising at r1
        DevShape f = DevCurve(9 / 60.0, 45, 10);  // needs 13.3 on average from 9: flat at 9
        CHECK(!f.full);
        NEAR((float)f.at(45), 6.75f);
    }
    {  // development after F, from where the beans cracked; drop at the first limit
        Curve c;
        c.pts = {{0, 50, PointTag::Charge}, {425, 200, PointTag::FCs}, {470, 210, PointTag::Drop}};  // 45 s, +10, 210
        c.build();
        DevPlan plan = c.devPlan();
        DevTrack late = DevStart(c, 440, 203.0f);  // 15 s late, 3 degrees over the curve
        CHECK(late.active);
        NEAR(late.target(440), 203.0f);
        CHECK(late.rate(441) <= late.shape.r0 + 1e-6);  // never rises
        CHECK(DevDropDue(late, plan, 470, 207.0f) == DropWhy::None);
        CHECK(DevDropDue(late, plan, 485, 207.0f) == DropWhy::Time);   // 45 s after F
        CHECK(DevDropDue(late, plan, 470, 213.0f) == DropWhy::Rise);   // +10 since F
        DevTrack hot = DevStart(c, 425, 205.0f);  // cracked 5 over: aims at 210 (not +10) at 45 s
        CHECK(DevDropDue(hot, plan, 440, 210.0f) == DropWhy::Temp);
        CHECK(std::fabs(hot.target(425 + (int)std::lround(hot.shape.reach)) - 210.0f) < 0.2f);
        CHECK(hot.target(480) > 210.0f);  // and keeps rising, so BT crosses it
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
