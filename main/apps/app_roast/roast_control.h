/*
 * What Roast writes to the roaster while following a curve. The roaster runs its own PID (P/I/D in registers 21-23,
 * never written by this app); Roast only sets its target (SV, register 9) once a second, and holds a heater output
 * when switched to manual. Kept free of hardware so it can be unit tested on the host (tools/tests).
 *
 *  - Lookahead: SV = curve(t + L). The PID reacts late (heater -> air -> beans), so aiming at where the curve will be
 *    in L seconds keeps the beans closer to it; Artisan follows a background curve the same way
 *    (pid_control.py: backgroundSmoothedBTat(tx + lookahead)). Past the curve's end the curve holds its last value.
 *  - First-crack floor: around first crack the beans often run above the curve, the PID then cuts heat right when
 *    the crack absorbs heat, and the rate of rise collapses. Inside the window (from the curve's FC time minus
 *    `lead`, or the F mark if earlier, until the drop) SV is never below BT + delta, so the PID does not pull heat
 *    to get back onto the line (Kaffelogic's approach). The floor is capped at curve(t) + cap, so the PID never
 *    chases a runaway bean temperature upwards. Drop-now and the safety drop are unchanged.
 *  - Manual hold: P holds the heater at its average over the last 30 s, not one sample.
 */
#pragma once
#include "roast_curve.h"
#include "roast_session.h"
#include <vector>

namespace roast {

struct SvConfig {
    int lookahead    = 8;      // s, 0..20
    bool fcFloor     = true;
    float floorDelta = 1.0f;   // degC above BT
    float floorCap   = 8.0f;   // degC above curve(t)
    int floorLead    = 20;     // s before the curve's first-crack time
};

/// Start of the first-crack window: the earlier of (curve FC time - lead) and the F mark. -1: no window.
int FcFloorStart(int curveFcT, int fcMarkT, int lead);

struct SvResult {
    float sv     = 0;      // degC to write
    float ahead  = 0;      // curve(t + lookahead)
    bool floored = false;  // BT + delta was above the curve
    bool capped  = false;  // ... and was limited to curve(t) + cap
};

/// SV at roast second t. `inWindow`: inside the first-crack window (and not dropped). bt may be NAN.
SvResult ComputeSv(const Curve& c, int t, float bt, bool inWindow, const SvConfig& cfg);

/// Heater output (%) to hold when switching to manual: mean of the samples in (now - window, now]; `fallback`
/// (the current reading) when there are none.
float HoldHeat(const std::vector<Sample>& samples, int now, int window, float fallback);

}  // namespace roast
