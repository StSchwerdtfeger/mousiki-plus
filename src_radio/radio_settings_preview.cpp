// The radio's Settings > GENERAL and ANIMA/VIZ: the preview box on the right of the rows, like the music player's (src/settings_preview.cpp).
// It shows a live example of the option that is highlighted (never several at once): the volume bar and the buttons, the spectrum bars,
// the oscilloscope / sphere / spectrogram, MilkDrop, the Latin letters, the marquee, the frame of the upper pane, the play mode -- or, for a
// row that has no picture, a few lines of text that say what it does. The signals are made here (the radio's "TEST Stereo Chords" sound,
// and a spectrogram that reads "Mousiki Plus"): no stream, no file and no network are needed, and what plays is not touched.
#include "translit_preview.h"
#include "radio_ui_internal.h"
#include "bars_presets.h"
#include "bars_help.h"
#include "creator.h"
#include "fft_visualizer.h"
#include "hifi_bars.h"
#include "milkdrop.h"
#include "oscilloscope_visualizer.h"
#include "pane_style.h"
#include "preview_shared.h"
#include "sphere_visualizer.h"
#include "spectrogram.h"
#include "translit.h"
#include "volume_bar.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace muisc::radio::detail {

namespace {

enum PvKind { kPvNone, kPvVolume, kPvDummy, kPvBars, kPvScope, kPvSpectro, kPvSphere, kPvMilk, kPvTranslit, kPvMarquee, kPvPane, kPvPlay, kPvDesc };

constexpr int kPreviewX = 73;     // the column of the box's left border (the longest row text ends before it)
constexpr double kTwoPi = 6.28318530717958647692;
const char* const R = "\x1b[0m";

struct PvState {
    std::chrono::steady_clock::time_point last{};
    bool have_last = false;
    double dt = 0.033, t = 0.0, sig = 0.0;
    FftVisualizer fft;
    HifiBars hifi;
    RadioScope osc;
    SphereVisualizer sph;
    SpectroAnalyzer spec;
    double spec_idle = 0.0;
    std::vector<float> text_audio;   // a spectrogram the Creator makes from the text "Mousiki Plus" (interleaved stereo, 48 kHz)
    size_t text_pos = 0;
    bool text_made = false;
    bool md_active = false, md_locked = false, md_fresh = false;   // the MilkDrop preview put its preset on (the one from before is in md_prev)
    std::string md_prev;
};
PvState& pv() { static PvState p; return p; }


int vis_w(const std::string& s) { return display_width(strip_csi(s)); }

std::vector<std::string> split_nl(const std::string& t) {
    std::vector<std::string> v;
    size_t a = 0;
    while (a <= t.size()) {
        const size_t b = t.find('\n', a);
        v.push_back(t.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return v;
}

// The spectrogram's "gradient" colours: the two ends of the OSCI gradient (as the main screen does)
void spectro_gradient(const Style& s) {
    int r0 = 255, g0 = 255, b0 = 255, r1 = 255, g1 = 255, b1 = 255;
    palette_rgb(s, 0, 0.0f, r0, g0, b0);
    palette_rgb(s, 0, 1.0f, r1, g1, b1);
    auto c8 = [](int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); };
    spectro_set_gradient({c8(r0), c8(g0), c8(b0)}, {c8(r1), c8(g1), c8(b1)});
}

// The short texts of the DESCRIPTION box: what an option does, in a few plain lines. "" = nothing to say (the row has a preview or none).
std::string desc_of(int id) {
    switch (id) {
        case kRsTermSize:
            return "The terminal's size at the start (columns x rows); 0 leaves the terminal as it is.\n[ Back to default ] puts it back to 120 x 32.";
        case kRsPauseFade:
            return "true: pause and resume fade over about 8 ms (no click).\nfalse: the pause cuts the sound at once.\nOne value for all modes.";
        case kRsAutoPlay:
            return "true: the radio tunes the station it was left on as soon as it starts.\nfalse: it starts silent; tune a station yourself.\nOne value for all modes (the music player starts the track it opens with).";
        case kRsOrder1: case kRsOrder2: case kRsOrder3: case kRsOrder4:
            return "The order of the \".\" cycle in the scope block: which view comes at this place. Naming a view puts it here, the view that was here takes the place it left.\nViews that are switched off below are skipped.";
        case kRsStartMode:
            return "Which mode Mousiki opens in: the music player, the radio or the audio books.";
        case kRsOnAir:
            return "The ON AIR sign left of the station info. false: the station info moves to the left edge (the pulse wave goes with the sign).";
        case kRsSharpCorners:
            return "true: every frame has sharp corners (┌ ┐ └ ┘) -- except this Settings screen, which then gets round ones (╭ ╮ ╰ ╯).\nfalse: the frames are round; the Settings screen is sharp.";
        case kRsSignBorder:
            return "true: the sign sits between two columns of colons.\nfalse: no frame around it.";
        case kRsPulse:
            return "true: a circle of light bursts out from behind the sign, in step with the colour wave. Needs the sign.";
        case kRsGradSpeed:
            return "How fast the colours of the ON AIR sign run (cycles per second).";
        case kRsPulseSpeed:
            return "How fast the circle of light behind the sign repeats (loops per second).";
        case kRsStereo:
            return "true: the radio plays in stereo.\nfalse: both channels are mixed into one (mono).";
        case kRsNormalize:
            return "Plays quiet and loud stations at about the same loudness, so the volume need not be changed between them. Target and boost are set in the SHIFT+v overlay.";
        case kRsTuneNoise:
            return "true: radio static fades in and out while another station is tuned.";
        case kRsTimeshift:
            return "How much of the tuned station is kept for pause, rewind and recording from the past (5 to 60 minutes). It is kept on disk: 11.5 MB per minute.";
        case kRsShareProfiles:
            return "true: the radio uses the player's oscilloscope and spectrogram settings, so both look the same. The radio's own values are kept and come back when you turn this off.";
        case kRsUseTermSettings:
            return "true: Mousiki leaves the terminal as it is. Terminal Size and Full Screen are ignored (they keep their values).\nfalse: Mousiki sets the window to Terminal Size / Full Screen at the start.";
        case kRsMediaKeys:
            return "Lets the system's media keys, headphone buttons and now-playing widgets control Mousiki (Windows: media pop-up; Linux: MPRIS, playerctl; macOS: Now Playing). Read at the next start.";
        case kRsReduceMotion:
            return "true: when paused (or nothing plays) and no key was pressed for 3 seconds, the screen is redrawn about 10 times a second instead of at the full frame rate (4 when nothing moves). Saves CPU and terminal traffic, useful over SSH. Any key brings the full rate back; a turning sphere keeps it.";
        case kRsUseMilkdrop:
            return "true: MilkDrop can be shown in the scope block (Osci/sphere = milkdrop). It needs projectM and presets.\nfalse: the MilkDrop visuals are switched off.";
        case kRsMdFps:
            return "How many pictures a second the visuals draw. Lower = less work for the graphics card and the terminal.";
        case kRsMdRes:
            return "The width of the picture the visuals draw, in pixels. Lower = faster, softer.";
        case kRsMdGpu:
            return "The graphics card that draws the visuals. automatic: what the system gives.\nLinux: choose a card here.\nWindows: Settings > System > Display > Graphics, add mousiki.exe and set High performance or Power saving.\nmacOS: the system decides, there is no choice (a MacBook with two graphics chips switches by itself).";
        case kRsMdMesh:
            return "The grid a preset warps the picture with. Finer = smoother warps, but more work for the graphics card; low helps a weak card or battery mode. The visuals restart.";
        case kRsMdAspect:
            return "true: the picture keeps the shape of Aspect Ratio and is never cut off; a wider or taller area gets bars.\nfalse: it fills the whole area, and a wide area cuts the preset at the top and bottom.";
        case kRsMdRatio:
            return "The shape of the picture: auto (the area's own shape, at most 16:9) or a fixed ratio from 1:1 to 21:9, with bars where the area has another shape. Needs Aspect Correction on.";
        case kRsMdDuration:
            return "How long a preset plays before the next one follows. off: no change by time (hard cuts can still change it).";
        case kRsMdVar:
            return "Lets the time of a preset stray by up to this many seconds at random, so the changes do not come like clockwork.";
        case kRsMdShuffle:
            return "true: the next preset is chosen at random.\nfalse: the presets follow in the order of their files.";
        case kRsMdFolder:
            return "The preset folder that the shuffle below plays from: the Favourites, the Recommended list, or a folder you made in the preset overlay (N there; SHIFT+A adds a preset to one).";
        case kRsMdBright: case kRsMdSat: case kRsMdContrast:
            return "The look of the MilkDrop pictures: Brightness 50 % = as drawn (lower darker, higher brighter), Saturation 100 % = the colours as drawn (0 = greyscale), Contrast 0 % = as drawn.";
        case kRsMdFavShuffle:
            return "true: only the presets of the folder chosen in Shuffle Folder play (the Favourites: add them with a in the preset overlay). Without any in it all presets play.";
        case kRsMdHardCuts:
            return "true: a strong beat cuts to the next preset at once. Hard Cut Sensitivity and Hard Cut Interval decide how easily.";
        case kRsMdBeat:
            return "How strong a beat must be for a hard cut. Lower = more cuts. Only matters with Hard Cuts on.";
        case kRsMdHcd:
            return "The shortest time between two hard cuts. Lower = strong beats may cut more often. Only matters with Hard Cuts on.";
        case kRsMdClean:
            return "true: a preset starts from a blank picture, not from what the one before left behind (sharper, but no blend of the remains).";
        case kRsMdTransition:
            return "How long one preset blends into the next. cut: an instant change.";
        case kRsMdUseFx:
            return "true: the effect chosen in MilkDrop Effect is put over the visuals.\nfalse: they are shown as they are drawn.";
        case kRsMdFx:
            return "The video effect (the same list as the music player's Video Effect) put over the visuals while Use Effect is on.";
        case kRsMdColors:
            return "Sixel terminals only. adaptive: the colours are taken from each picture. fixed: one fixed colour set for all pictures (steadier, less exact).";
        case kRsMdShowName:
            return "true: the preset's name shows in a small box over the picture for 3 seconds after it changed.";
        default: break;
    }
    return "";
}

PvKind kind_of(int id, const RadioSettings& c) {
    switch (id) {
        case kRsPaneStyle: case kRsSharpCorners: return kPvPane;
        case kRsDummy: return kPvDummy;
        case kRsVolIndicator: return kPvVolume;
        case kRsVisualizer: case kRsBarStyle: case kRsHifiRes: case kRsHifiChan: case kRsHifiColor: case kRsHifiScale: case kRsHifiAdapt: case kRsHifiSplit:
        case kRsHifiFScale: case kRsHifiGain: case kRsHifiRange: case kRsBehavior: case kRsFluidity: case kRsDegr: case kRsVisc: return kPvBars;
        case kRsUseOsci: case kRsOsciStyle: return kPvScope;
        case kRsUseSphere: return kPvSphere;
        case kRsOrder1: case kRsOrder2: case kRsOrder3: case kRsOrder4: case kRsPauseFade: case kRsAutoPlay: case kRsTermSize: return kPvDesc;
        case kRsUseSpectro: return kPvSpectro;
        case kRsScope:   // (Osci/sphere: what the value in use shows)
            return c.scope_mode == 1 ? kPvSphere : c.scope_mode == 3 ? kPvSpectro : c.scope_mode == 4 ? kPvMilk : c.scope_mode == 2 ? kPvDesc : kPvScope;
        case kRsMarquee: return kPvMarquee;
        case kRsLatinMacrons: case kRsLatinWo: case kRsLatinHanzi: return kPvTranslit;
        case kRsPlayback: return kPvPlay;
        case kRsMdUseFx: case kRsMdFx: case kRsMdColors: case kRsMdShowName: case kRsMdBright: case kRsMdSat: case kRsMdContrast: return kPvMilk;
        default: return desc_of(id).empty() ? kPvNone : kPvDesc;
    }
}

struct PvOut {
    std::vector<std::string> rows;   // ANSI allowed
    int w = 0;                       // the visible width of the rows (centred); 0 = left aligned
    std::string caption;
    bool milk = false;               // the rows are made in the box (a live preset)
    bool picture = false;            // the rows are placeholders for a spectrogram picture the terminal draws (main loop: pv_spectro_gfx)
};

void milk_end() {   // the preset from before comes back
    PvState& P = pv();
    if (!P.md_active) return;
    P.md_active = false;
    Milkdrop& M = Milkdrop::instance();
    if (P.md_locked) { M.set_locked(false); P.md_locked = false; }
    if (!P.md_prev.empty()) M.play_path(P.md_prev);
    P.md_prev.clear();
}

}  // namespace

void settings_preview_end() { milk_end(); pv().spec.set_visible(false); }

void build_settings_preview(const Style& s, const UiModel& m, const RadioSettings& c, std::vector<std::string>& out) {
    const SettingsModel& sm = m.settings;
    const int W = out.empty() ? 0 : std::clamp(m.cols, kUiCols, kMaxCols);
    const int H = static_cast<int>(out.size());
    PvState& P = pv();
    const bool shown_tab = sm.open && (sm.tab == 1 || sm.tab == 2) && sm.theme_ui == 0 && !sm.adev_open && !sm.remote_ov.open;
    if (!shown_tab || W < 100) { milk_end(); P.spec.set_visible(false); return; }
    rs_set_hifi_rows(c.bars_style == 1);
    const int id = rs_row_id(sm.tab, sm.row);
    PvKind kind = kind_of(id, c);

    // the clock
    const auto now = std::chrono::steady_clock::now();
    P.dt = P.have_last ? std::clamp(std::chrono::duration<double>(now - P.last).count(), 0.004, 0.1) : 0.033;
    P.last = now; P.have_last = true;
    P.t += P.dt;

    const bool want_spec = kind == kPvSpectro;
    if (!want_spec) P.spec.set_visible(false);
    P.spec_idle = want_spec ? 0.0 : P.spec_idle + P.dt;
    if (P.spec_idle > 10.0) P.spec.release();   // (moved on: the analysis memory goes back)
    // MilkDrop: where it cannot run the rows keep their text
    PvKind shown = kind;
    if (kind == kPvMilk && !radio_milkdrop_usable(c)) shown = desc_of(id).empty() ? kPvNone : kPvDesc;
    if (shown == kPvMilk) {   // the sample preset, locked; the one from before comes back afterwards
        Milkdrop& M = Milkdrop::instance();
        if (!P.md_active) {
            P.md_prev = M.preset_path();
            if (!M.locked()) { M.set_locked(true); P.md_locked = true; }
            M.play_named("amandio c - secret garden 4");
            P.md_active = true; P.md_fresh = true;
        }
    } else milk_end();
    if (shown == kPvNone) return;

    const int iw = std::min(W - 3, 200) - kPreviewX - 1;      // inside the box
    const int visible = std::max(1, H - 5);
    int bh = std::min(visible - 1, 19);                       // the box, borders included
    int ih = bh - 2;
    if (ih < 6 || iw < 30) return;
    int body_h = ih - 1;                                      // the last row is the caption

    // ---- the signal, only for what is shown ----
    std::vector<float> lr;
    const bool wants_signal = shown == kPvBars || shown == kPvScope || shown == kPvSpectro || shown == kPvSphere;
    if (wants_signal) {
        const size_t n = static_cast<size_t>(std::lround(P.dt * 48000.0));
        double t = P.sig;
        if (shown == kPvSpectro || shown == kPvBars) {   // (the bars listen to the same lively sound)
            if (!P.text_made) {
                P.text_made = true;
                const creator::Gray g = creator::gray_from_text("Mousiki Plus", 6);
                creator::SpecOptions so;
                so.rate = 48000; so.seconds = 8.0; so.octaves = 6;
                creator::Stereo st;
                if (!g.empty() && creator::render_spectrogram_sound(g, so, st) && !st.l.empty()) {
                    const size_t gap = 48000 * 3 / 2;
                    P.text_audio.assign((st.l.size() + gap) * 2, 0.0f);
                    for (size_t i = 0; i < st.l.size(); ++i) { P.text_audio[2 * i] = st.l[i]; P.text_audio[2 * i + 1] = st.r[i]; }
                }
            }
            if (!P.text_audio.empty()) {
                const size_t total = P.text_audio.size() / 2;
                lr.resize(n * 2);
                for (size_t i = 0; i < n; ++i) {
                    const size_t k = (P.text_pos + i) % total;
                    lr[2 * i] = P.text_audio[2 * k]; lr[2 * i + 1] = P.text_audio[2 * k + 1];
                }
                P.text_pos = (P.text_pos + n) % total;
            } else preview_chord_frames(t, n, lr);
        } else preview_chord_frames(t, n, lr);
        P.sig = t;
        std::vector<float> mono(n);
        for (size_t i = 0; i < n; ++i) mono[i] = 0.5f * (lr[2 * i] + lr[2 * i + 1]);
        if (shown == kPvBars || shown == kPvSphere) {
            P.fft.set_fluidity(c.visualizer_fluidity);
            P.fft.set_degradation_speed(c.visualizer_degradation_speed);
            P.fft.set_viscosity(c.visualizer_viscosity);
            P.fft.push_samples(mono.data(), mono.size(), 48000);
        }
        if (shown == kPvScope) P.osc.push_frames(lr.data(), n);
        if (shown == kPvBars && c.bars_style == 1) P.hifi.push(lr.data(), n, 48000);
        if (shown == kPvSpectro) { P.spec.set_visible(true); P.spec.push(lr.data(), n, 48000); }
    }

    PvOut o;
    const bool compact = shown == kPvVolume || shown == kPvDummy || shown == kPvBars || shown == kPvMarquee || shown == kPvPane || shown == kPvPlay;
    const int cw = compact ? iw - 4 : iw - 2;   // content width (one blank column each side; the compact previews have two, like the DESCRIPTION box)
    switch (shown) {
    case kPvVolume: {
        const int hashes = (85 * kVolBar) / 100;
        std::vector<Seg> segs = {{s.border, "VOLUME BAR:["}};
        for (const Seg& g : volume_fill_segs(s, hashes)) segs.push_back(g);
        segs.push_back({s.vol_possible, std::string(static_cast<size_t>(kVolBar - hashes), '-')});
        segs.push_back({s.border, "]"});
        segs.push_back({"", " 85%"});
        o.rows = {seg_line(segs, 12 + kVolBar + 1 + 4)};
        o.w = 12 + kVolBar + 1 + 4;
        o.caption = std::string("Volume Indicator: ") + (c.volume_gradient ? "gradient" : "no gradient");
        break;
    }
    case kPvDummy: {   // the real buttons: three framed boxes; the middle one alternates MUTE / TUNE, << and >> flash in the bottom border colour
        if (!c.dummy_buttons) { o.rows = {"(Dummy Buttons are off)"}; o.w = 23; o.caption = "Dummy Buttons: off"; break; }
        const int bw = kBtnW;
        const bool muted = (static_cast<int>(P.t / 2.0) % 2) == 1;
        const bool flash = std::fmod(P.t, 4.0) < 0.5, back = static_cast<int>(P.t / 4.0) % 2 == 0;
        auto center = [](const std::string& t, int w) { const int pad = std::max(0, w - display_width(t)); return spaces(pad / 2) + t + spaces(pad - pad / 2); };
        auto mid = [&](const std::string& t, const std::string& ink) { return box_line(s, ink + center(t, bw - 4) + kReset, s.border); };
        const std::string lb = (flash && back) ? s.border_bottom : s.border, rb = (flash && !back) ? s.border_bottom : s.border;
        o.rows = {box_top(s, bw, "", s.border) + box_top(s, bw, "", s.border) + box_top(s, bw, "", s.border),
                  mid("<<<", lb) + mid(muted ? "TUNE" : "MUTE", s.border) + mid(">>>", rb),
                  box_bottom(s, bw, "", s.border_bottom) + box_bottom(s, bw, "", s.border_bottom) + box_bottom(s, bw, "", s.border_bottom)};
        o.w = 3 * bw;
        o.caption = "Dummy Buttons: on";
        break;
    }
    case kPvBars: {
        const int n = std::min(cw, 48);
        std::vector<std::string> rows;
        const std::string meta = s.header;
        if (c.bars_style == 1) {   // Hi-Fi: the same drawing as under the station info (four rows high, the test signal in stereo)
            HifiView hv;
            hv.cells = n; hv.rows = 4; hv.resolution = c.hifi_resolution; hv.channels = c.hifi_channels; hv.color = c.hifi_color; hv.scale = c.hifi_scale; hv.adapt = c.hifi_adapt;
            hv.fscale = c.hifi_fscale; hv.gain = c.hifi_gain; hv.range = c.hifi_range; hv.split = c.hifi_split;
            hv.fluidity = c.hifi_fluidity; hv.degradation = c.hifi_degradation; hv.viscosity = c.hifi_viscosity; hv.dt = P.dt;
            spectro_gradient(s);
            const auto& pal = spectro_palette(c.spectro.scheme);
            const std::vector<std::string> hr = hifi_rows(P.hifi, hv, [&](float t) { return s.viz_at(t); },
                [&](float f) {
                    const auto& col = pal[static_cast<size_t>(std::clamp(48 + static_cast<int>(f * 207.0f), 0, 255))];
                    return "\x1b[38;2;" + std::to_string(col[0]) + ";" + std::to_string(col[1]) + ";" + std::to_string(col[2]) + "m";
                });
            rows = {s.key + "Station: TEST Stereo Chords" + R, s.key + "Song   : Test signal" + R, ""};
            for (const auto& l : hr) rows.push_back(l + R);
        } else {
            const auto bars = P.fft.compute_bars(n, P.dt);
            std::string top, bot;
            for (int i = 0; i < n; ++i) {
                const float t = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
                const std::string a = s.viz_at(t);
                top += a + fft_glyph(std::max(0, bars[static_cast<size_t>(i)] - 4));
                bot += a + fft_glyph(std::min(bars[static_cast<size_t>(i)], 4));
            }
            rows = {s.key + "Station: TEST Stereo Chords" + R, s.key + "Song   : Test signal" + R, "", top + R, bot + R};
        }
        (void)meta;
        {   // what the highlighted option does (the box grows with it)
            std::string key;
            switch (id) {
                case kRsVisualizer: key = "onoff"; break; case kRsBarStyle: key = "style"; break; case kRsHifiRes: key = "hifires"; break; case kRsHifiChan: key = "hifichan"; break;
                case kRsHifiColor: key = "hificolor"; break; case kRsHifiScale: key = "hifiscale"; break; case kRsHifiAdapt: key = "hifiadapt"; break; case kRsHifiSplit: key = "hifisplit"; break;
                case kRsHifiFScale: key = "hififscale"; break; case kRsHifiGain: key = "hifigain"; break; case kRsHifiRange: key = "hifirange"; break; case kRsBehavior: key = "behavior"; break;
                case kRsFluidity: key = "fluidity"; break; case kRsDegr: key = "degradation"; break; case kRsVisc: key = "viscosity"; break; default: break;
            }
            const std::string help = muisc::bars_help(key);
            if (!help.empty()) { rows.push_back(""); for (const std::string& w : wrap_lines(help, std::max(20, n), 5)) rows.push_back(w); }
        }
        o.rows = rows;
        o.w = std::max(n, 28);
        o.caption = std::string(bars_preset_name(bars_preset_of(c.bar_fluidity(), c.bar_degradation(), c.bar_viscosity()))) + ": fluidity " + std::to_string(c.bar_fluidity()) +
                    "  degradation " + std::to_string(c.bar_degradation()) + "  viscosity " + std::to_string(c.bar_viscosity());
        break;
    }
    case kPvScope: {
        const int rows_n = std::min(body_h, 12);
        const auto cells = P.osc.render(cw, rows_n, scope_params(c, P.dt));
        const int pal = c.osci().palette;
        for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
            std::string line, last;
            for (int x = 0; x < cw; ++x) {
                const auto& cell = cells[static_cast<size_t>(i)][static_cast<size_t>(x)];
                if (cell.braille == 0) { line += ' '; continue; }
                const float t = cw > 1 ? static_cast<float>(x) / static_cast<float>(cw - 1) : 0.0f;
                const float lvl = std::round(cell.level / 255.0f * 7.0f) / 7.0f;
                std::string a;
                if (pal == 0) a = dim_ansi(s.osci_at(t), 0.30f + 0.70f * lvl);
                else { int r = 255, g = 255, b = 255; palette_rgb(s, pal, cell.hue / 255.0f, r, g, b); a = rgb_seq(r, g, b, 0.30f + 0.70f * lvl); }
                if (a != last) { line += a; last = a; }
                line += utf8_cp(0x2800 + cell.braille);
            }
            o.rows.push_back(line + R);
        }
        o.w = cw;
        o.caption = "oscilloscope";
        break;
    }
    case kPvSpectro: {
        // always the plain defaults (scrolling, mel scale, Roseus colours), whatever the spectrogram is set to: the preview only shows what a spectrogram is
        SpectroSettings sp;
        sp.motion = 1; sp.span = 10;
        spectro_gradient(s);
        P.spec.update(sp);
        if (m.gfx_proto != GfxProto::None) {   // the picture wherever the terminal can show one, braille only without Kitty / Sixel
            m.pv_spectro_set = sp;
            m.pv_spectro = &P.spec;
            for (int r = 0; r < std::min(body_h, 12); ++r) o.rows.push_back(gfx_hole(cw));
            o.picture = true;
            o.caption = "spectrogram (image; default look)";
        } else {
            o.rows = P.spec.braille(sp, cw, std::min(body_h, 12));
            o.caption = "spectrogram (braille; default look)";
        }
        o.w = cw;
        break;
    }
    case kPvSphere: {
        const auto bars = P.fft.compute_bars(48, P.dt);
        const int sw = std::min(cw, 40);
        const auto rows = P.sph.render(sw, std::min(body_h, 12), bars, P.dt);
        const std::string a = s.osci_at(0.5f);
        for (const std::string& r : rows) o.rows.push_back(a + r + R);
        o.w = sw;
        o.caption = "sphere";
        break;
    }
    case kPvMilk: o.milk = true; break;
    case kPvTranslit: {
        // What the option changes, for the highlighted row: the examples with the result for each value; the value in use is lit (the same
        // examples as the player's Settings: translit_preview.cpp).
        TranslitPreviewIn tin;
        tin.kind = id == kRsLatinMacrons ? 0 : id == kRsLatinWo ? 1 : 2;
        tin.macrons = c.latin_macrons ? 1 : 0;
        tin.wo = c.latin_wo ? 1 : 0;
        tin.hanzi = c.latin_hanzi;
        tin.cw = cw;
        tin.t = P.t;
        tin.lit = "\x1b[7m";
        tin.off = R;
        tin.reader_line = "kanji reader: optional (pip install pykakasi janome); the radio uses it while Latin letters are on";
        const TranslitPreviewOut tout = translit_preview(tin);
        o.rows.insert(o.rows.end(), tout.rows.begin(), tout.rows.end());
        const std::string& cap = tout.caption;
        const int want = std::min(visible - 1, static_cast<int>(o.rows.size()) + 3);   // longer than the box: it grows so that nothing is cut off
        if (want > bh) { bh = want; ih = bh - 2; body_h = ih - 1; }
        if (static_cast<int>(o.rows.size()) > body_h) o.rows.resize(static_cast<size_t>(body_h));
        o.w = 0;
        o.caption = cap;
        break;
    }
    case kPvMarquee: {
        const std::string text = "Meowsome Moew Orchestra And The Best-Of-All-Meows feat. Bozai   ";
        const int w = std::max(8, std::min(cw - 10, 24));
        std::string shown_text;
        if (c.marquee) {
            const std::string loop = text + text;
            const size_t off = static_cast<size_t>(P.t * 6.0) % text.size();
            shown_text = loop.substr(off, static_cast<size_t>(w));
        } else shown_text = text.substr(0, static_cast<size_t>(w - 3)) + "...";
        o.rows = {s.key + "Station: " + R + shown_text};
        o.w = 9 + w;
        o.caption = c.marquee ? "Marquee on: the name scrolls" : "Marquee off: cut with ...";
        break;
    }
    case kPvPane: {   // the frame of the upper pane, as it is drawn (the same helpers): sign | station info | scope block
        const int w = std::clamp(cw, 24, 44);
        const panestyle::Glyphs g{c.box_upper_left, c.box_upper_right, c.box_lower_left, c.box_lower_right, c.box_horizontal, c.box_vertical};
        PanePreview pp = pane_preview(c.top_pane_style, w, "sign", "station info", "scope / viz", g, s.border, s.border_bottom, s.legend, R);
        o.rows = std::move(pp.rows);
        o.w = w;
        o.caption = pp.caption;
        break;
    }
    case kPvPlay: {   // the small play-mode box beside the search bar (the letter changes with the value), then what each mode does
        const int dw = iw - 4;
        const std::string letter = c.playback_shuffle ? "S" : "L";
        const std::string sp(static_cast<size_t>(std::max(0, (dw - 5) / 2)), ' ');
        o.rows.push_back(sp + s.border + "╭───╮" + R);
        o.rows.push_back(sp + s.border + c.box_vertical + R + " " + s.border + letter + R + " " + s.border + c.box_vertical + R);
        o.rows.push_back(sp + s.border + "╰───╯" + R);
        o.rows.push_back("");
        static const char* const kModes[] = {"L  list: the next station of the list", "S  shuffle: a random next station"};
        for (int i = 0; i < 2; ++i) {
            const bool on = (i == 1) == c.playback_shuffle;
            o.rows.push_back((on ? s.focus : std::string()) + truncate_str(kModes[i], dw) + (on ? std::string(R) : std::string()));
        }
        o.rows.push_back("");
        for (const std::string& w : wrap_lines("The letter is shown in the box next to the search bar; m changes it.", dw, 3)) o.rows.push_back(w);
        o.w = dw;
        break;
    }
    case kPvDesc: {   // a few lines of text: what the highlighted option does
        const int dw = iw - 4;
        for (const std::string& para : split_nl(id == kRsScope ? std::string("The scope block is switched off: only the station info and the sign show in the upper pane.") : desc_of(id))) {
            if (para.empty()) continue;
            if (!o.rows.empty()) o.rows.push_back("");
            for (const std::string& w : wrap_lines(para, dw, 14)) o.rows.push_back(w);
        }
        o.w = dw;
        break;
    }
    default: return;
    }

    // ---- the box ----
    const bool text_box = shown == kPvDesc;
    if (compact || text_box) {   // two blank rows above, one below (and the caption row), as tall as the content needs
        o.rows.insert(o.rows.begin(), 2, std::string());
        o.rows.push_back("");
        bh = std::min(visible - 1, static_cast<int>(o.rows.size()) + 3);
        ih = bh - 2; body_h = ih - 1;
    }
    std::string label = "Preview";
    {
        std::string head;
        for (const SettingsLine& L : rs_layout(sm.tab)) {
            if (L.header) { if (*L.text) head = L.text; continue; }
            if (L.id != id) continue;
            label = std::string("Preview: ") + (std::string(L.text) == "On/Off" ? head : std::string(L.text));
            break;
        }
    }
    if (shown == kPvDesc) label = "DESCRIPTION";
    if (display_width(label) > iw - 2) label = "Preview";
    const int free_rows = std::max(0, (visible - 1 - bh) / 2);
    const int top = 3 + (free_rows >= 2 ? free_rows - 1 : free_rows);   // (the line index in `out`: under the tab strip and its blank line)
    const int bottom = top + bh - 1;
    if (bottom >= H - 2) return;
    const int x0 = kPreviewX, pw = iw + 2;
    auto bar_at = [&](int y) -> std::string {   // the frame colour of line y: as the settings frame's own
        if (!c.border_smooth) return ansi_fg(y >= H - 3 ? c.border_bottom : c.border_top);
        const float t = H > 1 ? static_cast<float>(y) / static_cast<float>(H - 1) : 0.0f;
        return gradient_fg(c.border_top, c.border_bottom, std::round(t * 32.0f) / 32.0f);
    };
    std::vector<std::string> box(static_cast<size_t>(bh));
    {   // the frame
        const std::string head = bar_at(top) + "┌─ " + R + ansi_fg(c.tab_current) + label + R + bar_at(top) + " ";
        const int used = 3 + display_width(label) + 1;
        box[0] = head + repeat("─", std::max(0, iw + 1 - used)) + "┐" + R;
        for (int i = 1; i < bh - 1; ++i) box[static_cast<size_t>(i)] = bar_at(top + i) + "│" + R + spaces(iw) + bar_at(top + i) + "│" + R;
        box[static_cast<size_t>(bh) - 1] = bar_at(bottom) + "└" + repeat("─", iw) + "┘" + R;
    }
    int xo = o.w > 0 ? std::max(1, (iw - o.w) / 2 + 1) : (compact || text_box ? 3 : 2);
    int vtop = 0;
    if (o.milk) {   // the live preset, drawn like the preset overlay's preview (a picture in Kitty / Sixel terminals, braille otherwise)
        std::vector<std::string> pic;
        if (P.md_fresh) { P.md_fresh = false; m.gfx.w = 0; m.gfx.h = 0; m.gfx.level.clear(); m.gfx.grid.clear(); }
        m.gfx.active = false;
        const int mw = iw - 2, mh = body_h;
        if (build_milkdrop_block(s, m, mw, pic, mh)) {
            o.rows = pic;
            o.w = mw;
            if (m.gfx.active) { m.gfx.col = x0 + 2; m.gfx.row = top + 1; m.gfx.crop = 0; }
            const std::string nm = Milkdrop::instance().preset_name();
            o.caption = id == kRsMdColors ? std::string("Sixel colours: ") + (c.md_fixed_colors ? "fixed" : "adaptive") + (m.gfx_proto == GfxProto::Sixel ? "" : " (Sixel only)")
                                          : nm.empty() ? std::string("MilkDrop") : "MilkDrop: " + nm;
        } else {
            std::string msg;
            radio_milkdrop_usable(c, &msg);
            if (msg.empty()) msg = "milkdrop: starting ...";
            o.rows = {truncate_str(msg, std::max(1, mw))};
            o.w = 0;
            o.caption = "MilkDrop";
        }
        xo = 2; vtop = 0;
    }
    const int nrows = std::min(static_cast<int>(o.rows.size()), body_h);
    if (!o.milk) vtop = std::max(0, (body_h - nrows) / 2);
    if (o.picture && nrows > 0) {   // where the picture goes (0-based row and column)
        m.pv_spectro_area[0] = top + 1 + vtop;
        m.pv_spectro_area[1] = x0 + xo;
        m.pv_spectro_area[2] = o.w;
        m.pv_spectro_area[3] = nrows;
    }
    for (int i = 0; i < nrows; ++i) {
        const std::string& row = o.rows[static_cast<size_t>(i)];
        if (row.empty()) continue;
        const int ri = 1 + vtop + i;
        if (ri >= bh - 1) break;
        const std::string left = bar_at(top + ri) + "│" + R + spaces(xo - 1);
        const int rest = std::max(0, iw - (xo - 1) - vis_w(row));
        box[static_cast<size_t>(ri)] = left + row + R + spaces(rest) + bar_at(top + ri) + "│" + R;
    }
    if (!o.caption.empty()) {
        std::string cap = o.caption;
        if (display_width(cap) > iw - 2) cap = truncate_str(cap, iw - 2);
        const int l = std::max(0, (iw - display_width(cap)) / 2);
        const int ri = bh - 2;
        box[static_cast<size_t>(ri)] = bar_at(top + ri) + "│" + R + spaces(l) + ansi_fg(c.tab_current) + cap + R + spaces(std::max(0, iw - l - display_width(cap))) + bar_at(top + ri) + "│" + R;
    }
    for (int i = 0; i < bh; ++i) out[static_cast<size_t>(top + i)] = splice_line(out[static_cast<size_t>(top + i)], x0, box[static_cast<size_t>(i)], pw);
}

}  // namespace muisc::radio::detail

void muisc::radio::radio_settings_preview_end() { detail::settings_preview_end(); }
