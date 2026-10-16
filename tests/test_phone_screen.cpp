// The remote app's phone screen of the radio: exactly the asked size, every tap zone inside it, the main commands present.
#include "test_framework.h"
#include "radio_ui.h"
#include "terminal_ui.h"

#include <set>

using namespace muisc;
using namespace muisc::radio;

namespace {
int visible_width(const std::string& s) {
    std::string plain;
    for (size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '\x1b' && k + 1 < s.size() && s[k + 1] == '[') {
            k += 2;
            while (k < s.size() && !(s[k] >= 0x40 && s[k] <= 0x7e)) ++k;
            continue;
        }
        plain += s[k];
    }
    return display_width(plain);
}
}

TEST(phone_screen_radio_fits_and_has_the_commands) {
    mt::TempHome home;
    for (const auto& [W, H] : std::vector<std::pair<int, int>>{{44, 46}, {44, 42}, {60, 50}, {44, 30}}) {
        UiModel m;
        RadioStatus st;
        RadioSettings cfg;
        std::vector<remote::net::Zone> zones;
        const auto lines = build_phone_screen(m, st, cfg, W, H, zones);
        CHECK_EQ(static_cast<int>(lines.size()), H);
        for (const auto& l : lines) CHECK_EQ(visible_width(l), W);
        std::set<std::string> cmds;
        for (const auto& z : zones) {
            CHECK(z.x >= 0 && z.y >= 0 && z.w > 0 && z.h > 0 && z.x + z.w <= W && z.y + z.h <= H);
            cmds.insert(z.cmd);
        }
        if (H >= 42)
            for (const char* c : {"prev", "next", "play-pause", "shuffle", "volume-up", "volume-down", "toggle-mute", "live",
                                  "playback-mode", "remote-view", "switch-mode", "@manual", "@menu"})
                CHECK(cmds.count(c) == 1);
    }
}

// The STATIONS list of the phone (the box of the main screen opens it): every station on some page, a tap zone `tune <n>`
// per station, the paging boxes, and still exactly the asked size.
TEST(phone_screen_radio_station_list_pages_and_tunes) {
    mt::TempHome home;
    std::vector<Station> stations(25);
    for (size_t i = 0; i < stations.size(); ++i) { stations[i].name = "Station " + std::to_string(i + 1); stations[i].genre = "jazz"; }
    const int W = 44, H = 46;
    const int per = remote::net::phone_list_rows(H);
    const int pages = (25 + per - 1) / per;
    std::set<std::string> tuned;
    for (int page = 0; page < pages; ++page) {
        UiModel m;
        m.stations = &stations;
        m.phone_view = 1;
        m.phone_page = page;
        RadioStatus st;
        RadioSettings cfg;
        std::vector<remote::net::Zone> zones;
        const auto lines = build_phone_screen(m, st, cfg, W, H, zones);
        CHECK_EQ(static_cast<int>(lines.size()), H);
        for (const auto& l : lines) CHECK_EQ(visible_width(l), W);
        std::set<std::string> cmds;
        for (const auto& z : zones) {
            CHECK(z.x >= 0 && z.y >= 0 && z.x + z.w <= W && z.y + z.h <= H);
            cmds.insert(z.cmd);
            if (z.cmd == "tune") tuned.insert(z.arg);
        }
        for (const char* c : {"remote-scroll", "remote-view"}) CHECK(cmds.count(c) == 1);
    }
    CHECK_EQ(static_cast<int>(tuned.size()), 25);
    CHECK(tuned.count("1") == 1 && tuned.count("25") == 1);
}
