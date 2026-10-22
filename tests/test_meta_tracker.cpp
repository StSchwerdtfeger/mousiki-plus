// Saving the title of a tracker module (apply_meta_entry() on .it / .xm / .s3m / .mod): the song name in the header is
// overwritten in place, other tag fields cannot be stored there.
#include "test_framework.h"
#include "meta_editor.h"
#include "path_utf8.h"

#include <fstream>
#include <sstream>

using namespace muisc;

namespace {
fs::path write_file(const std::string& name, const std::string& bytes) {
    const fs::path p = fs::temp_directory_path() / ("mousiki_mt_" + name);
    std::ofstream(p, std::ios::binary) << bytes;
    return p;
}
std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
MetaEditEntry title_entry(const fs::path& p, const std::string& title) {
    MetaEditEntry e;
    e.path = path_utf8(p);
    e.edited[static_cast<int>(MetaField::Title)] = true;
    e.value[static_cast<int>(MetaField::Title)] = title;
    return e;
}
}

TEST(meta_tracker_it_title_is_written_in_the_header) {
    std::string data = "IMPM" + std::string(26, 'x') + std::string(200, '\x07');
    const fs::path p = write_file("a.it", data);
    std::string err;
    CHECK(apply_meta_entry(title_entry(p, "My Song"), &err, nullptr));
    const std::string out = read_file(p);
    CHECK_EQ(out.size(), data.size());
    CHECK_EQ(out.substr(0, 4), std::string("IMPM"));
    CHECK_EQ(std::string(out.c_str() + 4), std::string("My Song"));
    CHECK_EQ(out.substr(30), data.substr(30));   // everything after the name is untouched
    fs::remove(p);
}

TEST(meta_tracker_xm_and_mod_titles) {
    const fs::path x = write_file("b.xm", "Extended Module: " + std::string(20, ' ') + std::string(100, 'z'));
    std::string err;
    CHECK(apply_meta_entry(title_entry(x, "XM tune"), &err, nullptr));
    CHECK_EQ(std::string(read_file(x).c_str() + 17), std::string("XM tune"));
    fs::remove(x);
    const fs::path m = write_file("c.mod", std::string(20, 'q') + std::string(100, 'z'));
    CHECK(apply_meta_entry(title_entry(m, "A very long title that does not fit"), &err, nullptr));
    CHECK_EQ(read_file(m).substr(0, 20), std::string("A very long title th"));
    fs::remove(m);
}

TEST(meta_tracker_artist_cannot_be_saved) {
    const fs::path p = write_file("d.it", "IMPM" + std::string(26, 'x') + std::string(50, 'y'));
    MetaEditEntry e = title_entry(p, "T");
    e.edited[static_cast<int>(MetaField::Artist)] = true;
    e.value[static_cast<int>(MetaField::Artist)] = "Someone";
    std::string err;
    CHECK(!apply_meta_entry(e, &err, nullptr));
    CHECK(!err.empty());
    CHECK_EQ(read_file(p).substr(4, 26), std::string(26, 'x'));   // nothing was written
    fs::remove(p);
}
