// The script overlay (Mode::CliScripts): the library of the command line's scripts (<config folder>/cli/*.cli, plus startup and aliases) on
// the left, a multi-line editor on the right, and the rows of what the last runs answered at the bottom of the editor's frame.
// Opened with `open scripts` / `edit [name]` in the command line, or with ENTER on "/cs:words" in the search bar.
#include "app.h"
#include "app_state_cliov.h"
#include "cli_core.h"
#include "text_edit.h"
#include "line_block.h"
#include "app_internal.h"
#include "utf8_util.h"
#include "console_log.h"
#include <algorithm>
#include <cstdlib>
#include <set>

namespace muisc {

using cli::Origin;

namespace {
constexpr int kKeyCtrlR = 0x12;   // (CTRL+S has its own key code, kKeyCtrlS)
constexpr size_t kMaxLines = 500;
}

void App::cliov_refresh() {
    CliOvState& o = *cliov_;
    const std::vector<std::string> found = cli::script_names();
    std::set<std::string> all(found.begin(), found.end());
    all.insert("startup");   // (the two special files are always listed, so they can be created here)
    all.insert("aliases");
    std::vector<std::string> v;
    v.push_back("startup");
    v.push_back("aliases");
    for (const std::string& n : all) if (n != "startup" && n != "aliases") v.push_back(n);
    o.names = v;
    o.texts.clear();
    for (const std::string& n : o.names) {
        std::vector<std::string> l;
        std::string err, t;
        if (cli::read_script(n, l, err)) for (const std::string& x : l) t += x + "\n";
        o.texts.push_back(t);
    }
    const std::string cur = (o.sel >= 0 && o.sel < static_cast<int>(o.shown.size())) ? o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])] : std::string();
    o.shown.clear();
    for (size_t i = 0; i < o.names.size(); ++i) if (cli::script_matches(o.names[i], o.texts[i], o.filter)) o.shown.push_back(static_cast<int>(i));
    o.sel = 0;
    for (size_t i = 0; i < o.shown.size(); ++i) if (!cur.empty() && o.names[static_cast<size_t>(o.shown[i])] == cur) o.sel = static_cast<int>(i);
    o.scroll = std::clamp(o.scroll, 0, std::max(0, static_cast<int>(o.shown.size()) - 1));
}

void App::cliov_load(const std::string& name) {
    CliOvState& o = *cliov_;
    std::vector<std::string> l;
    std::string err;
    if (!cli::read_script(name, l, err)) l.clear();   // (startup / aliases that do not exist yet: an empty editor)
    if (l.empty()) l.push_back("");
    o.lines = l;
    o.cur = name;
    o.cy = 0; o.cx = o.anchor = 0; o.top = 0; o.mark = -1;
    o.dirty = false; o.discard_armed = false;
    o.note.clear();
}

void App::cliov_open(const std::string& name, const std::string& filter) {
    CliOvState& o = *cliov_;
    o.filter = filter;
    o.filtering = false;
    o.prompt = 0;
    o.focus = 0;
    cliov_refresh();
    if (!name.empty()) {
        for (size_t i = 0; i < o.shown.size(); ++i) if (o.names[static_cast<size_t>(o.shown[i])] == name) o.sel = static_cast<int>(i);
        if (cli::valid_name(name)) { cliov_load(name); o.focus = 1; }
    } else if (!o.shown.empty() && !filter.empty()) {
        o.sel = 0;
        cliov_load(o.names[static_cast<size_t>(o.shown[0])]);
    }
    mode_ = Mode::CliScripts;
    force_redraw_ = true;
}

bool App::cliov_save() {
    CliOvState& o = *cliov_;
    if (o.cur.empty()) { o.note = "no script: [n] makes a new one"; return false; }
    std::vector<std::string> l = o.lines;
    while (l.size() > 1 && l.back().empty()) l.pop_back();
    std::string err;
    if (!cli::write_script(o.cur, l, err)) { o.note = err; return false; }
    o.dirty = false;
    o.discard_armed = false;
    if (o.cur == "aliases") { cli_aliases_loaded_ = false; }
    o.note = "saved " + o.cur + ".cli";
    log_event("CLI: saved script " + o.cur);
    cliov_refresh();
    return true;
}

void App::cliov_run(bool from_editor) {
    CliOvState& o = *cliov_;
    std::vector<std::string> lines;
    std::string label = o.cur;
    if (from_editor || !o.shown.empty()) {
        if (from_editor) lines = o.lines;
        else {
            label = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(std::clamp(o.sel, 0, static_cast<int>(o.shown.size()) - 1))])];
            std::string err;
            if (!cli::read_script(label, lines, err)) { o.out.push_back("error: " + err); return; }
        }
    }
    if (label.empty()) { o.note = "nothing to run"; return; }
    if (label == "aliases") { o.note = "aliases.cli is the alias list; it is not run"; return; }
    o.out.push_back("> run " + label + (from_editor && o.dirty ? " (the editor text, not saved)" : ""));
    cli::Job job;
    job.origin = Origin::Script;   // (a script: caution commands need force)
    job.to_overlay = true;
    job.label = label;
    for (const std::string& l : lines) job.lines.push_back(l);
    log_event("CLI: running script " + label + " from the editor");
    cli_step(job);
    if (job.lines.empty() && !job.waiting) { o.out.push_back(job.failed ? "(stopped by an error)" : "(done)"); cli_finish(job); }
    else cli_jobs_.push_back(std::move(job));
    while (o.out.size() > 100) o.out.erase(o.out.begin());
}

bool App::cs_in_search() const {
    if (mode_ != Mode::Search) return false;
    const std::string& b = search_.buffer;
    size_t i = 0;
    while (i < b.size() && b[i] == ' ') ++i;
    return b.size() >= i + 3 && (b[i] == 'c' || b[i] == 'C') && (b[i + 1] == 's' || b[i + 1] == 'S') && b[i + 2] == ':';
}

void App::handle_cliov_key(int key) {
    CliOvState& o = *cliov_;
    const bool arrow = last_key_was_arrow();
    const bool shifted = last_key_was_shifted();
    force_redraw_ = true;
    auto line_end = [&] { o.cx = o.anchor = o.lines[static_cast<size_t>(o.cy)].size(); };
    auto fit_cx = [&] { o.cx = o.anchor = std::min(o.cx, o.lines[static_cast<size_t>(o.cy)].size()); };

    if (o.help_open) {   // the list of the commands ([h]): UP / DOWN / PgUp / PgDn / HOME / END move, ESC, ENTER or h close it
        const int total = static_cast<int>(cli_command_list().size());
        const int page = std::max(1, o.rows - 1);
        if (arrow && (key == 'A' || key == 'B')) o.help_sel += (key == 'A' ? -1 : 1) * ((shifted || last_key_was_page()) ? page : 1);
        else if (!arrow && key == kKeyHome) o.help_sel = 0;
        else if (!arrow && key == kKeyEnd) o.help_sel = total - 1;
        else if (!arrow && (key == 27 || key == '\r' || key == '\n' || key == 'h' || key == 'H' || key == 'q')) { o.help_open = false; return; }
        o.help_sel = std::clamp(o.help_sel, 0, std::max(0, total - 1));
        return;
    }

    if (o.prompt == 1 || o.prompt == 2) {   // a name: a new script / the new name of the one selected
        if (!arrow && key == 27) { o.prompt = 0; o.note.clear(); return; }
        if (!arrow && (key == '\r' || key == '\n')) {
            const std::string n = cli::trim(o.name_buf);
            std::string err;
            if (!cli::valid_name(n)) { o.note = "a name has letters, digits, - _ . only"; return; }
            if (o.prompt == 1) {
                bool exists = false;
                for (const std::string& x : o.names) if (x == n) exists = true;
                if (exists && cli::script_path(n) != std::filesystem::path() && std::filesystem::exists(cli::script_path(n))) { o.note = "\"" + n + "\" exists already"; return; }
                cliov_load(n);
                o.lines = {"# " + n, ""};
                o.cy = 1; o.cx = o.anchor = 0;
                o.dirty = true;
                o.focus = 1;
                o.prompt = 0;
                if (!cliov_save()) return;
                o.dirty = false;
                cliov_refresh();
                for (size_t i = 0; i < o.shown.size(); ++i) if (o.names[static_cast<size_t>(o.shown[i])] == n) o.sel = static_cast<int>(i);
                o.note = "new script " + n;
            } else {
                const std::string from = o.names.empty() || o.shown.empty() ? std::string() : o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])];
                std::string extra;
                if (!cli::rename_script(from, n, err, &extra)) { o.note = err; return; }
                if (o.cur == from) o.cur = n;
                else if (o.cur == n && !o.dirty) cliov_load(n);   // (the file of that name is another one now: show it)
                o.prompt = 0;
                cliov_refresh();
                for (size_t i = 0; i < o.shown.size(); ++i) if (o.names[static_cast<size_t>(o.shown[i])] == n) o.sel = static_cast<int>(i);
                o.note = "renamed to " + n + (extra.empty() ? std::string() : " (" + extra + ")");
            }
            return;
        }
        edit_focus("cliov-name", o.name_buf);
        edit_text_key(o.name_buf, edit_caret_, edit_anchor_, key, 48, &o.note);
        return;
    }
    if (o.prompt == 3) {   // delete: y / n (the file is moved to cli/deleted/, nothing is erased)
        if (!arrow && (key == 'y' || key == 'Y')) {
            const std::string n = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])];
            std::string err;
            if (cli::trash_script(n, err)) {
                o.note = n + " moved to the folder deleted/ of the cli folder";
                if (o.cur == n) { o.cur.clear(); o.lines = {""}; o.dirty = false; o.cy = 0; o.cx = o.anchor = 0; }
            } else o.note = err;
            o.prompt = 0;
            cliov_refresh();
        } else if (!arrow) { o.prompt = 0; o.note = "not deleted"; }
        return;
    }

    if (o.filtering) {   // the search field of the library
        if (!arrow && (key == 27)) { o.filtering = false; return; }
        if (!arrow && (key == '\r' || key == '\n')) { o.filtering = false; return; }
        if (arrow && (key == 'A' || key == 'B')) o.filtering = false;
        else {
            edit_focus("cliov-filter", o.filter);
            if (edit_text_key(o.filter, edit_caret_, edit_anchor_, key, 80, &o.note)) { cliov_refresh(); o.sel = 0; o.scroll = 0; }
            return;
        }
    }

    if (!arrow && key == 27 && o.focus == 1 && o.mark >= 0) { o.mark = -1; o.note.clear(); return; }   // (the first ESC only takes the marks away)
    if (!arrow && key == 27) {
        if (o.dirty && !o.discard_armed) { o.discard_armed = true; o.note = "unsaved changes: CTRL+S saves, ESC again closes without saving"; return; }
        o.dirty = false; o.discard_armed = false;
        mode_ = o.from_settings ? Mode::Settings : Mode::Browse;   // (opened from the GENERAL tab: back there)
        o.from_settings = false;
        force_redraw_ = true;
        return;
    }
    if (!arrow && key == 9) { o.focus ^= 1; o.mark = -1; return; }
    if (!arrow && key == kKeyCtrlS) { cliov_save(); return; }
    if (!arrow && key == kKeyCtrlR) { cliov_run(o.focus == 1); return; }
    o.discard_armed = false;

    if (o.focus == 0) {   // the library
        const int n = static_cast<int>(o.shown.size());
        auto pick = [&] {
            if (n == 0) return;
            const std::string name = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])];
            if (o.dirty && name != o.cur && !o.discard_armed) { o.discard_armed = true; o.note = "unsaved changes in " + o.cur + ": CTRL+S first, or the same key again to discard them"; return; }
            cliov_load(name);
            o.focus = 1;
        };
        if (arrow && (key == 'A' || key == 'B')) {
            const int step = shifted ? 8 : 1;
            o.sel = std::clamp(o.sel + (key == 'A' ? -step : step), 0, std::max(0, n - 1));
            return;
        }
        if (!arrow && (key == '\r' || key == '\n' || key == 'e' || key == 'E')) { pick(); return; }
        if (!arrow && key == '/') { o.filtering = true; edit_focus("cliov-filter", o.filter); return; }
        if (!arrow && (key == 'n' || key == 'N')) { o.prompt = 1; o.name_buf.clear(); edit_caret_ = edit_anchor_ = 0; o.note = "name of the new script:"; return; }
        if (!arrow && (key == 'm' || key == 'M') && n > 0) {   // rename the script that is marked (the cursor row)
            const std::string cur_name = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])];
            if (!std::filesystem::exists(cli::script_path(cur_name))) { o.note = cur_name + " has no file yet"; return; }
            o.prompt = 2; o.name_buf = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])]; edit_caret_ = edit_anchor_ = o.name_buf.size(); o.note = "new name:"; return; }
        if (!arrow && (key == 'x' || key == 'X') && n > 0) {
            const std::string nm = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(o.sel)])];
            if (!std::filesystem::exists(cli::script_path(nm))) { o.note = nm + " has no file yet"; return; }
            o.prompt = 3; o.note = "move " + nm + " to deleted/ ? [y/n]"; return;
        }
        if (!arrow && (key == 'h' || key == 'H')) { o.help_open = true; o.help_sel = 0; o.help_top = 0; return; }
        if (!arrow && (key == 'r' || key == 'R')) { cliov_run(false); return; }
        if (!arrow && (key == 'c' || key == 'C')) { o.out.clear(); return; }
        return;
    }

    // the editor
    std::string& line = o.lines[static_cast<size_t>(o.cy)];
    const int n = static_cast<int>(o.lines.size());
    // Whole lines: SHIFT+UP / DOWN marks them (from the line where it began to the caret's line), CTRL+C copies, CTRL+X cuts, DELETE /
    // BACKSPACE deletes them, CTRL+V puts lines (also from another program) above the caret's line, or in place of the marked ones.
    // PgUp / PgDn page (they used to share SHIFT+UP / DOWN with it: a keyboard without them still pages with the mouse-free keys HOME / END).
    if (arrow && (key == 'A' || key == 'B') && shifted && !last_key_was_page()) {
        if (o.mark < 0) o.mark = o.cy;
        o.cy = std::clamp(o.cy + (key == 'A' ? -1 : 1), 0, n - 1);
        fit_cx();
        const int cnt = std::abs(o.cy - o.mark) + 1;
        o.note = std::to_string(cnt) + (cnt == 1 ? " line" : " lines") + " marked";
        return;
    }
    if (o.mark >= 0) {
        const int lo = std::min(o.mark, o.cy), hi = std::max(o.mark, o.cy);
        const int cnt = hi - lo + 1;
        const std::string what = std::to_string(cnt) + (cnt == 1 ? " line" : " lines");
        if (!arrow && key == kKeyCtrlC) { clipboard_set(lineblock::join_lines(o.lines, lo, hi)); o.note = "COPIED " + what; o.mark = -1; return; }
        if (!arrow && (key == kKeyCtrlX || key == kKeyDelete || key == 127 || key == 8)) {
            if (key == kKeyCtrlX) clipboard_set(lineblock::join_lines(o.lines, lo, hi));
            int at = 0;
            lineblock::erase_lines(o.lines, lo, hi, at);
            o.cy = at; o.cx = o.anchor = 0; o.mark = -1; o.dirty = true;
            o.note = (key == kKeyCtrlX ? "CUT " : "DELETED ") + what;
            return;
        }
        if (!arrow && key == kKeyCtrlV) {   // the marked lines are replaced by what is on the clipboard
            const std::string clip = clipboard_get();
            if (lineblock::clean_text(clip).empty()) { o.note = "the clipboard is empty"; return; }
            int at = 0;
            lineblock::erase_lines(o.lines, lo, hi, at);
            o.cy = at; o.cx = 0; o.mark = -1;
            std::string text = lineblock::clean_text(clip);
            if (text.back() != '\n') text += "\n";   // (a text of one line replaces lines too: as a whole line)
            const int added = lineblock::paste_lines(o.lines, o.cy, o.cx, text, kMaxLines);
            o.anchor = o.cx = 0;
            o.dirty = true;
            o.note = "PASTED " + std::to_string(std::max(0, added)) + " lines in place of " + what;
            return;
        }
        o.mark = -1;   // any other key takes the marks away and does what it does
        o.note.clear();
    }
    if (!arrow && key == kKeyCtrlV) {   // lines from the clipboard (a copy of marked lines, or a text of several lines from another program)
        const int added = lineblock::paste_lines(o.lines, o.cy, o.cx, clipboard_get(), kMaxLines);
        if (added >= 0) {
            o.anchor = o.cx;
            o.dirty = true;
            o.note = "PASTED " + std::to_string(added) + (added == 1 ? " line" : " lines") + (added >= 1 && o.lines.size() >= kMaxLines ? " (500 lines at most)" : "");
            return;
        }
    }
    if (arrow && (key == 'A' || key == 'B')) {
        const int step = shifted ? std::max(1, o.rows - 1) : 1;
        o.cy = std::clamp(o.cy + (key == 'A' ? -step : step), 0, n - 1);
        fit_cx();
        return;
    }
    if (arrow && key == 'D' && o.cx == 0 && o.cy > 0) { --o.cy; line_end(); return; }
    if (arrow && key == 'C' && o.cx >= line.size() && o.cy + 1 < n) { ++o.cy; o.cx = o.anchor = 0; return; }
    if (!arrow && (key == '\r' || key == '\n')) {
        if (n >= static_cast<int>(kMaxLines)) { o.note = "500 lines at most"; return; }
        const size_t at = std::min(o.cx, line.size());
        const std::string rest = line.substr(at);
        line.resize(at);
        o.lines.insert(o.lines.begin() + o.cy + 1, rest);
        ++o.cy; o.cx = o.anchor = 0;
        o.dirty = true;
        return;
    }
    if (!arrow && (key == 127 || key == 8) && o.cx == 0 && o.anchor == 0 && o.cy > 0) {   // BACKSPACE at the start joins with the line above
        const size_t at = o.lines[static_cast<size_t>(o.cy - 1)].size();
        o.lines[static_cast<size_t>(o.cy - 1)] += line;
        o.lines.erase(o.lines.begin() + o.cy);
        --o.cy; o.cx = o.anchor = at;
        o.dirty = true;
        return;
    }
    if (key == kKeyDelete && o.cx >= line.size() && o.cy + 1 < n) {
        line += o.lines[static_cast<size_t>(o.cy + 1)];
        o.lines.erase(o.lines.begin() + o.cy + 1);
        o.dirty = true;
        return;
    }
    edit_focus("cliov-editor-" + std::to_string(o.cy), line);
    o.cx = edit_caret_ = std::min(o.cx, line.size());
    edit_anchor_ = o.anchor = std::min(o.anchor, line.size());
    if (edit_text_key(line, edit_caret_, edit_anchor_, key, 600, &o.note)) o.dirty = true;
    o.cx = edit_caret_; o.anchor = edit_anchor_;
}

void App::build_cliov_screen(std::ostringstream& frame, int W, int target_height) const {
    const CliOvState& o = *cliov_;
    const std::string border = ansi_for(settings_.border_color, false);
    const int lw = std::clamp(W / 4 + 7, 24, 40);   // (wide enough for the library legend: [n]ew [m]ren [x]del [r]un [h]elp at 120 columns)
    const int rw = std::max(30, W - lw);
    const int H = std::max(8, target_height);
    const int inner_rows = H - 2;                                   // between the top and the bottom border
    const int out_rows = std::clamp(inner_rows / 5, 2, 4);
    const int ed_rows = std::max(2, inner_rows - out_rows - 1);     // minus the separator line
    // The legend sits in the bottom border, as in the other overlays. The commands are packed in the order of their importance with the
    // separator; what does not fit the width of the pane is left out.
    auto legend_text = [](const std::vector<std::string>& items, const std::string& sep, int cols) {
        std::string t;
        for (const std::string& it : items) {
            const std::string add = t.empty() ? it : sep + it;
            if (display_width(t + add) > cols) break;
            t += add;
        }
        return t;
    };
    o.rows = ed_rows;

    // --- the library (left) ---
    std::vector<std::string> L;
    L.push_back(box_top("SCRIPTS (" + std::to_string(o.shown.size()) + ")" + (o.focus == 0 ? " \xE2\x97\x80" : ""), lw, border));
    {   // row 0: the search field, or a prompt, or the last note
        std::string row;
        if (o.prompt == 1 || o.prompt == 2) {
            const EditPaint p = paint_edit_field(o.name_buf, edit_caret_, edit_anchor_, std::max(1, lw - 4 - 6), std::string(), true);
            L.push_back(box_line_field("name: ", p.s, p.cols, lw, border));
        } else if (o.filtering) {
            const EditPaint p = paint_edit_field(o.filter, edit_caret_, edit_anchor_, std::max(1, lw - 4 - 2), std::string(), true);
            L.push_back(box_line_field("/ ", p.s, p.cols, lw, border));
        } else {
            L.push_back(box_line(o.filter.empty() ? (o.note.empty() ? std::string("[/] search names and text") : truncate_str(o.note, lw - 4)) : "/ " + o.filter + (o.note.empty() ? std::string() : "  " + o.note), lw, border));
        }
    }
    const int list_rows = inner_rows - 1;
    const int total = static_cast<int>(o.shown.size());
    const int sel = std::clamp(o.sel, 0, std::max(0, total - 1));
    int scroll = std::clamp(o.scroll, 0, std::max(0, total - list_rows));
    if (sel < scroll) scroll = sel;
    if (sel >= scroll + list_rows) scroll = sel - list_rows + 1;
    const_cast<CliOvState&>(o).scroll = scroll;
    for (int r = 0; r < list_rows; ++r) {
        const int i = scroll + r;
        if (i >= total) { L.push_back(box_line("", lw, border)); continue; }
        const std::string& nm = o.names[static_cast<size_t>(o.shown[static_cast<size_t>(i)])];
        std::string label = nm;
        if (nm == "startup") label += "  (runs at the start)";
        else if (nm == "aliases") label += "  (the aliases)";
        if (nm == o.cur) label += o.dirty ? " *" : " \xE2\x80\xA2";
        L.push_back(box_line((i == sel ? (o.focus == 0 ? "> " : "- ") : "  ") + truncate_str(label, lw - 6), lw, border));
    }
    const std::vector<std::string> lib_items = o.focus == 0
        ? std::vector<std::string>{"[n]ew", "[m] rename", "[x] del", "[r]un", "[h]elp", "[ENTER] load", "[/] search", "[TAB] editor"}
        : std::vector<std::string>{"[TAB] library"};
    std::string lib_leg = legend_text(lib_items, " ", lw - 5);
    if (o.focus == 0)   // the four commands should all show: shorter spellings for a narrow pane
        for (const char* v : {"[n]ew [m] rename [x] del [r]un [h]elp", "[n]ew [m]ren [x]del [r]un [h]elp", "[n]ew [m]ren [x]del [r]un [h]", "[n]ew [m]ren [x]del [h]elp", "[n] [m] [x] [r] [h]"})
            if (display_width(v) <= lw - 5) { lib_leg = v; break; }
    L.push_back(box_bottom(lw, lib_leg, border));

    // --- the editor (right) ---
    std::vector<std::string> R;
    if (o.help_open) R.push_back(box_top("COMMANDS (" + std::to_string(cli_command_list().size()) + ")  \xE2\x97\x80", rw, border)); else
    R.push_back(box_top(std::string("EDITOR  ") + (o.cur.empty() ? "(no script: [n] in the library)" : o.cur + ".cli") + (o.dirty ? " *" : "") + (o.focus == 1 ? " \xE2\x97\x80" : ""), rw, border));
    const int total_lines = static_cast<int>(o.lines.size());
    int top = std::clamp(o.top, 0, std::max(0, total_lines - ed_rows));
    if (o.cy < top) top = o.cy;
    if (o.cy >= top + ed_rows) top = o.cy - ed_rows + 1;
    o.top = top;
    const int text_w = std::max(4, rw - 4 - 4);
    const std::vector<std::string> cmds = o.help_open ? cli_command_list() : std::vector<std::string>();
    if (o.help_open) {   // the commands instead of the text: the usage on the left (as typed), the short description after it
        const int total = static_cast<int>(cmds.size());
        int htop = std::clamp(o.help_top, 0, std::max(0, total - ed_rows));
        if (o.help_sel < htop) htop = o.help_sel;
        if (o.help_sel >= htop + ed_rows) htop = o.help_sel - ed_rows + 1;
        o.help_top = htop;
        int uw = 0;
        for (const std::string& c : cmds) uw = std::max<int>(uw, static_cast<int>(display_width(c.substr(0, c.find('\t')))));
        const int ucol = std::min<int>(static_cast<int>(uw), std::max(10, (rw - 4) * 2 / 5));
        for (int r = 0; r < ed_rows; ++r) {
            const int i = htop + r;
            if (i >= total) { R.push_back(box_line("", rw, border)); continue; }
            const size_t tab = cmds[static_cast<size_t>(i)].find('\t');
            std::string use = truncate_str(cmds[static_cast<size_t>(i)].substr(0, tab), ucol);
            while (static_cast<int>(display_width(use)) < ucol) use += ' ';
            std::string row = (i == o.help_sel ? "> " : "  ") + use + "  " + cmds[static_cast<size_t>(i)].substr(tab + 1);
            R.push_back(box_line(truncate_str(row, rw - 4), rw, border));
        }
    } else
    for (int r = 0; r < ed_rows; ++r) {
        const int i = top + r;
        if (i >= total_lines) { R.push_back(box_line("", rw, border)); continue; }
        char num[16];
        std::snprintf(num, sizeof num, "%3d ", i + 1);
        if (o.focus == 1 && o.mark >= 0 && i >= std::min(o.mark, o.cy) && i <= std::max(o.mark, o.cy)) {   // a marked line
            const std::string t = truncate_str(o.lines[static_cast<size_t>(i)], text_w);
            R.push_back(box_line_field(num, "\x1b[7m" + t + "\x1b[0m", display_width(t), rw, border));
        } else if (i == o.cy && o.focus == 1) {
            const EditPaint p = paint_edit_field(o.lines[static_cast<size_t>(i)], o.cx, o.anchor, text_w, std::string(), true);
            R.push_back(box_line_field(num, p.s, p.cols, rw, border));
        } else {
            R.push_back(box_line(num + truncate_str(o.lines[static_cast<size_t>(i)], text_w), rw, border));
        }
    }
    {   // the separator and the answers of the last runs
        std::string sep = "\xE2\x94\x80\xE2\x94\x80 OUTPUT ";
        const int fill = std::max(0, rw - 4 - 10);
        for (int i = 0; i < fill; ++i) sep += "\xE2\x94\x80";
        R.push_back(box_line(sep, rw, border));
        const int n = static_cast<int>(o.out.size());
        for (int r = 0; r < out_rows; ++r) {
            const int i = n - out_rows + r;
            R.push_back(box_line(i >= 0 && i < n ? truncate_str(o.out[static_cast<size_t>(i)], rw - 4) : std::string(), rw, border));
        }
    }
    std::vector<std::string> items = {"[CTRL+S] save", "[CTRL+R] run", "[SHIFT+UP/DOWN] mark lines", "[TAB] library", "[ESC] close"};
    if (o.focus == 1 && o.mark >= 0) items = {"[CTRL+C] copy", "[CTRL+X] cut", "[DEL] delete", "[CTRL+V] replace", "[ESC] unmark"};
    if (o.help_open) items = {"[UP/DOWN] scroll", "[PgUp/PgDn] page", "[HOME/END]", "[ESC] back"};
    const std::string leg = legend_text(items, " | ", rw - 4);
    R.push_back(box_bottom(rw, o.focus == 1 && o.mark < 0 && !o.note.empty() && o.prompt == 0 ? truncate_str(o.note, rw - 6) : leg, border));

    for (size_t i = 0; i < L.size() && i < R.size(); ++i) frame << L[i] << R[i] << "\n";
}

}  // namespace muisc
