#include "keyboard_layout.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#if defined(_WIN32)
#include <windows.h>
#else
#include "process_util.h"
#endif

namespace muisc {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "de,us" / "de(nodeadkeys)" / "de_DE.UTF-8" -> "de"
std::string first_code(const std::string& v) {
    std::string s = lower(v);
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    std::string out;
    while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i])) && out.size() < 3) out += s[i++];
    return out;
}

#if !defined(_WIN32)
std::string value_after(const std::string& text, const std::string& key) {   // "key: value" line
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t p = line.find(key);
        if (p == std::string::npos) continue;
        std::string v = line.substr(p + key.size());
        while (!v.empty() && (v[0] == ' ' || v[0] == ':' || v[0] == '\t' || v[0] == '=')) v.erase(0, 1);
        return v;
    }
    return "";
}

// macOS: the layout NAME of the selected input source ("German", "U.S.", "British", "Austrian", ...)
std::string mac_code(const std::string& text) {
    const std::string t = lower(text);
    struct M { const char* name; const char* code; };
    static const M m[] = {{"german", "de"}, {"austrian", "at"}, {"swiss", "ch"}, {"spanish", "es"}, {"italian", "it"},
                          {"french", "fr"}, {"british", "gb"}, {"irish", "ie"}, {"australian", "au"}, {"canadian", "ca"},
                          {"u.s.", "us"}, {"abc", "us"}, {"us", "us"}};
    for (const auto& e : m) if (t.find(e.name) != std::string::npos) return e.code;
    return "";
}
#endif

std::string detect() {
#if defined(_WIN32)
    const LANGID lang = LOWORD(reinterpret_cast<uintptr_t>(GetKeyboardLayout(0)));
    switch (PRIMARYLANGID(lang)) {
        case LANG_GERMAN: return SUBLANGID(lang) == 0x03 ? "at" : SUBLANGID(lang) == 0x02 ? "ch" : "de";
        case LANG_SPANISH: return "es";
        case LANG_ITALIAN: return "it";
        case LANG_FRENCH: return "fr";
        case LANG_ENGLISH:
            switch (SUBLANGID(lang)) {
                case 0x02: return "gb";   // SUBLANG_ENGLISH_UK (numeric: not every SDK header defines every constant)
                case 0x03: return "au";   // SUBLANG_ENGLISH_AUS
                case 0x04: return "ca";   // SUBLANG_ENGLISH_CAN
                default: return "us";
            }
        default: return "";
    }
#else
    if (const char* e = std::getenv("MOUSIKI_KEYBOARD")) if (*e) return first_code(e);   // manual override, e.g. MOUSIKI_KEYBOARD=de
    if (const char* e = std::getenv("XKB_DEFAULT_LAYOUT")) if (*e) return first_code(e);
#if defined(__APPLE__)
    const ProcResult r = run_capture("defaults read com.apple.HIToolbox AppleSelectedInputSources 2>/dev/null");
    if (r.exit_code == 0) { const std::string c = mac_code(r.out); if (!c.empty()) return c; }
#else
    {
        const ProcResult r = run_capture("setxkbmap -query 2>/dev/null");
        if (r.exit_code == 0) { const std::string v = value_after(r.out, "layout:"); if (!first_code(v).empty()) return first_code(v); }
    }
    {
        const ProcResult r = run_capture("localectl status 2>/dev/null");
        if (r.exit_code == 0) { const std::string v = value_after(r.out, "X11 Layout:"); if (!first_code(v).empty()) return first_code(v); }
    }
    {
        std::ifstream in("/etc/default/keyboard");
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("XKBLAYOUT=", 0) != 0) continue;
            std::string v = line.substr(10);
            v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
            if (!first_code(v).empty()) return first_code(v);
        }
    }
#endif
    // last resort: the region of the locale (de_DE.UTF-8 -> de)
    for (const char* var : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* e = std::getenv(var);
        if (e && *e && std::string(e) != "C" && std::string(e) != "POSIX") return first_code(e);
    }
    return "";
#endif
}

} // namespace

std::string keyboard_layout_code() {
    static const std::string code = detect();
    return code;
}

std::string mode_switch_key_label() {
    const std::string c = keyboard_layout_code();
    if (c == "de" || c == "at" || c == "es" || c == "it") return "SHIFT and +";
    if (c == "us" || c == "gb" || c == "ie" || c == "au" || c == "ca" || c == "en" || c == "nz") return "SHIFT+8";
    return "*";
}

std::string symbol_key_label(const std::string& key) {
    if (key != "(" && key != ")" && key != "=" && key != "<") return key;
    const std::string c = keyboard_layout_code();
    if (key == "<") {   // the menus index: its own key on German-style keyboards, SHIFT+, on US-style ones
        if (c == "us" || c == "gb" || c == "ie" || c == "au" || c == "ca" || c == "en" || c == "nz") return "SHIFT+,";
        return key;
    }
    if (key == "=") {   // the big scope overlay: SHIFT+0 on a German keyboard
        if (c == "de" || c == "at" || c == "ch" || c == "es" || c == "it") return "SHIFT+0";
        return key;
    }
    if (c == "de" || c == "at" || c == "ch" || c == "es" || c == "it") return key == "(" ? "SHIFT+8" : "SHIFT+9";
    if (c == "us" || c == "gb" || c == "ie" || c == "au" || c == "ca" || c == "en" || c == "nz") return key == "(" ? "SHIFT+9" : "SHIFT+0";
    return key;
}

} // namespace muisc
