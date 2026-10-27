#pragma once
// MOUSIKI_SCRIPTS_DIR: an optional override for the folder with the helper scripts. The value comes from the
// environment, so it is checked before a file in it is used: it has to be an absolute path to an existing folder
// without ".." parts, and the wanted file has to be a regular file in it (not a link out of it, not a folder).
// Returns the file's canonical path, or an empty path when the override is not set or does not pass.
#include "path_utf8.h"
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace muisc {

inline std::filesystem::path scripts_file_from_env(const std::string& filename) {
    namespace fs = std::filesystem;
    const char* env = std::getenv("MOUSIKI_SCRIPTS_DIR");
    if (!env || !*env) return {};
    if (filename.empty() || filename.find_first_of("/\\") != std::string::npos || filename == "..") return {};
    const fs::path dir = path_from_utf8(env);
    if (!dir.is_absolute()) return {};
    for (const fs::path& part : dir) if (part == "..") return {};
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return {};
    const fs::path file = dir / filename;
    const fs::file_status st = fs::symlink_status(file, ec);
    if (ec || !fs::is_regular_file(st)) return {};
    const fs::path canon = fs::weakly_canonical(file, ec);
    return ec ? fs::path() : canon;
}

}  // namespace muisc
