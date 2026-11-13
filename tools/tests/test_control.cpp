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

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
