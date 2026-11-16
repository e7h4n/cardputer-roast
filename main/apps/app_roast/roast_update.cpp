#include "roast_update.h"
#include <cJSON.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#ifdef ESP_PLATFORM
#include <esp_app_desc.h>
#endif

namespace roast {
namespace update {

const char* Installed()
{
#ifdef ESP_PLATFORM
    return esp_app_get_description()->version;
#else
    return "dev";
#endif
}

static bool number(const char*& p, int& out)
{
    if (!isdigit((unsigned char)*p)) return false;
    long v = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (*p++ - '0');
        if (v > 1000000) return false;
    }
    out = (int)v;
    return true;
}

bool ParseRelease(const std::string& s, Version& v)
{
    const char* p = s.c_str();
    if (*p == 'v' || *p == 'V') p++;
    Version r;
    if (!number(p, r.major) || *p++ != '.' || !number(p, r.minor) || *p++ != '.' || !number(p, r.patch) || *p)
        return false;
    v = r;
    return true;
}

int Compare(const Version& a, const Version& b)
{
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool Offer(const std::string& installed, const std::string& latest)
{
    Version l, i;
    if (!ParseRelease(latest, l)) return false;
    if (!ParseRelease(installed, i)) return true;  // a local or PR build: any release is an option
    return Compare(l, i) > 0;
}

static bool fail(std::string* why, const char* reason)
{
    if (why) *why = reason;
    return false;
}

bool ParseManifest(const std::string& json, Manifest& m, std::string* why)
{
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return fail(why, "latest.json is not JSON");
    auto str = [&](const char* k) {
        const cJSON* v = cJSON_GetObjectItemCaseSensitive(root, k);
        return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
    };
    Manifest r;
    r.version         = str("version");
    r.bin             = str("bin");
    r.sha256          = str("sha256");
    const cJSON* size = cJSON_GetObjectItemCaseSensitive(root, "size");
    double sz         = cJSON_IsNumber(size) ? size->valuedouble : -1;
    cJSON_Delete(root);
    Version v;
    if (!ParseRelease(r.version, v)) return fail(why, "latest.json: no release version");
    bool plain = r.bin.size() > 4 && r.bin.size() < 100 && r.bin.compare(r.bin.size() - 4, 4, ".bin") == 0;
    for (char c : r.bin)
        if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) plain = false;
    if (!plain || r.bin[0] == '.') return fail(why, "latest.json: bad image name");
    bool hex = r.sha256.size() == 64;
    for (char& c : r.sha256) {
        c = (char)tolower((unsigned char)c);
        if (!isxdigit((unsigned char)c)) hex = false;
    }
    if (!hex) return fail(why, "latest.json: bad sha256");
    if (sz < 64 * 1024 || sz > 4 * 1024 * 1024 || sz != (double)(uint32_t)sz) return fail(why, "latest.json: bad size");
    r.size = (uint32_t)sz;
    m      = r;
    return true;
}

std::string AssetUrl(const std::string& tag, const std::string& file)
{
    return std::string("https://github.com/") + kRepo + "/releases/download/" + tag + "/" + file;
}

}  // namespace update
}  // namespace roast
