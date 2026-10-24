// Chapter marks and splitting (see book_split.h).
#include "book_split.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace muisc {

bool parse_chapter_mark(const std::string& raw, ChapterMark& out) {
    // "file name" 12.5 "title"
    if (raw.empty() || raw[0] != '"') return false;
    const size_t q2 = raw.find('"', 1);
    if (q2 == std::string::npos) return false;
    out.file = raw.substr(1, q2 - 1);
    const char* p = raw.c_str() + q2 + 1;
    char* e = nullptr;
    out.sec = std::strtod(p, &e);
    if (!e || e == p) return false;
    std::string rest = e;
    const size_t a = rest.find('"'), b = rest.rfind('"');
    out.title = (a != std::string::npos && b != std::string::npos && b > a) ? rest.substr(a + 1, b - a - 1) : std::string();
    return true;
}

std::string format_chapter_mark(const ChapterMark& m) {
    char b[48];
    std::snprintf(b, sizeof b, "%.1f", std::max(0.0, m.sec));
    std::string t = m.title;
    std::replace(t.begin(), t.end(), '"', '\'');   // CUE has no escape for a quote
    return "\"" + m.file + "\" " + b + " \"" + t + "\"";
}

std::vector<ChapterMark> chapter_marks_of(const BookCue& cue, const std::string& file) {
    std::vector<ChapterMark> v;
    for (const std::string& raw : cue.get_all_raw("REM CHAPTER")) {
        ChapterMark m;
        if (parse_chapter_mark(raw, m) && m.file == file) v.push_back(m);
    }
    std::stable_sort(v.begin(), v.end(), [](const ChapterMark& a, const ChapterMark& b) { return a.sec < b.sec; });
    return v;
}

void chapter_marks_store(BookCue& cue, const std::string& file, const std::vector<ChapterMark>& marks) {
    std::vector<std::string> out;
    for (const std::string& raw : cue.get_all_raw("REM CHAPTER")) {   // the marks of other files stay
        ChapterMark m;
        if (parse_chapter_mark(raw, m) && m.file == file) continue;
        out.push_back(raw);
    }
    for (ChapterMark m : marks) { m.file = file; out.push_back(format_chapter_mark(m)); }
    cue.set_all_raw("REM CHAPTER", out);
}

bool parse_clock(const std::string& text, double& sec) {
    std::string t;
    for (char c : text) if (!std::isspace(static_cast<unsigned char>(c))) t += c;
    if (t.empty()) return false;
    std::vector<std::string> part;
    std::string cur;
    for (char c : t) { if (c == ':') { part.push_back(cur); cur.clear(); } else cur += c; }
    part.push_back(cur);
    if (part.size() > 3) return false;
    double v[3] = {0, 0, 0};
    const size_t n = part.size();
    for (size_t i = 0; i < n; ++i) {
        const std::string& s = part[i];
        if (s.empty()) return false;
        bool dot = false;
        for (char c : s) {
            if (c == '.' && !dot && i == n - 1) { dot = true; continue; }   // only the seconds may have a fraction
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        v[3 - n + i] = std::strtod(s.c_str(), nullptr);
    }
    if (n >= 2 && v[2] >= 60.0) return false;
    if (n == 3 && v[1] >= 60.0) return false;
    sec = v[0] * 3600.0 + v[1] * 60.0 + v[2];
    return true;
}

std::string clock_hms(double sec) {
    const long long t = static_cast<long long>(std::max(0.0, sec) + 0.5);
    char b[32];
    std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    return b;
}

bool is_default_title(const std::string& title) {
    if (title.size() < 9 || title.compare(0, 8, "Chapter ") != 0) return false;
    for (size_t i = 8; i < title.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(title[i]))) return false;
    return true;
}

std::string default_title(int number) { return "Chapter " + std::to_string(number); }

void order_marks(std::vector<ChapterMark>& marks) {
    std::stable_sort(marks.begin(), marks.end(), [](const ChapterMark& a, const ChapterMark& b) { return a.sec < b.sec; });
    for (size_t i = 0; i < marks.size(); ++i)
        if (marks[i].title.empty() || is_default_title(marks[i].title)) marks[i].title = default_title(static_cast<int>(i) + 1);
}

std::string clean_part_title(const std::string& title) {
    std::string out;
    for (char c : title) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') out += ' ';
        else out += c;
    }
    std::string squeezed;   // runs of blanks become one
    for (char c : out) { if (c == ' ' && !squeezed.empty() && squeezed.back() == ' ') continue; squeezed += c; }
    while (!squeezed.empty() && (squeezed.back() == ' ' || squeezed.back() == '.')) squeezed.pop_back();
    while (!squeezed.empty() && (squeezed.front() == ' ' || squeezed.front() == '.')) squeezed.erase(0, 1);
    if (squeezed.size() > 100) {   // cut at a character boundary
        size_t n = 100;
        while (n > 0 && (static_cast<unsigned char>(squeezed[n]) & 0xC0) == 0x80) --n;
        squeezed.erase(n);
    }
    return squeezed;
}

std::vector<SplitPart> plan_split(std::vector<ChapterMark> marks, double duration, const std::string& ext, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return std::vector<SplitPart>(); };
    std::stable_sort(marks.begin(), marks.end(), [](const ChapterMark& a, const ChapterMark& b) { return a.sec < b.sec; });
    std::vector<ChapterMark> m;
    if (marks.empty() || marks.front().sec > 0.5) { ChapterMark z; z.sec = 0.0; m.push_back(z); }
    for (const ChapterMark& c : marks) {
        if (!m.empty() && c.sec - m.back().sec < 1.0) continue;
        if (duration > 0 && c.sec >= duration - 1.0) continue;
        m.push_back(c);
    }
    m[0].sec = 0.0;
    if (m.size() < 2) return fail("set at least one chapter mark besides the start");
    order_marks(m);
    const size_t digits = std::max<size_t>(2, std::to_string(m.size()).size());
    std::vector<SplitPart> parts;
    for (size_t i = 0; i < m.size(); ++i) {
        SplitPart p;
        p.start = m[i].sec;
        p.end = i + 1 < m.size() ? m[i + 1].sec : -1.0;
        p.rstart = p.start;
        p.rend = p.end;
        p.title = m[i].title;
        std::string n = std::to_string(i + 1);
        n.insert(0, digits - std::min(digits, n.size()), '0');
        std::string t = clean_part_title(p.title);
        if (t.empty()) t = default_title(static_cast<int>(i) + 1);
        p.file = n + " " + t + ext;
        parts.push_back(std::move(p));
    }
    return parts;
}

std::vector<std::pair<double, double>> parse_silences(const std::string& log, double duration) {
    std::vector<std::pair<double, double>> out;
    bool open = false;
    double start = 0.0;
    size_t pos = 0;
    while (pos < log.size()) {
        size_t nl = log.find('\n', pos);
        if (nl == std::string::npos) nl = log.size();
        const std::string line = log.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t a = line.find("silence_start:");
        const size_t b = line.find("silence_end:");
        if (a != std::string::npos) { start = std::strtod(line.c_str() + a + 14, nullptr); open = true; }
        else if (b != std::string::npos && open) {
            const double end = std::strtod(line.c_str() + b + 12, nullptr);
            if (end > start) out.emplace_back(start, end);
            open = false;
        }
    }
    if (open && duration > start) out.emplace_back(start, duration);
    return out;
}

int trim_silence(std::vector<SplitPart>& parts, const std::vector<std::pair<double, double>>& silences, double duration, double keep) {
    int trimmed = 0;
    const double eps = 0.05;
    for (SplitPart& p : parts) {
        const double rs = p.rstart;
        const double re = p.rend >= 0 ? p.rend : duration;
        double ns = p.start, ne = p.end;
        for (const auto& sl : silences) {
            // blank at the beginning: a silence that is running when the part starts
            if (sl.first <= rs + eps && sl.second > rs + keep + eps) ns = std::max(ns, sl.second - keep);
            // blank at the end: a silence that is still running when the part ends
            if (re > 0 && sl.second >= re - eps && sl.first < re - keep - eps) ne = ne >= 0 ? std::min(ne, sl.first + keep) : sl.first + keep;
        }
        const double end_at = ne >= 0 ? ne : duration;
        if (end_at > 0 && end_at - ns < 1.0) continue;   // (nearly all blank: left alone)
        if (ns != p.start || ne != p.end) { p.start = ns; p.end = ne; ++trimmed; }
    }
    return trimmed;
}

bool remap_position(const std::vector<SplitPart>& parts, double sec, std::string& file, double& new_sec) {
    if (parts.empty()) return false;
    for (size_t i = parts.size(); i-- > 0;) {
        if (sec >= parts[i].rstart || i == 0) {
            file = parts[i].file;
            new_sec = std::max(0.0, sec - parts[i].start);
            if (parts[i].end >= 0) new_sec = std::min(new_sec, std::max(0.0, parts[i].end - parts[i].start));   // (in a trimmed blank: at the edge)
            return true;
        }
    }
    return false;
}

} // namespace muisc
