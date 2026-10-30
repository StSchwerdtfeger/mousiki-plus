// Mousiki's own terminal font (src/terminal_font.h) and the FONT overlay (src/font_picker.h).
#include "test_framework.h"
#include "terminal_font.h"
#include "font_picker.h"
#include "settings.h"
#include "terminal_ui.h"
#include "tiny_json.h"

using namespace muisc;

TEST(terminal_font_fc_list_is_parsed_and_deduplicated) {
    const auto v = terminal_font::parse_fc_list("DejaVu Sans Mono,DejaVu Sans Mono Book\nCascadia Mono\n\nDejaVu Sans Mono\nNoto Mono\n");
    CHECK_EQ(v.size(), static_cast<size_t>(3));
    CHECK_EQ(v[0], std::string("Cascadia Mono"));
    CHECK_EQ(v[1], std::string("DejaVu Sans Mono"));
}

TEST(terminal_font_windows_terminal_profile_is_valid_json) {
    const std::string j = terminal_font::wt_fragment_json("Cascadia Mono", "\"C:\\Program Files\\Mousiki\\mousiki.cmd\"", "C:\\x\\mousiki.ico");
    tinyjson::Value v;
    CHECK(tinyjson::parse(j, v));
    CHECK(j.find("\"name\": \"Mousiki\"") != std::string::npos);
    CHECK(j.find("\"face\": \"Cascadia Mono\"") != std::string::npos);
    // Terminal Default: the profile has no font of its own
    const std::string none = terminal_font::wt_fragment_json(terminal_font::kTerminalDefault, "mousiki", "");
    CHECK(tinyjson::parse(none, v));
    CHECK(none.find("\"font\"") == std::string::npos);
}

TEST(terminal_font_iterm_profile_inherits_the_users_profile) {
    const std::string j = terminal_font::iterm_profile_json("CascadiaMono-Regular", 13, "My Profile");
    tinyjson::Value v;
    CHECK(tinyjson::parse(j, v));
    CHECK(j.find("\"Dynamic Profile Parent Name\": \"My Profile\"") != std::string::npos);
    CHECK(j.find("\"Normal Font\": \"CascadiaMono-Regular 13\"") != std::string::npos);
    CHECK_EQ(terminal_font::guess_postscript_name("Cascadia Mono"), std::string("CascadiaMono-Regular"));
}

TEST(font_picker_lists_default_rows_first_and_picks) {
    font_ui::State st;
    font_ui::set_items(st, "Menlo", {"Andale Mono", "Cascadia Mono", "Menlo"});
    CHECK_EQ(st.items.size(), static_cast<size_t>(4));          // Terminal Default, Cascadia Mono, Andale Mono, Menlo
    CHECK_EQ(st.items[0], std::string(terminal_font::kTerminalDefault));
    CHECK_EQ(st.items[1], std::string(terminal_font::kDefaultFont));
    CHECK_EQ(st.items[static_cast<size_t>(st.sel)], std::string("Menlo"));   // opens on the font in use
    st.open = true;
    std::string picked;
    bool close = false;
    font_ui::key(st, 'a', false, false, 10, picked, close);    // a letter jumps to the next font starting with it
    CHECK_EQ(st.items[static_cast<size_t>(st.sel)], std::string("Andale Mono"));
    CHECK(font_ui::key(st, '\r', false, false, 10, picked, close));
    CHECK_EQ(picked, std::string("Andale Mono"));
    CHECK(!st.open);
    // a font that is set but not installed stays in the list
    font_ui::set_items(st, "Gone Mono", {"Menlo"});
    CHECK_EQ(st.items[static_cast<size_t>(st.sel)], std::string("Gone Mono"));
}

TEST(font_picker_panel_has_a_frame_of_equal_width) {
    font_ui::State st;
    font_ui::set_items(st, terminal_font::kDefaultFont, {"A Mono", "B Mono", "C Mono"});
    remote_ui::Style s;
    int w = 0;
    const auto lines = font_ui::build(st, s, terminal_font::kDefaultFont, "font: Cascadia Mono for this window", 120, 30, w);
    CHECK(lines.size() > 5);
    for (const std::string& l : lines) {
        std::string plain;
        for (size_t k = 0; k < l.size(); ++k) {
            if (l[k] == '\x1b' && k + 1 < l.size() && l[k + 1] == '[') { k += 2; while (k < l.size() && !(l[k] >= 0x40 && l[k] <= 0x7e)) ++k; continue; }
            plain += l[k];
        }
        CHECK_EQ(display_width(plain), w);
    }
}

TEST(terminal_font_setting_round_trip) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK_EQ(s.terminal_font, std::string("Cascadia Mono"));    // the default
    s.terminal_font = "Menlo";
    save_settings(s);
    CHECK_EQ(load_settings().terminal_font, std::string("Menlo"));
    CHECK(set_config_value("TerminalFont", "Terminal Default"));
    CHECK_EQ(load_settings().terminal_font, std::string("Terminal Default"));
}
