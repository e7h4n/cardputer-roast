// Full-rate exports and the bounded working sets used with an SD card.
#include <apps/app_roast/roast_session.h>
#include <cJSON.h>
#include <cassert>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

using namespace roast;

static std::string contents(const std::string& path)
{
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

static void checkCurves()
{
    auto curve = BuiltinCurves().front();
    curve.name = "Saved curve";
    std::string error;
    assert(SaveCurveJson(curve, &error));
    const auto catalog = ListCurves(true);
    for (const auto& entry : catalog) assert(entry.table.empty());
    auto preview = catalog.back();
    assert(preview.name.empty() && preview.path == curve.path);
    assert(LoadPreview(preview));
    assert(preview.table.size() < curve.table.size() && preview.endTime() == curve.endTime());
    Curve selected;
    assert(OpenLibraryCurve(preview, selected));
    assert(selected.table == curve.table);
    for (const auto& path : {curve.path, std::string(), std::string("sd/roast/curves/missing.json")}) {
        RestoreCurve(true, curve.name, path, selected);
        assert(selected.name == curve.name && selected.path == curve.path && selected.table == curve.table);
    }
}

static Sample sample(int t)
{
    Sample s{};
    s.t = t;
    s.bt10 = 900 + t + (t % 9);
    s.tgt10 = s.bt10 + 21;
    s.heat10 = (t % 7 == 0) ? 980 : 500;
    s.fan10 = 650;
    return s;
}

int main()
{
    checkCurves();
    Session session;
    session.reset(nullptr, 200, "test");
    assert(session.openLog("long"));
    const int fc = 437, drop = 1323, count = 1473;
    for (int t = 0; t < count; ++t) {
        const auto s = sample(t);
        if (t == fc) session.setMark(Mark::FCs, t, s.bt10 / 10.f);
        if (t == drop) session.setMark(Mark::Drop, t, s.bt10 / 10.f);
        session.addSample(s);
        assert(session.recent().size() <= 32 && session.samples().capacity() <= 160);
        assert(session.samples().front().t == 0 && session.samples().back().t == t);
        if (t > 35) assert(session.recent().back().ror10 == 2 * (s.bt10 - sample(t - 29).bt10));
    }
    const auto metrics = session.metrics();
    assert(metrics.total == drop && metrics.dev == drop - fc && metrics.heatSatSec == 1 + drop / 7);
    assert(std::fabs(metrics.meanAbsErr - 2.1f) < 0.001f && metrics.maxHeat == 98);
    auto ror = [](int t) { return 2 * (sample(t).bt10 - sample(t - 29).bt10) / 10.f; };
    assert(metrics.rorAtFC == ror(fc) && metrics.rorFC30 == ror(fc + 30) && metrics.rorAtDrop == ror(drop));
    std::string error;
    assert(session.finalize(&error));
    session.setRoastedWeight(170);
    session.setNote("edited after saving");
    assert(session.finalize(&error));
    const std::string base = "sd/roast/logs/long";
    const auto summary = contents(base + ".json"), artisan = contents(base + ".alog");
    cJSON* j = cJSON_Parse(summary.c_str());
    assert(j && cJSON_GetObjectItem(j, "loss_pct")->valueint == 15);
    cJSON_Delete(j);
    j = cJSON_Parse(artisan.c_str());
    assert(j);
    for (const std::string key : {"timex", "temp1", "temp2"}) {
        auto* series = cJSON_GetObjectItem(j, key.c_str());
        assert(cJSON_GetArraySize(series) == count);
        int t = 0;
        for (auto* item = series->child; item; item = item->next, ++t) {
            const auto s = sample(t);
            const double want = key == "timex" ? t : (key == "temp1" ? s.tgt10 : s.bt10) / 10.;
            assert(std::fabs(item->valuedouble - want) < 0.00001);
        }
    }
    auto* ti = cJSON_GetObjectItem(j, "timeindex");
    assert(cJSON_GetArrayItem(ti, 2)->valueint == fc && cJSON_GetArrayItem(ti, 6)->valueint == drop);
    cJSON_Delete(j);
    // A missing tail on the card must never replace the last good pair of exports.
    std::ofstream(base + ".csv", std::ios::trunc) << "0,90,92.1,";
    assert(!session.finalize(&error) && !error.empty());
    assert(contents(base + ".json") == summary && contents(base + ".alog") == artisan);

    assert(rename("sd", "card-removed") == 0);
    std::ofstream("sd") << "mount unavailable";
    session.reset(nullptr, 0, "");
    assert(!session.openLog("fallback"));
    for (int t = 0; t < count; ++t) session.addSample(sample(t));
    assert(session.samples().size() == 1200 && session.lastT() == 1199);
    assert(!session.finalize(&error));
    puts("SD history: 1473 full-rate rows, preview selection, export retry and RAM fallback passed");
}
