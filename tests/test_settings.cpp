// The music player's settings: defaults, the config.txt round trip, the shared oscilloscope rows, the hotkey table.
#include "test_framework.h"
#include "settings.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>

using namespace muisc;

TEST(settings_defaults_per_oscilloscope_style) {
    mt::TempHome home;
    const Settings s = load_settings();
    // braille: the classic beam with rough mode; image: the CRT beam with dwell brightness, rough mode off
    CHECK(!s.osci_set[0].crt);
    CHECK(s.osci_set[0].rough);
    CHECK(s.osci_set[1].crt);
    CHECK(s.osci_set[1].dwell);
    CHECK(!s.osci_set[1].rough);
    CHECK_EQ(s.osci_set[0].trace, 1024);
    CHECK(s.black_background);
    CHECK(!s.share_profiles);
}

TEST(settings_save_load_is_stable) {
    mt::TempHome home;
    Settings s = load_settings();
    s.osci_set[1].decay = 0.10f;
    s.osci_set[1].trace = 8192;
    s.osci_set[0].rough = false;
    s.osci_style = 1;
    s.osci_mode = 3;
    s.image_detail = 1;
    s.share_profiles = true;
    save_settings(s);
    const Settings back = load_settings();
    CHECK_NEAR(back.osci_set[1].decay, 0.10, 0.001);
    CHECK_EQ(back.osci_set[1].trace, 8192);
    CHECK(!back.osci_set[0].rough);
    CHECK_EQ(back.osci_style, 1);
    CHECK_EQ(back.osci_mode, 3);
    CHECK_EQ(back.image_detail, 1);
    CHECK(back.share_profiles);
    // a second round trip changes nothing (the first one may normalise, e.g. the lyrics flag; HKey lines come in map order)
    save_settings(back);
    const Settings again = load_settings();
    auto sorted_lines = [](const std::string& text) {
        std::vector<std::string> v; std::istringstream in(text); std::string l;
        while (std::getline(in, l)) v.push_back(l);
        std::sort(v.begin(), v.end());
        return v;
    };
    CHECK(sorted_lines(settings_to_text(again)) == sorted_lines(settings_to_text(back)));
}

TEST(settings_config_keys_are_case_insensitive_and_legacy_frame_rate_sets_both_styles) {
    VizPrefs v;
    CHECK(osci_config_key(v, "OsciDecay", "0.55"));
    CHECK(osci_config_key(v, "oscidecay", "0.60"));
    CHECK_NEAR(v.osci_set[0].decay, 0.60, 0.001);
    CHECK(osci_config_key(v, "FrameRate", "60"));               // up to v3.0.0: one value for both styles
    CHECK_EQ(v.osci_set[0].frame_rate, 60);
    CHECK_EQ(v.osci_set[1].frame_rate, 60);
    CHECK(osci_config_key(v, "OsciImageFrameRate", "120"));     // the per-style key that follows wins
    CHECK_EQ(v.osci_set[0].frame_rate, 60);
    CHECK_EQ(v.osci_set[1].frame_rate, 120);
    CHECK(!osci_config_key(v, "SomethingElse", "1"));
}

TEST(settings_osci_rows_step_and_clamp) {
    VizPrefs v;
    for (int i = 0; i < 200; ++i) osci_adjust(v, kOrDecay, +1);
    CHECK_NEAR(v.osci().decay, 0.99, 0.0001);                   // never above 0.99
    for (int i = 0; i < 200; ++i) osci_adjust(v, kOrDecay, -1);
    CHECK_NEAR(v.osci().decay, 0.0, 0.0001);
    for (int i = 0; i < 40; ++i) osci_adjust(v, kOrTrace, +1);
    CHECK_EQ(v.osci().trace, 8192);                             // 128 .. 1024 in steps, then 2048, 4096, 8192
    for (int i = 0; i < 40; ++i) osci_adjust(v, kOrTrace, -1);
    CHECK_EQ(v.osci().trace, 128);
    osci_adjust(v, kOrStyle, +1);
    CHECK_EQ(v.osci_style, 1);
    osci_reset(v);                                               // the style in use goes back to its defaults
    CHECK(v.osci().crt);
    CHECK(!v.osci().rough);
    CHECK(osci_visible_rows(v).size() > 8);
}

TEST(settings_osci_row_labels_and_values_exist) {
    VizPrefs v;
    for (int r = 0; r < kOsciRowCount; ++r) {
        if (r == kOrDisplay) continue;                           // the caller's row
        CHECK(!osci_row_label(r).empty());
        CHECK(!osci_row_value(v, r).empty());
    }
}

TEST(settings_default_hotkeys_do_not_collide) {
    Settings s;
    apply_default_hotkeys(s);
    std::map<std::string, std::string> owner;
    for (const auto& [action, key] : s.hotkeys) {
        if (key.empty()) continue;
        const auto [it, fresh] = owner.emplace(key, action);
        if (!fresh) ::mt::fail(__FILE__, __LINE__, "key '" + key + "' is bound to both " + it->second + " and " + action);
    }
    CHECK(owner.size() > 40);
}

TEST(settings_take_shared_keeps_the_grid_colour) {
    VizPrefs a, b;
    a.scope_grid_color = "99";
    b.scope_grid_color = "11";
    b.osci_set[1].decay = 0.33f;
    b.osci_style = 1;
    b.spectro.span = 45;
    a.take_shared_from(b);
    CHECK_EQ(a.scope_grid_color, std::string("99"));            // the colour belongs to the theme
    CHECK_NEAR(a.osci_set[1].decay, 0.33, 0.001);
    CHECK_EQ(a.osci_style, 1);
    CHECK_EQ(a.spectro.span, 45);
}

TEST(settings_update_check_and_media_keys_default_on_and_round_trip) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK(s.update_check);
    CHECK(s.media_keys);
    CHECK(read_update_check());   // no config.txt line yet: on
    CHECK(read_media_keys());
    s.update_check = false;
    s.media_keys = false;
    save_settings(s);
    const Settings back = load_settings();
    CHECK(!back.update_check);
    CHECK(!back.media_keys);
    CHECK(!read_update_check());
    CHECK(!read_media_keys());
}
