// In-memory NVS stand-in.
#pragma once
#include <map>
#include <string>
#include <cstdint>

class Settings {
public:
    Settings(const std::string& ns, bool = false) : _ns(ns) {}
    std::string GetString(const std::string& k, const std::string& d = "")
    {
        auto it = store().find(_ns + "/" + k);
        return it == store().end() ? d : it->second;
    }
    void SetString(const std::string& k, const std::string& v) { store()[_ns + "/" + k] = v; }
    int32_t GetInt(const std::string& k, int32_t d = 0)
    {
        auto it = store().find(_ns + "/" + k);
        return it == store().end() ? d : std::stoi(it->second);
    }
    void SetInt(const std::string& k, int32_t v) { store()[_ns + "/" + k] = std::to_string(v); }
    bool GetBool(const std::string& k, bool d = false) { return GetInt(k, d) != 0; }
    void SetBool(const std::string& k, bool v) { SetInt(k, v); }

private:
    std::string _ns;
    static std::map<std::string, std::string>& store()
    {
        static std::map<std::string, std::string> s;
        return s;
    }
};
