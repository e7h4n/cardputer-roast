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
    int16_t tgt10;   // target written x10 (0 when no curve)
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
    const std::vector<Sample>& samples() const
    {
        return _samples;
    }
    int lastT() const
    {
        return _samples.empty() ? 0 : _samples.back().t;
    }
    float lastBT() const
    {
        return _samples.empty() ? NAN : _samples.back().bt10 / 10.0f;
    }
    /// Least-squares slope over the last 30 s, degC/min. NAN when not enough data.
    float rorAt(int idx) const;
    float rorNow() const
    {
        return _samples.empty() ? NAN : rorAt((int)_samples.size() - 1);
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
    std::vector<float> _curveTable;
};

std::string NextLogId();

}  // namespace roast
