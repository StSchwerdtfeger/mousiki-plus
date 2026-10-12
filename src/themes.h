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

constexpr int kMaxNameLen = 25;

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
