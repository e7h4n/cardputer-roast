#include "roast_control.h"
#include <algorithm>
#include <cmath>

namespace roast {

int FcFloorStart(int curveFcT, int fcMarkT, int lead)
{
    int fromCurve = curveFcT >= 0 ? std::max(0, curveFcT - lead) : -1;
    if (fromCurve < 0) return fcMarkT;
    if (fcMarkT < 0) return fromCurve;
    return std::min(fromCurve, fcMarkT);
}

SvResult ComputeSv(const Curve& c, int t, float bt, bool inWindow, const SvConfig& cfg)
{
    SvResult r;
    int la  = std::max(0, std::min(20, cfg.lookahead));
    r.ahead = c.at(t + la);  // Curve::at holds the last value past the end
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

float HoldHeat(const std::vector<Sample>& samples, int now, int window, float fallback)
{
    double sum = 0;
    int n      = 0;
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        if (it->t > now) continue;
        if (it->t <= now - window) break;
        sum += it->heat10 / 10.0;
        n++;
    }
    return n ? (float)(std::round(sum / n * 10) / 10) : fallback;
}

}  // namespace roast
