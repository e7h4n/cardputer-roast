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
    float air  = sv + (rate - m.selfHeat(sv)) / m.rate;  // the air temperature at which the beans rise at `rate`
    return std::max(0.0f, std::min(100.0f, (air - m.ambient) / m.gain));
}

static float clampf(float v, float lo, float hi)
{
    return std::max(lo, std::min(hi, v));
}

// Observer: fold in the reading of this second (the slope it shows against the slope the model expected) ...
static void ObsMeasure(Observer& o, float bt, float dt, const PidConfig& c, bool afterF = false)
{
    const PlantModel& m = c.model;
    if (std::isnan(o.air)) return;
    if (!std::isnan(bt) && !std::isnan(o.lastBt) && dt > 0 && dt <= 3) {
        float model = m.rate * (o.air - o.lastBt) + m.selfHeat(o.lastBt);  // what the model expected this second
        float tau   = afterF && c.fcObsTau > 0 ? c.fcObsTau : c.obsTau;
        float a     = dt / (std::max(1.0f, tau) + dt);
        o.dist += a * ((bt - o.lastBt) / dt - model - o.dist);
    }
    o.lastBt = bt;  // NAN: the next reading starts afresh
}

// Before the first second: settled at the heat in effect.
static void ObsSeed(Observer& o, float heat, const PidConfig& c)
{
    o.air = c.model.ambient + c.model.gain * heat;
    for (float& h : o.hist) h = heat;
}

// ... then the heat written for the coming second: the modelled air it reaches the beans with.
static void ObsAdvance(Observer& o, float heat, float dt, const PidConfig& c)
{
    const PlantModel& m = c.model;
    if (std::isnan(o.air)) ObsSeed(o, heat, c);
    if (!(dt > 0)) dt = 1;
    for (int k = 0; k < 7; k++) o.hist[k] = o.hist[k + 1];
    o.hist[7]  = heat;
    float late = o.hist[7 - std::max(0, std::min(7, m.dead))];
    o.air += (m.ambient + m.gain * late - o.air) * (1 - std::exp(-dt / std::max(0.1f, m.airTau)));
}

void ObserverStep(Observer& o, float bt, float heat, float dt, const PidConfig& c)
{
    if (std::isnan(o.air)) ObsSeed(o, heat, c);
    ObsMeasure(o, bt, dt, c);
    ObsAdvance(o, heat, dt, c);
}

// The feedforward, with the Observer correction when the aim asks for it (and the start of that correction latched).
static float Feedforward(PidState& s, const PidAim& a, const PidConfig& c, float* corr)
{
    float k = 0;
    if (c.observe && a.observe) {
        if (!s.obs.on) {
            s.obs.on    = true;
            s.obs.dist0 = s.obs.dist;
        }
        k = clampf(s.obs.dist - s.obs.dist0, -c.obsLimit, c.obsLimit);
    } else {
        s.obs.on = false;
    }
    if (corr) *corr = k;
    if (!c.feedforward) return 0;
    return FeedForwardHeat(a.ffTemp, a.ffRate - k, c.model);
}

void PidReset(PidState& s, float heatNow)
{
    s        = PidState();
    s.primed = true;
    s.out    = std::max(0.0f, std::min(100.0f, heatNow));
}

void PidPrime(PidState& s, float heatNow, const PidAim& a, float bt, const PidConfig& c)
{
    float ff = Feedforward(s, a, c, nullptr);
    float kp = a.afterF && c.fcKp > 0 ? c.fcKp : c.kp;
    float p  = (std::isnan(bt) || std::isnan(a.target)) ? 0 : kp * (a.target - bt);
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
        ObserverStep(s.obs, NAN, s.out, dt, c);
        return r;
    }
    dt = std::min(dt, 3.0f);
    if (!std::isnan(s.lastBt)) {
        float a = dt / (5.0f + dt);  // ~5 s low-pass on the BT slope (0.1 degC steps, ~1 reading/s)
        s.dBt += a * ((bt - s.lastBt) / dt - s.dBt);
    }
    s.lastBt = bt;

    float e = a.target - bt;
    if (std::isnan(s.obs.air)) ObsSeed(s.obs, s.out, c);
    ObsMeasure(s.obs, bt, dt, c, a.afterF);
    r.ff    = Feedforward(s, a, c, &r.obs);
    float kp = a.afterF && c.fcKp > 0 ? c.fcKp : c.kp;
    r.p      = kp * e;
    float td = a.afterF ? c.fcTd : c.td;
    r.d      = kp * td * ((std::isnan(a.rate) ? 0 : a.rate) - s.dBt);
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
    ObsAdvance(s.obs, out, dt, c);
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

int ObserveStart(int curveFcT, int fcMarkT, int lead)
{
    return FcFloorStart(curveFcT, fcMarkT, lead);
}

PidAim CardputerAim(const Curve& c, int t, float bt, int windowStart, bool fcMarked, float fcOffset, int lookahead,
                    float cap)
{
    int la = std::max(0, std::min(20, lookahead));
    PidAim a;
    a.observe = windowStart >= 0 && t >= windowStart;
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
    (void)bt;  // no hold before F any more: it let BT run 2 degC ahead into the crack (2026-11-14)
    (void)cap;
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
    return d;
}

float DevTrack::target(int t) const
{
    return btF + plan.speed * (float)std::max(0, t - tF);
}

float DevTrack::rate(int) const
{
    return plan.speed;
}

PidAim DevAim(const DevTrack& d, int t, int lookahead)
{
    int la = std::max(0, std::min(20, lookahead));
    PidAim a;
    a.target  = d.target(t);
    a.ffTemp  = d.target(t + la);
    a.ffRate  = d.rate(t + la);
    a.rate    = d.rate(t);
    a.afterF  = true;
    a.observe = true;
    return a;
}

DropWhy DevDropDue(const DevTrack& d, const DevPlan& plan, int t, float bt)
{
    if (std::isnan(bt) || !plan.valid) return DropWhy::None;
    if (bt >= plan.dropBT) return DropWhy::Temp;
    if (d.active) return t - d.tF >= d.plan.dur ? DropWhy::Time : DropWhy::None;
    return plan.dropT > 0 && t >= plan.dropT ? DropWhy::Time : DropWhy::None;  // no F: the curve's drop time
}

}  // namespace roast

