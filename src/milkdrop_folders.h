// The preset folders of the MilkDrop visuals: lists of presets that the user makes (next to the Favourites and the Recommended list).
// One text file per folder in <config folder>/milkdrop_folders/<name>.txt: one preset file (full path) per line. Shared by the player and the radio.
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "path_utf8.h"

namespace muisc {
namespace mdf {

inline std::filesystem::path dir_of(const std::filesystem::path& config_dir) { return config_dir / "milkdrop_folders"; }

// A folder name for a file name: no path characters, no dots at the ends, at most 40 bytes; "" when nothing is left (or it is a name of the built-in lists).
inline std::string clean_name(const std::string& in) {
    std::string n;
    for (char c : in) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 32) continue;
        n += c;
    }
    while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
    size_t b = 0;
    while (b < n.size() && (n[b] == ' ' || n[b] == '.')) ++b;
    n.erase(0, b);
    if (n.size() > 40) { n.resize(40); while (!n.empty() && (static_cast<unsigned char>(n.back()) & 0xC0) == 0x80) n.pop_back(); if (!n.empty() && static_cast<unsigned char>(n.back()) >= 0xC0) n.pop_back(); }
    std::string l;
    for (char c : n) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (l == "favourites" || l == "favorites" || l == "recommended") return std::string();
    return n;
}

inline std::vector<std::string> names(const std::filesystem::path& config_dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir_of(config_dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".txt") continue;
        out.push_back(path_utf8(it->path().stem()));
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        std::string x = a, y = b;
        for (char& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : y) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return x < y;
    });
    return out;
}

inline bool exists(const std::filesystem::path& config_dir, const std::string& name) {
    std::error_code ec;
    return !name.empty() && std::filesystem::is_regular_file(dir_of(config_dir) / path_from_utf8(name + ".txt"), ec);
}

inline std::vector<std::string> load(const std::filesystem::path& config_dir, const std::string& name) {
    std::vector<std::string> out;
    std::ifstream in(dir_of(config_dir) / path_from_utf8(name + ".txt"), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (std::find(out.begin(), out.end(), line) == out.end()) out.push_back(line);
    }
    return out;
}

inline bool save(const std::filesystem::path& config_dir, const std::string& name, const std::vector<std::string>& paths) {
    std::error_code ec;
    std::filesystem::create_directories(dir_of(config_dir), ec);
    std::ofstream o(dir_of(config_dir) / path_from_utf8(name + ".txt"), std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << "# MilkDrop preset folder \"" << name << "\" (Mousiki): one preset file per line, full paths.\n";
    for (const std::string& p : paths) o << p << "\n";
    return static_cast<bool>(o);
}

// false when the name is not usable or the folder is there already
inline bool create(const std::filesystem::path& config_dir, const std::string& raw_name, std::string* made = nullptr) {
    const std::string n = clean_name(raw_name);
    if (n.empty() || exists(config_dir, n)) return false;
    if (made) *made = n;
    return save(config_dir, n, {});
}

// 0 added, 1 it was there already, 2 the folder could not be written
inline int add(const std::filesystem::path& config_dir, const std::string& name, const std::string& preset) {
    std::vector<std::string> v = load(config_dir, name);
    if (std::find(v.begin(), v.end(), preset) != v.end()) return 1;
    v.push_back(preset);
    return save(config_dir, name, v) ? 0 : 2;
}

inline bool remove_preset(const std::filesystem::path& config_dir, const std::string& name, const std::string& preset) {
    std::vector<std::string> v = load(config_dir, name);
    v.erase(std::remove(v.begin(), v.end(), preset), v.end());
    return save(config_dir, name, v);
}

inline bool remove_folder(const std::filesystem::path& config_dir, const std::string& name) {
    std::error_code ec;
    return std::filesystem::remove(dir_of(config_dir) / path_from_utf8(name + ".txt"), ec);
}

}  // namespace mdf
}  // namespace muisc
