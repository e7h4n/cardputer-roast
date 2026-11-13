/*
 * What Roast writes to the roaster while following a curve. Kept free of hardware so it can be unit tested on the
 * host (tools/tests). Two ways to follow a curve:
 *  - Cardputer PID (default): the roaster's PID is off and Roast writes the heater output (register 11) once a
 *    second: feedforward from a heater -> bean model plus a PI(D) correction (PidStep below).
 *  - Roaster PID: the roaster's own PID (registers 20-23, cycle 1000 ms, P 6 I 1 D 8; never written by this app)
 *    follows the target Roast writes to register 9 once a second. Fitted on R0012-R0014 it behaves as
 *    heat = 0.50 e + 0.036 integral(e dt) + 12 % (e = SV - BT in degC): integral time ~14 s against a ~57 s bean
 *    time constant, so it reacts late and then overshoots.
 * Both follow the curve with these (the Cardputer PID aims its error at the curve now and only its feedforward
 * ahead, and its first-crack floor holds rather than adds delta; app_roast.cpp pidAim):
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
#include <cmath>
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

/// Heater -> bean model, fitted on R0012-R0014 (fan 53-54 %, 100 and 200 g; bean temperature within 2.2-2.6 degC
/// RMS over the whole roast): the inlet air settles at ambient + gain * heat, the beans approach the air at `rate`
/// per second (time constant ~57 s), and the response starts ~5 s after a heater change.
struct PlantModel {
    float ambient = 26.0f;    // degC
    float gain    = 2.2f;     // degC of air per % heat
    float rate    = 0.0175f;  // 1/s
};

/// Cardputer PID, ISA form on the measurement: heat = ff + Kp (e + integral(e dt) / Ti) - Kp Td dBT/dt, e = SV - BT.
struct PidConfig {
    float kp         = 1.5f;  // % heat per degC of error
    float ti         = 60;    // s, integral time; 0 = no integral
    float td         = 0;     // s, derivative time (on BT, filtered over 5 s)
    bool feedforward = true;  // add the heat the model needs to follow the curve
    PlantModel model;
    float maxStep  = 3.0f;    // % per second, heater slew limit
    float iLimit   = 30.0f;   // |integral term| cap, % (with feedforward the PI only trims)
    float outMin   = 0.0f;
    float outMax   = 100.0f;
};

struct PidState {
    bool primed = false;
    float integ = 0;    // integral term, % (already multiplied by Kp / Ti)
    float out   = 0;    // last output, %
    float lastBt = NAN;
    float dBt    = 0;   // filtered dBT/dt, degC/s
};

struct PidTerms {
    float heat = 0;  // % to write
    float ff = 0, p = 0, i = 0, d = 0;
    bool limited = false;  // clamped or slew-limited
};

/// Heat (%) that holds BT at `sv` while rising at `svRate` (degC/s), from the model; 0..100.
float FeedForwardHeat(float sv, float svRate, const PlantModel& m);

/// What the Cardputer PID aims at in one step. The error is taken against the curve now (with the first-crack floor),
/// the feedforward against the curve `lookahead` seconds ahead: the heater reaches the beans ~6 s late, and only the
/// feedforward should anticipate that. (Aiming the error ahead too made the integral push BT ahead of the curve.)
struct PidAim {
    float target = NAN;  // degC: ComputeSv with lookahead 0
    float ffTemp = NAN;  // degC: curve(t + lookahead)
    float ffRate = 0;    // degC/s: slope there
    bool floored = false;  // first-crack floor holding (BT above the curve)
};

/// Fresh start at charge: no integral yet; the output ramps up from `heatNow` at the slew limit.
void PidReset(PidState& s, float heatNow);

/// Start the controller so its first output equals `heatNow` (bumpless: P back from manual).
void PidPrime(PidState& s, float heatNow, const PidAim& a, float bt, const PidConfig& c);

/// One control step, dt seconds after the previous one. bt NAN (no fresh reading): holds the last output.
PidTerms PidStep(PidState& s, const PidAim& a, float bt, float dt, const PidConfig& c);

/// Slope of the curve at t (degC/s), central difference over +-5 s.
float CurveRate(const Curve& c, int t);

/// Heater output (%) to hold when switching to manual: mean of the samples in (now - window, now]; `fallback`
/// (the current reading) when there are none.
float HoldHeat(const std::vector<Sample>& samples, int now, int window, float fallback);

}  // namespace roast
