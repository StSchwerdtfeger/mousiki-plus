#include "themes.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace themes {

const std::string* Theme::get(const std::string& key, bool radio) const {
    if (radio) {
        const std::string rk = "Radio." + key;
        for (const auto& p : kv) if (p.first == rk) return &p.second;
    }
    for (const auto& p : kv) if (p.first == key) return &p.second;
    return nullptr;
}

void Theme::set(const std::string& key, const std::string& value, bool radio_only) {
    const std::string k = radio_only ? "Radio." + key : key;
    for (auto& p : kv) if (p.first == k) { p.second = value; return; }
    kv.emplace_back(k, value);
}

namespace {

// One theme in a handful of roles; expand() turns it into every key of both programs.
struct Palette {
    const char* name;
    const char *b1, *b2;       // border top / bottom
    const char *d1, *d2;       // disk, visualizer, on-air sign, oscilloscope: the two ends of the gradient
    const char *mk, *mv;       // metadata key / value
    const char *pp, *pn;       // progress bar / volume: played / pending
    const char *lf, *pf, *cb;  // list text, playing row, cursor background
    const char *hd, *lg;       // header (accent) and legend (dim)
    const char *tc, *to;       // tab names: current / other
    const char *chf, *chb;     // text being edited
    const char *li, *ll, *lw;  // lyrics: inactive line, active line, active word
    const char *wb;            // lyrics: active word background
};

// Colours are 256-colour palette numbers. 30-47 and 90-107 are read as the plain 16-colour codes by the
// programs, so the themes stay out of that range (except 90, the usual dim grey, in the default).
const Palette kPalettes[] = {
    {"Serious MeowerShell",  "170","170","170","30","170","182","170","182","255","245","240","10","90","10","90","37","41","245","251","255","0"},
    {"Soft Pastel",          "183","152","218","153","183","230","218","146","253","152","238","183","246","218","244","232","218","245","224","230",""},
    {"Cyberpunk",            "201","51","201","51","51","227","201","54","195","227","53","201","63","51","135","232","227","63","51","201",""},
    {"Greyscale",            "255","240","255","238","250","255","255","240","252","246","238","255","244","255","243","232","252","243","250","255",""},
    {"Nord",                 "110","67","110","139","110","189","110","60","188","150","59","110","246","150","245","235","150","245","153","195",""},
    {"Dracula",              "141","212","141","212","117","231","212","60","253","84","60","141","246","212","246","235","84","246","117","228",""},
    {"Solarized Dark",       "136","73","136","73","73","187","64","240","248","73","236","136","240","166","240","234","136","240","245","230",""},
    {"Gruvbox",              "214","142","208","142","214","223","142","241","223","109","239","214","245","208","243","235","214","243","223","229",""},
    {"Monokai",              "197","81","197","81","148","230","148","59","253","186","237","197","242","148","242","235","148","242","186","230",""},
    {"Ocean",                "75","24","81","27","81","195","81","24","153","123","24","81","67","123","67","232","81","67","117","195",""},
    {"Forest",               "71","22","114","28","71","193","114","22","151","192","22","71","65","192","65","232","114","65","151","230",""},
    {"Sunset",               "209","125","214","162","209","223","214","242","224","216","52","209","138","214","242","232","214","138","223","230",""},
    {"Matrix",               "82","22","82","22","82","157","82","22","83","120","22","82","28","118","28","232","82","28","83","157",""},
    {"Amber Terminal",       "214","130","220","166","214","222","220","58","178","220","58","220","136","214","130","232","214","136","178","222",""},
    {"Sakura",               "211","175","218","169","211","255","218","132","225","218","89","211","139","218","139","232","218","139","224","255",""},
    {"Midnight",             "62","18","69","54","69","189","69","54","146","189","17","69","60","141","60","232","69","60","146","189",""},
    {"Colorblind Deuteranopia","214","75","214","75","75","229","214","67","255","220","24","214","246","220","244","232","214","244","117","229",""},
    {"Colorblind Protanopia","220","69","226","63","69","229","226","61","255","117","18","220","246","226","244","232","220","244","117","230",""},
    {"Colorblind Tritanopia","203","80","203","80","80","224","203","66","255","217","53","203","246","210","244","232","203","244","80","255",""},
    {"High Contrast",        "255","255","226","51","226","255","226","244","255","226","21","226","250","226","250","232","226","250","255","231",""},
    {"Coffee",               "137","58","173","242","180","223","173","242","187","180","58","180","243","180","243","232","180","243","187","230",""},
};

Theme expand(const Palette& p) {
    Theme t;
    t.name = p.name;
    t.builtin = true;
    auto a = [&](const char* k, const char* v) { t.kv.emplace_back(k, v); };
    // --- keys both programs share ---
    a("ColorBorderTop", p.b1);            a("ColorBorderBottom", p.b2);
    a("ColorMetadataKey", p.mk);          a("ColorMetadataVal", p.mv);
    a("ColorVizLeft", p.d1);              a("ColorVizRight", p.d2);
    a("ColorTabCurrent", p.tc);           a("ColorTabOther", p.to);
    a("ColorHeader", p.hd);               a("ColorLegend", p.lg);
    a("ColorFocus", p.hd);                // the pane focus indicator: the theme's header color
    a("Radio.ColorHeader", p.lg);         a("Radio.ColorLegend", p.hd);   // the radio draws them the other way round
    a("Radio.ColorFocus", p.lg);          // ... and its focus indicator follows ITS header colour
    a("ColorSearchFg", "");               a("ColorSearchBg", "");
    a("ColorSearchInactiveFg", "");       a("ColorSearchInactiveBg", "");
    a("ColorListInactiveFg", p.lf);       a("ColorListInactiveBg", "");
    a("ColorListPlayingFg", p.pf);        a("ColorListPlayingBg", "");
    a("ColorListCursorFg", "");           a("ColorListCursorBg", p.cb);
    // --- player only ---
    a("ColorDiskTop", p.d1);              a("ColorDiskBottom", p.d2);
    a("ColorProgressBarPlayed", p.pp);    a("ColorProgressBarPending", p.pn);
    a("ColorChangeFg", p.chf);            a("ColorChangeBg", p.chb);
    a("ColorQueueInactiveFg", p.lf);      a("ColorQueueInactiveBg", "");
    a("ColorQueuePlayingFg", p.pf);       a("ColorQueuePlayingBg", "");
    a("ColorQueueCursorFg", "");          a("ColorQueueCursorBg", p.cb);
    a("ColorLyricsInactiveFg", p.li);     a("ColorLyricsInactiveBg", "");
    a("ColorLyricsActiveLineFg", p.ll);   a("ColorLyricsActiveLineBg", "");
    a("ColorLyricsActiveWordFg", p.lw);   a("ColorLyricsActiveWordBg", p.wb);
    // --- radio only ---
    a("ColorOnAirUpperLeft", p.d1);       a("ColorOnAirBottomRight", p.d2);
    a("ColorVizCenter", "");
    a("ColorOsciLeft", p.d1);             a("ColorOsciRight", p.d2);
    a("ColorFrequencyLine", p.d1);        a("ColorFrequencyMHz", p.lg);
    a("ColorVolumeCurrent", p.pp);        a("ColorVolumePossible", p.pn);
    a("ColorPresetInactiveFg", p.lg);     a("ColorPresetInactiveBg", "");
    a("ColorPresetKeyFg", p.lf);
    return t;
}

// The hand-made default theme (not expressible as a Palette: it also colours the search bar, the playing row and so on).
Theme meowershell() {
    Theme t;
    t.name = "MeowerShell(Default)";
    t.builtin = true;
    static const char* const kv[][2] = {
        {"ColorBorderTop","170"},{"ColorBorderBottom","170"},{"ColorMetadataKey","170"},{"ColorMetadataVal","182"},
        {"ColorVizLeft","170"},{"ColorVizRight","30"},{"ColorTabCurrent","90"},{"ColorTabOther","10"},
        {"ColorHeader","10"},{"ColorFocus","10"},{"ColorLegend","90"},{"Radio.ColorHeader","90"},{"Radio.ColorLegend","10"},{"Radio.ColorFocus","90"},
        {"ColorSearchFg","37"},{"ColorSearchBg","41"},{"ColorSearchInactiveFg",""},{"ColorSearchInactiveBg",""},
        {"ColorListInactiveFg",""},{"ColorListInactiveBg",""},{"ColorListPlayingFg","30"},{"ColorListPlayingBg","182"},
        {"ColorListCursorFg","10"},{"ColorListCursorBg","238"},{"ColorDiskTop","170"},{"ColorDiskBottom","30"},
        {"ColorProgressBarPlayed","170"},{"ColorProgressBarPending","182"},{"ColorChangeFg","37"},{"ColorChangeBg","41"},
        {"ColorQueueInactiveFg",""},{"ColorQueueInactiveBg",""},{"ColorQueuePlayingFg","30"},{"ColorQueuePlayingBg","182"},
        {"ColorQueueCursorFg",""},{"ColorQueueCursorBg","240"},{"ColorLyricsInactiveFg",""},{"ColorLyricsInactiveBg",""},
        {"ColorLyricsActiveLineFg","182"},{"ColorLyricsActiveLineBg",""},{"ColorLyricsActiveWordFg",""},{"ColorLyricsActiveWordBg",""},
        {"ColorOnAirUpperLeft","170"},{"ColorOnAirBottomRight","30"},{"ColorVizCenter",""},{"ColorOsciLeft","170"},
        {"ColorOsciRight","30"},{"ColorFrequencyLine","170"},{"ColorFrequencyMHz","90"},{"ColorVolumeCurrent","170"},
        {"ColorVolumePossible","182"},{"ColorPresetInactiveFg","90"},{"ColorPresetInactiveBg",""},{"ColorPresetKeyFg","255"},
    };
    for (const auto& e : kv) t.kv.emplace_back(e[0], e[1]);
    return t;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void write_theme(std::ostream& out, const Theme& t) {
    out << "[" << t.name << "]\n";
    for (const auto& p : t.kv) out << p.first << "=" << p.second << "\n";
    out << "\n";
}

const char* kHeader =
    "# format: 4\n"
    "# Mousiki colour themes (shared by the player and the radio).\n"
    "# [Name] starts a theme; the lines below it use the key names of config.txt / radio_config.txt.\n"
    "# A key a theme leaves out keeps whatever colour is set. Keys starting with \"Radio.\" are the radio's own values (colours made in the radio are stored that way, so the radio and the player can look different in one theme).\n"
    "# Built-in themes can be edited here; new ones are made with CREATE THEME on the Colors tab.\n\n";

std::vector<Theme> parse_file(const fs::path& p) {
    std::vector<Theme> out;
    std::ifstream in(p);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            Theme t;
            t.name = trim(line.substr(1, line.size() - 2));
            if (t.name.empty()) t.name = "(unnamed)";
            out.push_back(t);
            continue;
        }
        if (out.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.back().set(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    return out;
}

bool is_builtin_name(const std::string& n) {
    if (lower(n) == lower(meowershell().name)) return true;
    for (const Palette& p : kPalettes) if (lower(p.name) == lower(n)) return true;
    return false;
}

} // namespace

fs::path file_path() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = std::getenv("USERPROFILE");
    fs::path base = (home && *home) ? fs::u8path(home) : fs::path(".");
    return base / ".config" / "mousiki" / "themes.txt";
}

std::string sanitize_name(const std::string& raw) {
    std::string s = trim(raw);
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '[' || c == ']' || c == '\n' || c == '\r'; }), s.end());
    s = trim(s);
    // Cut at 25 characters without splitting a UTF-8 sequence.
    int chars = 0;
    size_t i = 0;
    while (i < s.size() && chars < kMaxNameLen) {
        i++;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i++;
        chars++;
    }
    return s.substr(0, i);
}

std::vector<Theme> load_all() {
    std::vector<Theme> all;
    all.push_back(meowershell());   // list index 0: the base/fallback theme everywhere
    for (const Palette& p : kPalettes) all.push_back(expand(p));

    const fs::path path = file_path();
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path);
        if (out) {
            out << kHeader;
            for (const Theme& t : all) write_theme(out, t);
        }
        return all;
    }
    // A themes.txt written by an older version (no "# format: 3" line) is rewritten with the current built-ins; the
    // user's own themes are kept. Dropped on the way: the old built-in "Default" (now "Serious MeowerShell") and
    // any copy of a built-in theme that carries identical values (e.g. a MeowerShell the user saved by hand).
    {
        std::ifstream in(path);
        std::string first;
        std::getline(in, first);
        if (trim(first) != "# format: 4") {
            std::vector<Theme> mine;
            for (Theme& ft : parse_file(path)) {
                if (is_builtin_name(ft.name) || lower(ft.name) == "default") continue;
                // format 4 added ColorFocus (the pane focus indicator): a theme of your own gets its header color
                if (!ft.get("ColorFocus", false)) if (const std::string* hd = ft.get("ColorHeader", false)) ft.set("ColorFocus", *hd);
                bool dup = false;
                for (const Theme& b : all) {
                    if (b.kv.size() != ft.kv.size()) continue;
                    bool same = true;
                    for (const auto& kvp : ft.kv) { const std::string* v = b.get(kvp.first, false); if (!v || *v != kvp.second) { same = false; break; } }
                    if (same) { dup = true; break; }
                }
                if (!dup) mine.push_back(ft);
            }
            std::ostringstream buf;
            buf << kHeader;
            for (const Theme& t : all) write_theme(buf, t);
            for (const Theme& t : mine) write_theme(buf, t);
            { std::ofstream out(path, std::ios::trunc); if (out) out << buf.str(); }
            for (Theme& t : mine) all.push_back(t);
            return all;
        }
    }
    for (Theme& ft : parse_file(path)) {
        bool merged = false;
        for (Theme& t : all) {
            if (lower(t.name) != lower(ft.name)) continue;
            if (t.builtin) for (const auto& p : ft.kv) t.set(p.first, p.second);   // the file's edits win
            merged = true;
            break;
        }
        if (!merged) all.push_back(ft);
    }
    return all;
}

std::filesystem::path exchange_dir() { return file_path().parent_path(); }

std::string file_name_for(const std::string& theme_name) {
    std::string n = sanitize_name(theme_name);
    for (char& c : n) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    if (n.empty()) n = "theme";
    return n + ".theme";
}

bool write_theme_file(const fs::path& file, const Theme& t, std::string& err) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (!out) { err = "cannot write " + file.string(); return false; }
    out << "# format: 4\n# A Mousiki colour theme. Import it with SHIFT+i on the Colors tab of the settings.\n\n";
    write_theme(out, t);
    return true;
}

std::vector<Theme> read_theme_file(const fs::path& file) {
    std::vector<Theme> out;
    for (Theme& t : parse_file(file)) {
        bool has_color = false;
        for (const auto& p : t.kv) if (p.first.find("Color") != std::string::npos) { has_color = true; break; }
        if (has_color) out.push_back(t);
    }
    return out;
}

std::vector<fs::path> list_theme_files(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = lower(it->path().extension().string());
        if (ext != ".theme" && ext != ".txt") continue;
        if (it->path() == file_path()) continue;   // the theme store itself
        if (!read_theme_file(it->path()).empty()) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return lower(a.filename().string()) < lower(b.filename().string()); });
    return out;
}

std::string import_theme(Theme t, std::string& err) {
    t.builtin = false;
    std::string base = sanitize_name(t.name);
    if (base.empty()) base = "Imported";
    const std::vector<Theme> all = load_all();
    auto taken = [&](const std::string& n) {
        for (const Theme& x : all) if (lower(x.name) == lower(n)) return true;
        return false;
    };
    std::string name = base;
    for (int i = 2; taken(name) && i < 1000; ++i) {
        const std::string suffix = " (" + std::to_string(i) + ")";
        name = sanitize_name(base.substr(0, static_cast<size_t>(std::max(1, kMaxNameLen - static_cast<int>(suffix.size()))))) + suffix;
    }
    t.name = name;
    return save_theme(t, err) ? name : std::string();
}

bool save_theme(const Theme& in_theme, std::string& err) {
    Theme t = in_theme;
    t.name = sanitize_name(t.name);
    t.builtin = false;
    if (t.name.empty()) { err = "give the theme a name first"; return false; }
    if (is_builtin_name(t.name)) { err = "\"" + t.name + "\" is a built-in theme -- pick another name"; return false; }

    const fs::path path = file_path();
    std::vector<Theme> existing;
    {
        std::error_code ec;
        if (fs::exists(path, ec)) existing = parse_file(path);
    }
    bool replaced = false;
    for (Theme& e : existing) if (lower(e.name) == lower(t.name)) { e = t; replaced = true; }
    if (!replaced) existing.push_back(t);

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Rewrite the whole file: the header, then the built-ins (as they are now, with any edits), then the user's themes.
    std::vector<Theme> all = load_all();
    std::ostringstream buf;
    buf << kHeader;
    for (const Theme& b : all) if (b.builtin) write_theme(buf, b);
    for (const Theme& e : existing) if (!is_builtin_name(e.name)) write_theme(buf, e);
    std::ofstream out(path, std::ios::trunc);
    if (!out) { err = "cannot write " + path.string(); return false; }
    out << buf.str();
    return true;
}

} // namespace themes
