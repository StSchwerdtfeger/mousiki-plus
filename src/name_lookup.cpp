// Artist and Japanese name lookup: runs the helper script and parses its JSON replies (romanised names
// and kanji readings).
#include "name_lookup.h"

#include <chrono>
#include <fstream>
#include <sstream>

#include "path_utf8.h"
#include "process_util.h"
#include "tiny_json.h"
#if defined(_WIN32)
#include "win_compat.h"
#endif

namespace muisc {

namespace fs = std::filesystem;

NameLookupResult parse_name_lookup_line(const std::string& line) {
    NameLookupResult r;
    tinyjson::Value v;
    if (line.empty() || line[0] != '{' || !tinyjson::parse(line, v) || v.type != tinyjson::Type::Object) {
        r.error = "unreadable answer";
        return r;
    }
    if (auto* ok = v.find("ok")) r.ok = ok->as_bool(false);
    if (r.ok) {
        if (auto* l = v.find("latin")) r.latin = l->as_string();
        if (r.latin.empty()) { r.ok = false; r.notfound = true; }
    } else if (auto* nf = v.find("notfound")) {
        r.notfound = nf->as_bool(false);
    }
    if (!r.ok && !r.notfound) {
        if (auto* e = v.find("error")) r.error = e->as_string();
        if (r.error.empty()) r.error = "no answer";
    }
    return r;
}

NameLookupResult lookup_artist_name(const fs::path& script, const std::string& name) {
    NameLookupResult r;
    std::error_code ec;
    if (!fs::exists(script, ec)) { r.script_missing = true; r.error = "script not found: " + path_utf8(script); return r; }
    Args cmd = python_command();
    if (cmd.empty()) { r.python_missing = true; r.error = "python not found (Python 3 is required)"; return r; }
    cmd.push_back(path_utf8(script));
    cmd.push_back(name);
    auto child = spawn_capture(cmd, /*merge_stderr=*/false);
    if (!child) { r.error = "could not start " + command_text(cmd); return r; }
    std::string buffer;
    char chunk[2048];
    for (;;) {
        const std::ptrdiff_t n = child->read(chunk, sizeof chunk);
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));
    }
    child->wait();
    std::istringstream in(buffer);
    std::string line, last;
    while (std::getline(in, line)) { while (!line.empty() && line.back() == '\r') line.pop_back(); if (!line.empty() && line[0] == '{') last = line; }
    if (last.empty()) { r.error = "no answer from the script"; return r; }
    return parse_name_lookup_line(last);
}

KanjiReadResult read_kanji(const fs::path& script, const std::vector<std::string>& texts) {
    KanjiReadResult r;
    std::error_code ec;
    if (!fs::exists(script, ec)) { r.missing = true; r.error = "script not found: " + path_utf8(script); return r; }
    Args cmd = python_command();
    if (cmd.empty()) { r.missing = true; r.error = "python not found (Python 3 is required)"; return r; }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path req = fs::temp_directory_path(ec) / path_from_utf8("mousiki_kanji_" + std::to_string(stamp) + ".txt");
    {
        std::ofstream f(req, std::ios::binary | std::ios::trunc);
        if (!f) { r.error = "could not write a temporary file"; return r; }
        for (const std::string& t : texts) f << t << "\n";
    }
    cmd.push_back(path_utf8(script));
    cmd.push_back(path_utf8(req));
    auto child = spawn_capture(cmd, /*merge_stderr=*/false);
    if (!child) { fs::remove(req, ec); r.error = "could not start " + command_text(cmd); return r; }
    std::string buffer;
    char chunk[4096];
    for (;;) {
        const std::ptrdiff_t n = child->read(chunk, sizeof chunk);
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));
    }
    child->wait();
    fs::remove(req, ec);
    std::istringstream in(buffer);
    std::string line, last;
    while (std::getline(in, line)) { while (!line.empty() && line.back() == '\r') line.pop_back(); if (!line.empty() && line[0] == '{') last = line; }
    tinyjson::Value v;
    if (last.empty() || !tinyjson::parse(last, v) || v.type != tinyjson::Type::Object) { r.error = "no answer from the script"; return r; }
    if (auto* ok = v.find("ok")) r.ok = ok->as_bool(false);
    if (!r.ok) {
        if (auto* m = v.find("missing")) r.missing = m->as_bool(false);
        if (auto* e = v.find("error")) r.error = e->as_string();
        if (r.error.empty()) r.error = "no answer";
        return r;
    }
    if (auto* rd = v.find("readings")) {
        if (rd->type == tinyjson::Type::Object) for (const auto& kv : rd->obj) r.readings.emplace_back(kv.first, kv.second.as_string());
    }
    return r;
}

}  // namespace muisc
