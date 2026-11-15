// radio_milkdrop.cpp
// The MilkDrop visuals (src/milkdrop.h, projectM loaded at run time) in the radio: the fifth view of the scope block (OsciSphere=milkdrop,
// the "." cycle), drawn as a picture (Kitty: the picture's own colours; Sixel: 216 colours) or as half blocks on other terminals, and the
// visuals overlay (":", key action VisualsMenu, overlay 11): the preset and the MilkDrop options of ANIMA/VIZ -> MILKDROP. The sound comes
// from the radio's audio callback (radio_engine.cpp, milkdrop_feed()).
#include "radio_ui_internal.h"
#include "milkdrop.h"

namespace muisc::radio {

void radio_milkdrop_apply(const RadioSettings& c) {
    MilkdropOptions o;
    o.presets_dir = c.md_presets;
    o.fps = std::clamp(c.md_fps, 10, 60);
    o.max_width = std::clamp(c.md_res, 160, 1920);
    o.preset_seconds = std::max(0, c.md_secs);
    o.shuffle = c.md_shuffle;
    o.cut_sens = static_cast<float>(std::clamp(c.md_cut_sens, 5, 60)) / 10.0f;
    o.hard_cuts = c.md_hard_cuts;
    o.transition = std::clamp(c.md_transition, 0, 10);
    o.gpu = c.md_gpu;
    Milkdrop::instance().configure(o);
}

bool radio_milkdrop_usable(const RadioSettings& c, std::string* why) {
    if (!c.use_milkdrop) { if (why) *why = "switched off (Settings -> ANIMA/VIZ -> Use MilkDrop)"; return false; }
    radio_milkdrop_apply(c);
    return Milkdrop::instance().available(why);
}

namespace detail {

namespace {
// what the scope block's picture was made from (a new size, place or name box makes it again even without a new frame)
struct MdPic {
    std::string key;
    uint64_t frames = 0;
};
MdPic& md_pic() { static MdPic p; return p; }

// nearest-neighbour: the frame (any size) onto w x h pixels
void md_scale(const VideoFrame& f, int w, int h, std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(w) * h * 3);
    if (f.w <= 0 || f.h <= 0) { std::fill(out.begin(), out.end(), 0); return; }
    for (int y = 0; y < h; ++y) {
        const int sy = std::min(f.h - 1, static_cast<int>((static_cast<int64_t>(y) * f.h) / h));
        const uint8_t* srow = f.rgb.data() + static_cast<size_t>(sy) * f.w * 3;
        uint8_t* d = out.data() + static_cast<size_t>(y) * w * 3;
        for (int x = 0; x < w; ++x) {
            const int sx = std::min(f.w - 1, static_cast<int>((static_cast<int64_t>(x) * f.w) / w));
            const uint8_t* s = srow + static_cast<size_t>(sx) * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d += 3;
        }
    }
}
} // namespace

bool build_milkdrop_block(const Style& s, const UiModel& m, int scope_w, std::vector<std::string>& rows) {
    if (scope_w < 8 || !radio_milkdrop_usable(s.c)) return false;
    Milkdrop& md = Milkdrop::instance();
    const bool pic = m.gfx_proto != GfxProto::None;
    const int cw0 = std::max(4, m.cell_w), ch0 = std::max(8, m.cell_h);
    // a big block is drawn at half the pixels (Kitty scales it up, Sixel repeats them): the visuals are soft anyway
    const int scale = pic && scope_w * cw0 > 1280 ? 2 : 1;
    const int cws = std::max(2, cw0 / scale);
    const int chs = std::max(4, static_cast<int>(std::lround(static_cast<double>(cws) * ch0 / cw0)));
    const int pw = scope_w * cws, ph = kPanelH * chs;
    bool fresh = false;
    const VideoFrame* f = md.frame(pw, ph, fresh);   // (the render thread starts here and learns the size)
    // the preset's name for 3 s after a change, in a small box on the bottom row
    std::string name;
    if (s.c.md_show_name && md.since_change() <= 3.0) name = truncate_str(md.preset_name(), std::max(4, scope_w - 4));
    const int nw = name.empty() ? 0 : display_width(name) + 2;
    const int nl = std::max(0, (scope_w - nw) / 2);
    auto name_row = [&](const std::string& left, const std::string& right) {
        return left + "\x1b[0;97;40m " + name + " " + kReset + right;
    };
    if (!f) {   // starting: the status in the middle of the block
        rows.assign(kPanelH, spaces(scope_w));
        const std::string t = truncate_str(md.status().empty() ? std::string("milkdrop: starting ...") : md.status(), scope_w - 2);
        const int l = std::max(0, (scope_w - display_width(t)) / 2);
        rows[kPanelH / 2] = spaces(l) + s.header + t + kReset + spaces(std::max(0, scope_w - l - display_width(t)));
        return true;
    }
    MdPic& st = md_pic();
    if (fresh) ++st.frames;
    if (pic) {
        GfxFrame& g = m.gfx;
        const bool tc = m.gfx_proto == GfxProto::Kitty;
        const std::string key = std::to_string(pw) + "x" + std::to_string(ph) + "|" + std::to_string(scope_w) + "|" + std::to_string(nl) + "," +
                                std::to_string(nw) + (tc ? "|t" : "|p") + (s.c.black_background ? "|b" : "|-");
        const bool remake = key != st.key || !g.plain || g.w != pw || g.h != ph || g.level.size() != static_cast<size_t>(pw) * ph;
        g.plain = true; g.direct = true; g.truecolor = tc;
        g.scale = scale; g.out_cw = cw0; g.out_ch = ch0;
        g.cols = scope_w; g.rows = kPanelH; g.w = pw; g.h = ph;
        g.id = 0; g.grid.clear(); g.cut2_w = 0;
        g.cut_c = nl; g.cut_r = kPanelH - 1; g.cut_w = nw; g.cut_h = nw > 0 ? 1 : 0;
        g.opaque = s.c.black_background;
        g.erase = !s.c.black_background;
        g.active = true;
        g.fresh = fresh || remake;
        if (g.fresh) {
            std::vector<uint8_t> px;
            md_scale(*f, pw, ph, px);
            g.level.assign(static_cast<size_t>(pw) * ph, 255);
            if (tc) g.rgb.swap(px);
            else {   // Sixel: a 6 x 6 x 6 colour cube (216 colours of the 240 there are)
                g.hue.resize(static_cast<size_t>(pw) * ph);
                for (size_t i = 0, n = g.hue.size(); i < n; ++i) {
                    const int r = (px[i * 3] * 5 + 127) / 255, gg = (px[i * 3 + 1] * 5 + 127) / 255, b = (px[i * 3 + 2] * 5 + 127) / 255;
                    g.hue[i] = static_cast<uint8_t>(r * 36 + gg * 6 + b);
                }
                for (int i = 0; i < 216; ++i)
                    g.pal[static_cast<size_t>(i)] = {static_cast<uint8_t>(i / 36 * 51), static_cast<uint8_t>(i / 6 % 6 * 51), static_cast<uint8_t>(i % 6 * 51)};
            }
            if (nw > 0)   // the name box's cells stay free (their pixels transparent)
                for (int y = (kPanelH - 1) * chs; y < ph; ++y)
                    for (int x = nl * cws; x < (nl + nw) * cws && x < pw; ++x) g.level[static_cast<size_t>(y) * pw + x] = 0;
        }
        st.key = key;
        rows.assign(kPanelH, gfx_hole(scope_w));
        if (nw > 0) rows[kPanelH - 1] = name_row(gfx_hole(nl), gfx_hole(std::max(0, scope_w - nl - nw)));
        return true;
    }
    // other terminals: half blocks, two pixels a cell (the upper one the text colour, the lower one the background)
    std::vector<uint8_t> px;
    md_scale(*f, scope_w, kPanelH * 2, px);
    rows.assign(kPanelH, std::string());
    for (int r = 0; r < kPanelH; ++r) {
        std::string line;
        line.reserve(static_cast<size_t>(scope_w) * 40);
        const int last = r == kPanelH - 1 && nw > 0 ? nl : scope_w;
        for (int x = 0; x < last; ++x) {
            const uint8_t* a = px.data() + (static_cast<size_t>(2 * r) * scope_w + x) * 3;
            const uint8_t* b = px.data() + (static_cast<size_t>(2 * r + 1) * scope_w + x) * 3;
            char buf[64];
            std::snprintf(buf, sizeof buf, "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xE2\x96\x80", a[0], a[1], a[2], b[0], b[1], b[2]);
            line += buf;
        }
        line += kReset;
        if (last < scope_w) line = name_row(line, spaces(std::max(0, scope_w - nl - nw)));
        rows[static_cast<size_t>(r)] = line;
    }
    return true;
}

// ---- the visuals overlay (overlay 11) ----

std::vector<std::string> build_md_overlay(const Style& s, const UiModel& m) {
    const RadioSettings& c = s.c;
    const int W = kMdOverlayW, inner = W - 4;
    auto row = [&](const std::string& plain, bool hi, bool dim = false) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return box_line(s, (hi ? std::string("\x1b[7m") : dim ? std::string("\x1b[90m") : std::string()) + body + kReset, s.border);
    };
    Milkdrop& md = Milkdrop::instance();
    std::string why;
    const bool ok = radio_milkdrop_usable(c, &why);
    std::string preset = md.preset_name();
    if (preset.empty()) preset = md.running() ? "(starting ...)" : "(shows when the visuals run)";
    char beat[8];
    std::snprintf(beat, sizeof beat, "%.1f", std::clamp(c.md_cut_sens, 5, 60) / 10.0);
    const std::string labels[kMdRowCount] = {"Preset", "Random Preset", "Lock Preset", "Preset Duration", "Shuffle Presets", "Hard Cut Sensitivity",
                                             "Hard Cuts", "Preset Transition", "Frame Rate", "Resolution", "GPU", "Show Preset Name", "Show in the Scope Block"};
    const std::string vals[kMdRowCount] = {
        preset, "[ENTER]", md.locked() ? "on" : "off",
        c.md_secs <= 0 ? std::string("off") : std::to_string(c.md_secs) + " s",
        c.md_shuffle ? "on" : "off", beat, c.md_hard_cuts ? "on" : "off",
        c.md_transition <= 0 ? std::string("cut") : std::to_string(c.md_transition) + " s",
        std::to_string(c.md_fps) + " fps", std::to_string(c.md_res) + " px", milkdrop_gpu_label(c.md_gpu),
        c.md_show_name ? "on" : "off", c.scope_mode == 4 ? "on" : "off",
    };
    std::vector<std::string> lines;
    lines.push_back(box_top(s, W, "Visuals: MilkDrop", s.border));
    for (int i = 0; i < kMdRowCount; ++i) {
        const bool sel = i == m.overlay_row;
        std::string plain;
        if (i == 0) {
            const std::string lab = std::string(sel ? "> " : "  ") + pad_right(labels[i], 10);
            plain = lab + "< " + truncate_str(vals[i], std::max(4, inner - display_width(lab) - 4)) + " >";
        } else {
            plain = std::string(sel ? "> " : "  ") + pad_right(labels[i], 24) + pad_left(vals[i], std::max(1, inner - 26));
        }
        lines.push_back(row(plain, sel, !ok));
    }
    lines.push_back(row("", false));
    if (!ok) {
        lines.push_back(row("Not available: " + why, false, true));
        lines.push_back(row("Install projectM 4 and a preset pack (see the manual).", false, true));
    } else {
        char info[160];
        const int idx = md.preset_index(), cnt = md.preset_count();
        if (cnt > 0 && idx >= 0) std::snprintf(info, sizeof info, "preset %d of %d   one picture %.0f ms", idx + 1, cnt, md.frame_ms());
        else if (cnt > 0) std::snprintf(info, sizeof info, "a preset outside the list (%d in it)   one picture %.0f ms", cnt, md.frame_ms());
        else std::snprintf(info, sizeof info, "%s", md.running() ? "starting ..." : "not running (nothing shows it)");
        lines.push_back(row(info, false, true));
        const std::string r = md.renderer();
        if (!r.empty()) lines.push_back(row("drawn by " + r + (r.find("llvmpipe") != std::string::npos || r.find("softpipe") != std::string::npos ? " (software)" : ""), false, true));
    }
    push_legend_block(s, lines, W, {"[UP/DOWN] select   [LEFT/RIGHT] change   [ENTER] random",
                                    "[N] / [B] next / previous   [L] lock   [" + menus_key_label(c, "VisualsMenu") + " / ESC] close"});
    return lines;
}

} // namespace detail

void md_overlay_adjust(RadioSettings& c, int row, int dir, std::string& notice) {
    Milkdrop& md = Milkdrop::instance();
    switch (row) {
        case 0: if (dir > 0) md.next_preset(); else md.prev_preset(); break;
        case 1: md.random_preset(); break;
        case 2: md.set_locked(!md.locked()); notice = md.locked() ? "milkdrop: the preset stays" : "milkdrop: the presets change again"; break;
        case 3: c.md_secs = radio_md_step(2, c.md_secs, dir); break;
        case 4: c.md_shuffle = !c.md_shuffle; break;
        case 5: c.md_cut_sens = std::clamp(c.md_cut_sens + dir, 5, 60); break;
        case 6: c.md_hard_cuts = !c.md_hard_cuts; break;
        case 7: c.md_transition = std::clamp(c.md_transition + dir, 0, 10); break;
        case 8: c.md_fps = radio_md_step(0, c.md_fps, dir); break;
        case 9: c.md_res = radio_md_step(1, c.md_res, dir); break;
        case 10: c.md_gpu = radio_md_gpu_step(c.md_gpu, dir); notice = "milkdrop GPU: " + milkdrop_gpu_label(c.md_gpu) + " -- the visuals restart on it"; break;
        case 11: c.md_show_name = !c.md_show_name; break;
        case 12: {
            std::string why;
            if (c.scope_mode == 4) { c.scope_mode = c.use_osci ? 0 : 1; notice = "scope block: back to the " + scope_mode_name(c); }
            else if (!radio_milkdrop_usable(c, &why)) notice = "milkdrop: cannot run here -- " + why;
            else { c.scope_mode = 4; notice = "scope block: milkdrop"; }
            break;
        }
        default: break;
    }
    radio_milkdrop_apply(c);
}

} // namespace muisc::radio
