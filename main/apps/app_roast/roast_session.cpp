#include "roast_session.h"
#include <hal/utils/settings/settings.h>
#include <cJSON.h>
#include <climits>
#include <ctime>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/stat.h>

namespace roast {

#ifndef ROAST_SD_ROOT
#define ROAST_SD_ROOT "/sdcard"
#endif
static const char* kLogDir = ROAST_SD_ROOT "/roast/logs";
static const size_t kMaxSamples = 1200;  // 20 minutes at 1 Hz

const char* MarkName(Mark m)
{
    switch (m) {
        case Mark::Dry:
            return "Dry";
        case Mark::FCs:
            return "FC";
        case Mark::FCe:
            return "FC end";
        case Mark::Drop:
            return "Drop";
        default:
            return "?";
    }
}

std::string NextLogId()
{
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char b[32];
    if (tmv.tm_year + 1900 >= 2025) {
        strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tmv);
        return b;
    }
    // Counter in NVS; skip numbers whose files already exist (NVS can be wiped, e.g. when Launcher is installed).
    Settings s("roast", true);
    int32_t seq = s.GetInt("logseq", 0);
    for (int guard = 0; guard < 10000; guard++) {
        seq++;
        snprintf(b, sizeof(b), "R%04d", (int)seq);
        struct stat st;
        std::string p = std::string(kLogDir) + "/" + b + ".csv";
        std::string q = std::string(kLogDir) + "/" + b + ".json";
        if (stat(p.c_str(), &st) != 0 && stat(q.c_str(), &st) != 0) break;
    }
    s.SetInt("logseq", seq);
    return b;
}

void Session::reset(const Curve* curve, float greenG, const std::string& bean)
{
    close();
    _samples.clear();
    _samples.reserve(kMaxSamples);  // ~19 KB; with BLE up the largest free block is ~31 KB
    _actions.clear();
    for (int i = 0; i < (int)Mark::Count; i++) {
        _markT[i]  = -1;
        _markBT[i] = NAN;
    }
    _greenG   = greenG;
    _roastedG = 0;
    _bean     = bean;
    _note.clear();
    _id.clear();
    curveName = curve ? curve->name : "";
    curveEnd  = curve ? curve->endTime() : 0;
    _curveTable.clear();
    if (curve) _curveTable = curve->table;
    _active = true;
}

float Session::rorAt(int idx) const
{
    // Same as myERoast (app.js parseTemperatureData): 30 one-second readings, |BT(now) - BT(29 readings ago)| / 0.5,
    // shown only after 35 s. Kept identical so numbers match the phone app's RoR.
    if (idx < 29 || idx >= (int)_samples.size() || _samples[idx].t <= 35) return NAN;
    return fabsf((_samples[idx].bt10 - _samples[idx - 29].bt10) / 10.0f) / 0.5f;
}

void Session::addSample(const Sample& in)
{
    Sample s = in;
    if (_samples.size() >= kMaxSamples) {
        // Never grow past the reservation (a vector doubling would need ~38 KB and abort). Keep logging to SD only.
        if (_csv) fprintf(_csv, "%d,%.1f,%.1f,%.1f,%.1f,,%d,%d\n", s.t, s.bt10 / 10.0, s.tgt10 / 10.0, s.heat10 / 10.0,
                          s.fan10 / 10.0, s.pid, s.mtime);
        return;
    }
    _samples.push_back(s);
    float r                = rorAt((int)_samples.size() - 1);
    _samples.back().ror10  = std::isnan(r) ? INT16_MIN : (int16_t)lroundf(r * 10);
    if (_csv) {
        const Sample& x = _samples.back();
        char ror[12] = "";
        if (x.ror10 != INT16_MIN) snprintf(ror, sizeof(ror), "%.1f", x.ror10 / 10.0);
        fprintf(_csv, "%d,%.1f,%.1f,%.1f,%.1f,%s,%d,%d\n", x.t, x.bt10 / 10.0, x.tgt10 / 10.0, x.heat10 / 10.0,
                x.fan10 / 10.0, ror, x.pid, x.mtime);
        if (++_sinceFlush >= 10) flush();
    }
}

void Session::setMark(Mark m, int t, float bt)
{
    _markT[(int)m]  = t;
    _markBT[(int)m] = bt;
    if (_csv) fprintf(_csv, "#mark,%d,%s,%.1f\n", t, MarkName(m), bt);
}

void Session::clearMark(Mark m)
{
    if (_csv && _markT[(int)m] >= 0) fprintf(_csv, "#unmark,%d,%s\n", _markT[(int)m], MarkName(m));
    _markT[(int)m]  = -1;
    _markBT[(int)m] = NAN;
}

void Session::addAction(int t, const std::string& text)
{
    _actions.push_back({t, text});
    if (_csv) fprintf(_csv, "#action,%d,%s\n", t, text.c_str());
}

RoastMetrics Session::metrics() const
{
    RoastMetrics m;
    for (int i = 0; i < (int)Mark::Count; i++) {
        m.markT[i]  = _markT[i];
        m.markBT[i] = _markBT[i];
    }
    int drop = _markT[(int)Mark::Drop];
    m.total  = drop >= 0 ? drop : lastT();
    int fc   = _markT[(int)Mark::FCs];
    auto rorNear = [&](int t) -> float {
        for (int i = (int)_samples.size() - 1; i >= 0; i--)
            if (_samples[i].t <= t) return _samples[i].ror10 == INT16_MIN ? NAN : _samples[i].ror10 / 10.0f;
        return NAN;
    };
    if (fc >= 0) {
        m.rorAtFC = rorNear(fc);
        if (lastT() >= fc + 30) m.rorFC30 = rorNear(fc + 30);
        if (drop >= 0) {
            m.dev     = drop - fc;
            m.devPct  = drop > 0 ? 100.0f * m.dev / drop : NAN;
            m.deltaFC = _markBT[(int)Mark::Drop] - _markBT[(int)Mark::FCs];
        }
    }
    if (drop >= 0) m.rorAtDrop = rorNear(drop);
    float mh   = -1;
    int sat    = 0;
    double err = 0;
    int nerr   = 0;
    for (auto& s : _samples) {
        if (drop >= 0 && s.t > drop) break;
        mh = std::max(mh, s.heat10 / 10.0f);
        if (s.heat10 >= 950) sat++;
        if (s.t >= 60 && s.tgt10 > 0) {
            err += fabs((s.bt10 - s.tgt10) / 10.0);
            nerr++;
        }
    }
    if (mh >= 0) m.maxHeat = mh;
    m.heatSatSec = sat;
    if (nerr) m.meanAbsErr = (float)(err / nerr);
    if (_greenG > 0 && _roastedG > 0) m.lossPct = 100.0f * (_greenG - _roastedG) / _greenG;
    return m;
}

bool Session::openLog(const std::string& id)
{
    close();
    _id = id;
    if (!EnsureDir(kLogDir)) return false;
    std::string p = std::string(kLogDir) + "/" + id + ".csv";
    _csv          = fopen(p.c_str(), "w");
    if (!_csv) return false;
    fprintf(_csv, "# roast %s curve=%s green_g=%.1f bean=%s\n", id.c_str(), curveName.c_str(), _greenG,
            _bean.c_str());
    fprintf(_csv, "t,bt,target,heat,fan,ror,pid,machine_t\n");
    for (auto& a : _actions) fprintf(_csv, "#action,%d,%s\n", a.t, a.text.c_str());
    flush();
    return true;
}

void Session::flush()
{
    if (_csv) {
        fflush(_csv);
        fsync(fileno(_csv));
    }
    _sinceFlush = 0;
}

void Session::close()
{
    if (_csv) {
        fclose(_csv);
        _csv = nullptr;
    }
}

static void add_num(cJSON* o, const char* k, double v, int decimals = 1)
{
    if (std::isnan(v)) return;
    double p = pow(10, decimals);
    cJSON_AddNumberToObject(o, k, std::round(v * p) / p);
}

bool Session::finalize(std::string* err)
{
    if (_id.empty()) _id = NextLogId();
    if (!EnsureDir(kLogDir)) {
        if (err) *err = "No SD card: log kept in memory only";
        return false;
    }
    if (_csv) {
        fprintf(_csv, "#roasted_g,%.1f\n", _roastedG);
        close();
    }
    RoastMetrics m = metrics();

    // ---- summary json ----
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", _id.c_str());
    cJSON_AddStringToObject(root, "app", "Cardputer Roast 1.0");
    cJSON_AddStringToObject(root, "curve", curveName.c_str());
    if (!_bean.empty()) cJSON_AddStringToObject(root, "bean", _bean.c_str());
    if (!_note.empty()) cJSON_AddStringToObject(root, "note", _note.c_str());
    add_num(root, "green_g", _greenG);
    if (_roastedG > 0) add_num(root, "roasted_g", _roastedG);
    add_num(root, "loss_pct", m.lossPct);
    cJSON* marks = cJSON_AddObjectToObject(root, "marks");
    const char* keys[] = {"dry", "fc_start", "fc_end", "drop"};
    for (int i = 0; i < (int)Mark::Count; i++) {
        if (m.markT[i] < 0) continue;
        cJSON* o = cJSON_AddObjectToObject(marks, keys[i]);
        cJSON_AddNumberToObject(o, "t", m.markT[i]);
        cJSON_AddStringToObject(o, "mmss", FormatMmSs(m.markT[i]).c_str());
        add_num(o, "bt", m.markBT[i]);
    }
    cJSON_AddNumberToObject(root, "total_s", m.total);
    if (m.dev >= 0) cJSON_AddNumberToObject(root, "dev_s", m.dev);
    add_num(root, "dev_pct", m.devPct);
    add_num(root, "delta_fc_to_drop", m.deltaFC);
    add_num(root, "ror_at_fc", m.rorAtFC);
    add_num(root, "ror_fc_plus30", m.rorFC30);
    add_num(root, "ror_at_drop", m.rorAtDrop);
    add_num(root, "max_heat_pct", m.maxHeat);
    cJSON_AddNumberToObject(root, "heat_saturated_s", m.heatSatSec);
    add_num(root, "mean_abs_err_after_60s", m.meanAbsErr);
    cJSON* acts = cJSON_AddArrayToObject(root, "actions");
    for (auto& a : _actions) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "t", a.t);
        cJSON_AddStringToObject(o, "what", a.text.c_str());
        cJSON_AddItemToArray(acts, o);
    }
    char* txt = cJSON_Print(root);
    cJSON_Delete(root);
    bool ok = false;
    if (txt) {
        std::string p = std::string(kLogDir) + "/" + _id + ".json";
        FILE* f       = fopen(p.c_str(), "wb");
        if (f) {
            ok = fputs(txt, f) >= 0;
            fclose(f);
        }
        cJSON_free(txt);
    }

    // ---- .alog: streamed JSON (no booleans/null, so it is also a valid Python literal for Artisan).
    // Built with fprintf: a cJSON tree of ~3600 numbers would not fit in the heap next to BLE.
    {
        std::string p = std::string(kLogDir) + "/" + _id + ".alog";
        FILE* f       = fopen(p.c_str(), "wb");
        if (!f) {
            ok = false;
        } else {
            auto jstr = [](const std::string& v) {
                std::string o = "\"";
                for (unsigned char ch : v) {
                    if (ch == '"' || ch == '\\') o += '\\';
                    if (ch < 0x20) continue;
                    o += (char)ch;
                }
                return o + "\"";
            };
            auto idxOf = [&](int t) -> int {
                if (t < 0) return 0;
                for (size_t i = 0; i < _samples.size(); i++)
                    if (_samples[i].t >= t) return (int)i;
                return (int)_samples.size() - 1;
            };
            fprintf(f, "{\"version\": \"3.0.2\", \"mode\": \"C\", \"title\": %s, \"beans\": %s, ",
                    jstr(_bean.empty() ? curveName : _bean).c_str(), jstr(_bean).c_str());
            fprintf(f, "\"roastertype\": \"E-Roast\", \"samplinginterval\": 1.0, \"weight\": [%.1f, %.1f, \"g\"], ",
                    _greenG, _roastedG);
            const char* keys[3] = {"timex", "temp1", "temp2"};
            for (int k = 0; k < 3; k++) {
                fprintf(f, "\"%s\": [", keys[k]);
                for (size_t i = 0; i < _samples.size(); i++) {
                    const Sample& x = _samples[i];
                    if (k == 0)
                        fprintf(f, "%s%d.0", i ? ", " : "", x.t);
                    else
                        fprintf(f, "%s%.1f", i ? ", " : "", (k == 1 ? x.tgt10 : x.bt10) / 10.0);
                }
                fprintf(f, "], ");
            }
            int ti[8] = {0, m.markT[0] >= 0 ? idxOf(m.markT[0]) : 0, m.markT[1] >= 0 ? idxOf(m.markT[1]) : 0,
                         m.markT[2] >= 0 ? idxOf(m.markT[2]) : 0, 0, 0, m.markT[3] >= 0 ? idxOf(m.markT[3]) : 0, 0};
            fprintf(f, "\"timeindex\": [%d, %d, %d, %d, %d, %d, %d, %d], ", ti[0], ti[1], ti[2], ti[3], ti[4], ti[5],
                    ti[6], ti[7]);
            fprintf(f, "\"computed\": {");
            if (!_samples.empty()) fprintf(f, "\"CHARGE_BT\": %.1f, ", _samples.front().bt10 / 10.0);
            const char* ck[] = {"DRY", "FCs", "FCe", "DROP"};
            for (int i = 0; i < 4; i++) {
                if (m.markT[i] < 0) continue;
                fprintf(f, "\"%s_time\": %d, \"%s_BT\": %.1f, ", ck[i], m.markT[i], ck[i], m.markBT[i]);
            }
            if (!std::isnan(m.lossPct)) fprintf(f, "\"weight_loss\": %.1f, ", m.lossPct);
            fprintf(f, "\"totaltime\": %d}}\n", m.total);
            ok = (fclose(f) == 0) && ok;
        }
    }
    if (!ok && err) *err = "SD card write failed";
    return ok;
}

}  // namespace roast
