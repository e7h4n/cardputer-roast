// Host unit tests for roast_control (SV lookahead, first-crack floor, manual hold) and the curve lookup they use.
//   tools/tests/run.sh
#include <apps/app_roast/roast_control.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

static Sample S(int t, float heat)
{
    Sample s{};
    s.t      = (int16_t)t;
    s.heat10 = (int16_t)lroundf(heat * 10);
    return s;
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

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
