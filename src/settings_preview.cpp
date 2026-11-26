// Settings > GENERAL and ANIMA/VIZ: the preview box on the right of the rows. It shows a live example of the option that
// is highlighted (never several at once): the volume bar at 85 %, the lyrics, the disk or a cover, the spectrum bars, the
// progress waveform, the oscilloscope / spectrogram / sphere, the transliteration examples, the emoji, the marquee.
//
// The oscilloscope, spectrogram, sphere and bars are driven by their own instances (not the player's). Bars, oscilloscope
// and sphere are fed with the signal of the radio's "TEST Stereo Chords" station, made here in the program; the spectrogram
// with a spectrogram the Creator makes from the text "Mousiki Plus". No file, no network and no ffmpeg are needed.
#include <cstring>
#include "terminal_font.h"
#include "pane_style.h"
#include "corners.h"
#include "preview_shared.h"
#include "app.h"
#include "coverfetch_state.h"
#include "hifi_bars.h"
#include "app_internal.h"
#include "app_state_kanji.h"
#include "app_settings_tables.h"
#include "cover_art.h"
#include "creator.h"
#include "sample_cover_data.h"
#include "miniz.h"
#include "fft_visualizer.h"
#include "oscilloscope_visualizer.h"
#include "sphere_visualizer.h"
#include "spectrogram.h"
#include "bars_help.h"
#include "waveform.h"
#include "translit.h"
#include "translit_preview.h"
#include "terminal_ui.h"
#include "volume_bar.h"
#include "terminal_gfx.h"
#include "video_player.h"
#include "milkdrop.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace muisc {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr int kPreviewX = 71;      // the column of the box's left border
constexpr int kPreviewMinW = 107;  // narrower terminals have no room: no preview

enum PvKind { kPvNone, kPvLyrics, kPvDisk, kPvCover, kPvVolume, kPvBars, kPvWave, kPvScope, kPvSpectro, kPvSphere,
              kPvTranslit, kPvEmoji, kPvMarquee, kPvVideo, kPvClap, kPvDesc, kPvPlayback, kPvMilk, kPvPane };

}  // namespace

struct SettingsPreviewState {
    std::chrono::steady_clock::time_point last{};
    bool have_last = false;
    double dt = 0.033;
    double t = 0.0;               // seconds since the preview started (the lyrics' clock, the progress, the marquee)
    double sig = 0.0;             // the chord signal's own clock, in seconds
    double angle = 0.0;           // the disk
    FftVisualizer fft;
    HifiBars hifi;   // the Hi-Fi bars' own analysis of the test signal
    OscilloscopeVisualizer osc;
    SphereVisualizer sph;
    SpectroAnalyzer spec;
    double spec_idle = 0.0;       // seconds since the spectrogram preview was last shown (its memory goes back after ten)
    std::vector<float> env_raw, env_smooth;
    CoverImage cover;
    std::vector<float> text_audio;   // the spectrogram and spectrum bars preview's sound: a created spectrogram that reads "Mousiki Plus" (interleaved stereo, 48 kHz)
    size_t text_pos = 0;             // in frames
    bool text_made = false;
    std::shared_ptr<VideoPlayer> vid;   // the sample video of the Use Video preview (started when that row is shown)
    bool vid_tried = false, vid_ok = false;
    std::string vid_key;
    bool md_fresh = false;      // the MilkDrop preview just started: the picture slot is emptied first (it still held the main screen's cover)
    bool md_active = false;     // the MilkDrop preview has put its preset on (the one from before is in md_prev)
    std::string md_prev;
    std::string md_cur;         // the preset file the preview plays ("" = the sample preset)
    bool md_pv_locked = false;  // the preview locked the preset (no timer, no hard cut: a beat changed it to another one and blended it in); the lock comes off with it
};

namespace {


// A made-up song for the progress waveform: swells, a quiet part, a loud finish.
std::vector<float> sample_pcm() {
    const size_t n = 4096u * 48u;
    std::vector<float> pcm(n);
    unsigned long long seed = 12345;
    for (size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) / static_cast<double>(n);
        double amp = 0.30 + 0.45 * std::fabs(std::sin(x * kTwoPi * 3.0)) * (0.5 + 0.5 * x);
        if (x > 0.38 && x < 0.50) amp *= 0.45;
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        const double noise = (static_cast<double>((seed >> 33) & 0xFFFF) / 32768.0 - 1.0);
        pcm[i] = static_cast<float>(amp * (0.7 * std::sin(static_cast<double>(i) * 0.05) + 0.5 * noise));
    }
    return pcm;
}

// A small made-up cover (a sunset over water) for the cover looks.
void make_sample_cover(CoverImage& c) {
    // The colour version of the audio book picture (see sample_cover_data.h) as the sample cover, so every cover look (picture and
    // braille) is shown on the same familiar, good-looking image.
    const size_t n = static_cast<size_t>(kSampleCoverW) * static_cast<size_t>(kSampleCoverH);
    std::vector<unsigned char> idx(n);
    mz_ulong len = static_cast<mz_ulong>(n);
    c.ok = false;
    if (mz_uncompress(idx.data(), &len, kSampleCoverZ, static_cast<mz_ulong>(kSampleCoverZLen)) != MZ_OK || len != n) return;
    c.key = "settings-preview-sample-book-colour";
    c.w = kSampleCoverW; c.h = kSampleCoverH;
    c.level.assign(n, 1);
    c.rgb.assign(n * 3, 0);
    c.alpha.assign(n, 255);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char* p = &kSampleCoverPal[static_cast<size_t>(idx[i] & 63) * 3];
        c.rgb[3 * i] = p[0]; c.rgb[3 * i + 1] = p[1]; c.rgb[3 * i + 2] = p[2];
        c.level[i] = static_cast<unsigned char>(std::clamp(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2], 1.0, 255.0));
    }
    c.ok = true;
}


std::vector<std::string> split_lines_nl(const std::string& t) {
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

// The short texts of the DESCRIPTION box: what an option does, in a few plain lines. "" = no description (the row has a preview or none).
std::string pv_desc(int id, const Settings& s) {
    switch (id) {
        case kSidMultiViz:
            return "On a wide screen (a big monitor in full screen) the Viz/Lyrics field is split into up to three panes side by side, about 40 columns each: the first shows the field as usual (or the Main Viz), the others the 2nd and 3rd Viz. On a narrower screen only as many panes as fit are shown; off: one view.";
        case kSidMvMain:
            return "What the first pane shows: as field = what Lyrics/Viz Default and the \".\" key say (lyrics, a visual, the video ...), or one fixed visual. A visual that is on in another pane is not drawn twice.";
        case kSidMvSecond: case kSidMvThird:
            return "What this pane shows beside the first one: off, the sphere, the oscilloscope, the spectrogram, the video of the track or MilkDrop. Pictures (video, MilkDrop, image styles) need a free picture slot of the terminal: two at most besides the disk's cover; a pane without one falls back to the next visual that can be drawn as text.";
        case kSidNormalize:
            return "Plays quiet and loud tracks at about the same loudness, so the volume need not be changed between songs. Target and boost are set in the SHIFT+v overlay.";
        case kSidRateCap:
            return "true: a file above 96 kHz (176.4, 192 kHz ...) is played at 96 kHz. That is far beyond hearing, uses less memory and works on more sound devices.\nfalse: it plays at its own rate, up to 768 kHz.";
        case kSidFadeIn:
            return "The sound of each track comes up from silence at its start over the Length set below (the first audible sample when Cut Silence is on).";
        case kSidFadeOut:
            return "The sound of each track goes down to silence at its end over the Length set below.";
        case kSidFadeCross:
            return "The end of a track and the start of the next one overlap for the Length set below; the next track is loaded early (this switches Gapless Playback to full while it is on). A track shorter than twice the Length gets a shorter overlap. Not in the audio book player.";
        case kSidFadeLen:
            return "How long a fade in, fade out or crossfade lasts. A short track gets a shorter fade.";
        case kSidFadeCurve:
            return "How the volume moves during a fade.\nlinear: even steps.\nexponential: slow at first, fast at the end of a fade in.\nlogarithmic: fast at first, slow at the end of a fade in.\nlogistic: an S-curve, soft at both ends.\nA crossfade keeps the loudness even in the middle.";
        case kSidFadeTrim:
            return "The silence at the start and at the end of a track is not played (at most 15 s at each end). The track is measured once when it is loaded; it costs next to nothing. A long track (one the Track Buffer cannot hold whole: about 3 minutes or longer at the default 64 MB) is cut too: its start while it begins, its end once the whole file has been read, a few seconds after it started.";
        case kSidFadeThr:
            return "Below this level the start and the end of a track count as silence (Cut Silence). -60 dB only cuts real silence; -40 dB also cuts very quiet fade-outs.";
        case kSidVideoAuto:
            return "on: a track that has a video shows it by itself, a track without one shows the Lyrics/Viz Default. Switching it on switches Auto MilkDrop off (the two take turns).";
        case kSidBigMdNoVideo:
            return "on: the big video overlay opens on the MilkDrop visuals for a track that has no video, whatever the Viz/Lyrics field shows. A track with its own MilkDrop setting \"never\" keeps the video view, and the m key in the overlay still switches.";
        case kSidGapless:
            return "off: a short gap between two tracks.\nalbum: no gap between tracks with the same album tag (and the same artist or folder; no tag: the same folder) and the same sample rate.\nfull: no gap between any two tracks that follow by themselves; another sample rate is converted.";
        case kSidUseTermSettings:
            return "true: Mousiki leaves the terminal as it is. Terminal Size and Full Screen are ignored (they keep their values).\nfalse: Mousiki sets the window to Terminal Size / Full Screen at the start.";
        case kSidMediaKeys:
            return "Lets the system's media keys, headphone buttons and now-playing widgets control Mousiki (Windows: media pop-up; Linux: MPRIS, playerctl; macOS: Now Playing). Read at the next start.";
        case kSidVideoMp3:
            return "In SEARCH YT a video that is downloaded without a song as its target also gets its sound saved as an mp3 in the download folder, connected to the video.\nWith a song as the target the video is attached to that song and no mp3 is made (CTRL+o in SEARCH YT asks for one per download).";
        case kSidMdGpu:
            return "The graphics card that draws the visuals. automatic: what the system gives.\nLinux: choose a card here.\nWindows: Settings > System > Display > Graphics, add mousiki.exe and set High performance or Power saving.\nmacOS: the system decides, there is no choice (a MacBook with two graphics chips switches by itself).";
        case kSidMdMesh:
            return "The grid a preset warps the picture with. Finer = smoother warps, but more work for the graphics card; low helps a weak card or battery mode. The visuals restart.";
        case kSidMdAspect:
            return "true: the picture keeps the shape of Aspect Ratio and is never cut off; a wider or taller area gets bars.\nfalse: it fills the whole area, and a wide area cuts the preset at the top and bottom.";
        case kSidMdRatio:
            return "The shape of the picture: auto (the area's own shape, at most 16:9) or a fixed ratio from 1:1 to 21:9, with bars where the area has another shape. Needs Aspect Correction on.";
        case kSidReduceMotion:
            return "true: when paused (or nothing plays) and no key was pressed for 3 seconds, the screen is redrawn about 10 times a second instead of at the full frame rate (4 when nothing moves). Saves CPU and terminal traffic, useful over SSH. Any key brings the full rate back; a turning sphere keeps it.";
        case kSidMdPreset:
            return "The preset the visuals start with. [ENTER] opens the preset overlay to choose it.";
        case kSidMdUseFx:
            return "true: the effect chosen in MilkDrop Effect is put over the visuals.\nfalse: they are shown as they are drawn.";
        case kSidMdFx:
            return "The video effect (the same list as Video Effect) put over the visuals while Use Effect is on.";
        case kSidMdColors:
            return "Sixel terminals only. adaptive: the colours are taken from each picture. fixed: one fixed colour set for all pictures (steadier, less exact).";
        case kSidMdShowName:
            return "true: the preset's name shows in a small box over the picture for 3 seconds after it changed.";
        case kSidMdAuto:
            return "true: the field shows the MilkDrop visuals for every track.\nIf Auto Video (which would show a track's video in the field) is on, it is turned off while this is on, and comes back on when you turn this off again.";
        case kSidMdLock:
            return "true: the preset stays until you change it yourself via n / b, Random or the preset overlay, whatever the time or a beat says.";
        case kSidMdDuration:
            return "How long a preset plays before the next one follows. off: no change by time (hard cuts can still change it).";
        case kSidMdBright: case kSidMdSat: case kSidMdContrast:
            return "The look of the MilkDrop pictures, like the video's grade: Brightness 50 % = as drawn (lower darker, higher brighter), Saturation 100 % = the colours as drawn (0 = greyscale), Contrast 0 % = as drawn.";
        case kSidMdVar:
            return "Lets the time of a preset stray by up to this many seconds at random, so the changes do not come like clockwork.";
        case kSidMdShuffle:
            return "true: the next preset is chosen at random.\nfalse: the presets follow in the order of their files.";
        case kSidMdFolder:
            return "The preset folder that the shuffle below plays from: the Favourites, the Recommended list, or a folder you made in the preset overlay (n there; SHIFT+a adds a preset to one).";
        case kSidMdFavShuffle:
            return "true: only the presets of the folder chosen in Shuffle Folder play (the Favourites: add them with a in the preset overlay). Without any in it all presets play.";
        case kSidMdHardCuts:
            return "true: a strong beat cuts to the next preset at once. Hard Cut Sensitivity and Hard Cut Interval decide how easily.";
        case kSidMdCutSens:
            return "How strong a beat must be for a hard cut. Lower = more cuts. Only matters with Hard Cuts on.";
        case kSidMdHcd:
            return "The shortest time between two hard cuts. Lower = strong beats may cut more often. Only matters with Hard Cuts on.";
        case kSidMdClean:
            return "true: a preset starts from a blank picture, not from what the one before left behind (sharper, but no blend of the remains).";
        case kSidMdTransition:
            return "How long one preset blends into the next. cut: an instant change.";
        case kSidShareProfiles:
            return "true: the radio uses the player's oscilloscope and spectrogram settings, so both look the same. The radio's own values are kept and come back when you turn this off.";
        case kSidSpectroHistory:
            return "true: the spectrogram keeps analysing the sound while it is not on screen, so its picture is complete when you open it (about 11 MB of memory).\nfalse: only while it is shown (less memory).";
        default: break;
    }
    (void)s;
    return "";
}

PvKind kind_of(int id, const Settings& s) {
    switch (id) {
        case kSidLyrAlign: case kSidLyrAnim: case kSidUseLyrics: return kPvLyrics;
        case kSidUseVideo: case kSidVideoLyrics: case kSidVideoSize: case kSidVideoFx: case kSidVideoRes: case kSidVideoFps: case kSidVideoBuffer: case kSidVideoContrast: case kSidVideoBrightness: case kSidVideoColors: case kSidVideoChroma: case kSidVideoDither: case kSidVideoBase: case kSidVideoFxContrast: case kSidVideoGradSat: case kSidVideoGradVivid: case kSidVideoRound: return kPvVideo;   // (a sample video; the lyrics preview when it is not there)
        case kSidVideoCover: return kPvNone;
        case kSidVideoClap: return kPvClap;
        case kSidMdPreset: return s.milkdrop_default_preset.empty() ? kPvDesc : kPvMilk;   // (Default Preset: its own preset live once one is chosen; before, the description)
        case kSidMdUseFx: case kSidMdFx: case kSidMdColors: case kSidMdShowName: case kSidMdBright: case kSidMdSat: case kSidMdContrast: return kPvMilk;   // (a live preset: with the effect, the name box and the Sixel colours as set)
        case kSidDisk: case kSidDiskSpeed: case kSidDiskBorder: case kSidDiskVivid: return kPvDisk;
        case kSidSharpCorners: case kSidThickBorders: return kPvPane;
        case kSidAlbumCover: case kSidCoverMode: case kSidCoverContrast: case kSidCoverBrightness: case kSidCoverColors: case kSidCoverGradSat: case kSidCoverVivid: case kSidCoverChroma:
        case kSidCoverSearch: return kPvCover;
        case kSidVolIndicator: case kSidDummy: case kSidVolVivid: return kPvVolume;
        case kSidVisualizer: case kSidVisFluidity: case kSidVisDegr: case kSidVisVisc: case kSidBarStyle: case kSidHifiRes: case kSidHifiChan: case kSidHifiColor: case kSidHifiScale: case kSidHifiAdapt: case kSidHifiSplit: case kSidHifiFScale: case kSidHifiGain: case kSidHifiRange: case kSidBarBehavior: case kSidBarsVivid: return kPvBars;
        case kSidWave: case kSidWaveStyle: case kSidWaveVivid: return kPvWave;
        case kSidUseOsci: case kSidScopeVivid: return kPvScope;   // (Modes Share Settings: no preview)
        case kSidUseSpectro: case kSidSpectroVivid: return kPvSpectro;
        case kSidUseSphere: case kSidPauseSphere: return kPvSphere;
        case kSidLyricViz: case kSidNoLyricsViz: case kSidNoVideoViz: {
            // Lyrics/Viz Defaults is edited in viz_default (0 sphere, 1 osci, 2 spectro, 3 lyrics, 4 last used: the view in use, lyric_viz);
            // lyric_viz alone is only what the field shows now, so the preview showed the wrong picture until the row was changed twice.
            int v = id == kSidLyricViz ? s.viz_default : id == kSidNoVideoViz ? s.novideo_viz : s.nolyrics_viz;
            if (id == kSidNoVideoViz && v == 4) v = 3;   // the lyrics (3 = lyrics here); "none" has nothing to preview, shown as the lyrics too
            if (id == kSidLyricViz && v == 4) v = (s.element_lyrics || s.video_view) ? 3 : s.lyric_viz;   // the view in use right now
            if (id == kSidLyricViz && v == 5) return kPvVideo;    // video: the sample video
            if (id == kSidLyricViz && v == 6) return kPvMilk;     // milkdrop: a preset
            if (id == kSidNoLyricsViz && v == 4) return kPvVideo;
            if (id == kSidNoLyricsViz && v == 5) return kPvMilk;
            if (id == kSidNoVideoViz && v == 5) return kPvMilk;
            return v == 0 ? kPvSphere : v == 1 ? kPvScope : v == 2 ? kPvSpectro : kPvLyrics;
        }
        case kSidLatinMacrons: case kSidLatinWo: case kSidLatinHanzi: case kSidLatinLookup: return kPvTranslit;
        case kSidEmoji: return kPvEmoji;
        case kSidMarquee: return kPvMarquee;
        case kSidPaneStyle: return kPvPane;
        case kSidPlayback: return kPvPlayback;
        default: return pv_desc(id, s).empty() ? kPvNone : kPvDesc;
    }
}

std::string braille_utf8(int bits) {
    const unsigned cp = 0x2800u + static_cast<unsigned>(bits);
    std::string s;
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
    return s;
}

// What a renderer hands back: rows (ANSI allowed), their visible width, a caption, and where to start (centred if -1).
struct PvOut {
    std::vector<std::string> rows;
    int w = 0;
    std::string caption;
    bool picture = false;   // the rows are placeholders for a spectrogram picture the terminal draws (rows x w cells)
    int cover_mode = -1;    // >= 0: the rows are placeholders for the sample cover as a picture (Kitty / Sixel) in that Cover Mode
    bool milk = false;      // MilkDrop: the rows are made in the box (a live preset)
    bool clap = false;      // Show Clap Loading: the rows are made in the box (the loading picture of a video as it would show)
    bool video = false;     // the rows are placeholders for the sample video as a picture (vw x vh pixels shown on vcols x vrows cells)
    int vcols = 0, vrows = 0, vw = 0, vh = 0, cw0 = 10, ch0 = 20;
    bool vfresh = false;
    const VideoFrame* vframe = nullptr;
};

}  // namespace

void App::settings_preview_draw(std::ostringstream& frame, int W, int MAX_Y, int tab,
                                const std::function<std::string(int)>& B) const {
    if (W < kPreviewMinW) return;
    const int id = settings_item_id(tab, settings_row_);
    const PvKind kind = kind_of(id, settings_);
    if (!pv_) pv_ = std::make_shared<SettingsPreviewState>();
    SettingsPreviewState& P = *pv_;
    if (fxpick_.open) {   // the effect list is open over the settings and has a larger preview of its own: this box is not drawn behind it (a cut-off part of it showed above the list); what it runs stops
        P.spec.set_visible(false);
        if (P.vid) P.vid->stop();
        return;
    }

    // the clock
    const auto now = std::chrono::steady_clock::now();
    P.dt = P.have_last ? std::clamp(std::chrono::duration<double>(now - P.last).count(), 0.004, 0.1) : 0.033;
    P.last = now;
    P.have_last = true;
    P.t += P.dt;

    const bool want_spec = kind == kPvSpectro;
    if (!want_spec) P.spec.set_visible(false);
    P.spec_idle = want_spec ? 0.0 : P.spec_idle + P.dt;
    if (P.spec_idle > 10.0) P.spec.release();   // (moved on to another row: the analysis memory goes back; it is allocated again when its row is shown)
    if (kind != kPvVideo && P.vid) P.vid->stop();   // (the decoder only runs while its row is shown)
    PvKind shown = kind;
    if (kind == kPvVideo) {
        if (!P.vid_tried) {   // the sample video: scripts/preview_video.mp4 (8 seconds of a Pixabay cat video, see THIRD_PARTY_LICENSES.txt)
            P.vid_tried = true;
            const fs::path f = find_preview_video();
            std::error_code ec;
            if (!f.empty() && fs::exists(f, ec)) {
                P.vid = std::make_shared<VideoPlayer>();
                P.vid_ok = P.vid->set_source(path_utf8(f));
            }
        }
        if (!P.vid_ok) shown = kPvLyrics;   // no sample (or no ffmpeg): the lyrics stand in for it, as before
    }
    {   // the MilkDrop preview: the preset "amandio c - secret garden 4" while its row is shown; the preset from before comes back afterwards
        Milkdrop& M = Milkdrop::instance();
        if (kind == kPvMilk && !milkdrop_usable())   // (no MilkDrop here: the rows of the MilkDrop settings keep their text, the field rows show the lyrics)
            shown = (id == kSidMdPreset || id == kSidMdUseFx || id == kSidMdFx || id == kSidMdColors || id == kSidMdShowName || id == kSidMdBright || id == kSidMdSat || id == kSidMdContrast) ? kPvDesc : kPvLyrics;
        // (the preset chosen as Default Preset; none chosen: the sample preset "amandio c - secret garden 4")
        const std::string md_want = shown == kPvMilk ? settings_.milkdrop_default_preset : std::string();
        auto md_play = [&] { if (md_want.empty()) M.play_named("amandio c - secret garden 4"); else M.play_path(md_want); P.md_cur = md_want; P.md_fresh = true; };
        if (shown == kPvMilk && !P.md_active) { P.md_prev = M.preset_path(); if (!M.locked()) { M.set_locked(true); P.md_pv_locked = true; } md_play(); P.md_active = true; }
        else if (shown == kPvMilk && P.md_active && P.md_cur != md_want) md_play();
        else if (shown != kPvMilk && P.md_active) { settings_preview_end(); }
    }
    if (kind == kPvNone) return;

    const std::string R = "\x1b[0m";
    const int iw = W - 77;                                        // inside the box
    const int visible = std::max(1, MAX_Y - 3);
    int bh = std::min(visible - 1, 19);                           // the box, borders included (the transliteration previews may make it taller)
    int ih = bh - 2;                                              // rows inside
    if (ih < 6 || iw < 30) return;
    int body_h = ih - 1;                                    // the last row is the caption

    // ---- the signal, only for what is shown ------------------------------------------------------------------
    std::vector<float> lr;
    const bool wants_signal = kind == kPvBars || kind == kPvScope || kind == kPvSpectro || kind == kPvSphere;
    if (wants_signal) {
        const size_t n = static_cast<size_t>(std::lround(P.dt * 48000.0));
        double t = P.sig;
        if (kind == kPvSpectro || kind == kPvBars) {   // (the spectrum bars listen to the same lively sound)
            // a spectrogram the Creator makes from the text "Mousiki Plus" (made once, played in a loop)
            if (!P.text_made) {
                P.text_made = true;
                const creator::Gray g = creator::gray_from_text("Mousiki Plus", 6);
                creator::SpecOptions so;
                so.rate = 48000;
                so.seconds = 8.0;
                so.octaves = 6;
                creator::Stereo st;
                if (!g.empty() && creator::render_spectrogram_sound(g, so, st) && !st.l.empty()) {
                    const size_t gap = 48000 * 3 / 2;   // a pause between two rounds
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
        if (kind == kPvBars || kind == kPvSphere) {
            P.fft.set_fluidity(settings_.visualizer_fluidity);
            P.fft.set_degradation_speed(settings_.visualizer_degradation_speed);
            P.fft.set_viscosity(settings_.visualizer_viscosity);
            P.fft.push_samples(mono.data(), mono.size(), 48000);
        }
        if (kind == kPvScope) P.osc.push_frames(lr.data(), n);
        if (kind == kPvBars && settings_.bars_style == 1) P.hifi.push(lr.data(), n, 48000);
        if (kind == kPvSpectro) {
            P.spec.set_visible(true);
            P.spec.push(lr.data(), n, 48000);
        }
    }

    PvOut out;
    VideoFrame vfx;       // (the sample video's frame with the retro effect on it)
    VideoFrame vscaled;   // (the sample video's frame scaled to the picture's size while the decoder catches up)
    const bool compact = kind == kPvVolume || kind == kPvBars || kind == kPvWave || kind == kPvTranslit || kind == kPvEmoji || kind == kPvMarquee || kind == kPvPane;
    const int cw = compact ? iw - 4 : iw - 2;   // content width (one blank column each side; the compact previews have two, like the DESCRIPTION box)
    switch (shown) {
    case kPvLyrics: {
        struct S { double at; const char* text; };
        static const S kLines[] = {{0.0, "Meow, Meooooow"}, {3.0, "Meow meow Meeeeow meow"},
                                   {6.0, "Mrrrow, meooow, mew mew"}, {9.0, "Meow... Meeeoooow!"}};
        std::vector<LyricLine> lines;
        for (const S& s : kLines) {
            LyricLine l;
            l.start_time = s.at;
            l.full_text = s.text;
            std::istringstream iss(s.text);
            std::string w;
            double at = s.at;
            while (iss >> w) { l.words.push_back({at, w}); at += 0.6; }
            lines.push_back(l);
        }
        const double el = std::fmod(P.t, 12.0);
        int active = 0;
        for (size_t i = 0; i < lines.size(); ++i) if (lines[i].start_time <= el) active = static_cast<int>(i);
        std::vector<std::string> rows(static_cast<size_t>(body_h), std::string(static_cast<size_t>(cw), ' '));
        if (settings_.lyrics_animation == 4) {
            std::string word = lines[static_cast<size_t>(active)].full_text;
            for (const auto& wt : lines[static_cast<size_t>(active)].words) if (wt.first <= el) word = wt.second;
            const int pad = std::max(0, (cw - display_width(word)) / 2);
            rows[static_cast<size_t>(body_h / 2)] = std::string(static_cast<size_t>(pad), ' ') + ansi_for(settings_.active_word_color) + word + R;
        } else if (settings_.lyrics_animation == 3) {
            auto wr = render_lyric_line_wrapped(lines[static_cast<size_t>(active)], el, cw, true, settings_);
            const int st = std::max(0, (body_h - static_cast<int>(wr.size())) / 2);
            for (size_t i = 0; i < wr.size() && st + static_cast<int>(i) < body_h; ++i) rows[static_cast<size_t>(st) + i] = wr[i];
        } else {
            std::vector<std::string> flat;
            int a_start = 0, a_count = 1;
            for (int li = 0; li < static_cast<int>(lines.size()); ++li) {
                auto wr = render_lyric_line_wrapped(lines[static_cast<size_t>(li)], el, cw, li == active, settings_);
                if (li == active) { a_start = static_cast<int>(flat.size()); a_count = static_cast<int>(wr.size()); }
                for (auto& r : wr) flat.push_back(std::move(r));
            }
            const int start = a_start + a_count / 2 - body_h / 2;
            for (int row = 0; row < body_h; ++row) {
                const int idx = start + row;
                if (idx >= 0 && idx < static_cast<int>(flat.size())) rows[static_cast<size_t>(row)] = flat[static_cast<size_t>(idx)];
            }
        }
        out.rows = rows;
        out.w = cw;
        const char* al = settings_.lyrics_alignment == 1 ? "left" : settings_.lyrics_alignment == 2 ? "right" : "center";
        out.caption = std::string("alignment: ") + al;
        break;
    }
    case kPvDisk:
    case kPvCover: {
        if (!settings_.element_disk) {   // the field is removed: no disk and no cover
            out.caption = "Disk / Cover field removed";
            break;
        }
        const bool border_row = kind == kPvDisk && id == kSidDiskBorder;   // (Classic Disk Border: the disk with its ":  :" column, or centred without it, as on the main screen)
        const std::string sep_text = "  " + settings_.meta_separator + "  ";
        const int sep_cols = border_row ? display_width(sep_text) : 0;
        const int dc = std::min(30, cw - sep_cols), dr = std::min(15, body_h);
        std::vector<std::string> rows;
        P.angle = std::fmod(P.angle + kAngularVelocity * settings_.disk_rotation_speed * P.dt, kTwoPi);
        const bool cover_look = kind == kPvCover;
        if (cover_look) {
            if (!P.cover.ok) make_sample_cover(P.cover);
            if (cf_->full_res && gfxs_.proto != GfxProto::None && settings_.cover_mode <= 4) {
                // a real picture (like the Style Option overlay): the box gets placeholders, the picture is set up below
                for (int i = 0; i < dr; ++i) rows.push_back(gfx_hole(dc));
                out.cover_mode = settings_.cover_mode;
                out.caption = settings_.album_cover ? "sample cover (image)" : "sample cover (image; Album Cover is off)";
            } else {
                rows = album_cover_lines_for(P.cover, settings_.cover_mode, settings_.cover_contrast, settings_.cover_brightness,
                                             settings_.cover_colors, P.angle, dc, dr, settings_.cover_chroma, settings_.cover_grad_sat, settings_.cover_vivid_eff());
                out.caption = std::string(gfxs_.proto != GfxProto::None && cf_->full_res ? "sample cover (braille)" : "sample cover")
                              + (settings_.album_cover ? "" : " (Album Cover is off)");
            }
        }
        if (rows.empty()) {
            rows = disk_.frame(P.angle);
            if (static_cast<int>(rows.size()) > dr) rows.resize(static_cast<size_t>(dr));
            for (size_t i = 0; i < rows.size(); ++i) {
                const float t = rows.size() > 1 ? static_cast<float>(i) / static_cast<float>(rows.size() - 1) : 0.0f;
                rows[i] = disk_gradient_ansi(settings_.disk_color, settings_.disk_color_end, t, settings_.disk_vivid) + rows[i] + R;
            }
            out.caption = id == kSidDiskVivid ? "plain disk, gradient vividness " + std::to_string(settings_.disk_vivid) + "%" : "disk speed " + std::to_string(settings_.disk_rotation_speed).substr(0, 4);
        }
        if (border_row && !rows.empty() && !cover_look) {
            const std::string bsep = ansi_for(settings_.border_color, false) + sep_text + R;
            const int lead = std::max(0, (sep_cols - 1) / 2);   // (without the column the disk sits in the middle of that space, as on the main screen)
            for (std::string& r : rows) r = settings_.disk_border ? r + bsep : std::string(static_cast<size_t>(lead), ' ') + r + std::string(static_cast<size_t>(std::max(0, sep_cols - lead)), ' ');
            out.caption = settings_.disk_border ? "Classic Disk Border: on" : "Classic Disk Border: off";
        }
        out.rows = rows;
        out.w = dc + sep_cols;
        break;
    }
    case kPvVolume: {
        const int vol = 85;
        const VolBarLook vlook = volume_bar_look(settings_.volume_indicator, 38);   // (as on the main screen)
        const int vcells = vlook.cells;
        const int hashes = (vol * vcells) / 100;
        const std::string played = ansi_for(settings_.progress_played_color), unplayed = ansi_for(settings_.progress_remaining_color);
        const std::string border = ansi_for(settings_.border_color, false);
        std::string fill;
        const double ang = volume_clock_angle();
        if (settings_.volume_gradient) {
            const auto cells = volume_bar_cells(settings_.volume_indicator, hashes, ang, vcells);
            for (size_t i = 0; i < cells.size(); ++i)
                fill += vivid_gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, static_cast<float>(i) / (vcells - 1), settings_.vivid_of(settings_.volume_vivid)) + cells[i];
        } else {
            fill = played + volume_bar_fill(settings_.volume_indicator, hashes, ang, vcells);
        }
        const std::string open = vlook.brackets ? "[" : "", close = vlook.brackets ? "]" : "";
        const std::string bar_short = border + open + R + fill + R + unplayed + std::string(static_cast<size_t>(vcells - hashes), '-') + R + border + close + R + " 85%";
        const std::string bar = border + "\x1b[1mVOLUME\x1b[22m:" + bar_short;
        const int vw = 7 + (vlook.brackets ? 2 : 0) + vcells + 4;
        std::vector<std::string> rows;
        int w = vw;
        if (id == kSidDummy) {
            // The real buttons (as in the PROGRESS BAR panel): three framed 13-column boxes; the middle one shows PAUSE while
            // playing and PLAY while paused (alternating here), and << / >> flash in the bottom border colour while seeking.
            if (settings_.element_dummy_buttons) {
                const int bw = 13;
                // With a smooth border gradient the three rows fade from the top to the bottom colour (as the frame does
                // line by line on the main screen); rough keeps the top colour and only the bottom line in the bottom colour.
                const bool smooth = settings_.border_smooth;
                const std::string ba = ansi_for(settings_.border_color, false), bb = ansi_for(settings_.border_color_bottom, false);
                const std::string bm = smooth ? gradient_ansi(settings_.border_color, settings_.border_color_bottom, 0.5f, false) : ba;
                const bool paused = (static_cast<int>(P.t / 2.0) % 2) == 1;
                const bool flash = std::fmod(P.t, 4.0) < 0.5;
                const bool back = static_cast<int>(P.t / 4.0) % 2 == 0;
                auto mid = [&](const std::string& text, const std::string& tx) {
                    const std::string bar = bm + settings_.box_vertical + R;
                    return bar + " " + tx + center_pad(text, 9) + R + " " + bar;
                };
                const std::string lb = (flash && back) ? bb : bm, rb = (flash && !back) ? bb : bm;
                const std::string pl = paused ? "PLAY" : "PAUSE";
                const bool all = cw >= 3 * bw;
                std::string t, m, bt;
                if (all) {
                    t = box_top("", bw, ba) + box_top("", bw, ba) + box_top("", bw, ba);
                    m = mid("<<<", lb) + mid(pl, bm) + mid(">>>", rb);
                    bt = box_bottom(bw, "", bb) + box_bottom(bw, "", bb) + box_bottom(bw, "", bb);
                    w = 3 * bw;
                } else {   // a narrow box: the play / pause button alone
                    t = box_top("", bw, ba); m = mid(pl, bm); bt = box_bottom(bw, "", bb);
                    w = bw;
                }
                rows = {t, m, bt};
            } else { rows = {"(Dummy Buttons are off)"}; w = 23; }
            out.caption = settings_.element_dummy_buttons ? "Dummy Buttons: on" : "Dummy Buttons: off";
        } else {
            rows = {cw < vw ? bar_short : bar};   // (a narrow box: without the "VOLUME:" label)
            if (cw < vw) w = vw - 7;
            out.caption = std::string("Volume Indicator: ") + (settings_.volume_gradient ? "gradient" : "no gradient");
        }
        out.rows = rows;
        out.w = w;
        break;
    }
    case kPvBars: {
        std::vector<std::string> rows;
        const int n = std::min(cw, 48);
        const auto bars = P.fft.compute_bars(n, P.dt);
        std::string top, bot;
        for (int i = 0; i < n; ++i) {
            const float t = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
            const std::string a = !settings_.viz_center_color.empty()
                ? vivid_multi_stop_gradient_ansi(settings_.viz_left_color, settings_.viz_center_color, settings_.viz_right_color, t, settings_.vivid_of(settings_.bars_vivid))
                : vivid_gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t, settings_.vivid_of(settings_.bars_vivid));
            top += a + fft_glyph(std::max(0, bars[static_cast<size_t>(i)] - 4));
            bot += a + fft_glyph(std::min(bars[static_cast<size_t>(i)], 4));
        }
        rows = {ansi_for(settings_.border_color, false) + "Title : Chords" + R, ansi_for(settings_.border_color, false) + "Artist: Test signal" + R, "", top + R, bot + R};
        if (settings_.bars_style == 1) {   // Hi-Fi: the same drawing as under the meta data (four rows high, the test signal in stereo)
            HifiView hv;
            hv.cells = n; hv.rows = 4; hv.resolution = settings_.hifi_resolution; hv.channels = settings_.hifi_channels; hv.color = settings_.hifi_color; hv.scale = settings_.hifi_scale; hv.adapt = settings_.hifi_adapt; hv.fscale = settings_.hifi_fscale; hv.gain = settings_.hifi_gain; hv.range = settings_.hifi_range; hv.split = settings_.hifi_split;
            hv.fluidity = settings_.hifi_fluidity; hv.degradation = settings_.hifi_degradation; hv.viscosity = settings_.hifi_viscosity; hv.dt = P.dt;
            spectro_gradient_from(settings_);
            const auto& pal = spectro_palette(settings_.spectro.scheme);
            const std::vector<std::string> hr = hifi_rows(P.hifi, hv,
                [&](float t) {
                    return !settings_.viz_center_color.empty() ? vivid_multi_stop_gradient_ansi(settings_.viz_left_color, settings_.viz_center_color, settings_.viz_right_color, t, settings_.vivid_of(settings_.bars_vivid))
                                                              : vivid_gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t, settings_.vivid_of(settings_.bars_vivid));
                },
                [&](float f) {
                    const auto& c = pal[static_cast<size_t>(std::clamp(48 + static_cast<int>(f * 207.0f), 0, 255))];
                    return "\x1b[38;2;" + std::to_string(c[0]) + ";" + std::to_string(c[1]) + ";" + std::to_string(c[2]) + "m";
                });
            rows = {ansi_for(settings_.border_color, false) + "Title : Chords" + R, ansi_for(settings_.border_color, false) + "Artist: Test signal" + R, ""};
            for (const auto& l : hr) rows.push_back(l + R);
        }
        {   // what the highlighted option does (the box grows with it)
            std::string key;
            switch (id) {
                case kSidVisualizer: key = "onoff"; break; case kSidBarStyle: key = "style"; break; case kSidHifiRes: key = "hifires"; break; case kSidHifiChan: key = "hifichan"; break;
                case kSidHifiColor: key = "hificolor"; break; case kSidHifiScale: key = "hifiscale"; break; case kSidHifiAdapt: key = "hifiadapt"; break; case kSidHifiSplit: key = "hifisplit"; break;
                case kSidHifiFScale: key = "hififscale"; break; case kSidHifiGain: key = "hifigain"; break; case kSidHifiRange: key = "hifirange"; break; case kSidBarBehavior: key = "behavior"; break;
                case kSidVisFluidity: key = "fluidity"; break; case kSidVisDegr: key = "degradation"; break; case kSidVisVisc: key = "viscosity"; break; default: break;
            }
            const std::string help = bars_help(key);
            if (!help.empty()) { rows.push_back(""); for (const std::string& w : wrap_lines(help, std::max(20, n), 5)) rows.push_back(w); }
        }
        out.rows = rows;
        out.w = n;
        out.caption = std::string(bars_preset_name(bars_preset_of(settings_.bar_fluidity(), settings_.bar_degradation(), settings_.bar_viscosity()))) + ": fluidity " + std::to_string(settings_.bar_fluidity()) + "  degradation " + std::to_string(settings_.bar_degradation()) +
                      "  viscosity " + std::to_string(settings_.bar_viscosity());
        break;
    }
    case kPvWave: {
        if (P.env_raw.empty()) {
            const auto pcm = sample_pcm();
            P.env_raw = WaveformQuantizer::generate_high_res_envelope(pcm, 4096, false, 1);
            P.env_smooth = WaveformQuantizer::generate_high_res_envelope(pcm, 4096, true, 1);
        }
        const int n = cw;
        const double frac = std::fmod(P.t / 20.0, 1.0);
        const int active = static_cast<int>(frac * n);
        const std::string cp = vivid_colour_ansi(settings_.progress_played_color, settings_.wave_vivid), cu = vivid_colour_ansi(settings_.progress_remaining_color, settings_.wave_vivid);
        if (settings_.element_waveform) {
            const auto levels = WaveformQuantizer::resample_for_ui(settings_.waveform_smooth ? P.env_smooth : P.env_raw, n);
            std::string top, mid, bot;
            for (int i = 0; i < n; ++i) {
                const BrailleColumn col = WaveformQuantizer::get_column(i < static_cast<int>(levels.size()) ? levels[static_cast<size_t>(i)] : 0);
                const std::string& c = i < active ? cp : cu;
                if (i == 0 || i == active) { top += c; mid += c; bot += c; }
                top += col.top; mid += col.mid; bot += col.bot;
            }
            out.rows = {top + R, mid + R, bot + R};
            out.caption = std::string("Waveform Style: ") + (settings_.waveform_smooth ? "smooth" : "raw");
        } else {
            const int inner = std::max(0, n - 2);
            const int filled = std::clamp(static_cast<int>(frac * inner), 0, inner);
            out.rows = {"", "[" + cp + std::string(static_cast<size_t>(filled), '#') + R + cu + std::string(static_cast<size_t>(inner - filled), '-') + R + "]", ""};
            out.caption = "waveform off: the plain bar";
        }
        out.w = n;
        break;
    }
    case kPvScope: {
        const int rows_n = std::min(body_h, 12);
        const OsciSet& os = settings_.osci();
        const auto cells = P.osc.render(cw, rows_n, osci_scope_params(settings_, P.dt, 0));
        for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
            std::string line, last;
            for (int cell = 0; cell < cw; ++cell) {
                const auto& c = cells[static_cast<size_t>(i)][static_cast<size_t>(cell)];
                if (c.braille == 0) { line += ' '; continue; }
                const float t = cw > 1 ? static_cast<float>(cell) / static_cast<float>(cw - 1) : 0.0f;
                const float lvl = std::round(c.level / 255.0f * 7.0f) / 7.0f;
                const float k = 0.30f + 0.70f * lvl;
                std::string ansi;
                if (os.palette == 0) ansi = osci_dim_ansi(vivid_gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t, settings_.vivid_of(settings_.scope_vivid)), k);
                else { int r = 255, g = 255, b = 255; osci_palette_rgb(settings_, os.palette, c.hue / 255.0f, r, g, b); ansi = osci_rgb_seq(r, g, b, k); }
                if (ansi != last) { line += ansi; last = ansi; }
                line += braille_utf8(c.braille);
            }
            out.rows.push_back(line + R);
        }
        out.w = cw;
        out.caption = "oscilloscope";
        break;
    }
    case kPvSpectro: {
        // always the plain defaults (the regular spectrogram as an image, mel scale, Roseus colours), whatever the spectrogram is set to:
        // the preview only shows what a spectrogram is, it is not connected to the SHIFT+i values (nor to the waterfall)
        SpectroSettings sp;
        sp.motion = 1;   // scrolling, so the small picture is full at once; span: a short stretch of time across the box
        sp.span = 10;
        P.spec.update(sp);
        const int srows = std::min(body_h, 12);
        if (gfxs_.proto != GfxProto::None) {   // the picture wherever the terminal can show one, braille only without Kitty / Sixel
            // the image style (the default): the terminal draws the picture, the box holds placeholders (main loop: pv_spectro_gfx_)
            pv_spectro_set_ = sp;
            for (int r = 0; r < srows; ++r) out.rows.push_back(gfx_hole(cw));
            out.picture = true;
            out.caption = "spectrogram (image; default look)";
        } else {
            out.rows = P.spec.braille(sp, cw, srows);
            out.caption = "spectrogram (braille; default look)";
        }
        out.w = cw;
        break;
    }
    case kPvSphere: {
        const auto bars = P.fft.compute_bars(48, P.dt);
        const auto rows = P.sph.render(std::min(cw, 40), std::min(body_h, 12), bars, P.dt);
        const std::string a = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, 0.5f);
        for (const std::string& r : rows) out.rows.push_back(a + r + R);
        out.w = std::min(cw, 40);
        out.caption = "sphere";
        break;
    }
    case kPvTranslit: {
        // What the option changes, for the highlighted row: the examples of the manual with the result for each value of the setting; the value in
        // use is lit. Under it a few examples that never change. Rows that are wider than the box scroll (marquee).
        TranslitPreviewIn tin;   // (translit_preview.cpp: the same examples in the radio's Settings)
        tin.kind = id == kSidLatinMacrons ? 0 : id == kSidLatinWo ? 1 : id == kSidLatinHanzi ? 2 : 3;
        tin.macrons = settings_.latin_macrons ? 1 : 0;
        tin.wo = settings_.latin_wo ? 1 : 0;
        tin.hanzi = settings_.latin_hanzi;
        tin.lookup_on = settings_.latin_lookup;
        tin.cw = cw;
        tin.t = P.t;
        tin.lit = "\x1b[7m";
        tin.off = R;
        // the optional kanji reader (pykakasi; janome cuts the words): what it does here, in one line
        tin.reader_line = kanji_->broken ? "kanji reader: not installed (pip install pykakasi janome)"
                        : !settings_.latin_letters ? "kanji reader: reads while Latin letters are on (" + hotkey_text("HKeyTranslit", "\"") + ")"
                                                   : "kanji reader: on (its readings come in after a moment)";
        const TranslitPreviewOut tout = translit_preview(tin);
        out.rows.insert(out.rows.end(), tout.rows.begin(), tout.rows.end());
        const std::string& cap = tout.caption;
        {   // longer than the box: the box grows (as far as the screen allows) so that nothing is cut off
            const int want = std::min(visible - 1, static_cast<int>(out.rows.size()) + 3);
            if (want > bh) { bh = want; ih = bh - 2; body_h = ih - 1; }
        }
        if (static_cast<int>(out.rows.size()) > body_h) out.rows.resize(static_cast<size_t>(body_h));
        out.w = 0;   // left aligned
        out.caption = cap;
        break;
    }
    case kPvEmoji: {
        const int em = settings_.emoji_mode;
        out.rows.push_back(std::string("now: Replace Emoji ") + (em == 2 ? "All" : em == 1 ? "Safe" : "Off"));
        out.rows.push_back("");
        out.rows.push_back("            All   Safe  Off");
        static const char* const kName[] = {"note", "radio", "alien", "book", "red heart", "thumbs up"};
        static const char* const kEmoji[] = {"\U0001f3b5", "\U0001f4fb", "\U0001f47e", "\U0001f4d6",
                                             "\u2764\ufe0f", "\U0001f44d\U0001f3fd"};
        // Safe keeps a plain one-codepoint emoji and turns every joined / flag / skin-tone / VS16 form into "?".
        static const bool kSafeKeeps[] = {true, true, true, true, false, false};
        for (int i = 0; i < 6; ++i) {
            std::string n = kName[i];
            n.resize(12, ' ');
            // (the real emoji are last on their row so a different width cannot push anything; 26 columns in all)
            out.rows.push_back(n + "?     " + (kSafeKeeps[i] ? std::string("kept  ") : std::string("?     ")) + kEmoji[i]);
        }
        out.rows.push_back("");
        for (const std::string& w : wrap_lines("Safe: plain emoji stay, risky ones become \"?\"", cw, 3)) out.rows.push_back(w);
        out.w = 0;
        out.caption = "";
        break;
    }
    case kPvVideo: {   // the sample video, looping, drawn like the video view of the main screen
        VideoPlayer& V = *P.vid;
        int cw0 = 10, ch0 = 20;
        gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
        cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
        const bool picture = gfxs_.proto != GfxProto::None;
        const int avail_w = std::max(8, cw), avail_h = std::max(3, body_h);
        int vcols = 0, vrows = 0, dw = 0, dh = 0, fw = 0, fh = 0;
        if (picture) {
            V.fit(avail_w * cw0, avail_h * ch0, 1.0, fw, fh);
            vcols = std::clamp((fw + cw0 / 2) / cw0, 1, avail_w);
            vrows = std::clamp((fh + ch0 / 2) / ch0, 1, avail_h);
            const int maxw = gfxs_.proto == GfxProto::Sixel ? 480 : 640;
            const double f = std::min(1.0, static_cast<double>(maxw) / (vcols * cw0));
            dw = std::max(2, static_cast<int>(vcols * cw0 * f) & ~1);
            dh = std::max(2, static_cast<int>(vrows * ch0 * f) & ~1);
        } else {
            V.fit(avail_w * 2, avail_h * 4, 2.0 * cw0 / ch0, fw, fh);
            vcols = std::clamp((fw + 1) / 2, 1, avail_w);
            vrows = std::clamp((fh + 3) / 4, 1, avail_h);
            dw = vcols * 2; dh = vrows * 4;
        }
        double vt = P.t;
        const double vdur = V.duration();
        if (vdur > 1.0) vt = std::fmod(vt, vdur);
        bool fresh = false;
        const VideoFrame* fr = V.frame_for(vt, dw, dh, true, &fresh);
        if (fr && (fr->w != dw || fr->h != dh)) { vscaled = VideoPlayer::resample(*fr, dw, dh); fr = &vscaled; }
        {
            VideoLook look;
            look.fx = settings_.video_fx; look.contrast = settings_.video_contrast; look.brightness = settings_.video_brightness; look.colors = settings_.video_colors; look.chroma = settings_.video_chroma; look.dither = settings_.video_dither; look.base = settings_.video_base; look.fxc = settings_.video_fxc;
            if (video_fx_uses_gradient(look.fx)) look.grad = video_fx_gradient(settings_);
            if (fr && look.active()) { vfx = *fr; VideoPlayer::apply_look(vfx, look, vt, !picture, V.previous_frame()); fr = &vfx; }
        }
        if (!fr) {   // the decoder is starting (or failed): say it in the middle
            std::string msg = V.status();
            if (msg.empty()) msg = "video: ...";
            out.rows = {truncate_str(msg, std::max(1, cw))};
            out.w = display_width(out.rows[0]);
            out.caption = "sample video";
            break;
        }
        if (picture) {
            for (int r = 0; r < vrows; ++r) out.rows.push_back(gfx_hole(vcols));
            out.video = true; out.vframe = fr; out.vfresh = fresh;
            out.vcols = vcols; out.vrows = vrows; out.vw = dw; out.vh = dh; out.cw0 = cw0; out.ch0 = ch0;
            out.caption = "sample video (image)";
        } else {
            const auto cells = VideoPlayer::braille(*fr, vcols, vrows);
            for (int r = 0; r < vrows; ++r) {
                std::string line, last_ansi;
                for (int c = 0; c < vcols; ++c) {
                    const auto& cell = cells[static_cast<size_t>(r)][static_cast<size_t>(c)];
                    if (cell.mask == 0 || (settings_.video_round && vrows >= 3 && vcols >= 4 && (r == 0 || r == vrows - 1) && (c == 0 || c == vcols - 1))) { line += ' '; continue; }   // (rounded: the four corner cells stay empty)
                    const float tt = vcols > 1 ? static_cast<float>(c) / static_cast<float>(vcols - 1) : 0.0f;
                    const float lvl = std::round(cell.level / 255.0f * 7.0f) / 7.0f;
                    const std::string ansi = osci_dim_ansi(vivid_gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, tt, settings_.vivid_of(settings_.scope_vivid)), 0.30f + 0.70f * lvl);
                    if (ansi != last_ansi) { line += ansi; last_ansi = ansi; }
                    const int cp = 0x2800 + cell.mask;
                    line += static_cast<char>(0xE0 | (cp >> 12));
                    line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    line += static_cast<char>(0x80 | (cp & 0x3F));
                }
                out.rows.push_back(line + "\x1b[0m");
            }
            out.caption = "sample video (braille)";
        }
        out.w = vcols;
        break;
    }
    case kPvDesc: {   // a few lines of text: what the highlighted option does
        const int dw = iw - 4;
        for (const std::string& para : split_lines_nl(pv_desc(id, settings_))) {
            if (para.empty()) continue;
            if (!out.rows.empty()) out.rows.push_back("");
            for (const std::string& w : wrap_lines(para, dw, 14)) out.rows.push_back(w);
        }
        // two columns of room each side, two rows above and below; the box is as tall as the text needs (and the screen allows)
        out.rows.insert(out.rows.begin(), 2, std::string());   // (two blank rows above; the one below and the caption row (empty here) make two)
        out.rows.push_back("");
        bh = std::min(visible - 1, static_cast<int>(out.rows.size()) + 3);
        ih = bh - 2; body_h = ih - 1;
        out.w = dw;
        break;
    }
    case kPvPlayback: {
        const int dw = iw - 4;   // the small play-mode box beside the search bar (the letter changes with the value), then what each mode does
        const int mode = std::clamp(settings_.play_mode, 0, 4);
        const std::string border = ansi_for(settings_.border_color, false);
        const std::string lit = ansi_for(settings_.tab_current_color, false);
        const std::string letter(1, play_mode_letter());
        const int pad = std::max(0, (dw - 5) / 2);
        const std::string sp(static_cast<size_t>(pad), ' ');
        out.rows.push_back(sp + border + "\u256d\u2500\u2500\u2500\u256e" + R);
        out.rows.push_back(sp + border + settings_.box_vertical + R + " " + border + letter + R + " " + border + settings_.box_vertical + R);
        out.rows.push_back(sp + border + "\u2570\u2500\u2500\u2500\u256f" + R);
        out.rows.push_back("");
        static const char* const kModes[] = {"L  list: play along the list", "R  loop: repeat this track", "S  shuffle: a random next track", "O  stop: stop after this track", "Q  queue: play the queue, then stop"};
        for (int i = 0; i < 5; ++i) out.rows.push_back((i == mode ? lit : std::string()) + truncate_str(kModes[i], dw) + (i == mode ? R : std::string()));
        out.rows.push_back("");
        for (const std::string& w : wrap_lines("The letter is shown in the box next to the search bar; m / j change it.", dw, 3)) out.rows.push_back(w);
        out.rows.insert(out.rows.begin(), 2, std::string());   // (two blank rows above; the one below and the caption row (empty here) make two)
        out.rows.push_back("");
        bh = std::min(visible - 1, static_cast<int>(out.rows.size()) + 3);
        ih = bh - 2; body_h = ih - 1;
        out.w = dw;
        break;
    }
    case kPvMarquee: {
        const std::string text = "Meowsome Moew Orchestra And The Best-Of-All-Meows feat. Bozai   ";
        const int w = std::max(8, std::min(cw - 8, 24));
        std::string shown;
        if (settings_.marquee) {
            const std::string loop = text + text;
            const size_t off = static_cast<size_t>(P.t * 6.0) % text.size();
            shown = loop.substr(off, static_cast<size_t>(w));
        } else {
            shown = text.substr(0, static_cast<size_t>(w - 3)) + "...";
        }
        out.rows = {"Title : " + shown};
        out.w = 8 + w;
        out.caption = settings_.marquee ? "Marquee on: the title scrolls" : "Marquee off: cut with ...";
        break;
    }
    case kPvPane: {   // the frame of the upper pane, as it is drawn (the same helpers): disk | meta data | lyrics / viz field
        const int w = std::clamp(cw, 24, 44);
        // The corners as the option gives them (the Settings screen itself is the other way round): marked, so the corner pass leaves them alone.
        panestyle::Glyphs pg = panestyle::glyphs(settings_);
        for (std::string* g : {&pg.ul, &pg.ur, &pg.ll, &pg.lr}) {
            if (settings_.sharp_corners) sharpen_corners(*g);
            *g = std::string(kKeepCornerMark) + *g;
        }
        PanePreview pp = pane_preview(settings_.top_pane_style, w, "disk", "meta", "viz", pg,
                                      ansi_for(settings_.border_color, false), ansi_for(settings_.border_color_bottom, false),
                                      ansi_for(settings_.legend_color, false), "\x1b[0m");
        out.rows = std::move(pp.rows);
        out.w = w;
        out.caption = pp.caption;
        break;
    }
    case kPvMilk: out.milk = true; break;
    case kPvClap: {   // what shows while a video loads: the text note, the small clapperboard (a picture or braille) or nothing
        out.clap = true;
        out.caption = settings_.video_clap == 1 ? "loading: clap" : settings_.video_clap == 0 ? "loading: text note" : "loading: none";
        break;
    }
    default: return;
    }

    // ---- the box ------------------------------------------------------------------------------------------------
    auto pos = [&](int y, int x, const std::string& s) { frame << "\x1b[" << y << ";" << x << "H" << s; };
    if (compact) {   // as the DESCRIPTION box: two blank rows above, one below (and the caption row), as tall as the content needs
        out.rows.insert(out.rows.begin(), 2, std::string());
        out.rows.push_back("");
        bh = std::min(visible - 1, static_cast<int>(out.rows.size()) + 3);
        ih = bh - 2; body_h = ih - 1;
    }
    std::string label = "Preview";
    {
        std::string head;
        for (const SetLine& L : settings_layout(tab)) {
            if (L.header) { if (*L.text) head = L.text; continue; }
            if (L.id != id) continue;
            label = std::string("Preview: ") + (std::string(L.text) == "On/Off" ? head : std::string(L.text));
            break;
        }
    }
    if (shown == kPvDesc || shown == kPvPlayback) label = "DESCRIPTION";
    if (display_width(label) > iw - 2) label = "Preview";
    // centred on the option rows, one row higher (the tallest boxes, which already fill the field, start one row below the first option row)
    const int free_rows = std::max(0, (visible - 1 - bh) / 2);
    const int top = 4 + (free_rows >= 2 ? free_rows - 1 : free_rows);
    const int bottom = top + bh - 1;
    const int x0 = kPreviewX, x1 = kPreviewX + iw + 1;
    pos(top, x0, B(top) + "┌─ " + R + ansi_for(settings_.tab_current_color, false) + label + R + B(top) + " ");
    {
        const int used = 3 + display_width(label) + 1;   // "┌─ " + label + " "
        std::string dash;
        for (int i = used; i < iw + 1; ++i) dash += "─";
        frame << dash << R;
    }
    pos(top, x1, B(top) + "┐" + R);
    for (int y = top + 1; y < bottom; ++y) {
        pos(y, x0, B(y) + "│" + R + std::string(static_cast<size_t>(iw), ' '));
        pos(y, x1, B(y) + "│" + R);
    }
    pos(bottom, x0, B(bottom) + "└");
    {
        std::string dash;
        for (int i = 0; i < iw; ++i) dash += "─";
        frame << dash << "┘" << R;
    }
    const int inner_x = x0 + 1;
    if (out.clap) {   // the loading picture of a video, made by the same code as the video view (the box inside is iw x body_h cells)
        out.rows.assign(static_cast<size_t>(body_h), std::string(static_cast<size_t>(iw), ' '));
        out.w = iw;
        if (settings_.video_clap == 1) {
            clap_loading_fill(out.rows, iw, body_h, inner_x - 1, top, gfx_, P.vid_key, 0);
            if (gfxs_.proto != GfxProto::None && body_h >= 4) pv_cover_pic_ = true;   // (the picture uses the picture slot gfx_, like the sample cover)
        } else if (settings_.video_clap == 0) {
            const std::string msg = "video: starting ...";
            const int pad = std::max(0, (iw - display_width(msg)) / 2);
            out.rows[static_cast<size_t>(body_h / 2)] = std::string(static_cast<size_t>(pad), ' ') + msg + std::string(static_cast<size_t>(std::max(0, iw - pad - display_width(msg))), ' ');
        }
    }
    if (out.milk) {   // the live preset, drawn like the preset overlay's preview (a picture in Kitty / Sixel terminals, braille otherwise)
        std::vector<std::string> pic;
        if (P.md_fresh) {   // (the picture slot held the main screen's cover and disk: it must not show until the first MilkDrop picture is in it)
            P.md_fresh = false;
            gfx_.w = 0; gfx_.h = 0; gfx_.level.clear(); gfx_.grid.clear();
            milkdrop_pic_key_.clear();
        }
        gfx_.active = false;   // (the drawing switches it on again once there is a picture)
        const std::vector<std::string> nb = milkdrop_name_box(cw - 2, true);   // (Show Preset Name: the box, kept on while the row is shown)
        if (milkdrop_draw(pic, cw, body_h, inner_x, top, false, 0, nb, nb.empty() ? 0 : cw)) {
            out.rows = pic;
            if (gfxs_.proto != GfxProto::None && gfx_.active && gfx_.w > 0) pv_cover_pic_ = true;
            const std::string nm = Milkdrop::instance().preset_name();
            if (id == kSidMdColors) out.caption = std::string("Sixel colours: ") + (settings_.milkdrop_fixed_colors ? "fixed" : "adaptive") + (gfxs_.proto == GfxProto::Sixel ? "" : " (Sixel only)");
            else out.caption = nm.empty() ? "MilkDrop" : "MilkDrop: " + nm;
        } else {
            std::string msg = Milkdrop::instance().status();
            if (msg.empty()) msg = "milkdrop: starting ...";
            out.rows = {truncate_str(msg, std::max(1, cw))};
            out.caption = "MilkDrop";
        }
    }
    const int nrows = std::min(static_cast<int>(out.rows.size()), body_h);
    const int vtop = std::max(0, (body_h - nrows) / 2);
    const int xo = out.w > 0 ? std::max(1, (iw - out.w) / 2 + 1) : (compact ? 3 : 2);
    if (out.cover_mode >= 0 && nrows > 0) {   // the sample cover as a picture (it uses the picture slot gfx_)
        pv_cover_pic_ = true;
        meta_sty_gfx(pv_->cover, out.cover_mode, settings_.cover_contrast, settings_.cover_brightness, settings_.cover_colors, settings_.cover_chroma,
                     pv_->angle, inner_x + xo - 2, top + vtop, out.w, nrows, settings_.cover_grad_sat, settings_.cover_vivid_eff());
    }
    if (out.video && nrows > 0 && out.vframe) {   // the sample video as a picture (it uses the picture slot gfx_, like the sample cover)
        pv_cover_pic_ = true;
        GfxFrame& g = gfx_;
        const std::string key = std::to_string(out.vw) + "x" + std::to_string(out.vh) + "|" + std::to_string(out.vcols) + "x" + std::to_string(out.vrows) + "|" +
                                std::to_string(inner_x + xo - 2) + "," + std::to_string(top + vtop) + (settings_.video_round ? "|r" : "|-");
        const bool resized = key != P.vid_key || !g.plain || g.w != out.vw || g.h != out.vh || g.level.size() != static_cast<size_t>(out.vw) * out.vh || g.id != 0;
        g.plain = true; g.truecolor = false; g.direct = true; g.scale = 1;
        g.out_cw = out.cw0; g.out_ch = out.ch0;
        g.cols = out.vcols; g.rows = out.vrows; g.w = out.vw; g.h = out.vh;
        g.col = inner_x + xo - 2; g.row = top + vtop;
        g.id = 0; g.grid.clear(); g.cut_w = 0; g.crop = 0;
        g.opaque = settings_.black_background;
        g.erase = !settings_.black_background;
        g.active = true;
        g.fresh = out.vfresh || resized;
        if (g.fresh) {
            VideoPlayer::quantize(*out.vframe, g.hue, g.level, settings_.video_dither);
            if (settings_.video_round) {   // Video Rounded Corners: the corners cut round, like in the main screen (a quarter circle of about half a cell)
                const int rp = std::clamp(g.w / std::max(1, out.vcols) * 11 / 20, 2, std::min(g.w, g.h) / 2);
                const float rf = static_cast<float>(rp);
                for (int y = 0; y < rp; ++y)
                    for (int x = 0; x < rp; ++x) {
                        const float dx = rf - static_cast<float>(x) - 0.5f, dy = rf - static_cast<float>(y) - 0.5f;
                        if (dx * dx + dy * dy <= rf * rf) continue;
                        const int xs[2] = {x, g.w - 1 - x}, ys[2] = {y, g.h - 1 - y};
                        for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) g.level[static_cast<size_t>(ys[b]) * g.w + xs[a]] = 0;
                    }
            }
            const auto& pal = VideoPlayer::palette();
            for (size_t i = 0; i < pal.size() && i < g.pal.size(); ++i) g.pal[i] = pal[i];
            P.vid_key = key;
        }
    }
    if (out.picture && nrows > 0) {   // where the picture goes (0-based row and column)
        pv_spectro_area_[0] = top + vtop;
        pv_spectro_area_[1] = inner_x + xo - 2;
        pv_spectro_area_[2] = out.w;
        pv_spectro_area_[3] = nrows;
    }
    for (int i = 0; i < nrows; ++i)
        if (!out.rows[static_cast<size_t>(i)].empty()) pos(top + 1 + vtop + i, inner_x + xo - 1, out.rows[static_cast<size_t>(i)] + R);
    if (!out.caption.empty()) {
        std::string cap = out.caption;
        if (display_width(cap) > iw - 2) cap = truncate_str(cap, iw - 2);
        const int cx = inner_x + std::max(1, (iw - display_width(cap)) / 2);
        pos(bottom - 1, cx, ansi_for(settings_.tab_current_color, false) + cap + R);
    }
}

void App::settings_preview_end() const {   // the settings screen closes (or another row is shown): the MilkDrop preset from before comes back
    if (!pv_ || !pv_->md_active) return;
    pv_->md_active = false;
    if (pv_->md_pv_locked) { Milkdrop::instance().set_locked(settings_.milkdrop_lock); pv_->md_pv_locked = false; }
    if (!pv_->md_prev.empty()) Milkdrop::instance().play_path(pv_->md_prev);
    pv_->md_prev.clear();
}

void App::settings_preview_set_prev(const std::string& preset) const { if (pv_ && pv_->md_active) pv_->md_prev = preset; }

SpectroAnalyzer* App::settings_preview_spectro() const { return pv_ ? &pv_->spec : nullptr; }

}  // namespace muisc
