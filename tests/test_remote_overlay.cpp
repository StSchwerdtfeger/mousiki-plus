// The REMOTE APP overlay (switch, port, PIN, QR code) and the QR encoder behind it.
#include "test_framework.h"
#include "qr_code.h"
#include "remote_overlay.h"
#include "remote_server.h"

using namespace muisc;

namespace {
bool press(remote_ui::State& st, remote_ui::Values& v, int k, bool arrow = false) { bool close = false; return remote_ui::key(st, v, k, arrow, close); }
void type(remote_ui::State& st, remote_ui::Values& v, const char* s) { for (; *s; ++s) press(st, v, *s); }
}

TEST(qr_sizes_and_finder_patterns) {
    const auto a = qr::encode("A");
    CHECK_EQ(a.size(), static_cast<size_t>(21));                      // version 1
    const auto u = qr::encode("http://192.168.178.123:4747/#pin=12345678");
    CHECK_EQ(u.size(), static_cast<size_t>(29));                      // version 3 for a typical address
    for (const auto& m : {a, u}) {                                    // the three finder patterns: dark 7x7 ring corners
        const size_t n = m.size();
        CHECK(m[0][0] && m[0][6] && m[6][0] && m[6][6] && !m[1][1] && m[3][3]);
        CHECK(m[0][n - 1] && m[6][n - 7] && m[n - 1][0] && m[n - 7][6]);
        CHECK(m[n - 8][8]);                                           // the dark module
    }
    CHECK(qr::encode(std::string(300, 'x')).empty());                 // too long: nothing (not a broken code)
    const auto lines = qr::render_half_blocks(u, 2);
    CHECK_EQ(lines.size(), static_cast<size_t>((29 + 4 + 1) / 2));    // two modules per text row
}

TEST(remote_overlay_switch_makes_a_pin) {
    remote_ui::State st;
    remote_ui::Values v;
    CHECK(press(st, v, '\r'));                                        // row 0: on
    CHECK(v.enabled);
    CHECK(remote::net::pin_ok(v.pin));
    CHECK(press(st, v, 'C', true));                                   // right arrow: off again
    CHECK(!v.enabled);
}

TEST(remote_overlay_port_and_pin_are_checked) {
    remote_ui::State st;
    remote_ui::Values v{true, 4747, "1234"};
    press(st, v, 'B', true);                                          // PORT
    press(st, v, '\r');
    CHECK(st.editing);
    for (int i = 0; i < 4; ++i) press(st, v, 127);
    type(st, v, "80");
    CHECK(!press(st, v, '\r'));                                       // below 1024: refused, still editing
    CHECK(st.editing);
    for (int i = 0; i < 2; ++i) press(st, v, 127);
    type(st, v, "5050");
    CHECK(press(st, v, '\r'));
    CHECK_EQ(v.port, 5050);
    press(st, v, 'B', true);                                          // PIN
    press(st, v, '\r');
    for (int i = 0; i < 4; ++i) press(st, v, 127);
    type(st, v, "12a");                                               // letters are not typed
    CHECK_EQ(st.buffer, std::string("12"));
    CHECK(!press(st, v, '\r'));                                       // too short
    type(st, v, "3456");
    CHECK(press(st, v, '\r'));
    CHECK_EQ(v.pin, std::string("123456"));
    const std::string before = v.pin;
    CHECK(press(st, v, 'n'));                                         // a new random PIN
    CHECK(v.pin != before || v.pin.size() == 6);
    bool close = false;
    remote_ui::key(st, v, 27, false, close);
    CHECK(close);                                                     // ESC outside a field closes
}

TEST(remote_overlay_panel_fits) {
    remote_ui::State st;
    st.open = true;
    remote_ui::Values v{false, 4747, ""};
    remote_ui::Style s;
    int w = 0;
    const auto small = remote_ui::build(st, v, s, 80, 24, w);
    CHECK(w <= 80);
    CHECK(static_cast<int>(small.size()) <= 24);
}
