// yt-dlp: the comparison of the installed copy with the newest release, the recommendation, and the update (see ytdlp_update.h).
#include "ytdlp_update.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#include "bg_tasks.h"
#include "console_log.h"
#include "path_utf8.h"
#include "process_util.h"
#include "settings.h"
#include "tiny_json.h"
#include "time_util.h"

namespace muisc::ytdlp_update {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------------------------------------
// pure helpers
// ---------------------------------------------------------------------------------------------------------------------------

std::vector<int> parse_version(const std::string& v) {
    std::vector<int> out;
    size_t i = 0;
    while (i < v.size() && (v[i] == 'v' || v[i] == 'V' || v[i] == ' ')) ++i;
    while (i < v.size() && out.size() < 4) {
        if (v[i] < '0' || v[i] > '9') break;
        long n = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') { n = n * 10 + (v[i] - '0'); if (n > 99999999) n = 99999999; ++i; }
        out.push_back(static_cast<int>(n));
        if (i < v.size() && v[i] == '.') ++i; else break;
    }
    // a date: year.month.day (and an optional build number); anything else is not a yt-dlp version
    if (out.size() < 3 || out[0] < 2000 || out[0] > 2999 || out[1] < 1 || out[1] > 12 || out[2] < 1 || out[2] > 31) return {};
    return out;
}

bool is_newer(const std::string& latest, const std::string& installed) {
    const std::vector<int> a = parse_version(latest), b = parse_version(installed);
    if (a.empty() || b.empty()) return false;
    for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

namespace {
// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}
} // namespace

long long days_between(const std::string& older, const std::string& newer) {
    const std::vector<int> a = parse_version(older), b = parse_version(newer);
    if (a.empty() || b.empty()) return -1;
    return days_from_civil(b[0], static_cast<unsigned>(b[1]), static_cast<unsigned>(b[2])) -
           days_from_civil(a[0], static_cast<unsigned>(a[1]), static_cast<unsigned>(a[2]));
}

std::string age_text(long long days) {
    if (days <= 0) return "less than a day";
    if (days == 1) return "1 day";
    if (days < 14) return std::to_string(days) + " days";
    if (days < 60) return "about " + std::to_string((days + 3) / 7) + " weeks";
    if (days < 730) return "about " + std::to_string((days + 15) / 30) + " months";
    return "about " + std::to_string((days + 182) / 365) + " years";
}

std::string describe(const Status& s) {
    if (!s.found) return "yt-dlp was not found: online search and video downloads need it (setup.sh / setup.ps1 install it)";
    if (s.installed.empty()) return "yt-dlp does not run (" + (s.error.empty() ? std::string("no version came back") : s.error) + ")";
    if (s.latest.empty()) return "yt-dlp " + s.installed + " is installed (the newest release could not be read: " + (s.error.empty() ? std::string("no answer") : s.error) + ")";
    if (is_newer(s.latest, s.installed)) {
        const long long d = days_between(s.installed, s.latest);
        return "yt-dlp " + s.installed + " is installed, " + s.latest + " is the newest" + (d > 0 ? " (" + age_text(d) + " newer)" : std::string());
    }
    if (parse_version(s.installed) == parse_version(s.latest)) return "yt-dlp " + s.installed + " is the newest release";
    return "yt-dlp " + s.installed + " is up to date (newer than the release " + s.latest + ": a nightly build)";
}

bool recommended(const Status& s) {
    if (!s.found || s.installed.empty() || s.latest.empty() || !is_newer(s.latest, s.installed)) return false;
    return days_between(s.installed, s.latest) >= kRecommendDays;
}

std::string recommendation(const Status& s) {
    if (!recommended(s)) return std::string();
    return describe(s) + ". An old yt-dlp is the usual reason for searches and downloads that stop working: update it with the command  update yt-dlp  (`/c:`)";
}

std::string state_to_text(const State& s) {
    std::ostringstream o;
    o << "checked=" << s.checked << "\nlatest=" << s.latest << "\nnotified=" << s.notified << "\n";
    return o.str();
}

State state_from_text(const std::string& text) {
    State s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "checked") s.checked = std::atoll(v.c_str());
        else if (k == "latest") s.latest = v.substr(0, 40);
        else if (k == "notified") s.notified = v.substr(0, 40);
    }
    return s;
}

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool has_word(const std::string& hay, const std::string& w) {   // `w` between non-letters
    for (size_t at = hay.find(w); at != std::string::npos; at = hay.find(w, at + 1)) {
        const bool l = at == 0 || !std::isalpha(static_cast<unsigned char>(hay[at - 1]));
        const bool r = at + w.size() >= hay.size() || !std::isalpha(static_cast<unsigned char>(hay[at + w.size()]));
        if (l && r) return true;
    }
    return false;
}
} // namespace

Reading read_update_output(const std::string& out) {
    const std::string t = lower(out);
    Reading r;
    if (t.find("permission denied") != std::string::npos || t.find("access is denied") != std::string::npos ||
        t.find("unable to write") != std::string::npos || t.find("errno 13") != std::string::npos) { r.kind = Kind::NotWritable; return r; }
    // what yt-dlp says for a copy that something else owns: "You installed yt-dlp with pip or using a package manager ..." / "... from brew"
    static const char* const kManagers[] = {"pip", "pipx", "brew", "homebrew", "apt", "dnf", "pacman", "snap", "scoop", "winget", "choco", "chocolatey", "flatpak", "nix"};
    const bool owned = t.find("installed yt-dlp") != std::string::npos || t.find("package manager") != std::string::npos ||
                       t.find("updating is disabled") != std::string::npos || t.find("use that to update") != std::string::npos;
    if (owned) {
        for (const char* m : kManagers)
            if (has_word(t, m)) { r.kind = std::string(m) == "pip" ? Kind::Pip : Kind::PackageManager; r.manager = m; return r; }
        r.kind = Kind::PackageManager;
        return r;
    }
    if (t.find("up to date") != std::string::npos || t.find("up-to-date") != std::string::npos) { r.kind = Kind::UpToDate; return r; }
    if (t.find("updated yt-dlp") != std::string::npos || t.find("updating to") != std::string::npos) { r.kind = Kind::Updated; return r; }
    return r;
}

std::string manager_hint(const std::string& m) {
    if (m == "brew" || m == "homebrew") return "update it with Homebrew:  brew upgrade yt-dlp";
    if (m == "apt") return "update it with your package manager:  sudo apt update && sudo apt install --only-upgrade yt-dlp  (the packages of some systems are old: the pip or program-file version is newer)";
    if (m == "dnf") return "update it with your package manager:  sudo dnf upgrade yt-dlp";
    if (m == "pacman") return "update it with your package manager:  sudo pacman -Syu yt-dlp";
    if (m == "snap") return "update it with snap:  sudo snap refresh yt-dlp";
    if (m == "scoop") return "update it with Scoop:  scoop update yt-dlp";
    if (m == "winget") return "update it with winget:  winget upgrade yt-dlp.yt-dlp";
    if (m == "choco" || m == "chocolatey") return "update it with Chocolatey:  choco upgrade yt-dlp";
    if (m == "pipx") return "update it with pipx:  pipx upgrade yt-dlp";
    if (m == "flatpak") return "update it with Flatpak:  flatpak update";
    return "update it with the tool that installed it (a package manager, pipx or a virtual environment)";
}

// ---------------------------------------------------------------------------------------------------------------------------
// the work
// ---------------------------------------------------------------------------------------------------------------------------

namespace {

std::mutex g_mu;
std::atomic<bool> g_started{false};
std::atomic<bool> g_busy{false};
std::string g_notice;               // waiting to be shown
bool g_notice_ready = false;
Status g_check;  bool g_check_ready = false;
Result g_update; bool g_update_ready = false;

fs::path state_file() { return config_path().parent_path() / "ytdlp_check.txt"; }

State load_state() {
    std::ifstream f(state_file(), std::ios::binary);
    if (!f) return State{};
    std::stringstream ss; ss << f.rdbuf();
    return state_from_text(ss.str());
}
void save_state(const State& s) {
    std::error_code ec;
    fs::create_directories(state_file().parent_path(), ec);
    std::ofstream f(state_file(), std::ios::binary | std::ios::trunc);
    if (f) f << state_to_text(s);
}

std::string first_line(const std::string& s) {
    const size_t nl = s.find('\n');
    std::string l = s.substr(0, nl == std::string::npos ? s.size() : nl);
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
    while (!l.empty() && l.front() == ' ') l.erase(0, 1);
    return l;
}
std::string last_line(const std::string& s) {
    std::string t = s;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    const size_t nl = t.find_last_of('\n');
    std::string l = nl == std::string::npos ? t : t.substr(nl + 1);
    if (l.size() > 160) l.resize(160);
    return l;
}

std::string ytdlp_program() {
    const std::string p = resolve_program("yt-dlp");
    return p;
}

// The version the installed yt-dlp reports; "" when it was not found / does not run (`found` says which).
std::string installed_version(bool& found, std::string& error) {
    const std::string prog = ytdlp_program();
    found = !prog.empty();
    if (!found) return std::string();
    const ProcResult r = run_capture({prog, "--version"}, /*merge_stderr=*/false);
    const std::string v = first_line(r.out);
    if (!r.ok() || parse_version(v).empty()) { error = r.ok() ? "unexpected answer: " + v.substr(0, 60) : "exit code " + std::to_string(r.exit_code); return std::string(); }
    return v;
}

// The newest release, from the cache when it is younger than a day (unless `force`). Writes the cache. "" with `error` when unknown.
std::string newest_version(const fs::path& script, bool force, std::string& error) {
    State st = load_state();
    const long long age = epoch_now() - st.checked;
    if (!force && st.checked > 0 && age >= 0 && age < 24LL * 3600 && !parse_version(st.latest).empty()) return st.latest;
    Args cmd = python_command();
    if (cmd.empty()) { error = "Python 3 was not found"; return st.latest.empty() || force ? std::string() : st.latest; }
    cmd.push_back(path_utf8(script));
    cmd.push_back("yt-dlp/yt-dlp");
    const ProcResult r = run_capture(cmd, /*merge_stderr=*/false);
    tinyjson::Value answer;
    if (!tinyjson::parse_answer(r.out, answer) || !tinyjson::bool_field(answer, "ok")) {
        const std::string why = tinyjson::string_field(answer, "error");
        error = why == "NETWORK" ? "GitHub could not be reached" : why == "MODULE_MISSING" ? "the helper module is missing" : "no answer from GitHub";
        return std::string();
    }
    const std::string tag = tinyjson::string_field(answer, "tag");
    if (parse_version(tag).empty()) { error = "an unreadable release name: " + tag.substr(0, 30); return std::string(); }
    st.checked = epoch_now();
    st.latest = tag;
    save_state(st);
    return tag;
}

Status compare(const fs::path& script, bool force) {
    Status s;
    std::string err;
    s.installed = installed_version(s.found, err);
    if (!s.found) { s.error = "not found"; return s; }
    if (s.installed.empty()) { s.error = err; return s; }
    std::string nerr;
    s.latest = newest_version(script, force, nerr);
    if (s.latest.empty()) s.error = nerr;
    s.ok = !s.latest.empty();
    return s;
}

void log_line(const std::string& line) { ConsoleLog::instance().log_basic("yt-dlp: " + line); }

// The update: ask yt-dlp to update itself; a copy that pip owns is updated with pip; every other owner gets a hint.
Result run_update() {
    Result res;
    bool found = false;
    std::string err;
    res.before = installed_version(found, err);
    const std::string prog = ytdlp_program();
    if (!found) { res.message = "yt-dlp was not found: nothing to update (setup.sh / setup.ps1 install it)"; return res; }
    log_line("update: running  " + command_text({prog, "-U"}));
    const ProcResult up = run_capture({prog, "-U"}, /*merge_stderr=*/true);
    res.after = installed_version(found, err);
    const bool changed = !res.after.empty() && !res.before.empty() && res.after != res.before;
    const Reading rd = read_update_output(up.out);
    auto finish_changed = [&](const std::string& how) {
        res.ok = true; res.changed = true;
        res.message = "yt-dlp updated" + how + ": " + res.before + " -> " + res.after;
    };
    if (changed) { finish_changed(""); return res; }
    switch (rd.kind) {
    case Kind::UpToDate:
        res.ok = true;
        res.message = "yt-dlp " + res.after + " is already the newest release";
        return res;
    case Kind::NotWritable:
        res.message = "yt-dlp could not replace itself: its folder is not writable (" + last_line(up.out) +
                      "). Start Mousiki with the rights for that folder once, or install yt-dlp where you may write (pip install --user yt-dlp)";
        return res;
    case Kind::PackageManager:
        res.message = "this yt-dlp belongs to " + (rd.manager.empty() ? std::string("a package manager") : rd.manager) + ": " + manager_hint(rd.manager);
        return res;
    case Kind::Pip: {
        Args pip = python_command();
        if (pip.empty()) { res.message = "this yt-dlp was installed with pip, but Python 3 was not found: python -m pip install --user --upgrade yt-dlp"; return res; }
        for (const char* a : {"-m", "pip", "install", "--user", "--upgrade", "yt-dlp"}) pip.push_back(a);
        log_line("update: running  " + command_text(pip));
        const ProcResult pr = run_capture(pip, /*merge_stderr=*/true);
        res.after = installed_version(found, err);
        if (!res.after.empty() && res.after != res.before) { finish_changed(" with pip"); return res; }
        if (lower(pr.out).find("externally-managed") != std::string::npos) {
            res.message = "pip refused (the system's Python is managed by the operating system): use  pipx upgrade yt-dlp , a virtual environment, "
                          "or the yt-dlp program file from github.com/yt-dlp/yt-dlp/releases";
            return res;
        }
        res.ok = pr.ok();
        res.message = pr.ok() ? "pip finished, but the yt-dlp that Mousiki starts is still " + res.after + " (it may come from another place: pipx or a virtual environment)"
                              : "pip failed: " + last_line(pr.out);
        return res;
    }
    case Kind::Updated:
    case Kind::Unknown:
    default:
        res.message = up.ok() ? "yt-dlp -U finished, the version is still " + res.after + (last_line(up.out).empty() ? std::string() : " (" + last_line(up.out) + ")")
                              : "yt-dlp -U failed: " + last_line(up.out);
        return res;
    }
}

} // namespace

void start(const fs::path& script) {
    if (g_started.exchange(true)) return;
    if (!read_update_check()) return;
    if (const char* off = std::getenv("MOUSIKI_NO_YTDLP_CHECK"); off && *off && std::string(off) != "0") return;   // (the UI tests: no notice over their screens)
    bg_async([script] {
        try {
            const Status s = compare(script, /*force=*/false);
            log_line(describe(s));
            const std::string note = recommendation(s);
            if (!note.empty()) {
                State st = load_state();
                if (st.notified != s.latest) {
                    st.notified = s.latest;
                    save_state(st);
                    std::lock_guard<std::mutex> lk(g_mu);
                    g_notice = note;
                    g_notice_ready = true;
                }
            }
        } catch (...) {
            // a failed check is never an error for the program
        }
        return 0;
    });
}

bool take_notice(std::string& text) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_notice_ready) return false;
    text = g_notice;
    g_notice_ready = false;
    return true;
}

bool check_now(const fs::path& script) {
    if (g_busy.exchange(true)) return false;
    bg_async([script] {
        Status s;
        try { s = compare(script, /*force=*/true); } catch (...) { s.error = "the check failed"; }
        log_line(describe(s));
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_check = s;
            g_check_ready = true;
        }
        g_busy = false;
        return 0;
    });
    return true;
}

bool take_check(Status& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_check_ready) return false;
    out = g_check;
    g_check_ready = false;
    return true;
}

bool update_now() {
    if (g_busy.exchange(true)) return false;
    bg_async([] {
        Result r;
        try { r = run_update(); } catch (...) { r.message = "the update failed"; }
        log_line(r.message);
        if (r.changed) {   // the stored answer is for the old copy: the next start asks again
            State st = load_state();
            st.notified.clear();
            save_state(st);
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_update = r;
            g_update_ready = true;
        }
        g_busy = false;
        return 0;
    });
    return true;
}

bool take_update(Result& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_update_ready) return false;
    out = g_update;
    g_update_ready = false;
    return true;
}

bool busy() { return g_busy.load(); }

std::string run_command(const std::string& arg, const fs::path& script, bool allow_update, bool& ok) {
    std::string a = lower(arg);
    while (!a.empty() && a.back() == ' ') a.pop_back();
    while (!a.empty() && a.front() == ' ') a.erase(0, 1);
    ok = false;
    const bool do_update = a == "yt-dlp" || a == "ytdlp" || a == "yt-dlp now";
    const bool do_check = a.empty() || a == "check" || a == "yt-dlp check" || a == "ytdlp check";
    if (!do_update && !do_check) return "update: use  update  (compare) or  update yt-dlp  (install the newest yt-dlp)";
    if (do_update && !allow_update) return "update yt-dlp changes a program on this computer: type it yourself, or use `force update yt-dlp` in a script";
    if (busy()) return "an update check is still running";
    ok = do_update ? update_now() : check_now(script);
    if (!ok) return "an update check is still running";
    return do_update ? "updating yt-dlp ... (the answer follows in the status line and the console log)"
                     : "comparing yt-dlp with the newest release ... (the answer follows in the status line and the console log)";
}

} // namespace muisc::ytdlp_update
