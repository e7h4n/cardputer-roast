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

bool EnsureLogDir()
{
    return EnsureDir(kLogDir);
}

bool Session::begin(const std::string& id, const Curve* curve, float greenG, const std::string& bean)
{
    close();
    release();
    _active = _ioFailed = false;
    _sampleCount = 0;
    _plotStride = 1;
    _peakHeat = _fcRor = _fc30Ror = _dropRor = NAN;
    _errorSum = 0;
    _errorCount = _saturated = 0;
    _ctlSet = false;
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
    return openLog(id);
}

void Session::setControl(float heat, float ff, float p, float i, float d, float obs)
{
    _ctl[0] = heat;
    _ctl[1] = ff;
    _ctl[2] = p;
    _ctl[3] = i;
    _ctl[4] = d;
    _ctl[5] = obs;
    _ctlSet = true;
}

void Session::writeControl()
{
    if (_ctlSet)
        fprintf(_csv, ",%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", _ctl[0], _ctl[1], _ctl[2], _ctl[3], _ctl[4], _ctl[5]);
    else
        fputs(",,,,,,\n", _csv);
    _ctlSet = false;
}

void Session::addSample(const Sample& in)
{
    if (!_csv) return;
    Sample s = in;
    if (_recent.size() == 32) _recent.erase(_recent.begin());
    s.ror10 = INT16_MIN;
    if (_recent.size() >= 29 && s.t > 35)
        s.ror10 = 2 * std::abs(s.bt10 - _recent[_recent.size() - 29].bt10);
    _recent.push_back(s);

    // Keep evenly spaced points plus the latest sample as the chart grows.
    if (_sampleCount && (_sampleCount - 1) % _plotStride) _samples.pop_back();
    if (_samples.size() == 160) {
        for (size_t i = 0; i < 80; ++i) _samples[i] = _samples[2 * i];
        _samples.resize(80);
        _plotStride *= 2;
    }
    _samples.push_back(s);
    ++_sampleCount;
    const int fc = markT(Mark::FCs), drop = markT(Mark::Drop);
    if (fc >= 0 && s.t <= fc) _fcRor = rorNow();
    if (fc >= 0 && s.t <= fc + 30) _fc30Ror = rorNow();
    if (drop < 0 || s.t <= drop) {
        _peakHeat = std::fmax(_peakHeat, s.heat10 / 10.f);
        if (s.heat10 >= 950) ++_saturated;
        if (s.t >= 60 && s.tgt10 > 0) {
            _errorSum += std::abs(s.bt10 - s.tgt10) / 10.0;
            ++_errorCount;
        }
        if (drop >= 0) _dropRor = rorNow();
    }
    if (_ioFailed) return;
    char ror[12] = "";
    if (s.ror10 != INT16_MIN) snprintf(ror, sizeof(ror), "%.1f", s.ror10 / 10.0);
    fprintf(_csv, "%d,%.1f,%.1f,%.1f,%.1f,%s,%d,%d", s.t, s.bt10 / 10.0, s.tgt10 / 10.0, s.heat10 / 10.0,
            s.fan10 / 10.0, ror, s.pid, s.mtime);
    writeControl();
    if (ferror(_csv)) _ioFailed = true;
    if (++_sinceFlush >= 10) flush();
}

void Session::setMark(Mark m, int t, float bt)
{
    if (m == Mark::FCs) _fcRor = _fc30Ror = rorNow();
    if (m == Mark::Drop) _dropRor = rorNow();
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
    if (fc >= 0) {
        m.rorAtFC = _fcRor;
        if (lastT() >= fc + 30) m.rorFC30 = _fc30Ror;
        if (drop >= 0) {
            m.dev     = drop - fc;
            m.devPct  = drop > 0 ? 100.0f * m.dev / drop : NAN;
            m.deltaFC = _markBT[(int)Mark::Drop] - _markBT[(int)Mark::FCs];
        }
    }
    if (drop >= 0) m.rorAtDrop = _dropRor;
    m.maxHeat = _peakHeat;
    m.heatSatSec = _saturated;
    if (_errorCount) m.meanAbsErr = _errorSum / _errorCount;
    if (_greenG > 0 && _roastedG > 0) m.lossPct = 100.0f * (_greenG - _roastedG) / _greenG;
    return m;
}

bool Session::openLog(const std::string& id)
{
    close();
    if (!EnsureDir(kLogDir)) return false;
    std::string p = std::string(kLogDir) + "/" + id + ".csv";
    _csv          = fopen(p.c_str(), "w");
    if (!_csv) return false;
    fprintf(_csv, "# roast %s curve=%s green_g=%.1f bean=%s\n", id.c_str(), curveName.c_str(), _greenG,
            _bean.c_str());
    fprintf(_csv, "t,bt,target,heat,fan,ror,pid,machine_t,heat_cmd,ff,p,i,d,obs\n");
    for (auto& a : _actions) fprintf(_csv, "#action,%d,%s\n", a.t, a.text.c_str());
    flush();
    if (_ioFailed) {
        close();
        _ioFailed = false;
        return false;
    }
    _active = true;
    _id = id;
    _recent.reserve(32);
    _samples.reserve(160);
    return true;
}

void Session::flush()
{
    if (_csv) {
        if (fflush(_csv) != 0 || fsync(fileno(_csv)) != 0 || ferror(_csv)) _ioFailed = true;
    }
    _sinceFlush = 0;
}

void Session::close()
{
    if (_csv) {
        if (ferror(_csv)) _ioFailed = true;
        if (fclose(_csv) != 0) _ioFailed = true;
        _csv = nullptr;
    }
}

static void add_num(cJSON* o, const char* k, double v, int decimals = 1)
{
    if (std::isnan(v)) return;
    double p = pow(10, decimals);
    cJSON_AddNumberToObject(o, k, std::round(v * p) / p);
}

// FatFs may reject renaming over an existing file. Keep the previous export
// until the replacement has been closed and can be published.
static bool publishExport(const std::string& tmp, const std::string& path)
{
    if (rename(tmp.c_str(), path.c_str()) == 0) return true;
    const std::string previous = path + ".bak";
    if (rename(path.c_str(), previous.c_str()) != 0) return false;
    if (rename(tmp.c_str(), path.c_str()) == 0) {
        remove(previous.c_str());
        return true;
    }
    rename(previous.c_str(), path.c_str());
    return false;
}

bool Session::finalize(std::string* err)
{
    if (_id.empty()) {
        if (err) *err = "SD card log unavailable";
        return false;
    }
    if (_csv) {
        fprintf(_csv, "#roasted_g,%.1f\n", _roastedG);
        close();
    }
    if (_ioFailed) {
        if (err) *err = "SD card log write failed";
        return false;
    }
    RoastMetrics m = metrics();

    const std::string base = std::string(kLogDir) + "/" + _id;
    const std::string jsonTmp = base + ".json.tmp", alogTmp = base + ".alog.tmp";
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
        FILE* f = fopen(jsonTmp.c_str(), "wb");
        if (f) {
            ok = fputs(txt, f) >= 0;
            if (fclose(f) != 0) ok = false;
        }
        cJSON_free(txt);
    }

    // ---- .alog: streamed JSON (no booleans/null, so it is also a valid Python literal for Artisan).
    // Built with fprintf: a cJSON tree of ~3600 numbers would not fit in the heap next to BLE.
    {
        FILE* f = ok ? fopen(alogTmp.c_str(), "wb") : nullptr;
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
            fprintf(f, "{\"version\": \"3.0.2\", \"mode\": \"C\", \"title\": %s, \"beans\": %s, ",
                    jstr(_bean.empty() ? curveName : _bean).c_str(), jstr(_bean).c_str());
            fprintf(f, "\"roastertype\": \"E-Roast\", \"samplinginterval\": 1.0, \"weight\": [%.1f, %.1f, \"g\"], ",
                    _greenG, _roastedG);
            const char* keys[3] = {"timex", "temp1", "temp2"};
            FILE* csv = fopen((base + ".csv").c_str(), "r");
            int ti[8] = {};
            const int slots[] = {1, 2, 3, 6};
            for (int j = 0; j < 4; ++j)
                if (m.markT[j] >= 0 && _sampleCount) ti[slots[j]] = _sampleCount - 1;
            float firstBT = NAN;
            for (int k = 0; k < 3; ++k) {
                fprintf(f, "\"%s\": [", keys[k]);
                size_t count = 0;
                int previousT = -1;
                auto write = [&](int t, float target, float bt) {
                    if (k == 0) {
                        if (!count) firstBT = bt;
                        for (int j = 0; j < 4; ++j)
                            if (previousT < m.markT[j] && t >= m.markT[j]) ti[slots[j]] = count;
                        previousT = t;
                        fprintf(f, "%s%d.0", count ? ", " : "", t);
                    } else {
                        fprintf(f, "%s%.1f", count ? ", " : "", k == 1 ? target : bt);
                    }
                    ++count;
                };
                if (csv && fseek(csv, 0, SEEK_SET) == 0) {
                    char line[256];
                    while (fgets(line, sizeof line, csv)) {
                        bool data = line[0] >= '0' && line[0] <= '9';
                        if (!strchr(line, '\n')) {
                            int c;
                            while ((c = fgetc(csv)) != '\n' && c != EOF) {}
                            if (data) ok = false;
                            continue;
                        }
                        if (!data) continue;
                        int t;
                        float bt, target;
                        if (sscanf(line, "%d,%f,%f,", &t, &bt, &target) != 3) { ok = false; break; }
                        write(t, target, bt);
                    }
                    if (ferror(csv)) ok = false;
                } else ok = false;
                if (count != _sampleCount) ok = false;
                fprintf(f, "], ");
            }
            if (csv) fclose(csv);
            fprintf(f, "\"timeindex\": [%d, %d, %d, %d, %d, %d, %d, %d], ", ti[0], ti[1], ti[2], ti[3], ti[4], ti[5],
                    ti[6], ti[7]);
            fprintf(f, "\"computed\": {");
            if (!std::isnan(firstBT)) fprintf(f, "\"CHARGE_BT\": %.1f, ", firstBT);
            const char* ck[] = {"DRY", "FCs", "FCe", "DROP"};
            for (int i = 0; i < 4; i++) {
                if (m.markT[i] < 0) continue;
                fprintf(f, "\"%s_time\": %d, \"%s_BT\": %.1f, ", ck[i], m.markT[i], ck[i], m.markBT[i]);
            }
            if (!std::isnan(m.lossPct)) fprintf(f, "\"weight_loss\": %.1f, ", m.lossPct);
            fprintf(f, "\"totaltime\": %d}}\n", m.total);
            if (ferror(f)) ok = false;
            if (fclose(f) != 0) ok = false;
        }
    }
    if (ok) ok = publishExport(alogTmp, base + ".alog") && publishExport(jsonTmp, base + ".json");
    if (!ok) {
        remove(alogTmp.c_str());
        remove(jsonTmp.c_str());
        if (err) *err = "SD card log export failed";
    }
    return ok;
}

}  // namespace roast
