// Synced lyrics: finds the lyrics of a track, reads and writes the .lrc sidecar next to the audio
// file, and keeps earlier versions of the lyrics as separate files.
#include "lyrics_fetcher.h"
#include "TextSanitizer.h"
#include "path_utf8.h"
#include "process_util.h"
#include "tiny_json.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>
#if defined(_WIN32)
#include "win_compat.h"
#endif

namespace muisc {

// Sidecar lyrics file lives in a "lyrics" folder INSIDE the folder the
// track itself lives in, same stem, .lrc extension — e.g. "Music/Song
// Title.opus" -> "Music/lyrics/Song Title.lrc". That keeps fetched lyrics
// out of the track folder proper while staying per-track-folder, so two
// artists can both have a "Track 01" without their lyrics overwriting each
// other. Works for both a user's own library and the yt-dlp cache dir (both
// are "the music folder" for whatever track lives there), and is what lets
// a previously-fetched track show lyrics offline.
static fs::path sidecar_path(const fs::path& track_path) {
    if (track_path.empty()) return {};
    // replace_extension() works entirely in the path's native encoding.
    // The previous stem().string() + ".lrc" went through the ANSI code page
    // on Windows and threw std::system_error for any track whose filename
    // contained a character that code page can't represent -- on the lyrics
    // worker thread, where nothing caught it.
    fs::path p = track_path;
    p.replace_extension(".lrc");
    fs::path parent = p.parent_path();
    if (parent.empty()) return p; // a bare filename has no folder to nest under
    return parent / "lyrics" / p.filename();
}

// The pre-"lyrics/" location: right next to the track. Read-only, so a
// sidecar written by an older build still loads (and save_sidecar() then
// rewrites it into the new folder on the next fetch).
static fs::path legacy_sidecar_path(const fs::path& track_path) {
    if (track_path.empty()) return {};
    fs::path p = track_path;
    p.replace_extension(".lrc");
    return p;
}

static bool read_text_file(const fs::path& p, std::string& out_lrc) {
    if (p.empty()) return false;
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream oss;
    oss << in.rdbuf();
    out_lrc = oss.str();
    return !out_lrc.empty();
}

static bool load_sidecar(const fs::path& track_path, std::string& out_lrc) {
    if (read_text_file(sidecar_path(track_path), out_lrc)) return true;
    return read_text_file(legacy_sidecar_path(track_path), out_lrc);
}

static std::string lrc_source_tag(const std::string& text);   // below

static void save_sidecar(const fs::path& track_path, const std::string& lrc, const std::string& source = std::string()) {
    fs::path p = sidecar_path(track_path);
    if (p.empty() || lrc.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec); // the "lyrics" folder on first fetch, ignored on failure
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    // "[re:...]" is the LRC tag for the editor / origin; the version list shows it
    if (out.is_open()) out << (!source.empty() && lrc_source_tag(lrc).empty() ? "[re:" + source + "]\n" : std::string()) << lrc;
}

// The copy in use: the one in the "lyrics" folder, or (older builds) the one right next to the track.
static fs::path existing_sidecar_file(const fs::path& track_path) {
    std::error_code ec;
    const fs::path a = sidecar_path(track_path);
    if (!a.empty() && fs::exists(a, ec)) return a;
    const fs::path b = legacy_sidecar_path(track_path);
    if (!b.empty() && fs::exists(b, ec)) return b;
    return {};
}

// Lyrics written into the audio file (USLT / SYLT / LYRICS / UNSYNCEDLYRICS ... tags), as ffprobe sees them. A value
// with timestamps wins over plain text; otherwise the longest one. Empty when the file has none (or no ffprobe).
static std::string read_embedded_lyrics(const fs::path& track_path) {
    if (track_path.empty()) return {};
    const ProcResult r = run_capture({"ffprobe", "-v", "error", "-of", "json", "-show_entries", "format_tags:stream_tags",
                                      path_utf8(track_path)}, false);
    tinyjson::Value root;
    if (!r.ok() || !tinyjson::parse(r.out, root)) return {};
    // The tags sit under "format" and under each entry of "streams".
    std::vector<const tinyjson::Value*> tag_sets;
    if (const tinyjson::Value* f = root.find("format")) if (const tinyjson::Value* t = f->find("tags")) tag_sets.push_back(t);
    if (const tinyjson::Value* streams = root.find("streams"))
        for (const tinyjson::Value& st : streams->arr) if (const tinyjson::Value* t = st.find("tags")) tag_sets.push_back(t);
    static const std::regex ts_re(R"(\[\d+:\d+(?:\.\d+)?\])");
    std::string best_synced, best_plain;
    for (const tinyjson::Value* tags : tag_sets) {
        for (const auto& [key, value] : tags->obj) {
            if (value.type != tinyjson::Type::String) continue;
            std::string lk = key;
            for (auto& c : lk) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lk.find("lyr") == std::string::npos) continue;
            const std::string& val = value.str;
            if (val.size() < 8) continue;
            if (std::regex_search(val, ts_re)) { if (val.size() > best_synced.size()) best_synced = val; }
            else if (val.size() > best_plain.size()) best_plain = val;
        }
    }
    return !best_synced.empty() ? best_synced : best_plain;
}

static std::string lrc_source_tag(const std::string& text) {
    static const std::regex re(R"(\[\s*re\s*:\s*([^\]\r\n]*)\])", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(text, m, re)) return {};
    std::string v = m[1].str();
    while (!v.empty() && v.back() == ' ') v.pop_back();
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    return v;
}

static std::string lrc_kind(const std::string& text) {
    static const std::regex word_re(R"(<\d+:\d+(?:\.\d+)?>)");
    static const std::regex line_re(R"(\[\d+:\d+(?:\.\d+)?\])");
    if (std::regex_search(text, word_re)) return "word sync";
    if (std::regex_search(text, line_re)) return "line sync";
    return "no sync";
}

static std::string safe_name_part(std::string s) {
    for (auto& c : s) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
    if (s.empty()) s = "fetched";
    return s.substr(0, 24);
}

static std::string stamp_of(const fs::path& file, const char* fmt) {
    std::error_code ec;
    const auto ft = fs::last_write_time(file, ec);
    std::time_t t = std::time(nullptr);
    if (!ec) {
        const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        t = std::chrono::system_clock::to_time_t(sctp);
    }
    char buf[40];
    std::strftime(buf, sizeof buf, fmt, std::localtime(&t));
    return buf;
}

// Older copies live next to the copy in use: "<stem>.v<YYYYmmdd-HHMMSS>.<source>.lrc".
static fs::path archive_dir(const fs::path& track_path) {
    const fs::path s = sidecar_path(track_path);
    return s.empty() ? fs::path() : s.parent_path();
}
static std::string archive_prefix(const fs::path& track_path) {
    return path_utf8(track_path.stem()) + ".v";
}

struct ArchivedCopy { fs::path file; std::string stamp, source; };
static std::vector<ArchivedCopy> list_archived(const fs::path& track_path) {
    std::vector<ArchivedCopy> out;
    const fs::path dir = archive_dir(track_path);
    if (dir.empty()) return out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    const std::string prefix = archive_prefix(track_path);
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = path_utf8(e.path().filename());
        if (name.size() < prefix.size() + 16 + 4 || name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.compare(name.size() - 4, 4, ".lrc") != 0) continue;
        const std::string mid = name.substr(prefix.size(), name.size() - prefix.size() - 4);   // "20261011-061500.lrclib"
        if (mid.size() < 15 || mid[8] != '-') continue;
        ArchivedCopy a;
        a.file = e.path();
        a.stamp = mid.substr(0, 15);
        a.source = mid.size() > 16 ? mid.substr(16) : std::string();
        out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(), [](const ArchivedCopy& x, const ArchivedCopy& y) { return x.stamp > y.stamp; });
    return out;
}

// Puts the copy in use aside (renamed, so nothing is lost). A copy whose text an archived one already has is just dropped.
static void archive_current_sidecar(const fs::path& track_path) {
    const fs::path cur = existing_sidecar_file(track_path);
    if (cur.empty()) return;
    std::string text;
    read_text_file(cur, text);
    std::error_code ec;
    for (const auto& a : list_archived(track_path)) {
        std::string other;
        if (read_text_file(a.file, other) && other == text) { fs::remove(cur, ec); return; }
    }
    const fs::path dir = archive_dir(track_path);
    fs::create_directories(dir, ec);
    const fs::path dst = dir / path_from_utf8(archive_prefix(track_path) + stamp_of(cur, "%Y%m%d-%H%M%S") + "." +
                                              safe_name_part(lrc_source_tag(text)) + ".lrc");
    fs::rename(cur, dst, ec);
    if (ec) { fs::copy_file(cur, dst, fs::copy_options::overwrite_existing, ec); fs::remove(cur, ec); }
    // keep the ten newest
    auto all = list_archived(track_path);
    for (size_t i = 10; i < all.size(); ++i) fs::remove(all[i].file, ec);
}

// --- "[offset:+500]" tag --------------------------------------------------
// LRC convention: milliseconds, POSITIVE shifts the lyrics EARLIER. The app's
// own `delay` (see LyricsResult) is the opposite way round (positive = later,
// which is how the editor presents it), so the sign flips here and in
// save_lyrics_delay().
static bool is_offset_tag_line(const std::string& line) {
    static const std::regex re(R"(^\s*\[\s*offset\s*:\s*[+-]?\d+(?:\.\d+)?\s*\]\s*$)", std::regex::icase);
    return std::regex_match(line, re);
}

static double parse_lrc_delay(const std::string& lrc_text) {
    static const std::regex re(R"(\[\s*offset\s*:\s*([+-]?\d+(?:\.\d+)?)\s*\])", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(lrc_text, m, re)) return 0.0;
    try { return -std::stod(m[1].str()) / 1000.0; } catch (...) { return 0.0; }
}

bool delete_lyrics_version(const LyricsVersion& v) {
    if (v.kind == LyricsVersion::Kind::Embedded || v.file.empty()) return false;
    std::error_code ec;
    return fs::remove(v.file, ec) && !ec;
}

bool save_lyrics_delay(const fs::path& track_path, double delay_sec) {
    std::string text;
    if (!load_sidecar(track_path, text)) {
        // Nothing fetched. Lyrics inside the file can get a timing correction too: they are copied next to the track (the
        // file's tags stay untouched) and the copy carries the offset.
        text = read_embedded_lyrics(track_path);
        static const std::regex ts_re(R"(\[\d+:\d+(?:\.\d+)?\])");
        if (text.empty() || !std::regex_search(text, ts_re)) return false;   // nothing to correct (or not synced)
        save_sidecar(track_path, text, "embedded");
        if (!load_sidecar(track_path, text)) return false;
    }
    const std::string nl = (text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
    std::string out;
    if (std::abs(delay_sec) >= 0.0005) {
        char tag[40];
        std::snprintf(tag, sizeof tag, "[offset:%+ld]", std::lround(-delay_sec * 1000.0));
        out += tag + nl;
    }
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (is_offset_tag_line(line)) continue; // replaced by the new tag above
        out += line + nl;
    }
    fs::path p = sidecar_path(track_path);
    if (p.empty()) return false;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    if (!o.is_open()) return false;
    o << out;
    return o.good();
}

// --- enhanced/plain LRC parsing ----------------------------------------

static std::vector<LyricLine> parse_lrc(const std::string& lrc_text, bool enhanced) {
    std::string sanitized_lrc = sanitize_lyric_text(lrc_text);
    std::vector<LyricLine> lines;
    std::istringstream stream(sanitized_lrc);
    std::string raw_line;

    static const std::regex line_ts_re(R"(^\[(\d+):(\d+(?:\.\d+)?)\])");
    static const std::regex word_ts_re(R"(<(\d+):(\d+(?:\.\d+)?)>)");

    while (std::getline(stream, raw_line)) {
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();

        std::smatch m;
        if (!std::regex_search(raw_line, m, line_ts_re)) continue; // skip metadata/[ar:]/[ti:] tags etc.

        double line_time = std::stod(m[1].str()) * 60.0 + std::stod(m[2].str());
        std::string rest = raw_line.substr(m.position(0) + m.length(0));

        LyricLine line;
        line.start_time = line_time;

        if (enhanced && rest.find('<') != std::string::npos) {
            // "<mm:ss.xx>word <mm:ss.xx>word ..." — split on word timestamps.
            auto begin = std::sregex_iterator(rest.begin(), rest.end(), word_ts_re);
            auto end = std::sregex_iterator();
            std::vector<std::pair<double, size_t>> marks; // (time, text-start-offset)
            for (auto it = begin; it != end; ++it) {
                std::smatch wm = *it;
                double t = std::stod(wm[1].str()) * 60.0 + std::stod(wm[2].str());
                marks.emplace_back(t, static_cast<size_t>(wm.position(0) + wm.length(0)));
            }
            for (size_t i = 0; i < marks.size(); ++i) {
                size_t start = marks[i].second;
                size_t end_off = (i + 1 < marks.size())
                    ? rest.find('<', start)
                    : rest.size();
                if (end_off == std::string::npos) end_off = rest.size();
                std::string word = rest.substr(start, end_off - start);
                // trim
                while (!word.empty() && std::isspace((unsigned char)word.front())) word.erase(word.begin());
                while (!word.empty() && std::isspace((unsigned char)word.back())) word.pop_back();
                if (!word.empty()) {
                    line.words.emplace_back(marks[i].first, word);
                    if (!line.full_text.empty()) line.full_text += ' ';
                    line.full_text += word;
                }
            }
        }

        if (line.words.empty()) {
            // plain line-synced LRC (or enhanced parse yielded nothing usable)
            while (!rest.empty() && std::isspace((unsigned char)rest.front())) rest.erase(rest.begin());
            line.full_text = rest;
        }

        lines.push_back(std::move(line));
    }

    return lines;
}

// Lines of a text without timestamps, spread evenly over the track (4 s apart when its length is unknown).
static std::vector<LyricLine> plain_lines(const std::string& text, double track_seconds) {
    std::vector<std::string> rows;
    std::istringstream in(sanitize_lyric_text(text));
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        while (!l.empty() && std::isspace(static_cast<unsigned char>(l.front()))) l.erase(l.begin());
        if (!l.empty()) rows.push_back(l);
    }
    std::vector<LyricLine> out;
    const double step = (track_seconds > 10.0 && !rows.empty()) ? (track_seconds - 5.0) / static_cast<double>(rows.size()) : 4.0;
    for (size_t i = 0; i < rows.size(); ++i) {
        LyricLine ln;
        ln.start_time = 2.0 + step * static_cast<double>(i);
        ln.full_text = rows[i];
        out.push_back(std::move(ln));
    }
    return out;
}

// What the "Embedded in the file" version loads.
static bool load_embedded_result(const fs::path& track_path, double track_seconds, LyricsResult& result) {
    const std::string text = read_embedded_lyrics(track_path);
    if (text.empty()) return false;
    std::vector<LyricLine> lines = parse_lrc(text, /*enhanced=*/true);
    const bool synced = !lines.empty();
    if (!synced) lines = plain_lines(text, track_seconds);
    if (lines.empty()) return false;
    result.lines = std::move(lines);
    result.status = LyricsStatus::Ok;
    result.source = "file";
    result.raw_lrc = text;
    result.delay = parse_lrc_delay(text);
    result.unsynced = !synced;
    result.message = synced ? "lyrics from the file" : "lyrics from the file (not synced, spread over the track)";
    return true;
}

static std::atomic<bool> g_name_variants{true};
void set_lyrics_name_variants(bool on) { g_name_variants.store(on); }

LyricsResult fetch_synced_lyrics(const std::string& title, const std::string& artist,
                                  const std::string& helper_script_path, const fs::path& track_path,
                                  bool force_network, double track_seconds) {
    LyricsResult result;

    // 1) Local sidecar file — no subprocess at all if this hits.
    std::string local_lrc;
    if (!force_network && load_sidecar(track_path, local_lrc)) {
        result.lines = parse_lrc(local_lrc, /*enhanced=*/true);
        if (!result.lines.empty()) {
            result.status = LyricsStatus::Ok;
            result.source = "local";
            result.raw_lrc = local_lrc;
            result.delay = parse_lrc_delay(local_lrc);
            result.message = "lyrics loaded (cached)";
            return result;
        }
        // fall through to network chain if the sidecar was empty/unparseable
    }

    // 1b) Lyrics written into the file itself -- before any network call (only when no fetched copy is in use).
    if (!force_network && load_embedded_result(track_path, track_seconds, result)) return result;

    // 2) Python helper (scripts/fetch_lyrics.py): Better Lyrics first
    //    (word-level enhanced LRC from TTML), falling back to LRCLIB
    //    (line-synced only) -- see scripts/lrc.py for the actual fetch
    //    logic. Paxsenix and syncedlyrics are not in this chain: Paxsenix
    //    was unreliable, and syncedlyrics is replaced by calling Better
    //    Lyrics/LRCLIB directly.
    // Windows has no "python3" binary. Which invocation actually works
    // depends on how Python was installed, and the default-enabled Microsoft
    // Store alias means a bare "python" may resolve to a stub that opens the
    // Store instead of running anything. python_command() sorts that out
    // (on Windows it is "py -3" or a full path; elsewhere "python3").
    Args cmd = python_command();
    if (cmd.empty()) {
        result.status = LyricsStatus::PythonMissing;
        result.message = "Python 3 not found on PATH -- lyrics unavailable";
        return result;
    }

    cmd.push_back(helper_script_path);
    cmd.push_back(title);
    cmd.push_back(artist);
    if (g_name_variants.load()) {
        cmd.push_back("--variants");
        // the Latin spelling (made here: the script has no tables): only passed when it differs
        const std::string lt = transliterate(title), la = transliterate(artist);
        if (!lt.empty() && lt != title) { cmd.push_back("--alt-title"); cmd.push_back(lt); }
        if (!la.empty() && la != artist) { cmd.push_back("--alt-artist"); cmd.push_back(la); }
    }
    ProcResult r = run_capture(cmd, /*merge_stderr=*/false);

    if (r.exit_code < 0) {
        // The spawn itself failed -- the interpreter genuinely isn't runnable.
        result.status = LyricsStatus::PythonMissing;
        result.message = "python3 not found on PATH -- lyrics unavailable";
        return result;
    }

    if (r.out.empty()) {
        // python3 ran but the script produced no JSON — crash, timeout,
        // or killed by signal before it could emit().
        result.status = LyricsStatus::Error;
        result.message = "lyrics helper script produced no output (exit " +
                         std::to_string(r.exit_code) + ")";
        return result;
    }

    tinyjson::Value answer;
    tinyjson::parse_answer(r.out, answer);
    if (!tinyjson::bool_field(answer, "ok", false)) {
        const std::string err = tinyjson::string_field(answer, "error");
        const std::string detail = tinyjson::string_field(answer, "detail");

        if (err == "MODULE_MISSING") {
            result.status = LyricsStatus::ModuleMissing;
            result.message = "the lyrics helper (scripts/lrc.py) could not be loaded -- reinstall Mousiki";
        } else if (err == "NOT_FOUND") {
            result.status = LyricsStatus::NotFound;
            result.message = "no lyrics found for \"" + title + "\"";
        } else {
            result.status = LyricsStatus::Error;
            result.message = detail.empty() ? "lyrics fetch failed" : detail;
        }
        return result;
    }

    const std::string lrc = tinyjson::string_field(answer, "lrc");
    const std::string source = tinyjson::string_field(answer, "source");
    const bool enhanced = tinyjson::bool_field(answer, "enhanced", false);
    const std::string variant = tinyjson::string_field(answer, "variant");

    result.lines = parse_lrc(lrc, enhanced);
    result.status = LyricsStatus::Ok;
    result.source = source.empty() ? "better-lyrics" : source;
    result.raw_lrc = lrc;
    result.message = (enhanced ? "word-synced lyrics" : "line-synced lyrics") + std::string(" (") + result.source + ")" +
                     (variant.empty() ? std::string() : " found with: " + variant);

    // A refetch never loses work: the copy in use is put aside as an older version first when its text differs from the
    // new one OR when its saved timing offset differs from the offset of the new one. Only a copy with the same text
    // AND the same offset is simply overwritten -- there is nothing to keep.
    {
        std::string old_text;
        const fs::path old = existing_sidecar_file(track_path);
        if (!old.empty() && read_text_file(old, old_text)) {
            const bool same_text = (old_text == lrc || old_text.find(lrc) != std::string::npos);
            const bool offset_differs = std::abs(parse_lrc_delay(old_text) - parse_lrc_delay(lrc)) >= 0.0005;
            if (!same_text || offset_differs) archive_current_sidecar(track_path);
        }
    }
    save_sidecar(track_path, lrc, result.source); // cache to disk for offline reuse next time
    return result;
}

std::vector<LyricsVersion> list_lyrics_versions(const fs::path& track_path) {
    std::vector<LyricsVersion> out;
    if (track_path.empty()) return out;
    const fs::path cur = existing_sidecar_file(track_path);
    std::string cur_text;
    if (!cur.empty()) read_text_file(cur, cur_text);
    const std::string cur_src = lrc_source_tag(cur_text);
    const std::string emb = read_embedded_lyrics(track_path);

    // short on purpose (the list row is 60 columns): "line sync, +0.5 s, lrclib"
    auto detail_of = [](const std::string& text, const std::string& src) {
        std::string d = lrc_kind(text);
        const double delay = parse_lrc_delay(text);
        if (std::abs(delay) >= 0.0005) {
            char buf[40];
            std::snprintf(buf, sizeof buf, ", %+.1f s", delay);
            d += buf;
        }
        if (!src.empty()) d += ", " + src;
        return d;
    };

    if (!emb.empty() && cur_src != "embedded") {
        LyricsVersion v;
        v.kind = LyricsVersion::Kind::Embedded;
        v.label = "Embedded in the file";
        v.detail = lrc_kind(emb);
        v.active = cur.empty();
        out.push_back(std::move(v));
    }
    if (!cur.empty()) {
        LyricsVersion v;
        v.kind = LyricsVersion::Kind::Fetched;
        v.file = cur;
        v.label = cur_src == "embedded" ? "Embedded in the file, timing saved" : "Fetched " + stamp_of(cur, "%Y-%m-%d %H:%M");
        v.detail = detail_of(cur_text, cur_src == "embedded" ? std::string() : cur_src);
        v.active = true;
        out.push_back(std::move(v));
    }
    for (const auto& a : list_archived(track_path)) {
        std::string text;
        read_text_file(a.file, text);
        LyricsVersion v;
        v.kind = LyricsVersion::Kind::Archived;
        v.file = a.file;
        const std::string st = a.stamp;   // 20261011-061500
        v.label = "Earlier " + st.substr(0, 4) + "-" + st.substr(4, 2) + "-" + st.substr(6, 2) + " " + st.substr(9, 2) + ":" + st.substr(11, 2);
        v.detail = detail_of(text, lrc_source_tag(text));
        out.push_back(std::move(v));
    }
    return out;
}

std::string read_lyrics_version_text(const fs::path& track_path, const LyricsVersion& v) {
    std::string text;
    if (v.kind == LyricsVersion::Kind::Embedded || v.file.empty()) return read_embedded_lyrics(track_path);
    read_text_file(v.file, text);
    return text;
}

fs::path lyrics_folder_of(const fs::path& track_path) {
    const fs::path s = sidecar_path(track_path);
    return s.empty() ? fs::path() : s.parent_path();
}

std::vector<std::string> lyrics_plain_lines(const std::string& raw) {
    static const std::regex stamp_re(R"(\[\s*\d+:\d+(?:[.:]\d+)?\s*\])");     // [mm:ss.xx]
    static const std::regex word_re(R"(<\s*\d+:\d+(?:[.:]\d+)?\s*>)");          // <mm:ss.xx> (enhanced LRC)
    static const std::regex tag_re(R"(^\s*\[\s*[A-Za-z#][A-Za-z0-9_# ]*\s*:[^\]]*\]\s*$)");   // [ar:..] [ti:..] [offset:..] [re:..]
    std::vector<std::string> out;
    std::istringstream in(raw);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) line.erase(0, 3);   // UTF-8 byte order mark
        first = false;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (std::regex_match(line, tag_re)) continue;
        line = std::regex_replace(line, stamp_re, "");
        // A word tag stands between two words ("<00:34.2>Talking<00:35.4>away", no spaces in the file), so it becomes a space -- the same
        // boundary the karaoke view makes -- and the collapse below keeps one of them.
        line = std::regex_replace(line, word_re, " ");
        // collapse the spaces the removed tags leave behind, trim both ends
        std::string t;
        bool space = false;
        for (char c : line) {
            if (c == ' ' || c == '\t') { space = true; continue; }
            if (space && !t.empty()) t += ' ';
            space = false;
            t += c;
        }
        if (t.empty() && !out.empty() && out.back().empty()) continue;   // a run of blank lines is one
        out.push_back(t);
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    size_t lead = 0;
    while (lead < out.size() && out[lead].empty()) ++lead;
    out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(lead));
    return out;
}

bool activate_lyrics_version(const fs::path& track_path, const LyricsVersion& v) {
    std::error_code ec;
    if (v.active) return true;
    if (v.kind == LyricsVersion::Kind::Embedded) {
        archive_current_sidecar(track_path);          // the embedded lyrics load again once no fetched copy is in use
        return existing_sidecar_file(track_path).empty();
    }
    if (v.kind == LyricsVersion::Kind::Archived) {
        std::string text;
        if (!read_text_file(v.file, text)) return false;
        fs::remove(v.file, ec);                       // it takes the place of the copy in use (which goes to the archive)
        archive_current_sidecar(track_path);
        const fs::path p = sidecar_path(track_path);
        if (p.empty()) return false;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream o(p, std::ios::binary | std::ios::trunc);
        if (!o.is_open()) return false;
        o << text;
        return o.good();
    }
    return true;
}

} // namespace muisc
