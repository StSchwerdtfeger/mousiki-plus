#include "chip_meta.h"
#include "chiptune.h"
#include "path_utf8.h"
#include "tiny_json.h"
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace muisc {
namespace chipmeta {

using namespace tinyjson;

namespace {

std::string lower_ext(const fs::path& p) {
    std::string e = path_utf8(p.extension());
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// UTF-8 -> Latin-1 (the header encoding of these formats); what does not fit becomes '?'. At most `len` bytes.
std::string to_latin1(const std::string& title, size_t len) {
    std::string bytes;
    for (size_t i = 0; i < title.size() && bytes.size() < len;) {
        const unsigned char c = static_cast<unsigned char>(title[i]);
        unsigned cp = c;
        size_t n = 1;
        if (c >= 0xF0) { n = 4; cp = c & 0x07; } else if (c >= 0xE0) { n = 3; cp = c & 0x0F; } else if (c >= 0xC0) { n = 2; cp = c & 0x1F; }
        for (size_t k = 1; k < n && i + k < title.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(title[i + k]) & 0x3F);
        i += n;
        bytes += (cp < 256 && cp >= 32) ? static_cast<char>(cp) : '?';
    }
    bytes.resize(len, '\0');
    return bytes;
}

// Overwrite `len` bytes at `off` of a file.
bool patch_file(const fs::path& file, long off, const std::string& value, size_t len, std::string& err) {
    std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
    if (!f.is_open()) { err = "could not open the file for writing"; return false; }
    f.seekp(off);
    const std::string bytes = to_latin1(value, len);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) { err = "writing the header failed"; return false; }
    return true;
}

// Tracker module: the song name. MOD 20 bytes at 0, S3M 28 at 0, XM 20 at 17, IT / MPTM 26 at 4.
bool write_tracker_title(const fs::path& file, const std::string& title, std::string& err) {
    std::ifstream in(file, std::ios::binary);
    char head[48] = {};
    in.read(head, sizeof head);
    const std::streamsize got = in.gcount();
    long off = -1;
    size_t len = 0;
    if (got >= 4 && std::string(head, 4) == "IMPM") { off = 4; len = 26; }
    else if (got >= 17 && std::string(head, 17) == "Extended Module: ") { off = 17; len = 20; }
    else {
        in.clear();
        in.seekg(44);
        char m[4] = {};
        in.read(m, 4);
        if (std::string(m, 4) == "SCRM") { off = 0; len = 28; }
        else if (lower_ext(file) == ".mod") { off = 0; len = 20; }
    }
    in.close();
    if (off < 0) { err = "this tracker format has no title field Mousiki can write"; return false; }
    return patch_file(file, off, title, len, err);
}

// SID tune: name at 0x16, author at 0x36 (32 bytes each, Latin-1).
bool write_sid_field(const fs::path& file, int field, const std::string& value, std::string& err) {
    const SidInfo si = read_sid_header(file);
    if (!si.ok) { err = "not a PSID / RSID file"; return false; }
    return patch_file(file, field == static_cast<int>(MetaField::Title) ? 0x16 : 0x36, value, 32, err);
}

// --- side file, cached per folder (the lists ask for every row) ------------------------------------------------
struct Folder {
    fs::file_time_type stamp{};
    bool exists = false;
    std::map<std::string, std::array<std::string, kMetaFieldCount>> files;   // file name -> artist/title/album/year
};
std::mutex g_mutex;
std::map<std::string, Folder> g_cache;

void parse_into(const std::string& text, Folder& f) {
    Value root;
    if (!tinyjson::parse(text, root)) return;
    const Value* files = root.find("files");
    if (!files || files->type != Type::Object) return;
    for (const auto& kv : files->obj) {
        std::array<std::string, kMetaFieldCount> v{};
        for (int i = 1; i < kMetaFieldCount; ++i) {
            const Value* x = kv.second.find(meta_field_name(i));
            if (x) v[i] = x->as_string();
        }
        f.files[kv.first] = v;
    }
}

Folder& load_folder(const fs::path& dir) {   // caller holds g_mutex
    const fs::path sc = dir / kFolderName / kFileName;
    std::error_code ec;
    const bool ex = fs::exists(sc, ec);
    Folder& f = g_cache[path_utf8(dir)];
    fs::file_time_type stamp{};
    if (ex) stamp = fs::last_write_time(sc, ec);
    if (f.exists == ex && f.stamp == stamp && (ex || f.files.empty())) return f;
    f = Folder{};
    f.exists = ex;
    f.stamp = stamp;
    if (ex) {
        std::ifstream in(sc, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        parse_into(ss.str(), f);
    }
    return f;
}

bool write_folder(const fs::path& dir, const Folder& f, std::string& err) {
    Value files = Value::make_obj();
    for (const auto& kv : f.files) {
        Value o = Value::make_obj();
        bool any = false;
        for (int i = 1; i < kMetaFieldCount; ++i)
            if (!kv.second[i].empty()) { o.set(meta_field_name(i), Value::make_str(kv.second[i])); any = true; }
        if (any) files.set(kv.first, std::move(o));
    }
    std::error_code ec;
    const fs::path folder = dir / kFolderName;
    const fs::path sc = folder / kFileName;
    if (files.obj.empty()) {                       // nothing left: remove the file, and the folder if it is empty
        fs::remove(sc, ec);
        fs::remove(folder, ec);
        return true;
    }
    fs::create_directories(folder, ec);
    Value root = Value::make_obj();
    root.set("note", Value::make_str("Tags that tracker modules and chiptunes cannot store themselves, written by Mousiki Plus"));
    root.set("files", std::move(files));
    const fs::path tmp = folder / "metadata.json.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << tinyjson::write(root);
        if (!out) { err = "could not write " + path_utf8(sc); return false; }
    }
    fs::rename(tmp, sc, ec);
    if (ec) { err = "could not write " + path_utf8(sc) + ": " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

} // namespace

bool applies(const fs::path& media) { return is_chiptune_file(media); }

bool stored_in_file(const fs::path& media, int field) {
    if (is_tracker_file(media)) return field == static_cast<int>(MetaField::Title);
    if (is_sid_file(media)) return field == static_cast<int>(MetaField::Title) || field == static_cast<int>(MetaField::Artist);
    return false;
}

fs::path sidecar_path(const fs::path& media) { return media.parent_path() / kFolderName / kFileName; }

void overlay(const fs::path& media, std::string& artist, std::string& title, std::string& album, std::string& year) {
    if (!applies(media)) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    const Folder& f = load_folder(media.parent_path());
    auto it = f.files.find(path_utf8(media.filename()));
    if (it == f.files.end()) return;
    const auto& v = it->second;
    if (!v[static_cast<int>(MetaField::Artist)].empty()) artist = v[static_cast<int>(MetaField::Artist)];
    if (!v[static_cast<int>(MetaField::Title)].empty()) title = v[static_cast<int>(MetaField::Title)];
    if (!v[static_cast<int>(MetaField::Album)].empty()) album = v[static_cast<int>(MetaField::Album)];
    if (!v[static_cast<int>(MetaField::Year)].empty()) year = v[static_cast<int>(MetaField::Year)];
}

bool save_tags(const fs::path& media, const MetaEditEntry& e, std::string* err) {
    std::string msg;
    std::array<bool, kMetaFieldCount> to_side{};
    for (int i = 1; i < kMetaFieldCount; ++i) {
        if (!e.edited[i]) continue;
        if (stored_in_file(media, i)) {
            bool ok = true;
            if (is_tracker_file(media)) ok = write_tracker_title(media, e.value[i], msg);
            else ok = write_sid_field(media, i, e.value[i], msg);
            if (!ok) { if (err) *err = msg; return false; }
            to_side[i] = true;   // a value kept in the side file from before would hide the new one: clear it
        } else {
            to_side[i] = true;
        }
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    const fs::path dir = media.parent_path();
    Folder f = load_folder(dir);
    auto& v = f.files[path_utf8(media.filename())];
    for (int i = 1; i < kMetaFieldCount; ++i) {
        if (!to_side[i]) continue;
        v[i] = stored_in_file(media, i) ? std::string() : e.value[i];
    }
    if (!write_folder(dir, f, msg)) { if (err) *err = msg; return false; }
    g_cache.erase(path_utf8(dir));   // re-read with the new time stamp
    return true;
}

void rename_entry(const fs::path& folder, const std::string& old_name, const std::string& new_name) {
    std::lock_guard<std::mutex> lk(g_mutex);
    Folder f = load_folder(folder);
    auto it = f.files.find(old_name);
    if (it == f.files.end()) return;
    f.files[new_name] = it->second;
    f.files.erase(old_name);
    std::string err;
    write_folder(folder, f, err);
    g_cache.erase(path_utf8(folder));
}

} // namespace chipmeta
} // namespace muisc
