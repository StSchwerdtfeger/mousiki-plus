#include "translit_preview.h"

#include "string_util.h"
#include "terminal_ui.h"
#include "translit.h"

namespace muisc {

TranslitPreviewOut translit_preview(const TranslitPreviewIn& in) {
    TranslitPreviewOut out;
    struct PRow { std::string plain, ansi; };
    std::vector<PRow> rows;
        // one example: the source, then the results with the index of the one in use
        // (a row that does not fit the box is split: the source on one row, every result on a row of its own, indented and
        // named by `labels` -- so nothing has to scroll)
        auto example = [&](const std::string& src, const std::vector<std::string>& results, int use,
                           const std::vector<std::string>& labels = {}) {
            PRow r;
            const int pad = std::max(1, 14 - display_width(src));
            r.plain = src + std::string(static_cast<size_t>(pad), ' ') + "\u2192 ";
            r.ansi = r.plain;
            for (size_t k = 0; k < results.size(); ++k) {
                if (k) { r.plain += " | "; r.ansi += " | "; }
                r.plain += results[k];
                r.ansi += (static_cast<int>(k) == use ? lit : std::string()) + results[k] + off;
            }
            if (display_width(r.plain) <= in.cw || results.size() < 1) { rows.push_back(r); return; }
            rows.push_back({src, src});
            for (size_t k = 0; k < results.size(); ++k) {
                const std::string lab = k < labels.size() ? labels[k] + ": " : std::string();
                const bool u = static_cast<int>(k) == use && results.size() > 1;
                rows.push_back({"  " + lab + results[k], "  " + lab + (u ? lit : std::string()) + results[k] + (u ? off : std::string())});
            }
        };
        auto line = [&](const std::string& t) {   // wrapped to the box (no scrolling)
            if (display_width(t) <= in.cw) { rows.push_back({t, t}); return; }
            for (const std::string& w : wrap_lines(t, in.cw, 6)) rows.push_back({w, w});
        };
        // Every result is what Mousiki itself makes of the example with that value (transliterate_as()): nothing is written in by hand.
        // An example whose result never changes: the source with its translation, the result under it (lit, no arrow), as the wide rows above.
        auto same = [&](const std::string& src, const std::string& result) {
            rows.push_back({src, src});
            rows.push_back({"  " + result, "  " + lit + result + off});
        };
        const int m = in.macrons ? 1 : 0, wo = in.wo ? 1 : 0, hz = in.hanzi;
        auto tm = [&](const char* t, int v) { return transliterate_as(t, v, -1, -1); };
        auto tw = [&](const char* t, int v) { return transliterate_as(t, -1, v, -1); };
        auto th = [&](const char* t, int v) { return transliterate_as(t, -1, -1, v); };
        const std::string& reader = in.reader_line;
        std::string cap;
        if (in.kind == 0) {
            line("Long vowels of kana: off | on");
            example("\u30e9\u30fc\u30e1\u30f3", {tm("\u30e9\u30fc\u30e1\u30f3", 0), tm("\u30e9\u30fc\u30e1\u30f3", 1)}, m, {"off", "on"});      // ラーメン
            example("\u30b3\u30fc\u30d2\u30fc", {tm("\u30b3\u30fc\u30d2\u30fc", 0), tm("\u30b3\u30fc\u30d2\u30fc", 1)}, m, {"off", "on"});      // コーヒー
            example("\u3068\u3046\u304d\u3087\u3046", {tm("\u3068\u3046\u304d\u3087\u3046", 0), tm("\u3068\u3046\u304d\u3087\u3046", 1)}, m, {"off", "on"});   // とうきょう
            line("off: the long vowel spelled out (ou, oo, ii); on: a line over it (\u014d, \u012b).");
            cap = std::string("Latin Macrons: ") + (m ? "on" : "off");
        } else if (in.kind == 1) {
            line("The particle \u3092: wo | o");
            example("\u3042\u3044\u3092\u3053\u3081\u3066", {tw("\u3042\u3044\u3092\u3053\u3081\u3066", 1), tw("\u3042\u3044\u3092\u3053\u3081\u3066", 0)}, wo ? 0 : 1, {"wo", "o"});   // あいをこめて
            example("\u3053\u3048\u3092\u304d\u304b\u305b\u3066", {tw("\u3053\u3048\u3092\u304d\u304b\u305b\u3066", 1), tw("\u3053\u3048\u3092\u304d\u304b\u305b\u3066", 0)}, wo ? 0 : 1, {"wo", "o"});   // こえをきかせて
            line("Written \"wo\", spoken \"o\".");
            cap = std::string("Latin Wo: ") + (wo ? "wo" : "o");
        } else if (in.kind == 2) {
            line("Kanji/hanzi with no hint of the language: Leave | Chin. | Jap.");
            example("\u6771\u4eac", {th("\u6771\u4eac", 0), th("\u6771\u4eac", 1), th("\u6771\u4eac", 2)}, hz, {"Leave", "Chin.", "Jap."});   // 東京
            example("\u5f20\u60ac", {th("\u5f20\u60ac", 0), th("\u5f20\u60ac", 1), th("\u5f20\u60ac", 2)}, hz, {"Leave", "Chin.", "Jap."});   // 张悬 (Deserts Chang, a Taiwanese singer-songwriter): only Chinese writes it, so every value reads it as Chinese
            line("(\u5f20 \u60ac are only used in Chinese: the same for every value)");
            if (hz == 2 && th("\u6771\u4eac", 2) == "\u6771\u4eac") line("Jap. needs the kanji reader; without it they stay.");
            line("A short poem of kanji only (this option decides):");
            example("\u6625\u7720\u4e0d\u89c9\u6653  \"Spring sleep, dawn goes unnoticed\"", {th("\u6625\u7720\u4e0d\u89c9\u6653", 0), th("\u6625\u7720\u4e0d\u89c9\u6653", 1), th("\u6625\u7720\u4e0d\u89c9\u6653", 2)}, hz, {"Leave", "Chin.", "Jap."});   // 春眠不觉晓
            example("\u82b1\u843d\u77e5\u591a\u5c11  \"how many flowers fell?\"", {th("\u82b1\u843d\u77e5\u591a\u5c11", 0), th("\u82b1\u843d\u77e5\u591a\u5c11", 1), th("\u82b1\u843d\u77e5\u591a\u5c11", 2)}, hz, {"Leave", "Chin.", "Jap."});   // 花落知多少
            line("Jap. gives the Japanese sound readings of the characters: a Chinese poem is not Japanese, so it reads oddly.");
            line("A poem mixed with kana always stays the same (kana show it is Japanese, this option is not used):");
            same("\u8349\u306e\u540d\u3082  \"even the names of the grass\"", th("\u8349\u306e\u540d\u3082", hz));   // 草の名も
            same("\u5bc2\u3057\u3046 \u306a\u308a\u3051\u308a  \"has grown lonely\"", th("\u5bc2\u3057\u3046 \u306a\u308a\u3051\u308a", hz));   // 寂しうなりけり
            same("\u6625\u306e\u96e8  \"spring rain\"", th("\u6625\u306e\u96e8", hz));   // 春の雨
            if (th("\u6625\u306e\u96e8", hz).find("\u96e8") != std::string::npos)   // (the reader has not read it here: what it makes of it)
                line("with the reader: kusa no na mo / sabishiu narikeri / haru no ame");
            line(reader);
            cap = std::string("Translit. Kanji: ") + (hz == 1 ? "Chin." : hz == 2 ? "Jap." : "Leave");
        } else {   // kSidLatinLookup
            line("Asks MusicBrainz how an artist spells a kanji name in Latin letters (the name is sent there). Off: the name is spelled by the internal transliteration rules.");
            const std::string yanai = transliterate("\u67f3\u4e95 \u611b\u5b50");   // 柳井 愛子
            example("\u67f3\u4e95 \u611b\u5b50", {yanai, "Aiko Yanai"}, in.lookup_on ? 1 : 0, {"off: internal rules", "on: when found"});
            line("(the given name comes first in the Latin spelling.)");
            line("Not asked for names already in Latin or in kana only:");
            example("HAKU \u30cf\u30af\u3002", {transliterate("HAKU \u30cf\u30af\u3002")}, 0);   // the tag holds the Latin name itself
            example("\u30ab\u30cd\u30b3\u30a2\u30e4\u30ce", {transliterate("\u30ab\u30cd\u30b3\u30a2\u30e4\u30ce")}, 0);   // kana only
            cap = std::string("Latin Name Lookup: ") + (in.lookup_on ? "on" : "off");
        }
    for (const PRow& r : rows) {
        if (display_width(r.plain) <= in.cw) { out.rows.push_back(r.ansi); continue; }
        // too wide for the box: the text scrolls
        const std::string loop = r.plain + "    ";
        const int period = std::max(1, display_width(loop));
        const int at = static_cast<int>(in.t * 5.0) % period;
        out.rows.push_back(utf8_skip_take(loop + loop, at, in.cw));
    }
    out.caption = cap;
    return out;
}

}  // namespace muisc
