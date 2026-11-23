/*
 * One roast: 1 Hz samples, marks, operator actions, derived metrics and SD-card output.
 */
#pragma once
#include "roast_curve.h"
#include <string>
#include <vector>
#include <cstdio>
#include <cmath>

namespace roast {

enum class Mark : uint8_t { Dry = 0, FCs, FCe, Drop, Count };

const char* MarkName(Mark m);

struct Sample {
    int16_t t;       // seconds after charge
    int16_t bt10;    // bean temp x10
    int16_t tgt10;   // the Cardputer PID's target x10
    int16_t heat10;  // heater output x10
    int16_t fan10;   // fan setpoint x10
    int16_t ror10;   // RoR degC/min x10 (INT16_MIN when n/a)
    int16_t mtime;   // machine timer reg 2
    uint8_t pid;
};

struct Action {
    int t;
    std::string text;
};

struct RoastMetrics {
    int total = 0;
    int markT[(int)Mark::Count];
    float markBT[(int)Mark::Count];
    int dev           = -1;
    float devPct      = NAN;
    float deltaFC     = NAN;
    float rorAtFC     = NAN;
    float rorFC30     = NAN;
    float rorAtDrop   = NAN;
    float maxHeat     = NAN;
    int heatSatSec    = 0;
    float meanAbsErr  = NAN;
    float lossPct     = NAN;
    RoastMetrics()
    {
        for (int i = 0; i < (int)Mark::Count; i++) {
            markT[i]  = -1;
            markBT[i] = NAN;
        }
    }
};

class Session {
public:
    void reset(const Curve* curve, float greenG, const std::string& bean);
    bool active() const
    {
        return _active;
    }
    void addSample(const Sample& s);
    /// Cardputer PID terms for the next sample's CSV row (heat_cmd,ff,p,i,d,obs); only on SD, not kept in memory.
    /// obs: degC/min the self-heating estimate took off the feedforward's rate.
    void setControl(float heat, float ff, float p, float i, float d, float obs = 0);
    const std::vector<Sample>& samples() const
    {
        return _samples;
    }
    const std::vector<Sample>& recent() const { return _diskHistory ? _recent : _samples; }
    int lastT() const
    {
        return recent().empty() ? 0 : recent().back().t;
    }
    float lastBT() const
    {
        return recent().empty() ? NAN : recent().back().bt10 / 10.0f;
    }
    /// myERoast RoR from the latest 30 measurements, in degrees C per minute.
    float rorNow() const
    {
        return recent().empty() || recent().back().ror10 == INT16_MIN ? NAN : recent().back().ror10 / 10.0f;
    }

    void setMark(Mark m, int t, float bt);
    void clearMark(Mark m);
    int markT(Mark m) const
    {
        return _markT[(int)m];
    }
    float markBT(Mark m) const
    {
        return _markBT[(int)m];
    }
    void addAction(int t, const std::string& text);
    const std::vector<Action>& actions() const
    {
        return _actions;
    }

    void setRoastedWeight(float g)
    {
        _roastedG = g;
    }
    float roastedWeight() const
    {
        return _roastedG;
    }
    float greenWeight() const
    {
        return _greenG;
    }
    void setGreenWeight(float g)
    {
        _greenG = g;
    }
    void setNote(const std::string& n)
    {
        _note = n;
    }

    RoastMetrics metrics() const;

    /// Opens the CSV log on the SD card. Returns false (and keeps running in memory) if no card.
    bool openLog(const std::string& id);
    void flush();
    /// Writes <id>.json summary and <id>.alog (Artisan/myERoast importable); closes the CSV.
    bool finalize(std::string* err);
    const std::string& logId() const
    {
        return _id;
    }
    bool logging() const
    {
        return _csv != nullptr;
    }
    void close();
    /// Frees the samples and actions (after the log is written): sync needs the heap.
    void release()
    {
        std::vector<Sample>().swap(_samples);
        std::vector<Action>().swap(_actions);
        std::vector<Sample>().swap(_recent);
    }
    void end()
    {
        _active = false;
    }

    std::string curveName;
    int curveEnd = 0;

private:
    bool _active = false;
    std::vector<Sample> _samples;
    std::vector<Action> _actions;
    int _markT[(int)Mark::Count];
    float _markBT[(int)Mark::Count];
    float _greenG   = 0;
    float _roastedG = 0;
    std::string _bean;
    std::string _note;
    std::string _id;
    FILE* _csv       = nullptr;
    int _sinceFlush  = 0;
    bool _ctlSet     = false;
    void writeControl();
    float _ctl[6]    = {};
    // With an SD log, _samples is a bounded plot and _recent is the full-rate control window.
    std::vector<Sample> _recent;
    size_t _sampleCount = 0, _plotStride = 1;
    bool _diskHistory = false, _ioFailed = false;
    float _peakHeat = NAN, _fcRor = NAN, _fc30Ror = NAN, _dropRor = NAN;
    double _errorSum = 0;
    int _errorCount = 0, _saturated = 0;
};

std::string NextLogId();
/// Creates the log directory if needed.
bool EnsureLogDir();

}  // namespace roast
