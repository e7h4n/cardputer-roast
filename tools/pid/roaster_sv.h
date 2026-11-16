// The roaster's own PID as Roast once drove it (for the comparisons in tools/pid): Roast wrote its target (SV,
// register 9) once a second as curve(t + lookahead), and around first crack never below BT + delta (the first-crack
// floor), capped at curve(t) + cap. The window ran from the curve's FC time minus `lead` (or the F mark if earlier).
#pragma once
#include <apps/app_roast/roast_curve.h>
#include <algorithm>
#include <cmath>

namespace roast {

struct SvConfig {
    int lookahead    = 8;      // s, 0..20
    bool fcFloor     = true;
    float floorDelta = 1.0f;   // degC above BT
    float floorCap   = 8.0f;   // degC above curve(t)
    int floorLead    = 20;     // s before the curve's first-crack time
};

struct SvResult {
    float sv     = 0;      // degC to write
    float ahead  = 0;      // curve(t + lookahead)
    bool floored = false;  // BT + delta was above the curve
    bool capped  = false;  // ... and was limited to curve(t) + cap
};

/// Start of the first-crack window: the earlier of (curve FC time - lead) and the F mark. -1: no window.
inline int FcFloorStart(int curveFcT, int fcMarkT, int lead)
{
    int fromCurve = curveFcT >= 0 ? std::max(0, curveFcT - lead) : -1;
    if (fromCurve < 0) return fcMarkT;
    if (fcMarkT < 0) return fromCurve;
    return std::min(fromCurve, fcMarkT);
}

/// SV at roast second t. `inWindow`: inside the first-crack window. bt may be NAN.
inline SvResult ComputeSv(const Curve& c, int t, float bt, bool inWindow, const SvConfig& cfg)
{
    SvResult r;
    int la  = std::max(0, std::min(20, cfg.lookahead));
    r.ahead = c.at(t + la);
    r.sv    = r.ahead;
    if (cfg.fcFloor && inWindow && !std::isnan(bt) && !std::isnan(r.ahead)) {
        float floorV = bt + cfg.floorDelta;
        if (floorV > r.ahead) {
            float cap = c.at(t) + cfg.floorCap;
            r.floored = true;
            r.capped  = floorV > cap;
            r.sv      = std::max(r.ahead, std::min(floorV, cap));
        }
    }
    return r;
}

}  // namespace roast
