/*
 * Roast curves: keypoints (time s, bean temperature degC) expanded to a 1 Hz PID target table with the
 * same monotone cubic (Fritsch-Carlson) shaping the vendor app's curve editor uses.
 *
 * SD card layout (FAT, mounted at /sdcard):
 *   /sdcard/roast/curves/<name>.json   keypoint curves (editable on the device)
 *   /sdcard/roast/curves/<name>.alog   Artisan / myERoast exports (temp2 used as target, read only)
 *   /sdcard/roast/logs/                roast logs written by the app (.csv, .json, .alog)
 */
#pragma once
#include <cmath>
#include <string>
#include <vector>
#include <cstdint>

namespace roast {

enum class PointTag : uint8_t { None, Charge, Dry, FCs, FCe, SCs, SCe, Drop };

struct CurvePoint {
    int t       = 0;  // seconds after charge
    float c     = 0;  // target bean temperature; NAN = mark enabled but temperature not edited (anchor only)
    PointTag tag = PointTag::None;
};

/// Development plan of a curve, from its first crack and drop points: how long (`dur`), how fast (`speed`, the rise
/// divided by the time) and how hot at most (`dropBT`). After the F mark Roast rises at `speed` from where the beans
/// cracked, and drops when `dur` is over or `dropBT` is reached.
struct DevPlan {
    bool valid   = false;
    int dur      = 0;     // s
    float rise   = 0;     // degC (speed * dur)
    float speed  = 0;     // degC/s
    float dropBT = NAN;
    int dropT    = -1;    // the curve's drop time: without an F mark Roast drops here (or at dropBT)
};

struct Curve {
    std::string name;
    std::string note;
    std::string path;  // empty for built-ins
    int batchG    = 0;
    int totalT    = -1;  // total roast time (curve length, s); <0 = last point
    bool builtin  = false;
    bool readOnly = false;  // .alog tables cannot be edited as keypoints
    std::vector<CurvePoint> pts;
    std::vector<float> table;  // 1 Hz target, index = seconds after charge
    float offset = 0;          // temperature compensation (myERoast tempCompensation): shifts the whole curve, ±30 degC

    bool valid() const
    {
        return !table.empty();
    }
    int endTime() const
    {
        return table.empty() ? 0 : (int)table.size() - 1;
    }
    float at(int t) const;
    int tagTime(PointTag tag) const;  // -1 if absent
    float tagTemp(PointTag tag) const;
    void build();  // keypoints -> table (Charge, FCs, Drop; see roast_curve.cpp)
    DevPlan devPlan() const;
    void sortPoints();
};

const char* TagName(PointTag tag);
const char* TagKey(PointTag tag);
PointTag TagFromKey(const std::string& k);

std::vector<Curve> BuiltinCurves();
/// Built-ins first, then files from /sdcard/roast/curves (sorted by name).
std::vector<Curve> ListCurves(bool sdMounted);
bool LoadCurveFile(const std::string& path, Curve& out);
bool SaveCurveJson(Curve& c, std::string* err);  // writes c.path (creates it from the name if empty)
bool EnsureDir(const char* path);

std::string FormatMmSs(int sec);
bool ParseMmSs(const std::string& s, int& sec);

}  // namespace roast
