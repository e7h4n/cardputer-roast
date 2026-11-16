// Host unit tests for the firmware update check (roast_update.h): versions and latest.json.
//   tools/tests/run.sh
#include <apps/app_roast/roast_update.h>
#include <cstdio>
#include <string>

using namespace roast::update;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (cond) {                                                \
            g_pass++;                                              \
        } else {                                                   \
            g_fail++;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

int main()
{
    Version v;
    CHECK(ParseRelease("v0.3.47", v) && v.major == 0 && v.minor == 3 && v.patch == 47);
    CHECK(ParseRelease("1.2.3", v) && v.major == 1 && v.patch == 3);
    CHECK(!ParseRelease("v0.2.0-12-g26d8161", v));  // git describe of a local build
    CHECK(!ParseRelease("26d8161", v));             // PR build: the short hash
    CHECK(!ParseRelease("dev", v));
    CHECK(!ParseRelease("v0.3", v));
    CHECK(!ParseRelease("v0.3.47 ", v));
    CHECK(!ParseRelease("", v));

    Version a{0, 3, 9}, b{0, 3, 10}, c{0, 2, 99}, d{1, 0, 0};
    CHECK(Compare(a, b) < 0);  // numeric, not text: 10 > 9
    CHECK(Compare(b, c) > 0);
    CHECK(Compare(d, b) > 0);
    CHECK(Compare(a, a) == 0);

    CHECK(Offer("v0.3.46", "v0.3.47"));
    CHECK(!Offer("v0.3.47", "v0.3.47"));
    CHECK(!Offer("v0.3.48", "v0.3.47"));     // a newer build than the latest release: nothing to do
    CHECK(Offer("v0.2.0", "v0.3.47"));       // the old manual releases
    CHECK(Offer("v0.2.0-12-g26d8161", "v0.3.47"));  // not a release: any release is an option
    CHECK(Offer("dev", "v0.3.47"));
    CHECK(!Offer("dev", "26d8161"));         // never offer something that is not a release

    const std::string good =
        "{\"version\":\"v0.3.47\",\"bin\":\"cardputer-roast-v0.3.47.bin\",\"size\":1934608,"
        "\"sha256\":\"F37046E2C78C5E8F3648AFFAD8B3E9154D1788737258B4C838624CC7BE6627E0\"}\n";
    Manifest m;
    std::string why;
    CHECK(ParseManifest(good, m, &why));
    CHECK(m.version == "v0.3.47" && m.bin == "cardputer-roast-v0.3.47.bin" && m.size == 1934608);
    CHECK(m.sha256 == "f37046e2c78c5e8f3648affad8b3e9154d1788737258b4c838624cc7be6627e0");  // lowercased
    CHECK(AssetUrl(m.version, m.bin) ==
          "https://github.com/e7h4n/cardputer-roast/releases/download/v0.3.47/cardputer-roast-v0.3.47.bin");

    auto bad = [&](const std::string& json, const char* expect) {
        Manifest x;
        std::string w;
        bool ok = ParseManifest(json, x, &w);
        if (ok || w.find(expect) == std::string::npos) printf("  manifest %s -> %s\n", json.c_str(), w.c_str());
        return !ok && w.find(expect) != std::string::npos;
    };
    const char* sha = "f37046e2c78c5e8f3648affad8b3e9154d1788737258b4c838624cc7be6627e0";
    auto j = [&](const std::string& ver, const std::string& bin, const std::string& size, const std::string& h) {
        return "{\"version\":\"" + ver + "\",\"bin\":\"" + bin + "\",\"size\":" + size + ",\"sha256\":\"" + h + "\"}";
    };
    CHECK(bad("not json", "not JSON"));
    CHECK(bad(j("26d8161", "a.bin", "1934608", sha), "version"));
    CHECK(bad(j("v0.3.47", "../evil.bin", "1934608", sha), "image name"));  // a path, not a file name
    CHECK(bad(j("v0.3.47", "roast.elf", "1934608", sha), "image name"));
    CHECK(bad(j("v0.3.47", "a.bin", "1934608", "abc"), "sha256"));
    CHECK(bad(j("v0.3.47", "a.bin", "100", sha), "size"));
    CHECK(bad(j("v0.3.47", "a.bin", "5000000", sha), "size"));
    CHECK(bad(j("v0.3.47", "a.bin", "1934608.5", sha), "size"));

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
