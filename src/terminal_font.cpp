// Mousiki's own terminal font (see terminal_font.h).
#include "terminal_font.h"
#include "path_utf8.h"
#include "process_util.h"
#include "tiny_json.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace muisc::terminal_font {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string getenv_str(const char* k) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string();
}

#if defined(_WIN32) || defined(__APPLE__)
std::mutex g_mu;
#endif
// what restore() has to undo
#if defined(__APPLE__)
bool g_mac_changed = false;            // Terminal.app: the window's font was set ...
std::string g_mac_tty, g_mac_old_name; // ... on this tty, it had this font ...
std::string g_mac_old_size;            // ... at this size
bool g_iterm_switched = false;         // iTerm2: the tab is on the "Mousiki" profile
#endif
#if defined(_WIN32)
bool g_con_changed = false;            // Windows console: the window's font was set, this was the old one
CONSOLE_FONT_INFOEX g_con_old{};
#endif

#if defined(__APPLE__)
// The PostScript name of every monospace family (Terminal.app and iTerm2 want PostScript names): family -> name.
std::vector<std::pair<std::string, std::string>> mac_font_table() {
    static const char* const kScript =
        "use framework \"AppKit\"\n"
        "use scripting additions\n"
        "set fm to current application's NSFontManager's sharedFontManager()\n"
        "set names to (fm's availableFontNamesWithTraits:1024) as list\n"      // 1024 = NSFixedPitchFontMask
        "set out to {}\n"
        "repeat with n in names\n"
        "  set f to (current application's NSFont's fontWithName:n |size|:12)\n"
        "  if f is not missing value then set end of out to ((f's familyName()) as text) & tab & (n as text)\n"
        "end repeat\n"
        "set AppleScript's text item delimiters to linefeed\n"
        "return out as text\n";
    std::vector<std::pair<std::string, std::string>> rows;
    const ProcResult r = run_capture({"/usr/bin/osascript", "-e", kScript});
    if (!r.ok()) return rows;
    std::istringstream in(r.out);
    std::string line;
    while (std::getline(in, line)) {
        const size_t t = line.find('\t');
        if (t == std::string::npos) continue;
        rows.emplace_back(trim(line.substr(0, t)), trim(line.substr(t + 1)));
    }
    return rows;
}

// The PostScript name for a family: its "-Regular" member, else one without a style, else the shortest name; else a guess.
std::string mac_postscript_name(const std::string& family) {
    std::string best;
    for (const auto& [fam, ps] : mac_font_table()) {
        if (fam != family) continue;
        const bool regular = ps.size() > 8 && ps.compare(ps.size() - 8, 8, "-Regular") == 0;
        if (regular) return ps;
        if (best.empty() || (ps.find('-') == std::string::npos && best.find('-') != std::string::npos) || ps.size() < best.size()) best = ps;
    }
    return best.empty() ? guess_postscript_name(family) : best;
}

std::string own_tty() {
    const char* t = ttyname(STDOUT_FILENO);
    if (!t) t = ttyname(STDIN_FILENO);
    return t ? std::string(t) : std::string();
}

// Sets the font of the Terminal.app tab on `tty`; prints "old name<TAB>old size" (empty when the tab was not found).
const char* kMacSetScript =
    "on run argv\n"
    "  set ttyName to item 1 of argv\n"
    "  set fontName to item 2 of argv\n"
    "  tell application \"Terminal\"\n"
    "    repeat with w in windows\n"
    "      repeat with t in tabs of w\n"
    "        if tty of t is ttyName then\n"
    "          set oldName to font name of t\n"
    "          set oldSize to font size of t\n"
    "          set font name of t to fontName\n"
    "          return oldName & tab & (oldSize as text)\n"
    "        end if\n"
    "      end repeat\n"
    "    end repeat\n"
    "  end tell\n"
    "  return \"\"\n"
    "end run\n";
const char* kMacRestoreScript =
    "on run argv\n"
    "  tell application \"Terminal\"\n"
    "    repeat with w in windows\n"
    "      repeat with t in tabs of w\n"
    "        if tty of t is (item 1 of argv) then\n"
    "          set font name of t to (item 2 of argv)\n"
    "          try\n"
    "            set font size of t to ((item 3 of argv) as number)\n"
    "          end try\n"
    "        end if\n"
    "      end repeat\n"
    "    end repeat\n"
    "  end tell\n"
    "end run\n";

fs::path iterm_profile_file() {
    return fs::path(getenv_str("HOME")) / "Library" / "Application Support" / "iTerm2" / "DynamicProfiles" / "mousiki.json";
}
#endif

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow_w(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
int CALLBACK enum_font(const LOGFONTW* lf, const TEXTMETRICW*, DWORD type, LPARAM out) {
    if ((lf->lfPitchAndFamily & 0x3) == FIXED_PITCH && (type & TRUETYPE_FONTTYPE) && lf->lfFaceName[0] != L'@')
        reinterpret_cast<std::set<std::string>*>(out)->insert(narrow_w(lf->lfFaceName));
    return 1;
}
fs::path wt_fragment_file() {
    return fs::path(widen(getenv_str("LOCALAPPDATA"))) / L"Microsoft" / L"Windows Terminal" / L"Fragments" / L"Mousiki" / L"mousiki.json";
}
// The launcher next to the program folder (installed / portable: <dir>\mousiki.cmd, the program in <dir>\app), else the program.
std::string own_launcher() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof buf / sizeof buf[0]));
    if (n == 0) return "";
    const fs::path exe(std::wstring(buf, n));
    std::error_code ec;
    const fs::path cmd = exe.parent_path().parent_path() / L"mousiki.cmd";
    if (fs::exists(cmd, ec)) return path_utf8(cmd);
    return path_utf8(exe);
}
#endif

} // namespace

Host detect_host() {
#if defined(_WIN32)
    return getenv_str("WT_SESSION").empty() ? Host::WinConsole : Host::WindowsTerminal;
#else
    const std::string prog = getenv_str("TERM_PROGRAM");
    if (prog == "Apple_Terminal") return Host::AppleTerminal;
    if (prog == "iTerm.app") return Host::ITerm2;
    if (!getenv_str("WT_SESSION").empty()) return Host::WindowsTerminal;   // WSL inside Windows Terminal
    return Host::Other;
#endif
}

const char* host_name(Host h) {
    switch (h) {
        case Host::AppleTerminal: return "Terminal";
        case Host::ITerm2: return "iTerm2";
        case Host::WindowsTerminal: return "Windows Terminal";
        case Host::WinConsole: return "the console window";
        case Host::Other: break;
    }
    return "this terminal";
}

std::vector<std::string> parse_fc_list(const std::string& out) {
    std::set<std::string> fams;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        const std::string first = trim(line.substr(0, line.find(',')));   // fc-list gives "Family,Localized Family"
        if (!first.empty()) fams.insert(first);
    }
    return std::vector<std::string>(fams.begin(), fams.end());
}

std::vector<std::string> list_monospace_fonts() {
    std::set<std::string> fams;
#if defined(_WIN32)
    HDC dc = GetDC(nullptr);
    if (dc) {
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &lf, enum_font, reinterpret_cast<LPARAM>(&fams), 0);
        ReleaseDC(nullptr, dc);
    }
#elif defined(__APPLE__)
    for (const auto& row : mac_font_table()) fams.insert(row.first);
#else
    const ProcResult r = run_capture({"fc-list", ":spacing=mono", "family"});
    if (r.ok()) for (const std::string& f : parse_fc_list(r.out)) fams.insert(f);
#endif
    return std::vector<std::string>(fams.begin(), fams.end());
}

std::string guess_postscript_name(const std::string& family) {
    std::string s;
    for (char c : family) if (c != ' ') s += c;
    return s + "-Regular";
}

std::string wt_fragment_json(const std::string& font, const std::string& commandline, const std::string& icon) {
    std::string j = "{\n  \"profiles\": [\n    {\n      \"name\": \"Mousiki\",\n";
    j += "      \"commandline\": \"" + tinyjson::escape(commandline) + "\",\n";
    if (!icon.empty()) j += "      \"icon\": \"" + tinyjson::escape(icon) + "\",\n";
    if (!font.empty() && font != kTerminalDefault) j += "      \"font\": { \"face\": \"" + tinyjson::escape(font) + "\" },\n";
    j += "      \"hidden\": false\n    }\n  ]\n}\n";
    return j;
}

std::string iterm_profile_json(const std::string& ps, int size, const std::string& parent) {
    const std::string f = tinyjson::escape(ps) + " " + std::to_string(size);
    std::string j = "{\n  \"Profiles\": [\n    {\n";
    j += "      \"Name\": \"Mousiki\",\n      \"Guid\": \"io.github.mousiki.font-profile\",\n";
    if (!parent.empty()) j += "      \"Dynamic Profile Parent Name\": \"" + tinyjson::escape(parent) + "\",\n";
    j += "      \"Normal Font\": \"" + f + "\",\n      \"Non Ascii Font\": \"" + f + "\",\n      \"Use Non-ASCII Font\": false\n";
    j += "    }\n  ]\n}\n";
    return j;
}

static std::string apply_impl(const std::string& font_in, const std::string& launcher_in) {
    const std::string font = trim(font_in).empty() ? std::string(kDefaultFont) : trim(font_in);
    const bool none = font == kTerminalDefault;
    const Host host = detect_host();
#if defined(_WIN32)
    {   // the Windows Terminal profile "Mousiki" is kept up to date whatever the window is now: the launchers open it
        const std::string launcher = launcher_in.empty() ? own_launcher() : launcher_in;
        if (!launcher.empty() && !getenv_str("LOCALAPPDATA").empty()) {
            std::error_code ec;
            const fs::path f = wt_fragment_file();
            fs::create_directories(f.parent_path(), ec);
            const fs::path ico = path_from_utf8(launcher).parent_path() / "mousiki.ico";
            std::ofstream out(f, std::ios::binary | std::ios::trunc);
            if (out) out << wt_fragment_json(none ? std::string() : font, "\"" + launcher + "\"", fs::exists(ico, ec) ? path_utf8(ico) : std::string());
        }
    }
    if (host == Host::WindowsTerminal)
        return none ? "font: Windows Terminal keeps its own font (the Mousiki profile has none)"
                    : "font: " + font + " -- in Windows Terminal it applies when Mousiki opens through its launcher (profile \"Mousiki\")";
    {   // the console window itself
        std::lock_guard<std::mutex> lk(g_mu);
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_FONT_INFOEX cur{};
        cur.cbSize = sizeof cur;
        if (!GetCurrentConsoleFontEx(out, FALSE, &cur)) return "font: the console window does not tell its font";
        if (none) {
            if (g_con_changed) { SetCurrentConsoleFontEx(out, FALSE, &g_con_old); g_con_changed = false; }
            return "font: the console window's own font";
        }
        if (!g_con_changed) { g_con_old = cur; g_con_changed = true; }
        CONSOLE_FONT_INFOEX want = cur;
        const std::wstring w = widen(font);
        wcsncpy_s(want.FaceName, LF_FACESIZE, w.c_str(), _TRUNCATE);
        want.FontFamily = FF_DONTCARE;
        want.FontWeight = FW_NORMAL;
        if (!SetCurrentConsoleFontEx(out, FALSE, &want)) return "font: the console window refused " + font;
        return "font: " + font + " for this window";
    }
#else
    (void)launcher_in;
    if (host == Host::AppleTerminal) {
#if defined(__APPLE__)
        const std::string tty = own_tty();
        if (tty.empty()) return "font: Terminal window not found";
        std::thread([font, none, tty]() {   // in the background: macOS may first ask whether Mousiki may control Terminal
            if (none) {
                std::string name, size;
                {
                    std::lock_guard<std::mutex> lk(g_mu);
                    if (!g_mac_changed) return;
                    name = g_mac_old_name; size = g_mac_old_size;
                    g_mac_changed = false;
                }
                run_capture({"/usr/bin/osascript", "-e", kMacRestoreScript, tty, name, size});
                return;
            }
            const ProcResult r = run_capture({"/usr/bin/osascript", "-e", kMacSetScript, tty, mac_postscript_name(font)});
            const std::string out = trim(r.out);
            const size_t t = out.find('\t');
            std::lock_guard<std::mutex> lk(g_mu);
            if (r.ok() && t != std::string::npos && !g_mac_changed) {   // the font the window had before Mousiki's first change
                g_mac_old_name = out.substr(0, t);
                g_mac_old_size = out.substr(t + 1);
                g_mac_tty = tty;
                g_mac_changed = true;
            }
        }).detach();
        return none ? "font: Terminal's own font again" : "font: " + font + " for this Terminal window (macOS may ask once whether Mousiki may control Terminal)";
#else
        return "font: not available here";
#endif
    }
    if (host == Host::ITerm2) {
#if defined(__APPLE__)
        std::lock_guard<std::mutex> lk(g_mu);
        const std::string own = getenv_str("ITERM_PROFILE");
        if (none) {
            if (g_iterm_switched) {
                std::cout << "\x1b]1337;SetProfile=" << (own.empty() ? "Default" : own) << "\x07" << std::flush;
                g_iterm_switched = false;
            }
            return "font: iTerm2's own profile again";
        }
        const fs::path f = iterm_profile_file();
        const std::string json = iterm_profile_json(mac_postscript_name(font), 13, own);
        std::string old;
        { std::ifstream in(f, std::ios::binary); std::ostringstream ss; ss << in.rdbuf(); old = ss.str(); }
        if (old != json) {
            std::error_code ec;
            fs::create_directories(f.parent_path(), ec);
            std::ofstream out(f, std::ios::binary | std::ios::trunc);
            out << json;
            out.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(700));   // iTerm2 reads the new profile file first
        }
        std::cout << "\x1b]1337;SetProfile=Mousiki\x07" << std::flush;
        g_iterm_switched = true;
        return "font: " + font + " (iTerm2 profile \"Mousiki\" for this tab)";
#else
        return "font: not available here";
#endif
    }
    if (host == Host::WindowsTerminal) return "font: Windows Terminal (WSL) -- choose the font in its profile";
    return none ? "font: the terminal's own font" : "font: " + font + " -- this terminal does not let programs set its font; choose it in the terminal's settings";
#endif
}

namespace {
std::mutex g_note_mu;
std::string g_note;
} // namespace

std::string apply(const std::string& font, const std::string& launcher) {
    const std::string note = apply_impl(font, launcher);
    std::lock_guard<std::mutex> lk(g_note_mu);
    g_note = note;
    return note;
}

std::string last_note() {
    std::lock_guard<std::mutex> lk(g_note_mu);
    return g_note;
}

void restore() {
#if defined(_WIN32)
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_con_changed) {
        SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &g_con_old);
        g_con_changed = false;
    }
#elif defined(__APPLE__)
    std::string tty, name, size;
    bool mac = false, iterm = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        mac = g_mac_changed; tty = g_mac_tty; name = g_mac_old_name; size = g_mac_old_size;
        iterm = g_iterm_switched;
        g_mac_changed = false;
        g_iterm_switched = false;
    }
    if (mac) run_capture({"/usr/bin/osascript", "-e", kMacRestoreScript, tty, name, size});
    if (iterm) {
        const std::string own = getenv_str("ITERM_PROFILE");
        std::cout << "\x1b]1337;SetProfile=" << (own.empty() ? "Default" : own) << "\x07" << std::flush;
    }
#endif
}

} // namespace muisc::terminal_font
