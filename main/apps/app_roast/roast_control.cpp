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

float FeedForwardHeat(float sv, float svRate, const PlantModel& m)
{
    if (std::isnan(sv) || m.gain <= 0 || m.rate <= 0) return 0;
    float rate = std::isnan(svRate) ? 0 : svRate;
    float air  = sv + rate / m.rate;  // the air temperature at which the beans rise at `rate`
    return std::max(0.0f, std::min(100.0f, (air - m.ambient) / m.gain));
}

static float clampf(float v, float lo, float hi)
{
    return std::max(lo, std::min(hi, v));
}

void PidReset(PidState& s, float heatNow)
{
    s        = PidState();
    s.primed = true;
    s.out    = std::max(0.0f, std::min(100.0f, heatNow));
}

void PidPrime(PidState& s, float heatNow, const PidAim& a, float bt, const PidConfig& c)
{
    float ff = c.feedforward ? FeedForwardHeat(a.ffTemp, a.ffRate, c.model) : 0;
    float p  = (std::isnan(bt) || std::isnan(a.target)) ? 0 : c.kp * (a.target - bt);
    s.primed = true;
    s.out    = clampf(heatNow, c.outMin, c.outMax);
    s.integ  = c.ti > 0 ? clampf(s.out - ff - p, -c.iLimit, c.iLimit) : 0;
    s.lastBt = bt;
    s.dBt    = 0;
}

PidTerms PidStep(PidState& s, const PidAim& a, float bt, float dt, const PidConfig& c)
{
    PidTerms r;
    if (!s.primed) PidReset(s, 0);
    if (std::isnan(bt) || std::isnan(a.target) || !(dt > 0)) {  // no fresh reading: hold, do not integrate
        r.heat = s.out;
        r.i    = s.integ;
        return r;
    }
    dt = std::min(dt, 3.0f);
    if (!std::isnan(s.lastBt)) {
        float a = dt / (5.0f + dt);  // ~5 s low-pass on the BT slope (0.1 degC steps, ~1 reading/s)
        s.dBt += a * ((bt - s.lastBt) / dt - s.dBt);
    }
    s.lastBt = bt;

    float e = a.target - bt;
    r.ff    = c.feedforward ? FeedForwardHeat(a.ffTemp, a.ffRate, c.model) : 0;
    r.p     = c.kp * e;
    float td = a.afterF ? c.fcTd : c.td;
    r.d      = c.kp * td * ((std::isnan(a.rate) ? 0 : a.rate) - s.dBt);
    float iNew = s.integ;
    if (c.ti > 0) iNew = clampf(s.integ + c.kp / c.ti * e * dt, -c.iLimit, c.iLimit);
    float raw  = r.ff + r.p + iNew + r.d;
    float out  = clampf(raw, c.outMin, c.outMax);
    float step = c.maxStep * dt;
    out        = clampf(out, s.out - step, s.out + step);
    // Anti-windup: keep the new integral only if it does not push further into a limit that already holds.
    bool high = out < raw - 1e-3f, low = out > raw + 1e-3f;
    if (!((high && e > 0) || (low && e < 0))) s.integ = iNew;
    r.i       = s.integ;
    r.limited = high || low;
    s.out     = out;
    r.heat    = out;
    return r;
}

float CurveRate(const Curve& c, int t)
{
    int a = std::max(0, t - 5), b = t + 5;
    if (b <= a) return 0;
    float va = c.at(a), vb = c.at(b);
    if (std::isnan(va) || std::isnan(vb)) return 0;
    return (vb - va) / (float)(b - a);
}

float FcAnchorOffset(const Curve& c, int tF, float btAtF, float cap)
{
    float v = c.at(tF);
    if (std::isnan(v) || std::isnan(btAtF)) return 0;
    return clampf(btAtF - v, -cap, cap);
}

PidAim CardputerAim(const Curve& c, int t, float bt, int windowStart, bool fcMarked, float fcOffset, int lookahead,
                    float cap)
{
    int la = std::max(0, std::min(20, lookahead));
    PidAim a;
    a.ffRate = CurveRate(c, t + la);
    a.rate   = CurveRate(c, t);
    if (fcMarked) {
        a.afterF = true;
        a.target = c.at(t) + fcOffset;
        a.ffTemp = c.at(t + la) + fcOffset;
        return a;
    }
    a.target = c.at(t);
    a.ffTemp = c.at(t + la);
    if (windowStart >= 0 && t >= windowStart && !std::isnan(bt) && bt > a.target) {  // hold until F
        a.target  = std::min(bt, a.target + cap);
        a.floored = true;
    }
    return a;
}

DevTrack DevStart(const Curve& c, int tF, float btF)
{
    DevTrack d;
    d.plan = c.devPlan();
    if (!d.plan.valid || std::isnan(btF)) return d;
    d.active = true;
    d.tF     = tF;
    d.btF    = btF;
    // The tighter of the two temperature limits: a crack hotter than planned leaves less room below the drop
    // temperature, and the rate of rise then falls so that it is reached at the development time, not long before.
    double rise = std::max(0.1, std::min((double)d.plan.rise, (double)(d.plan.dropBT - btF)));
    double mean = rise / d.plan.dur;
    double r0   = std::max((double)CurveRate(c, tF), mean);  // past the curve's end its slope is 0
    d.shape     = DevCurve(r0, d.plan.dur, rise);
    return d;
}

float DevTrack::target(int t) const  // keeps rising past the limits, so BT crosses them and the drop comes
{
    return btF + (float)shape.at(t - tF, true);
}

float DevTrack::rate(int t) const
{
    return (float)shape.rate(t - tF);
}

DropWhy DevDropDue(const DevTrack& d, const DevPlan& plan, int t, float bt)
{
    if (std::isnan(bt)) return DropWhy::None;
    if (d.active) {
        if (t - d.tF >= d.plan.dur) return DropWhy::Time;
        if (bt - d.btF >= d.plan.rise) return DropWhy::Rise;
        if (bt >= d.plan.dropBT) return DropWhy::Temp;
        return DropWhy::None;
    }
    if (!plan.valid) return DropWhy::None;
    if (bt >= plan.dropBT) return DropWhy::Temp;
    return plan.dropT > 0 && t >= plan.dropT ? DropWhy::Time : DropWhy::None;  // no F: the curve's drop time
}

}  // namespace roast

