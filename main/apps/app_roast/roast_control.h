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

/// Heater -> bean model, fitted on R0012-R0014 and the 2026-11-14 Syrina D roast (fan 53-54 %, 100 and 200 g): the
/// inlet air settles at ambient + gain * heat, the beans approach the air at `rate` per second (time constant ~49 s)
/// plus their own heating above ~156 degC, and the response starts ~5 s after a heater change.
struct PlantModel {
    float ambient   = 26.0f;    // degC
    float gain      = 2.133f;   // degC of air per % heat
    float rate      = 0.0204f;  // 1/s
    // The beans heat faster than the heater alone explains from ~156 degC on (R0012-R0014 and the 2026-11-14
    // Syrina D roast; exothermic reactions, better heat transfer as they dry and expand, or the probe: not told apart).
    // Near first crack it is worth 4-5 % heat; without it the feedforward gave too much and the beans ran ahead.
    float extra     = 0.253f / 60;  // degC/s per degC above extraFrom
    float extraFrom = 156.2f;       // degC
    int dead        = 5;            // s, heater change -> air
    float airTau    = 1.0f;         // s, air lag after that
    float selfHeat(float bt) const  // degC/s
    {
        return bt > extraFrom ? extra * (bt - extraFrom) : 0;
    }
};

/// How much faster (or slower) this batch rises than the model says, estimated each second: the BT slope measured
/// minus the slope the model predicts from the heat written (air through the dead time and lag), low-passed. Near
/// first crack the beans' own heating differs by bean, batch and size (2026-11-14/15, one plant fitted per roast:
/// 5.0-6.9 % heat at 200 degC against the model's 4.3 %; bigger batches also respond less to the heater) and it grows
/// as BT rises, which the integral (Ti 60 s) follows only late: BT ran 0.4-0.9 degC ahead into the crack. From
/// `obsLead` s before the curve's first crack the feedforward takes off how much the estimate has changed since
/// (what it was before, the integral already carries), so the heat comes off as the beans speed up (tools/pid
/// fc_replay).
struct Observer {
    float air     = NAN;  // degC, modelled air reaching the beans in the coming second
    float hist[8] = {};   // heat written, newest last
    float lastBt  = NAN;
    float dist    = 0;    // degC/s, measured - modelled BT slope (low-passed)
    float dist0   = 0;    // `dist` when the feedforward started using it
    bool on       = false;
};

/// Cardputer PID: heat = ff + Kp (e + integral(e dt) / Ti + Td (rate - dBT/dt)), e = target - BT. D acts on the
/// rate-of-rise shortfall against the plan (the curve's slope, which has no steps), not on the error's derivative, so
/// the F re-anchor causes no kick; with rate 0 it is the usual derivative on the measurement.
/// From the F mark on, P and D are stronger (fcKp, fcTd) and the Observer faster (fcObsTau): the development lasts
/// 25-50 s and the beans add or lose 2-7 degC/min of their own in it (2026-11-14/15), which P 1.5 alone corrected only
/// after the drop. More D than this made the RoR swing on a 100 g batch (it responds more to the heater).
struct PidConfig {
    float kp         = 1.5f;  // % heat per degC of error
    float ti         = 60;    // s, integral time; 0 = no integral
    float td         = 0;     // s, derivative time before the F mark (on the rate-of-rise shortfall, BT slope 5 s)
    float fcKp       = 4.0f;  // % heat per degC from the F mark on; 0 = kp
    float fcTd       = 3;     // s, derivative time from the F mark on (rate-of-rise shortfall against the
                              // development speed). With the Observer it keeps all five 2026-11-14/15 developments
                              // within 10 % of the planned speed (tools/pid fc_replay)
    bool feedforward = true;  // add the heat the model needs to follow the curve
    PlantModel model;
    bool observe   = true;   // feedforward corrected by the Observer estimate (inside PidAim::observe)
    float obsTau   = 10;     // s, low-pass on the estimate
    float fcObsTau = 4;      // s, the same from the F mark on; 0 = obsTau
    float obsLimit = 0.25f;  // degC/s, largest correction (15 degC/min, ~6 % heat)
    int obsLead    = 120;    // s before the curve's first crack the correction starts (ObserveStart)
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
    Observer obs;
};

struct PidTerms {
    float heat = 0;  // % to write
    float ff = 0, p = 0, i = 0, d = 0;
    float obs = 0;   // degC/s the feedforward took off the plan's rate (Observer; 0 when not in use)
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
    float rate   = 0;    // degC/s: the plan's slope now (D acts on rate - BT rate: the rate-of-rise shortfall)
    bool floored = false;  // first-crack floor holding (BT above the curve)
    bool afterF  = false;  // from the F mark on: D uses fcTd
    bool observe = false;  // feedforward corrected by the Observer (from obsLead s before the curve's FC, and after F)
};

/// Fresh start at charge: no integral yet; the output ramps up from `heatNow` at the slew limit.
void PidReset(PidState& s, float heatNow);

/// Start the controller so its first output equals `heatNow` (bumpless: P back from manual).
void PidPrime(PidState& s, float heatNow, const PidAim& a, float bt, const PidConfig& c);

/// One control step, dt seconds after the previous one. bt NAN (no fresh reading): holds the last output.
PidTerms PidStep(PidState& s, const PidAim& a, float bt, float dt, const PidConfig& c);

/// Observer, one second: `bt` read now (NAN: none fresh), then `heat` written for the coming second. PidStep calls it;
/// call it directly while the heater is held by hand so the estimate keeps up.
void ObserverStep(Observer& o, float bt, float heat, float dt, const PidConfig& c);

/// Where the Observer starts correcting the feedforward: `lead` s before the curve's first crack, or the F mark if
/// that comes first; -1 when neither is known.
int ObserveStart(int curveFcT, int fcMarkT, int lead);

/// Slope of the curve at t (degC/s), central difference over +-5 s.
float CurveRate(const Curve& c, int t);

/// First crack, Cardputer PID: from the F mark on, the rest of the curve is followed shifted by this much
/// (BT - curve at F, within +-cap: the "floor cap" rule), so the plan's rate of rise is followed from where the beans
/// are. Holding the curve's absolute temperature through the crack either cut heat (BT ahead of the line) or reacted
/// only once BT had fallen back onto the line, by then the rate of rise had already collapsed.
float FcAnchorOffset(const Curve& c, int tF, float btAtF, float cap);

/// What the Cardputer PID aims at, at curve second t: the curve now; feedforward from the curve `lookahead` s ahead.
/// From F on (`fcMarked`, curves without a development plan): the curve shifted by `fcOffset`. There is no hold before
/// F any more (BT above the curve counted as on it): on 2026-11-14 it let BT run 2 degC ahead into the crack.
/// `windowStart` (-1: none): the Observer corrects the feedforward from there on (ObserveStart). (`bt` and `cap` are
/// unused.)
PidAim CardputerAim(const Curve& c, int t, float bt, int windowStart, bool fcMarked, float fcOffset, int lookahead,
                    float cap);

/// Development after the F mark: from where the beans cracked (tF, BT), rise at the curve's development speed;
/// Roast drops when the development time is over or the drop temperature is reached (DevDropDue).
struct DevTrack {
    bool active = false;
    int tF      = 0;     // curve second of the F mark
    float btF   = NAN;   // BT at F
    DevPlan plan;
    float target(int t) const;  // degC at curve second t
    float rate(int t) const;    // degC/s
};
DevTrack DevStart(const Curve& c, int tF, float btF);  // inactive when the curve has no plan or BT is unknown

/// What the Cardputer PID aims at during the development: the line from (tF, BT at F) at the planned speed, the
/// feedforward `lookahead` s ahead on it; after-F gains, Observer on.
PidAim DevAim(const DevTrack& d, int t, int lookahead);

/// Why the roast should drop now: the development time is over, or the drop temperature is reached. Before F (d
/// inactive): the drop temperature or the curve's drop time.
enum class DropWhy : uint8_t { None, Time, Temp };
DropWhy DevDropDue(const DevTrack& d, const DevPlan& plan, int t, float bt);

/// Heater output (%) to hold when switching to manual: mean of the samples in (now - window, now]; `fallback`
/// (the current reading) when there are none.
float HoldHeat(const std::vector<Sample>& samples, int now, int window, float fallback);

}  // namespace roast
