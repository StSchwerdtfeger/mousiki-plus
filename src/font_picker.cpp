// The FONT overlay (see font_picker.h).
#include "font_picker.h"
#include "terminal_font.h"
#include "terminal_ui.h"
#include <algorithm>
#include <cctype>

namespace muisc::font_ui {

namespace {
constexpr const char* kR = "\x1b[0m";

int vis(const std::string& s) {
    std::string plain;
    for (size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '\x1b' && k + 1 < s.size() && s[k + 1] == '[') {
            k += 2;
            while (k < s.size() && !(s[k] >= 0x40 && s[k] <= 0x7e)) ++k;
            continue;
        }
        plain += s[k];
    }
    return display_width(plain);
}
std::string blanks(int n) { return std::string(static_cast<size_t>(std::max(0, n)), ' '); }
std::string rep(const std::string& g, int n) { std::string o; for (int i = 0; i < n; ++i) o += g; return o; }
char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
} // namespace

void set_items(State& st, const std::string& current, const std::vector<std::string>& installed) {
    st.items.clear();
    st.items.push_back(terminal_font::kTerminalDefault);
    st.items.push_back(terminal_font::kDefaultFont);
    for (const std::string& f : installed)
        if (f != terminal_font::kDefaultFont && f != terminal_font::kTerminalDefault) st.items.push_back(f);
    if (std::find(st.items.begin(), st.items.end(), current) == st.items.end() && !current.empty())
        st.items.insert(st.items.begin() + 2, current);   // a font that is set but not (any more) installed stays choosable
    st.sel = 0;
    for (size_t i = 0; i < st.items.size(); ++i) if (st.items[i] == current) st.sel = static_cast<int>(i);
}

void open(State& st, const std::string& current) {
    set_items(st, current, terminal_font::list_monospace_fonts());
    st.open = true;
}

int list_rows(int max_h) { return std::clamp(max_h - 9, 3, 18); }

bool key(State& st, int k, bool arrow, bool shifted, int page_rows, std::string& picked, bool& close) {
    close = false;
    const int n = static_cast<int>(st.items.size());
    if (n == 0) { if (k == 27) { st.open = false; close = true; } return false; }
    const int page = std::max(1, page_rows);
    if (arrow && k == 'A') { st.sel = shifted ? std::max(0, st.sel - page) : (st.sel + n - 1) % n; return false; }
    if (arrow && k == 'B') { st.sel = shifted ? std::min(n - 1, st.sel + page) : (st.sel + 1) % n; return false; }
    if (k == kKeyHome) { st.sel = 0; return false; }
    if (k == kKeyEnd) { st.sel = n - 1; return false; }
    if (!arrow && (k == '\r' || k == '\n')) {
        picked = st.items[static_cast<size_t>(st.sel)];
        st.open = false;
        return true;
    }
    if (!arrow && k == 27) { st.open = false; close = true; return false; }
    if (!arrow && k > 32 && k < 127 && std::isalnum(k)) {   // jump to the next font starting with that letter (from the installed ones on)
        for (int step = 1; step <= n; ++step) {
            const int i = (st.sel + step) % n;
            if (i < 2) continue;
            const std::string& f = st.items[static_cast<size_t>(i)];
            if (!f.empty() && lower(f[0]) == lower(static_cast<char>(k))) { st.sel = i; break; }
        }
    }
    return false;
}

std::vector<std::string> build(const State& st, const remote_ui::Style& s, const std::string& current, const std::string& note,
                               int max_w, int max_h, int& width) {
    const int inner = std::clamp(max_w - 4, 30, 64);
    width = inner + 4;
    const int n = static_cast<int>(st.items.size());
    const int rows = std::min(list_rows(max_h), std::max(1, n));
    const int first = std::clamp(st.sel - rows / 2, 0, std::max(0, n - rows));
    std::vector<std::string> out;
    auto line = [&](const std::string& content) {
        out.push_back("\x01" + s.v + kR + " " + content + kR + blanks(inner - vis(content)) + " " + "\x01" + s.v + kR);
    };
    {
        const std::string title = " FONT (" + std::to_string(st.sel + 1) + " / " + std::to_string(n) + ") ";
        out.push_back("\x01" + s.ul + s.h + kR + s.header + title + kR + "\x01" + rep(s.h, inner + 1 - vis(title)) + s.ur + kR);
    }
    line("");
    for (int i = first; i < first + rows && i < n; ++i) {
        const std::string& f = st.items[static_cast<size_t>(i)];
        std::string label = f;
        if (i == 1) label += " (Mousiki's default)";
        const std::string mark = f == current ? "in use" : "";
        const int room = std::max(4, inner - 2 - vis(mark) - 1);
        const std::string shown = truncate_str(label, room);
        std::string body = (i == st.sel ? "> " : "  ") + shown + blanks(inner - 2 - vis(shown) - vis(mark)) + mark;
        line(i == st.sel ? s.cursor + body + kR : s.text + body + kR);
    }
    line(first + rows < n ? s.legend + "  ... " + std::to_string(n - first - rows) + " more" + kR : std::string());
    for (const std::string& l : wrap_lines(note, inner, 2)) line(s.legend + l + kR);
    {
        const std::string legend = " [↑↓] Select  [ENTER] Use  [a-z] Jump  [ESC] Back ";
        const std::string lg = truncate_str(legend, inner - 1);
        out.push_back("\x02" + s.ll + s.h + kR + s.legend + lg + kR + "\x02" + rep(s.h, inner + 1 - vis(lg)) + s.lr + kR);
    }
    const size_t total = out.size();
    for (size_t i = 0; i < total; ++i) {   // the frame's colour, as in the REMOTE APP overlay
        const float t = total > 1 ? static_cast<float>(i) / static_cast<float>(total - 1) : 0.0f;
        const std::string top = s.border_at ? s.border_at(t) : s.border;
        const std::string bottom = s.border_at ? s.border_at(t) : s.border_bottom;
        std::string& l = out[i];
        for (size_t at = l.find('\x01'); at != std::string::npos; at = l.find('\x01', at + top.size())) l.replace(at, 1, top);
        for (size_t at = l.find('\x02'); at != std::string::npos; at = l.find('\x02', at + bottom.size())) l.replace(at, 1, bottom);
    }
    return out;
}

} // namespace muisc::font_ui
