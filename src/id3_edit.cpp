#include "id3_edit.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "path_utf8.h"

namespace muisc {
namespace fs = std::filesystem;

namespace {

struct Frame {
    std::string id;       // 4 characters
    char flags[2] = {0, 0};
    std::string payload;  // as stored
};

bool valid_id(const std::string& id) {
    if (id.size() != 4) return false;
    for (char c : id) if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

uint32_t syncsafe(const unsigned char* p) { return (uint32_t(p[0]) << 21) | (uint32_t(p[1]) << 14) | (uint32_t(p[2]) << 7) | uint32_t(p[3]); }
void put_syncsafe(std::string& s, uint32_t v) {
    s += static_cast<char>((v >> 21) & 0x7F); s += static_cast<char>((v >> 14) & 0x7F); s += static_cast<char>((v >> 7) & 0x7F); s += static_cast<char>(v & 0x7F);
}
void put_be32(std::string& s, uint32_t v) {
    s += static_cast<char>((v >> 24) & 0xFF); s += static_cast<char>((v >> 16) & 0xFF); s += static_cast<char>((v >> 8) & 0xFF); s += static_cast<char>(v & 0xFF);
}

// UTF-8 -> UTF-16LE code units (a malformed byte becomes U+FFFD)
std::string utf8_to_utf16le(const std::string& s) {
    std::string out;
    auto unit = [&](uint32_t u) { out += static_cast<char>(u & 0xFF); out += static_cast<char>((u >> 8) & 0xFF); };
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t n = 1;
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 0x6 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu); n = 2; }
        else if ((c >> 4) == 0xE && i + 2 < s.size()) { cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu); n = 3; }
        else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) | ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            n = 4;
        }
        i += n;
        if (cp >= 0x10000) { cp -= 0x10000; unit(0xD800 + (cp >> 10)); unit(0xDC00 + (cp & 0x3FF)); }
        else unit(cp);
    }
    return out;
}

// the payload of a text frame: ASCII as ISO-8859-1; else UTF-16 with BOM (v2.3) or UTF-8 (v2.4)
std::string text_payload(const std::string& value, int version) {
    bool ascii = true;
    for (char c : value) if (static_cast<unsigned char>(c) >= 0x80 || c == 0) { ascii = false; break; }
    std::string p;
    if (ascii) { p += '\x00'; p += value; return p; }
    if (version >= 4) { p += '\x03'; p += value; return p; }
    p += '\x01';
    p += "\xFF\xFE";
    p += utf8_to_utf16le(value);
    p += std::string("\0\0", 2);
    return p;
}

bool read_exact(std::ifstream& in, std::string& dst, size_t n) {
    dst.resize(n);
    if (n == 0) return true;
    in.read(&dst[0], static_cast<std::streamsize>(n));
    return static_cast<size_t>(in.gcount()) == n;
}

} // namespace

Id3EditResult id3_edit_text_frames(const fs::path& file, const std::vector<std::pair<std::string, std::string>>& edits, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return Id3EditResult::Failed; };
    std::error_code ec;
    const uintmax_t file_size = fs::file_size(file, ec);
    if (ec) return fail("cannot read the file: " + ec.message());

    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return fail("cannot open the file");
    unsigned char h[10] = {};
    in.read(reinterpret_cast<char*>(h), 10);
    const bool has_tag = in.gcount() == 10 && std::memcmp(h, "ID3", 3) == 0;

    int version = 3;
    uint64_t audio_start = 0;
    std::vector<Frame> frames;
    if (has_tag) {
        version = h[3];
        if (version != 3 && version != 4) return Id3EditResult::NotApplicable;                 // v2.2 and anything newer
        if (h[5] & 0xF0) return Id3EditResult::NotApplicable;                                  // unsynchronised, extended header, experimental, footer
        if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return Id3EditResult::NotApplicable;
        const uint32_t tag_size = syncsafe(h + 6);
        if (tag_size > (64u << 20) || 10 + static_cast<uint64_t>(tag_size) > file_size) return Id3EditResult::NotApplicable;
        std::string body;
        if (!read_exact(in, body, tag_size)) return Id3EditResult::NotApplicable;
        audio_start = 10 + static_cast<uint64_t>(tag_size);
        size_t off = 0;
        while (off + 10 <= body.size()) {
            if (body[off] == 0) break;                                                         // padding
            Frame f;
            f.id = body.substr(off, 4);
            if (!valid_id(f.id)) return Id3EditResult::NotApplicable;
            const unsigned char* sz = reinterpret_cast<const unsigned char*>(body.data()) + off + 4;
            uint32_t n;
            if (version == 4) {
                if ((sz[0] | sz[1] | sz[2] | sz[3]) & 0x80) return Id3EditResult::NotApplicable;
                n = syncsafe(sz);
            } else {
                n = (uint32_t(sz[0]) << 24) | (uint32_t(sz[1]) << 16) | (uint32_t(sz[2]) << 8) | uint32_t(sz[3]);
            }
            f.flags[0] = body[off + 8];
            f.flags[1] = body[off + 9];
            off += 10;
            if (n > body.size() - off) return Id3EditResult::NotApplicable;
            f.payload = body.substr(off, n);
            off += n;
            frames.push_back(std::move(f));
        }
    }
    in.close();

    // the edits
    for (const auto& [key, value] : edits) {
        const std::string id = key == "YEAR" ? (version >= 4 ? "TDRC" : "TYER") : key;
        if (!valid_id(id)) return fail("not a frame name: " + key);
        for (size_t i = frames.size(); i-- > 0;) if (frames[i].id == id) frames.erase(frames.begin() + static_cast<long>(i));
        if (value.empty()) continue;
        Frame f;
        f.id = id;
        f.payload = text_payload(value, version);
        frames.push_back(std::move(f));
    }

    std::string tag;
    for (const Frame& f : frames) {
        tag += f.id;
        if (version == 4) put_syncsafe(tag, static_cast<uint32_t>(f.payload.size())); else put_be32(tag, static_cast<uint32_t>(f.payload.size()));
        tag += f.flags[0]; tag += f.flags[1];
        tag += f.payload;
    }
    tag.append(512, '\0');                                                                     // room for the next edit
    std::string head = "ID3";
    head += static_cast<char>(version); head += '\0'; head += '\0';
    put_syncsafe(head, static_cast<uint32_t>(tag.size()));

    const fs::path tmp = file.parent_path() / path_from_utf8(path_utf8(file.stem()) + ".mousiki-tmp" + path_utf8(file.extension()));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return fail("cannot write next to the file");
        out.write(head.data(), static_cast<std::streamsize>(head.size()));
        out.write(tag.data(), static_cast<std::streamsize>(tag.size()));
        std::ifstream audio(file, std::ios::binary);
        if (!audio.is_open()) { out.close(); fs::remove(tmp, ec); return fail("cannot read the file"); }
        audio.seekg(static_cast<std::streamoff>(audio_start));
        std::string buf(1 << 20, '\0');
        while (audio) {
            audio.read(&buf[0], static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = audio.gcount();
            if (got > 0) out.write(buf.data(), got);
        }
        out.flush();
        if (!out.good()) { out.close(); fs::remove(tmp, ec); return fail("writing failed (disk full?): the file is unchanged"); }
    }
    const uintmax_t want = head.size() + tag.size() + (file_size - audio_start);
    const uintmax_t got = fs::file_size(tmp, ec);
    if (ec || got != want) { fs::remove(tmp, ec); return fail("the new file is not complete: the file is unchanged"); }

    const fs::file_status orig = fs::status(file, ec);
    if (!ec) fs::permissions(tmp, orig.permissions(), ec);                                     // (the mode of the original stays)
    fs::rename(tmp, file, ec);
    if (ec) { std::error_code ig; fs::remove(tmp, ig); return fail("could not replace the file: " + ec.message() + " (is it playing right now?)"); }
    return Id3EditResult::Done;
}

} // namespace muisc
