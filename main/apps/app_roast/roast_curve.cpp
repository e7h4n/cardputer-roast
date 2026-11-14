#include "roast_curve.h"
#include "preset_dark.h"
#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <dirent.h>
#include <sys/stat.h>

namespace roast {

#ifndef ROAST_SD_ROOT
#define ROAST_SD_ROOT "/sdcard"
#endif
static const char* kCurveDir = ROAST_SD_ROOT "/roast/curves";

const char* TagName(PointTag tag)
{
    switch (tag) {
        case PointTag::Charge:
            return "Start";
        case PointTag::Dry:
            return "Dry";
        case PointTag::FCs:
            return "FC start";
        case PointTag::FCe:
            return "FC end";
        case PointTag::SCs:
            return "SC start";
        case PointTag::SCe:
            return "SC end";
        case PointTag::Drop:
            return "Drop";
        default:
            return "Point";
    }
}

const char* TagKey(PointTag tag)
{
    switch (tag) {
        case PointTag::Charge:
            return "CHARGE";
        case PointTag::Dry:
            return "DRY";
        case PointTag::FCs:
            return "FCs";
        case PointTag::FCe:
            return "FCe";
        case PointTag::SCs:
            return "SCs";
        case PointTag::SCe:
            return "SCe";
        case PointTag::Drop:
            return "DROP";
        default:
            return "";
    }
}

PointTag TagFromKey(const std::string& k)
{
    if (k == "CHARGE") return PointTag::Charge;
    if (k == "DRY") return PointTag::Dry;
    if (k == "FCs" || k == "FCS") return PointTag::FCs;
    if (k == "FCe" || k == "FCE") return PointTag::FCe;
    if (k == "SCs" || k == "SCS") return PointTag::SCs;
    if (k == "SCe" || k == "SCE") return PointTag::SCe;
    if (k == "DROP") return PointTag::Drop;
    return PointTag::None;
}

std::string FormatMmSs(int sec)
{
    char b[16];
    if (sec < 0) {
        snprintf(b, sizeof(b), "-%d:%02d", (-sec) / 60, (-sec) % 60);
    } else {
        snprintf(b, sizeof(b), "%d:%02d", sec / 60, sec % 60);
    }
    return b;
}

bool ParseMmSs(const std::string& s, int& sec)
{
    int m = 0, ss = 0;
    if (sscanf(s.c_str(), "%d:%d", &m, &ss) == 2 && ss >= 0 && ss < 60 && m >= 0) {
        sec = m * 60 + ss;
        return true;
    }
    // Digits only: "450" -> 4:50, "1030" -> 10:30, "5" -> 5:00 (curve keypoints are thought of in minutes)
    bool digits = !s.empty() && s.size() <= 4;
    for (char ch : s) digits = digits && isdigit((unsigned char)ch);
    if (digits) {
        int v = atoi(s.c_str());
        if (s.size() <= 2) {
            sec = v * 60;
            return true;
        }
        if (v % 100 < 60) {
            sec = (v / 100) * 60 + v % 100;
            return true;
        }
    }
    return false;
}

float Curve::at(int t) const
{
    if (table.empty()) return NAN;
    if (t <= 0) return table.front() + offset;
    if (t >= (int)table.size()) return table.back() + offset;
    return table[t] + offset;
}

int Curve::tagTime(PointTag tag) const
{
    for (auto& p : pts)
        if (p.tag == tag) return p.t;
    return -1;
}

float Curve::tagTemp(PointTag tag) const
{
    for (auto& p : pts)
        if (p.tag == tag) return p.c;
    return NAN;
}

void Curve::sortPoints()
{
    std::stable_sort(pts.begin(), pts.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.t < b.t; });
    // collapse duplicate times (keep the later definition)
    std::vector<CurvePoint> out;
    for (auto& p : pts) {
        if (!out.empty() && out.back().t == p.t)
            out.back() = p;
        else
            out.push_back(p);
    }
    pts.swap(out);
}

// ---------------------------------------------------------------------------------------------------------
// Curve construction from two points: first crack (FCs) and drop (DROP), plus the charge temperature (CHARGE,
// default the preset's). Other tags and untagged points in older files are ignored.
//  - Charge -> first crack: the vendor's dark-roast preset (a smooth curve, rate of rise falling ~27 -> 10
//    degC/min), stretched in time so its first crack (480 s) lands on ours and in temperature so it starts at the
//    charge temperature and passes through ours.
//  - First crack -> drop: the rate of rise starts where the preset part ends (no step) and falls linearly so that
//    the beans rise drop - FC degrees in drop - FC seconds. It never rises: when that would need a rising rate it
//    stays flat (and ends below the drop temperature at the drop time); when it would fall below kDevRorMin it
//    reaches the drop temperature early and holds it.
// Without FCs (or DROP) the whole preset is stretched onto the one point there is.
// Unlike the myERoast app (piecewise-linear corrections between marks, which put a rate-of-rise step at every
// mark: on Syrina D 100g -4.5 degC/min at first crack), this has no corners.
// ---------------------------------------------------------------------------------------------------------
namespace {
double presetAt(double tau)
{
    if (tau <= 0) return kPresetDark100[0] / 100.0;
    int i = (int)tau;
    if (i >= kPresetDarkLen - 1) return kPresetDark100[kPresetDarkLen - 1] / 100.0;
    double f = tau - i;
    return (kPresetDark100[i] * (1 - f) + kPresetDark100[i + 1] * f) / 100.0;
}

// Preset stretched so that preset time `pT` lands on `t1` and its temperatures run from t0Temp (at 0) to c1 (at t1).
struct Stretch {
    double t0Temp, c1, pT, t1, p0, p1;
    double at(double t) const
    {
        double tau = t * pT / t1;
        double k   = (p1 - p0) != 0 ? (c1 - t0Temp) / (p1 - p0) : 1;
        return t0Temp + (presetAt(tau) - p0) * k;
    }
    double rate(double t) const  // degC/s
    {
        return (at(t + 0.5) - at(t - 0.5));
    }
};
}  // namespace

DevShape DevCurve(double r0, double dur, double rise)
{
    DevShape d;
    d.r0 = std::max(0.0, r0);
    if (dur <= 0 || rise <= 0) {
        d.r1 = d.r0;
        d.reach = 0;
        return d;
    }
    double r1    = 2 * rise / dur - d.r0;
    double floor = std::max(kDevRorMin, kDevRorFloor * d.r0);
    if (r1 > d.r0) {  // would need a rising rate: hold it; ends below the target at `dur`
        d.r1    = d.r0;
        d.reach = dur;
        d.full  = false;
    } else if (r1 < floor) {  // would need a crash: fall only to the floor and reach `rise` early
        d.r1    = std::min(floor, d.r0);
        d.reach = d.r0 + d.r1 > 0 ? 2 * rise / (d.r0 + d.r1) : dur;
        if (d.reach > dur) d.reach = dur;
    } else {
        d.r1    = r1;
        d.reach = dur;
    }
    d.dur  = dur;
    d.rise = rise;
    return d;
}

double DevShape::at(double s, bool extend) const
{
    if (s <= 0 || reach <= 0) return 0;
    double u = std::min(s, reach);
    double v = r0 * u + (r1 - r0) * u * u / (2 * reach);
    if (full) v = std::min(v, rise);
    if (extend && s > reach) v += r1 * (s - reach);
    return v;
}

double DevShape::rate(double s) const
{
    if (s < 0) return r0;
    if (reach <= 0 || s >= reach) return r1;
    return r0 + (r1 - r0) * s / reach;
}

void Curve::build()
{
    if (readOnly) return;
    sortPoints();
    table.clear();
    if (pts.empty()) return;
    double charge = kPresetDark100[0] / 100.0;
    int tF = -1, tD = -1;
    double cF = NAN, cD = NAN;
    for (auto& p : pts) {
        if (p.tag == PointTag::Charge && !std::isnan(p.c)) charge = p.c;
        if (p.tag == PointTag::FCs) {
            tF = p.t;
            cF = p.c;
        }
        if (p.tag == PointTag::Drop) {
            tD = p.t;
            cD = p.c;
        }
    }
    if (tF > 0 && std::isnan(cF)) cF = Stretch{charge, presetAt(kPresetDark_FCs_t), (double)kPresetDark_FCs_t, (double)tF,
                                               presetAt(0), presetAt(kPresetDark_FCs_t)}.at(tF);
    if (tD > 0 && std::isnan(cD)) tD = -1;
    int total = tD > 0 ? tD : (tF > 0 ? tF : -1);
    if (total <= 0 || total > 1800) return;
    double p0 = presetAt(0);
    if (tF > 0) {
        Stretch pre{charge, cF, (double)kPresetDark_FCs_t, (double)tF, p0, presetAt(kPresetDark_FCs_t)};
        DevShape d;
        if (tD > tF) {  // when a limit comes before the drop time (the rate would have to crash), the curve ends there
            d     = DevCurve(pre.rate(tF), tD - tF, cD - cF);
            total = std::min(tD, tF + (int)std::ceil(d.reach));
        }
        table.resize(total + 1);
        for (int t = 0; t <= std::min(tF, total); t++) table[t] = (float)pre.at(t);
        for (int t = tF + 1; t <= total; t++) table[t] = (float)(cF + d.at(t - tF));
    } else {
        table.resize(total + 1);  // only a drop point: the whole preset onto it
        Stretch all{charge, cD, (double)kPresetDark_DROP_t, (double)tD, p0, presetAt(kPresetDark_DROP_t)};
        for (int t = 0; t <= tD; t++) table[t] = (float)all.at(t);
    }
    totalT = total;
}

DevPlan Curve::devPlan() const
{
    DevPlan p;
    int tF = tagTime(PointTag::FCs), tD = tagTime(PointTag::Drop);
    float cF = tagTemp(PointTag::FCs), cD = tagTemp(PointTag::Drop);
    if (readOnly || tF <= 0 || tD <= tF || std::isnan(cF) || std::isnan(cD) || cD <= cF) return p;
    p.valid  = true;
    p.dur    = tD - tF;
    p.rise   = cD - cF;
    p.dropBT = cD + offset;
    p.dropT  = tD;
    return p;
}

static Curve make_builtin(const char* name, const char* note, int batch, std::vector<CurvePoint> pts)
{
    Curve c;
    c.name    = name;
    c.note    = note;
    c.batchG  = batch;
    c.builtin = true;
    c.pts     = std::move(pts);
    c.build();
    return c;
}

std::vector<Curve> BuiltinCurves()
{
    std::vector<Curve> v;
    v.push_back(make_builtin("Gotiti 200g Light V3", "At most 65 s and +8 C after FC, max 207 C; untested", 200,
                             {{0, 50, PointTag::Charge}, {470, 199, PointTag::FCs}, {535, 207, PointTag::Drop}}));
    v.push_back(make_builtin("Gotiti 200g Pullback", "At most 85 s and +11 C after FC, max 211 C; untested", 200,
                             {{0, 50, PointTag::Charge}, {470, 200, PointTag::FCs}, {555, 211, PointTag::Drop}}));
    v.push_back(make_builtin("Dry run (no beans)", "Low temperature check of PID and manual heat, 3 min", 0,
                             {{0, 50, PointTag::Charge}, {120, 100, PointTag::FCs}, {180, 110, PointTag::Drop}}));
    return v;
}

bool EnsureDir(const char* path)
{
    std::string p(path);
    for (size_t i = 1; i <= p.size(); i++) {
        if (i == p.size() || p[i] == '/') {
            std::string sub = p.substr(0, i);
            struct stat st;
            if (stat(sub.c_str(), &st) != 0) {
                if (mkdir(sub.c_str(), 0775) != 0) return false;
            }
        }
    }
    return true;
}

static std::string read_file(const std::string& path, size_t limit = 160 * 1024)
{
    std::string s;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return s;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || (size_t)size > limit) {  // keep the heap safe; huge Artisan logs are skipped
        fclose(f);
        return s;
    }
    s.reserve(size);
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < limit) s.append(buf, n);
    fclose(f);
    return s;
}

static bool parse_json_curve(const std::string& txt, Curve& out)
{
    cJSON* root = cJSON_Parse(txt.c_str());
    if (!root) return false;
    bool ok = false;
    cJSON* name = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(name)) out.name = name->valuestring;
    cJSON* note = cJSON_GetObjectItem(root, "note");
    if (cJSON_IsString(note)) out.note = note->valuestring;
    cJSON* batch = cJSON_GetObjectItem(root, "batch_g");
    if (cJSON_IsNumber(batch)) out.batchG = batch->valueint;
    cJSON* tot = cJSON_GetObjectItem(root, "total_s");
    if (cJSON_IsNumber(tot)) out.totalT = tot->valueint;
    cJSON* pts = cJSON_GetObjectItem(root, "points");
    if (cJSON_IsArray(pts)) {
        cJSON* it;
        cJSON_ArrayForEach(it, pts)
        {
            CurvePoint p;
            if (cJSON_IsArray(it) && cJSON_GetArraySize(it) >= 2) {
                p.t = cJSON_GetArrayItem(it, 0)->valueint;
                p.c = (float)cJSON_GetArrayItem(it, 1)->valuedouble;
                cJSON* tg = cJSON_GetArrayItem(it, 2);
                if (cJSON_IsString(tg)) p.tag = TagFromKey(tg->valuestring);
            } else if (cJSON_IsObject(it)) {
                cJSON* t = cJSON_GetObjectItem(it, "t");
                cJSON* c = cJSON_GetObjectItem(it, "c");
                if (!cJSON_IsNumber(t)) continue;
                p.t       = t->valueint;
                p.c       = cJSON_IsNumber(c) ? (float)c->valuedouble : NAN;
                cJSON* tg = cJSON_GetObjectItem(it, "tag");
                if (cJSON_IsString(tg)) p.tag = TagFromKey(tg->valuestring);
            } else {
                continue;
            }
            out.pts.push_back(p);
        }
        out.build();
        ok = out.valid();
    }
    cJSON_Delete(root);
    return ok;
}

// Tolerant reader for Artisan .alog (Python literal) and myERoast .alog.txt exports.
static bool find_number_array(const std::string& s, const char* key, std::vector<float>& out)
{
    std::string k1 = std::string("'") + key + "'";
    std::string k2 = std::string("\"") + key + "\"";
    size_t p       = s.find(k1);
    if (p == std::string::npos) p = s.find(k2);
    if (p == std::string::npos) return false;
    p = s.find('[', p);
    if (p == std::string::npos) return false;
    size_t e = s.find(']', p);
    if (e == std::string::npos) return false;
    out.clear();
    const char* c   = s.c_str() + p + 1;
    const char* end = s.c_str() + e;
    while (c < end) {
        while (c < end && !(isdigit((unsigned char)*c) || *c == '-' || *c == '.' || *c == 'N')) c++;
        if (c >= end) break;
        if (*c == 'N') {  // None
            out.push_back(NAN);
            c += 4;
            continue;
        }
        char* stop;
        float v = strtof(c, &stop);
        if (stop == c) {
            c++;
            continue;
        }
        out.push_back(v);
        c = stop;
    }
    return !out.empty();
}

static bool parse_alog_curve(const std::string& txt, Curve& out)
{
    std::vector<float> temp2, timex, timeindex;
    if (!find_number_array(txt, "temp2", temp2)) return false;
    find_number_array(txt, "timex", timex);
    find_number_array(txt, "timeindex", timeindex);
    out.readOnly = true;
    out.table.clear();
    if (timex.size() == temp2.size() && timex.size() > 2) {
        int chargeIdx = (!timeindex.empty() && timeindex[0] > 0 && timeindex[0] < timex.size()) ? (int)timeindex[0] : 0;
        double t0 = timex[chargeIdx];
        double tEnd = timex.back() - t0;
        if (timeindex.size() > 6 && timeindex[6] > 0 && timeindex[6] < timex.size()) tEnd = timex[(int)timeindex[6]] - t0;
        size_t j = chargeIdx;
        for (int s = 0; s <= (int)tEnd && s < 3600; s++) {
            double ts = t0 + s;
            while (j + 1 < timex.size() && timex[j + 1] < ts) j++;
            double v;
            if (j + 1 >= timex.size())
                v = temp2[j];
            else {
                double a = timex[j], b = timex[j + 1];
                double f = b > a ? (ts - a) / (b - a) : 0;
                if (f < 0) f = 0;
                if (f > 1) f = 1;
                v = temp2[j] + (temp2[j + 1] - temp2[j]) * f;
            }
            out.table.push_back(std::isnan(v) ? (out.table.empty() ? 50.f : out.table.back()) : (float)v);
        }
    } else {
        for (float v : temp2) out.table.push_back(std::isnan(v) ? (out.table.empty() ? 50.f : out.table.back()) : v);
    }
    // events for display
    std::vector<float> comp;
    auto grab = [&](const char* key, PointTag tag) {
        size_t p = txt.find(key);
        if (p == std::string::npos) return;
        p = txt.find(':', p);
        if (p == std::string::npos) return;
        float v = strtof(txt.c_str() + p + 1, nullptr);
        if (v > 0 && v < out.table.size()) out.pts.push_back({(int)v, out.table[(int)v], tag});
    };
    out.pts.push_back({0, out.table.empty() ? 0 : out.table[0], PointTag::Charge});
    grab("DRY_time", PointTag::Dry);
    grab("FCs_time", PointTag::FCs);
    grab("FCe_time", PointTag::FCe);
    if (!out.table.empty()) out.pts.push_back({(int)out.table.size() - 1, out.table.back(), PointTag::Drop});
    std::stable_sort(out.pts.begin(), out.pts.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.t < b.t; });
    return out.table.size() > 10;
}

bool LoadCurveFile(const std::string& path, Curve& out)
{
    std::string txt = read_file(path);
    if (txt.empty()) return false;
    out.path = path;
    size_t slash = path.find_last_of('/');
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = base.find('.');
    out.name = dot == std::string::npos ? base : base.substr(0, dot);
    bool isJson = path.size() > 5 && strcasecmp(path.c_str() + path.size() - 5, ".json") == 0;
    if (isJson) return parse_json_curve(txt, out);
    return parse_alog_curve(txt, out);
}

std::vector<Curve> ListCurves(bool sdMounted)
{
    std::vector<Curve> v = BuiltinCurves();
    if (!sdMounted) return v;
    DIR* d = opendir(kCurveDir);
    if (!d) return v;
    std::vector<std::string> files;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        std::string lower = n;
        for (auto& ch : lower) ch = tolower((unsigned char)ch);
        if (lower.find(".json") != std::string::npos || lower.find(".alog") != std::string::npos)
            files.push_back(n);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        Curve c;
        if (LoadCurveFile(std::string(kCurveDir) + "/" + f, c)) v.push_back(std::move(c));
    }
    return v;
}

static std::string safe_filename(const std::string& name)
{
    std::string s;
    for (unsigned char ch : name) {
        if (isalnum(ch) || ch == '-' || ch == '_')
            s += (char)ch;
        else if (ch == ' ' || ch == '.')
            s += '_';
        else if (ch >= 0x80)
            continue;  // FAT long names are fine, but keep ASCII for portability
    }
    if (s.empty()) s = "curve";
    return s;
}

bool SaveCurveJson(Curve& c, std::string* err)
{
    if (!EnsureDir(kCurveDir)) {
        if (err) *err = std::string("Cannot create ") + kCurveDir;
        return false;
    }
    if (c.path.empty() || c.builtin || c.readOnly) {
        std::string base = safe_filename(c.name);
        std::string p    = std::string(kCurveDir) + "/" + base + ".json";
        for (int i = 2; i < 100; i++) {
            struct stat st;
            if (stat(p.c_str(), &st) != 0) break;
            p = std::string(kCurveDir) + "/" + base + "_" + std::to_string(i) + ".json";
        }
        c.path     = p;
        c.builtin  = false;
        c.readOnly = false;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", c.name.c_str());
    if (!c.note.empty()) cJSON_AddStringToObject(root, "note", c.note.c_str());
    cJSON_AddNumberToObject(root, "batch_g", c.batchG);
    if (c.totalT > 0) cJSON_AddNumberToObject(root, "total_s", c.totalT);
    cJSON* arr = cJSON_AddArrayToObject(root, "points");
    for (auto& p : c.pts) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "t", p.t);
        if (!std::isnan(p.c)) cJSON_AddNumberToObject(o, "c", std::round(p.c * 10) / 10.0);
        if (p.tag != PointTag::None) cJSON_AddStringToObject(o, "tag", TagKey(p.tag));
        cJSON_AddItemToArray(arr, o);
    }
    char* txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (!txt) return false;
    FILE* f = fopen(c.path.c_str(), "wb");
    bool ok = false;
    if (f) {
        ok = fputs(txt, f) >= 0;
        fclose(f);
    }
    cJSON_free(txt);
    if (!ok && err) *err = "Write failed: " + c.path;
    return ok;
}

}  // namespace roast
