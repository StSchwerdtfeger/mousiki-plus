#pragma once
// ---------------------------------------------------------------------------
// Search inside the help texts (the cheat sheet and the help wizard, SHIFT+s), shared by the music player and the radio.
//
// A query is split into words. A text matches when its words are found in it, in any order. Each query word is looked
// for in three ways, best first:
//   1. as it is (a piece of a word: "queue" finds "queued"),
//   2. as a typo of a word of the text (one wrong, missing, extra or swapped letter; two for long words),
//   3. as a related term ("delete" finds "remove", "exit" finds "close", "song" finds "track" ...).
// Every place that matched is returned as a byte range of the text, so the caller can mark it. A text that matches
// only some of the query words is a partial hit: it is returned too (ranked below the full hits) when at least half
// of the words matched, so one wrong word does not hide everything.
//
// Upper and lower case letters are the same for the ASCII letters; every other byte (umlauts, Japanese ...) must match
// exactly.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace muisc {
namespace helpsearch {

inline char fold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
inline std::string folded(const std::string& s) { std::string o = s; for (char& c : o) c = fold(c); return o; }
inline bool is_word_byte(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u >= 0x80;
}

struct Range { size_t from = 0, to = 0; };   // byte offsets in the text, to exclusive

struct Hit {
    int matched = 0;            // how many query words were found
    int total = 0;              // how many query words there are
    int score = 0;              // higher = better
    unsigned mask = 0;          // bit i set: query word i (the first 31 only) was found
    std::vector<Range> ranges;  // sorted, merged, never overlapping
    bool full() const { return total > 0 && matched == total; }
};

inline std::vector<std::string> split_words(const std::string& q) {
    std::vector<std::string> w;
    std::string cur;
    for (char c : q) {
        if (is_word_byte(c)) cur += fold(c);
        else if (!cur.empty()) { w.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

// Related terms, both ways. Kept short on purpose: the words people use for the same thing in this program.
inline const std::vector<std::vector<std::string>>& synonym_groups() {
    static const std::vector<std::vector<std::string>> g = {
        {"delete", "remove", "erase", "clear"},
        {"exit", "close", "quit", "leave"},
        {"song", "track", "title", "tune"},
        {"folder", "directory", "path"},
        {"tag", "tags", "metadata", "meta"},
        {"lyrics", "karaoke", "words"},
        {"volume", "loudness", "sound"},
        {"start", "open", "launch", "show"},
        {"find", "search", "look"},
        {"playlist", "playlists"},
        {"shuffle", "random"},
        {"pause", "stop"},
        {"download", "save", "export"},
        {"color", "colour", "theme"},
        {"phone", "remote", "android"},
    };
    return g;
}

inline int edit_distance_le(const std::string& a, const std::string& b, int limit) {
    // Damerau-Levenshtein (adjacent swaps count as one), -1 when above the limit.
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    if (std::abs(n - m) > limit) return -1;
    std::vector<std::vector<int>> d(static_cast<size_t>(n + 1), std::vector<int>(static_cast<size_t>(m + 1), 0));
    for (int i = 0; i <= n; ++i) d[static_cast<size_t>(i)][0] = i;
    for (int j = 0; j <= m; ++j) d[0][static_cast<size_t>(j)] = j;
    for (int i = 1; i <= n; ++i) {
        for (int j = 1; j <= m; ++j) {
            const int cost = a[static_cast<size_t>(i - 1)] == b[static_cast<size_t>(j - 1)] ? 0 : 1;
            int v = std::min({d[static_cast<size_t>(i - 1)][static_cast<size_t>(j)] + 1,
                              d[static_cast<size_t>(i)][static_cast<size_t>(j - 1)] + 1,
                              d[static_cast<size_t>(i - 1)][static_cast<size_t>(j - 1)] + cost});
            if (i > 1 && j > 1 && a[static_cast<size_t>(i - 1)] == b[static_cast<size_t>(j - 2)] && a[static_cast<size_t>(i - 2)] == b[static_cast<size_t>(j - 1)])
                v = std::min(v, d[static_cast<size_t>(i - 2)][static_cast<size_t>(j - 2)] + 1);
            d[static_cast<size_t>(i)][static_cast<size_t>(j)] = v;
        }
    }
    const int r = d[static_cast<size_t>(n)][static_cast<size_t>(m)];
    return r <= limit ? r : -1;
}

namespace detail {
struct Token { size_t from, to; };
inline std::vector<Token> tokens(const std::string& low) {
    std::vector<Token> t;
    size_t i = 0;
    while (i < low.size()) {
        if (!is_word_byte(low[i])) { ++i; continue; }
        size_t j = i;
        while (j < low.size() && is_word_byte(low[j])) ++j;
        t.push_back({i, j});
        i = j;
    }
    return t;
}
// Finds `w` as a piece of text: every occurrence is marked; a word start or a whole word scores more.
inline int find_pieces(const std::string& low, const std::vector<Token>& toks, const std::string& w, std::vector<Range>& out) {
    int best = 0;
    for (const Token& t : toks) {
        const size_t pos = low.find(w, t.from);
        if (pos == std::string::npos || pos + w.size() > t.to) continue;
        int s = 60;
        if (pos == t.from) s += 25;
        if (pos == t.from && pos + w.size() == t.to) s += 15;
        best = std::max(best, s);
        out.push_back({pos, pos + w.size()});
    }
    return best;
}
inline int find_typos(const std::string& low, const std::vector<Token>& toks, const std::string& w, std::vector<Range>& out) {
    if (w.size() < 4) return 0;   // a short word is too easy to confuse
    const int limit = w.size() >= 8 ? 2 : 1;
    int best = 0;
    for (const Token& t : toks) {
        const std::string tok = low.substr(t.from, t.to - t.from);
        if (tok.size() < 3) continue;
        const int d = edit_distance_le(w, tok, limit);
        if (d < 0) {
            // a typed start of a longer word with one slip ("queu" -> queue) is covered by the piece search; a word the
            // query is a typo of the START of ("playlst" vs "playlists") is checked here
            if (tok.size() > w.size() && edit_distance_le(w, tok.substr(0, w.size()), limit) >= 0) {
                best = std::max(best, 35);
                out.push_back({t.from, t.to});
            }
            continue;
        }
        best = std::max(best, 45 - 10 * d);
        out.push_back({t.from, t.to});
    }
    return best;
}
inline int find_related(const std::string& low, const std::vector<Token>& toks, const std::string& w, std::vector<Range>& out) {
    int best = 0;
    for (const auto& group : synonym_groups()) {
        bool in_group = std::find(group.begin(), group.end(), w) != group.end();
        if (!in_group && w.size() >= 4) {   // a typo or a start of a word of the group ("delet" for "delete")
            for (const std::string& g : group)
                if (g.compare(0, w.size(), w) == 0 || edit_distance_le(w, g, 1) >= 0) { in_group = true; break; }
        }
        if (!in_group) continue;
        for (const std::string& other : group) {
            if (other == w) continue;
            std::vector<Range> r;
            if (find_pieces(low, toks, other, r) > 0) {
                best = 30;
                out.insert(out.end(), r.begin(), r.end());
            }
        }
    }
    return best;
}
inline void merge(std::vector<Range>& r) {
    std::sort(r.begin(), r.end(), [](const Range& a, const Range& b) { return a.from < b.from; });
    std::vector<Range> m;
    for (const Range& x : r) {
        if (!m.empty() && x.from <= m.back().to) m.back().to = std::max(m.back().to, x.to);
        else m.push_back(x);
    }
    r.swap(m);
}
} // namespace detail

// Matches the query words against one text. The result is empty (matched == 0) when nothing was found.
inline Hit match(const std::vector<std::string>& words, const std::string& text) {
    Hit h;
    h.total = static_cast<int>(words.size());
    if (words.empty()) return h;
    const std::string low = folded(text);
    const auto toks = detail::tokens(low);
    for (size_t wi = 0; wi < words.size(); ++wi) {
        const std::string& w = words[wi];
        std::vector<Range> r;
        int s = detail::find_pieces(low, toks, w, r);
        if (s == 0) { r.clear(); s = detail::find_typos(low, toks, w, r); }
        if (s == 0) { r.clear(); s = detail::find_related(low, toks, w, r); }
        if (s > 0) {
            ++h.matched;
            if (wi < 31) h.mask |= 1u << wi;
            h.score += s;
            h.ranges.insert(h.ranges.end(), r.begin(), r.end());
        }
    }
    detail::merge(h.ranges);
    // The words that sit close together are worth a little more (a phrase beats scattered words).
    if (h.matched >= 2 && !h.ranges.empty()) {
        const size_t span = h.ranges.back().to - h.ranges.front().from;
        if (span < 40) h.score += 10;
    }
    return h;
}

// Whether a hit is worth showing: all words, or at least half of them.
inline bool worth_showing(const Hit& h) { return h.matched > 0 && h.matched * 2 >= h.total; }

// Ranking: full hits before partial ones, then by score.
inline bool better(const Hit& a, const Hit& b) {
    if (a.full() != b.full()) return a.full();
    if (a.matched != b.matched) return a.matched > b.matched;
    return a.score > b.score;
}

} // namespace helpsearch
} // namespace muisc
