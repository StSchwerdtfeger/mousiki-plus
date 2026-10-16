// Colour themes shared by the music player and the radio.
//
// A theme is a name plus "Key=Value" lines using the config-file key names of the Colors tabs
// (ColorBorderTop, ColorListCursorBg, ...). Applying a theme sets every key the app knows and leaves the
// rest alone, so one themes.txt serves both programs. Where the radio's meaning differs from the player's
// (ColorHeader / ColorLegend are swapped there) a "Radio." prefix gives the radio its own value.
//
// The file lives next to config.txt: ~/.config/mousiki/themes.txt. It is written on first use with the
// built-in themes, and holds the themes the user creates; edit it freely.
#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace themes {

constexpr int kMaxNameLen = 27;

struct Theme {
    std::string name;
    bool builtin = false;
    std::vector<std::pair<std::string, std::string>> kv;

    // The value stored for `key`; for the radio "Radio.<key>" wins. nullptr = not defined by this theme.
    const std::string* get(const std::string& key, bool radio) const;
    // Sets `key` (plain). With radio_only the key is stored as "Radio.<key>".
    void set(const std::string& key, const std::string& value, bool radio_only = false);
};

// Index of the theme called `name` (case-insensitive, as the store compares names), or -1.
int find_name(const std::vector<Theme>& all, const std::string& name);

// ---- colour numbers ----
// Every colour number is a 256-colour palette number (1-255, 0 = none) in every field. Before format 6 of themes.txt /
// before "ColorCodes=palette" in config.txt and radio_config.txt, 30-37 and 90-97 (text fields) and 40-47 and 100-107
// (background fields) were plain 16-colour terminal codes. The old files are converted once, when they are loaded.
enum class ColorRole { Text, Background, Gradient };   // Gradient: ends of a colour fade -- always were palette numbers
ColorRole role_of_key(const std::string& config_key);  // "ColorBorderTop", "Radio.ColorLegend", ...
// The palette number that looks like the old plain code `value` had in a field of that role; anything else is unchanged.
// Black is "16" (palette 0 is "no colour" in the settings).
std::string legacy_to_palette(const std::string& value, ColorRole role);

std::filesystem::path file_path();

// Built-in themes first (Default is always index 0), then the user's, in file order.
// Creates themes.txt with the built-ins when it does not exist yet.
std::vector<Theme> load_all();

// Adds or replaces a user theme in themes.txt. False with `err` set when the name is empty, taken by a
// built-in theme, or the file cannot be written.
bool save_theme(const Theme& t, std::string& err);
// Removes a theme of your own from themes.txt. False with `err` set for a built-in theme or an unknown name.
bool delete_theme(const std::string& name, std::string& err);

std::string sanitize_name(const std::string& raw);

// ---- export / import (SHIFT+e / SHIFT+i on the Colors tab) ----
// Each built-in theme as an earlier version wrote it (first) and as it is now (second): a config.txt / radio_config.txt that
// still holds exactly the old colours of a built-in theme is moved to the new ones when the program starts.
std::vector<std::pair<Theme, Theme>> renewed_builtins();
// Does the settings file `config` (config.txt, or radio_config.txt with `radio`) hold exactly the colours of `t`, as written
// (an empty value counts as empty)? At least 10 of the theme's keys must be in the file.
bool file_holds(const std::filesystem::path& config, const Theme& t, bool radio);
std::filesystem::path exchange_dir();                       // default folder: ~/.config/mousiki
// Writes `t` as a stand-alone theme file (same format as themes.txt). Folder is created when missing.
bool write_theme_file(const std::filesystem::path& file, const Theme& t, std::string& err);
std::string file_name_for(const std::string& theme_name);   // "<name>.theme", unsafe characters replaced
// Theme files in `dir` (*.theme / *.txt holding at least one [Name] section with colour keys), name-sorted.
std::vector<std::filesystem::path> list_theme_files(const std::filesystem::path& dir);
std::vector<Theme> read_theme_file(const std::filesystem::path& file);
// Adds an imported theme to themes.txt under a free name (a built-in or existing name gets " (2)", " (3)" ...).
// Returns the name it was stored under, "" with `err` set on failure.
std::string import_theme(Theme t, std::string& err);

} // namespace themes
