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

static const char* kCurveDir = "/sdcard/roast/curves";

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
// Curve construction identical to the myERoast curve editor (pages/edit/edit.js, 1.0.18):
//   new curve = dark-roast preset; TOTAL resizes the table (truncate / extend with the last value);
//   moving a mark in time does not reshape the curve; INIT temperature blends in with a raised-cosine taper;
//   every mark temperature edit adds a linear ramp between the neighbouring anchors (_applyTempChangeAt).
// Edits are applied in the editor's list order: INIT, DRY, FCs, FCe, DROP, then free (untagged) points.
// ---------------------------------------------------------------------------------------------------------
namespace {
struct Anchor {
    int idx;
    double temp;
};

void apply_temp_change(std::vector<double>& a, int t, double oldT, double newT, std::vector<Anchor> r)
{
    int n = (int)a.size();
    if (t < 0) t = 0;
    if (t > n - 1) t = n - 1;
    double s = newT - oldT;
    if (r.empty()) return;
    std::vector<Anchor> o = r;
    std::stable_sort(o.begin(), o.end(), [](const Anchor& x, const Anchor& y) { return x.idx < y.idx; });
    std::vector<Anchor> u;
    for (auto& x : o)
        if (u.empty() || u.back().idx != x.idx) u.push_back(x);
    int h = -1;
    for (int i = 0; i < (int)u.size(); i++)
        if (u[i].idx == t) h = i;
    if (h < 0) return;
    int l = 0, c = n - 1;
    if (h > 0) l = u[h - 1].idx;
    if (h < (int)u.size() - 1) c = u[h + 1].idx;
    if (c <= l) return;
    std::vector<double> f = a;
    if (t > l)
        for (int i = l; i <= t; i++) f[i] = a[i] + s * (i - l) / (t - l);
    if (c > t)
        for (int i = t; i <= c; i++) f[i] = a[i] + s * (c - i) / (c - t);
    bool found = false;
    for (auto& x : r)
        if (x.idx == t) {
            x.temp = newT;
            found = true;
        }
    if (!found) r.push_back({t, newT});
    for (auto& x : r) f[x.idx] = x.temp;
    a.swap(f);
}
}  // namespace

void Curve::build()
{
    if (readOnly) return;
    sortPoints();
    table.clear();
    if (pts.empty()) return;
    int total = totalT > 0 ? totalT : pts.back().t;
    if (total <= 0 || total > 1800) return;

    // marks in the vendor list order (edit.js `s`)
    const PointTag order[6] = {PointTag::Dry, PointTag::FCs, PointTag::FCe, PointTag::SCs, PointTag::SCe, PointTag::Drop};
    const int presetT[6]    = {kPresetDark_DRY_t, kPresetDark_FCs_t, kPresetDark_FCe_t,
                               kPresetDark_SCs_t, kPresetDark_SCe_t, kPresetDark_DROP_t};
    int markT[6];
    double markC[6];
    for (int k = 0; k < 6; k++) {
        markT[k] = -1;
        markC[k] = NAN;
    }
    std::vector<CurvePoint> freePts;
    bool hasInit    = false;
    double initTemp = 0;
    for (auto& p : pts) {
        if (p.tag == PointTag::Charge || (p.t == 0 && p.tag == PointTag::None)) {
            if (!std::isnan(p.c)) {
                hasInit  = true;
                initTemp = p.c;
            }
            continue;
        }
        bool tagged = false;
        for (int k = 0; k < 6; k++)
            if (p.tag == order[k]) {
                markT[k] = p.t;
                markC[k] = p.c;
                tagged   = true;
            }
        if (!tagged && !std::isnan(p.c)) freePts.push_back(p);
    }
    if (markT[5] < 0) {  // the vendor DROP mark always exists (preset time, clamped to TOTAL)
        markT[5] = std::min(presetT[5], total);
        if (!freePts.empty() && freePts.back().t == markT[5]) {
            markC[5] = freePts.back().c;
            freePts.pop_back();
        }
    }
    for (int k = 0; k < 6; k++)
        if (markT[k] > total) markT[k] = total;

    std::vector<double> a(kPresetDarkLen);
    for (int i = 0; i < kPresetDarkLen; i++) a[i] = kPresetDark100[i] / 100.0;
    {  // TOTAL: resize (truncate, or extend with the last value)
        double last = a.back();
        a.resize(total + 1, last);
    }
    auto anchors = [&]() {
        std::vector<Anchor> r;
        if (hasInit) r.push_back({0, initTemp});
        for (int k = 0; k < 6; k++)
            if (markT[k] >= 0) {
                int i = std::min(std::max(markT[k], 0), (int)a.size() - 1);
                r.push_back({i, a[i]});
            }
        return r;
    };
    if (hasInit) {  // INIT: raised-cosine blend over max(60 s, 20 % of the chart span)
        double nmin  = total / 60.0;
        int maxTime  = 60 * (nmin <= 14 ? 14 : (int)std::ceil(nmin + 2));
        int o        = std::max(60, (int)std::lround(0.2 * maxTime));
        double first = a[0];
        for (int h = 0; h < (int)a.size() && h < o; h++) {
            double m = 0.5 * (1 + std::cos(M_PI * h / o));
            if (m < 0.01) continue;
            double v = a[h] + (initTemp - first) * m;
            a[h]     = v < 0 ? 0 : (v > 300 ? 300 : v);
        }
    }
    for (int k = 0; k < 6; k++) {  // temperature edits in list order; NAN = enabled mark used only as an anchor
        if (markT[k] < 0 || std::isnan(markC[k])) continue;
        int i = std::min(markT[k], (int)a.size() - 1);
        apply_temp_change(a, i, a[i], markC[k], anchors());
    }
    std::vector<Anchor> fp;  // free points: added after the marks, then edited
    for (auto& p : freePts) {
        int i = std::min(p.t, (int)a.size() - 1);
        fp.push_back({i, a[i]});
    }
    for (size_t j = 0; j < freePts.size(); j++) {
        auto r = anchors();
        for (auto& x : fp) r.push_back(x);
        int i = fp[j].idx;
        apply_temp_change(a, i, a[i], freePts[j].c, r);
        fp[j].temp = freePts[j].c;
    }
    table.resize(a.size());
    for (size_t i = 0; i < a.size(); i++) table[i] = (float)a[i];
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
    v.push_back(make_builtin("Gotiti 200g Light V3", "Drop 75-85 s after FC at +5-6 C, max 206 C; untested", 200,
                             {{0, 50, PointTag::Charge},
                              {285, 150, PointTag::Dry},
                              {470, 199, PointTag::FCs},
                              {570, 209, PointTag::Drop}}));
    v.push_back(make_builtin("Gotiti 200g Pullback", "+4 C in the 40 s after FC, latest drop 9:35; untested", 200,
                             {{0, 50, PointTag::Charge},
                              {285, 150, PointTag::Dry},
                              {470, 200, PointTag::FCs},
                              {510, 204, PointTag::FCe},
                              {575, 208, PointTag::Drop}}));
    v.push_back(make_builtin("Dry run (no beans)", "Low temperature check of PID and manual heat, 3 min", 0,
                             {{0, 50, PointTag::Charge}, {90, 90, PointTag::Dry}, {120, 100, PointTag::FCe}, {180, 110, PointTag::Drop}}));
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
        if (err) *err = "Cannot create /sdcard/roast/curves";
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
