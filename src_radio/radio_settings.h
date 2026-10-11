#pragma once
#include "themes.h"
// Radio mode -- its OWN settings, separate from the music player's `Settings` / config.txt.
//
// The radio reads and writes `radio_config.txt` (next to stations.txt and presets.txt in ~/.config/mousiki/, or
// ./radio_config.txt) and uses nothing from src/settings.*. Colours are plain decimal 256-colour palette indices
// as strings ("1".."255"); "0" or an empty string means "no colour" (the terminal's own). Values 30-47 and 90-107
// are sent as direct SGR codes, like the player does.
//
// Every colour field is named after WHAT it colours (not after the player element it was once borrowed from):
//   on_air_*      the ON AIR sign's diagonal gradient (and the needle / lamp / LIVE text, which use its first colour)
//   viz_*         the FFT spectrum under the station info
//   osci_*        the oscilloscope (X-Y scope, top right)
//   freq_line / freq_mhz   the line + ticks of the frequency band / its numbers and the small dots
//   preset_*      the PRESETS pane (unassigned slots, the key letters)
//   volume_*      the "#" and "-" of the volume bar
//   list_*        the STATIONS list rows (also used for the station names in the PRESETS pane)
//   header        secondary text: captions ("off air"), the ON AIR sign while nothing is live, empty-pane messages, status lines
//   legend        key command legends: the "[TAB] Switch pane | ..." hint lines, hint tails, error lines, cheatsheet titles
#include "scope_metrics.h"
#include "spectrogram.h"
#include <filesystem>
#include "equalizer.h"
#include <map>
#include <vector>
#include <string>

namespace muisc::radio {

// Palettes of the scope (Color row of the overlay). 0 = the OSCI gradient of the COLORS tab laid over the picture from left to
// right; the others colour by the beam speed (about the pitch: slow = first colour, fast = last colour).
constexpr int kOsciPaletteCount = 9;
extern const char* const kOsciPaletteNames[kOsciPaletteCount];   // gradient, settings, temperature, aurora, magma, ice, neon, spectrum, classic green

struct OsciSet {
    float decay = 0.80f;                // afterglow per frame, 0.00 .. 0.99
    float dot_threshold = 0.28f;        // braille: brightness a subpixel needs to light a dot, 0.01 .. 1.00
    float tail = 0.45f;                 // brightness of the oldest trace sample, 0.00 .. 1.00
    bool interp = true;                 // Line/Vec. Interpol.: connect the samples with lines
    bool z = false;                     // Z-Axis (XYZ mode): the beam intensity follows a Z signal
    float z_depth = 0.70f;              // 0.00 .. 1.00, how strongly Z modulates the beam
    int z_source = 0;                   // 0 = beam speed, 1 = signal level
    int trace = 1024;                   // samples per frame, 128 .. 1024
    bool rotate = false;                // 45 degree rotation (M/S view)
    bool mono_phase = true;             // near-mono signals draw a phase portrait instead of a diagonal line
    int palette = 0;                    // see kOsciPaletteNames
    float glow = 0.60f;                 // image: size / strength of the bloom around the beam, 0.00 .. 1.00
    int frame_rate = 30;                // screen refresh while this style is in use: 30 | 45 | 60 | 90 | 120 | 165 fps
    bool music = false;                 // oscilloscope music mode (SHIFT+o): unprocessed signal, fixed scale, no phase portrait
};

struct RadioSettings {
    // --- colours ----------------------------------------------------------------------------------
    std::string border_top = "170";
    std::string border_bottom = "170";          // the frame fades from border_top (top) to border_bottom (bottom)
    bool border_smooth = true;                  // BorderGradient=smooth|rough: smooth fades the frame line by line; rough = only the bottom border line takes border_bottom

    std::string on_air_upper_left = "170";      // ON AIR sign: gradient from the upper left ...
    std::string on_air_bottom_right = "30";     // ... to the bottom right (plus the travelling colour wave)

    std::string meta_key = "170";               // station info labels ("Station", "Title" ...)
    std::string meta_val = "182";               // station info values (also the station name next to the needle)

    std::string viz_left = "170";               // FFT spectrum, left edge
    std::string viz_center;                     // optional middle stop (config file only); empty = plain left -> right
    std::string viz_right = "30";               // FFT spectrum, right edge

    std::string osci_left = "170";              // oscilloscope, left edge
    std::string osci_right = "30";              // oscilloscope, right edge

    std::string freq_line = "170";              // the frequency line and its ticks
    std::string freq_mhz = "90";                // the numbers (88 92 96 ...) and the small dots for stations

    std::string preset_inactive_fg = "90";      // PRESETS pane: key slots that have no station ("5 -")
    std::string preset_inactive_bg;
    std::string preset_key_fg = "255";          // PRESETS pane: the short-cut key letters (1 2 ... e r t d f g)

    std::string volume_current = "170";         // the "#" of the volume bar
    std::string volume_possible = "182";        // the "-" of the volume bar

    // Search bar input (the "/s:" box and the other "Search:" rows). Empty = follow LIST INACTIVE FG/BG,
    // which is what these bars always looked like; "0" = explicit terminal default.
    std::string search_fg;                      // typed query: FG / BG, behind the query text only
    std::string search_bg;
    std::string search_inactive_fg;             // the rest of the bar (prefix, empty field)
    std::string search_inactive_bg;

    std::string list_fg = "255";                // STATIONS rows (and the station names in PRESETS)
    std::string list_bg;
    std::string list_playing_fg = "245";        // the tuned station
    std::string list_playing_bg;
    std::string list_cursor_fg;                 // the hovered row
    std::string list_cursor_bg = "240";

    std::string tab_current = "10";             // settings screen: the current tab name "[ON/OFF]" and the "< ↔ >" hint
    std::string tab_other = "90";               // settings screen: the other tab names

    std::string focus = "90";                   // FOCUS: the pane focus indicator "\u25c0" after a pane's title (default = header)
    std::string header = "90";                  // secondary text (see the top of this file)
    std::string legend = "10";                  // key command legends (see the top of this file)

    // --- looks / tuning (config file only for now; the settings screen has the colours only) ----------
    // --- the ON/OFF tab ---------------------------------------------------------------------------
    bool on_air_ascii = true;                   // the ON AIR sign (with its colour wave)
    bool pulse_wave = true;                     // the faint circle burst behind the sign
    bool dummy_buttons = true;                  // the <<< MUTE >>> boxes next to the frequency band
    int scope_mode = 3;                         // right of the station info: 0 = oscilloscope, 1 = sphere, 2 = off, 3 = spectrogram (default)
    SpectroSettings spectro;                    // the spectrogram (SHIFT+i overlay, SHIFT+u full screen), Audacity's defaults
    bool element_visualizer = true;             // the FFT spectrum under the station info
    bool stereo = true;                         // false = fold left + right to mono
    // Which mode Mousiki starts in ("player" / "radio"): the same setting as the music player's, kept in ITS config.txt
    // (StartMode=); the radio only reads and writes that one line (player_start_mode() / set_player_start_mode()).
    std::string start_mode = "player";
    bool use_osci = true;                       // ON/OFF "Use oscilloscope": false = never in the scope block / "." cycle
    bool use_spectro = true;                    // ON/OFF "Use spectrogram": the same for the spectrogram
    // The help wizard (SHIFT+w): false means it has never been shown here, so
    // the first start of the radio after an install opens it by itself. Set
    // and written the first time it is closed, exactly like the player's
    // WizardSeen -- the two modes count separately, so each greets once.
    bool wizard_seen = false;
    bool normalize = true;                      // loudness normalisation (SHIFT+v overlay, `v` toggles)
    double normalize_target_lufs = -16.0;       // -40 .. 0
    double normalize_max_boost_db = 9.0;        // 0 .. 24
    // Equaliser (SHIFT+e overlay): the player's 10-band EQ with its presets; own values, saved in radio_config.txt.
    bool eq_enabled = false;
    EqGains eq_gains = {};
    std::vector<EqCustomPreset> eq_custom_presets;
    double gradient_wave_speed = 0.25;          // ON AIR sign colour wave, cycles per second
    double pulse_wave_speed = 0.125;            // circle burst behind the sign, loops per second (independent of the colour wave)
    bool playback_shuffle = true;               // station surf mode: true = shuffle (S), false = list (L); `m` toggles
    // --- the PATHS tab (empty = the default; see StoragePaths below) -----------------------------------
    std::string stations_path;                  // station lists: <path>/stations/stations.txt + <path>/station_lists/stationlists.txt
    std::string presets_path;                   // <path>/presets/presets.txt
    std::string download_path;                  // recordings (y) + YouTube downloads go to <path>/radio_downloads
    bool download_same_as_player = true;        // true: use the music player's download folder instead
    std::string history_path;                   // <path>/radio_history/history_radio.txt
    // --- sleep timer (SHIFT+z overlay) and the REFERENCE tab ------------------------------------------
    bool marquee = true;                        // ON/OFF tab: a station name that is too long scrolls; false = cut off with "..."
    bool tune_noise = false;                    // ON/OFF tab: static that fades in / out when another station is tuned
    // ON/OFF tab: how much of the tuned station is kept for pause / rewind / recording from the past, as an index into
    // kTimeshiftMinutes (5 / 15 / 30 / 45 / 60 min). Kept on disk: 11.5 MB per minute.
    int timeshift_idx = 2;
    bool sleep_fade = true;                     // the sleep timer fades the volume out before it stops the stream
    std::map<std::string, std::string> keys;    // rebound keys by action id (kKeyActions); absent = the default
    int visualizer_fluidity = 1;                // 1-10, higher = smoother / slower rise
    int visualizer_degradation_speed = 8;       // 1 (slow fade) - 10 (near-instant)
    int visualizer_viscosity = 3;               // 0 (bars independent) - 10 (heavy neighbour blending)
    OsciSet osci_set[2];                        // the scope's parameters: [0] = braille, [1] = image (each style keeps its own)
    bool scope_grid = true;                     // the big scope overlay (SHIFT+0): axes / grid over both pictures (SHIFT+a there; ScopeGrid=)
    bool scope_values = true;                   // ... and the axis values at the grid lines (SHIFT+v there; ScopeValues=)
    std::string scope_grid_color;               // its colour 0..255; empty = the colour of inactive list elements (SHIFT+c there; ScopeGridColor=)
    ScopeMetrics scope_metrics;                 // ... and its grid metrics, per picture (SHIFT+p there; ScopeYt* / ScopeXy*)
    int osci_mode = 0;                          // ON/OFF: scope mode 0 = XY, 1 = YT (amplitude over time), 2 = both on top of each other (overlap), 3 = both in two rows, 4 = both in two columns (OsciMode=xy|yt|overlap|row|col)
    int osci_style = 0;                         // ON/OFF: 0 = braille, 1 = image (real pixels through the terminal's graphics protocol)
    std::string gfx_protocol = "auto";          // auto | kitty | sixel | off (MOUSIKI_RADIO_GFX overrides)
    std::string cell_pixels;                    // "WxH" pixels of one terminal cell; empty = ask the terminal
    int image_scale = 2;                        // image style: 1 = full, 2 = half, 3 = a third of the screen resolution
    OsciSet& osci() { return osci_set[osci_style == 1 ? 1 : 0]; }                 // the set of the style in use
    const OsciSet& osci() const { return osci_set[osci_style == 1 ? 1 : 0]; }

    std::string box_upper_left = "╭";      // ╭
    std::string box_upper_right = "╮";     // ╮
    std::string box_lower_left = "╰";      // ╰
    std::string box_lower_right = "╯";     // ╯
    std::string box_vertical = "│";        // │
    std::string box_horizontal = "─";      // ─
    std::string list_separator = "|";           // between the columns of the lists
};

// ---- the COLORS tab of the settings screen -------------------------------------------------------------
// One row of the tab: a group name, one or two colour cells (member pointers into RadioSettings). A row with
// no second cell (label2 == nullptr) is foreground-only.
// ---- colour themes (see src/themes.h) -------------------------------------------------------------------
void theme_apply(RadioSettings& s, const themes::Theme& t);
themes::Theme theme_capture(const RadioSettings& s, themes::Theme base);
bool theme_matches(const RadioSettings& s, const themes::Theme& t);
int theme_index(const RadioSettings& s, const std::vector<themes::Theme>& all);   // the theme `s` matches exactly, or -1

struct ColorRowSpec {
    const char* group;                          // "" = continuation of the group above
    const char* label1; std::string RadioSettings::* field1;
    const char* label2; std::string RadioSettings::* field2;
};
extern const ColorRowSpec kColorRows[];
extern const int kColorRowCount;
// Rows before this index are drawn above the divider line of the tab, the rest below it.
constexpr int kColorRowsAboveDivider = 7;

// The COLORS tab, top to bottom: row 0 = THEME | CREATE THEME, row 1 = the BORDER_COLOR row (colour row 0), row 2 =
// SMOOTHNESS (rough / smooth), rows 3.. = the other colour rows. color_field() and the theme creator count colour rows alone.
inline int color_row_of_tab(int t) { return t == 1 ? 0 : (t >= 3 ? t - 2 : -1); }
inline int tab_row_of_color(int c) { return c == 0 ? 1 : c + 2; }
inline int color_tab_row_count() { return kColorRowCount + 2; }   // + THEME + SMOOTHNESS

// Cell (row, col 0/1) of the COLORS tab, or nullptr if that row has no such cell.
std::string* color_field(RadioSettings& s, int row, int col);
const std::string* color_field(const RadioSettings& s, int row, int col);

// ---- the ON/OFF and ANIMA/VIZ tabs of the settings screen ----------------------------------------------------------
// Both tabs are lists of category headers and rows (like the music player's): tab 1 ON/OFF = MODES (Playback, Start Mode)
// + ON/OFF (the switches), tab 2 ANIMA/VIZ = ON AIR SIGN | VISUALIZER | MODES (the scope pick and its style). The cursor
// (SettingsModel::row) counts the rows only; rs_row_line() gives the line it is drawn on.
struct SettingsLine { bool header; int id; const char* text; };       // a category header, or a row (id + label)
const std::vector<SettingsLine>& rs_layout(int tab);                   // tab 1 or 2
int rs_row_count(int tab);                                              // selectable rows of the tab
int rs_row_line(int tab, int row);                                      // line (headers counted) of a row
const char* rs_row_label(int tab, int row);
std::string rs_row_value(const RadioSettings& s, int tab, int row);    // "true" / "false", "osci" / "sphere" / "off" / "spectro", ...
void rs_row_change(RadioSettings& s, int tab, int row, int dir);        // left (-1) / right (+1) / ENTER (0 = next)
extern const int kOnOffRowCount;                                        // rows of tab 1
extern const int kAnimRowCount;                                         // rows of tab 2
inline constexpr int kTimeshiftMinutes[5] = {5, 15, 30, 45, 60};
inline int timeshift_minutes(const RadioSettings& s) { return kTimeshiftMinutes[s.timeshift_idx < 0 ? 0 : s.timeshift_idx > 4 ? 4 : s.timeshift_idx]; }
std::string scope_mode_name(const RadioSettings& s);                    // "osci" | "sphere" | "off" | "spectro"
// Start Mode lives in the music player's config.txt (StartMode=player|radio): read it / write just that line.
std::string player_start_mode();
bool set_player_start_mode(const std::string& mode);

// ---- the REFERENCE tab: the keys that can be changed ------------------------------------------------------
// A key is written like the music player writes it: one character (case matters: S and s are different keys), or
// SPACE / TAB / BACKSPACE. Enter, Esc and the arrows keep their jobs and cannot be bound. The preset slot keys
// (1234567890ertdfg) and j / k are fixed; they only take part in the conflict check.
struct KeyAction { const char* header; const char* id; const char* label; const char* def; };
extern const KeyAction kKeyActions[];
extern const int kKeyActionCount;
std::string key_binding(const RadioSettings& s, int action);                 // current key string (the default when unset)
int key_code(const std::string& key);                                        // key string -> code, 0 = not a valid key
// "" when `key` is free, else what it is already used for ("NEXT CHANNEL", "PRESET SLOT KEY" ...). `except` = the action being edited.
std::string key_conflict(const RadioSettings& s, const std::string& key, int except);
// Translates a typed key into the key the main screen's switch handles (the default key of the action the typed key is
// bound to); 0 for a default key that was rebound away. Keys that are not bound to anything pass through unchanged.
int key_translate(const RadioSettings& s, int typed);
// The code of the key an action is bound to right now.
int key_of(const RadioSettings& s, const char* id);
// the ABOUT tab text is about_app_text() (about_text.h), shared with the music player

// ---- the PATHS tab -----------------------------------------------------------------------------------------
// Rows: 0 STATION LISTS PATH | 1 PRESETS PATH | 2 DOWNLOAD PATH | 3 "Same folder as music player" (bool) | 4 HISTORY PATH.
extern const int kPathRowCount;
extern const char* const kPathRows[];
constexpr bool path_row_is_bool(int row) { return row == 3; }
std::string* path_row_text(RadioSettings& s, int row);              // nullptr for the bool row
const std::string* path_row_text(const RadioSettings& s, int row);

// Where everything ends up for the current settings.
struct StoragePaths {
    std::filesystem::path stations_file, lists_file, presets_file;   // the three data files
    std::filesystem::path history_dir;                               // holds history_radio.txt + archive_radio.txt
    std::filesystem::path download_dir;                              // recordings + downloads
    bool player_download = false;                                    // download_dir is the music player's folder
    std::string download_note;                                       // "" or why it is what it is
};
StoragePaths resolve_storage_paths(const RadioSettings& s);
// The music player's DownloadFolder (read from its config.txt, `DownloadFolder=`); "" = the player's built-in default.
std::string player_download_folder();
std::filesystem::path radio_default_dir();                           // ~/.config/mousiki

// The help wizard (SHIFT+w) is one feature, so it looks the same in both
// modes: the radio draws it in the MUSIC PLAYER's colours rather than its
// own. These are read straight out of the player's config.txt, the same
// read-only peek player_download_folder() does -- the radio has no other
// connection to the player's settings. Every field keeps the player's own
// default when config.txt is missing or does not set that row, so the wizard
// is always drawn in sensible colours even on a first start.
struct WizardPalette {
    std::string border      = "90";   // ColorBorderTop
    std::string border_bot  = "90";   // ColorBorderBottom
    std::string header      = "10";   // ColorHeader   -- headings
    std::string legend      = "90";   // ColorLegend   -- key hints
    std::string list;                 // ColorListInactiveFg -- body text ("" = the terminal's own)
    std::string cursor_fg;            // ColorListCursorFg   -- the contents cursor
    std::string cursor_bg   = "238";  // ColorListCursorBg
    std::string accent;               // ColorLyricsActiveWordFg, falling back to the cursor / header colour
    std::string disk_top    = "36";   // ColorDiskTop    -- the welcome page's disk
    std::string disk_bottom = "34";   // ColorDiskBottom
};
WizardPalette player_wizard_palette();

// ---- the SHIFT+o (oscilloscope) and SHIFT+v (loudness) overlays: same knobs and ranges as the music player's ----
struct Knob { const char* label; double lo, hi, step; };
constexpr Knob kOsciKnobs[3] = {{"Decay", 0.00, 0.99, 0.01}, {"Dot threshold", 0.01, 1.00, 0.01}, {"Tail", 0.00, 1.00, 0.02}};
// Overlay rows (ids). Which of them are shown depends on the style: osci_visible_rows().
enum OsciRow { kOrDecay, kOrDot, kOrTail, kOrInterp, kOrZ, kOrZDepth, kOrZSource, kOrTrace, kOrRotate, kOrMono, kOrPalette, kOrGlow,
               kOrStyle, kOrCells, kOrProtocol, kOrFps, kOrDisplay, kOrRes, kOrMusic, kOrMode, kOsciRowCount };
// image style: the picture is rendered at 1/1, 1/2 or 1/3 of the screen resolution and enlarged on the way to the terminal
const char* osci_res_name(int scale);   // "full" | "half" | "third"
int osci_res_parse(const std::string& v, int def);
std::vector<int> osci_visible_rows(const RadioSettings& s);
constexpr Knob kNormKnobs[3] = {{"Normalize", 0.0, 1.0, 1.0}, {"Target level", -40.0, 0.0, 1.0}, {"Max boost", 0.0, 24.0, 1.0}};
std::string osci_row_label(int row);
std::string osci_row_value(const RadioSettings& s, int row);
void osci_adjust(RadioSettings& s, int row, int dir);     // dir -1 / +1
void osci_reset(RadioSettings& s);
// "Use oscilloscope / spectrogram" off: what is switched off never stays in the scope block.
void radio_enforce_visuals(RadioSettings& s);
void norm_adjust(RadioSettings& s, int row, int dir);     // row 0 = on/off (left off, right on)
void norm_reset(RadioSettings& s);                        // target + boost back to the defaults; on/off is left alone

// ---- config file -------------------------------------------------------------------------------------
std::filesystem::path radio_config_path();               // where radio_config.txt is read from / written to
RadioSettings load_radio_settings(std::string* source_out = nullptr);   // defaults when there is no file
bool save_radio_settings(const RadioSettings& s, std::string* err = nullptr);

// ---- colour helpers (the radio's own; same value rules as above) -------------------------------------------
bool color_rgb(const std::string& color, int& r, int& g, int& b);   // 0-255 colour number -> RGB (false: none)
std::string ansi_fg(const std::string& color);           // "\x1b[38;5;Nm" ... or "" for none
std::string ansi_bg(const std::string& color);
std::string sgr_params(const std::string& color);        // "38;5;N" without the escape, or ""
std::string gradient_fg(const std::string& start, const std::string& end, float t);                       // truecolor between two palette colours
std::string gradient3_fg(const std::string& left, const std::string& center, const std::string& right, float t);   // 3 stops; no centre: 2 stops
std::string gradient_bar(const std::string& start, const std::string& end, int width);                    // a strip of blocks (settings preview)
// True for "", "0" or a number 0..255 (what a colour cell accepts).
bool valid_color_value(const std::string& v);

} // namespace muisc::radio
