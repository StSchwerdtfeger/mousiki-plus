// Mousiki's own terminal font (src/terminal_font.h): the profile fragment, the iTerm2 profile.
#include "test_framework.h"
#include "terminal_font.h"
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

TEST(terminal_font_fragment_carries_the_retrowave_scheme) {
    const std::string with = terminal_font::wt_fragment_json("Cascadia Mono", "\"C:\\m.exe\"", "");
    tinyjson::Value v;
    CHECK(tinyjson::parse(with, v));
    CHECK(with.find("\"colorScheme\": \"Mousiki Retrowave\"") != std::string::npos);
    CHECK(with.find("\"green\": \"#929292\"") != std::string::npos);
    CHECK(with.find("\"background\": \"#000000\"") != std::string::npos);
    CHECK(terminal_font::wt_fragment_json("x", "x", "", false).find("colorScheme") == std::string::npos);
}

TEST(terminal_font_iterm_profile_inherits_the_users_profile) {
    const std::string j = terminal_font::iterm_profile_json("CascadiaMono-Regular", 13, "My Profile");
    tinyjson::Value v;
    CHECK(tinyjson::parse(j, v));
    CHECK(j.find("\"Dynamic Profile Parent Name\": \"My Profile\"") != std::string::npos);
    CHECK(j.find("\"Normal Font\": \"CascadiaMono-Regular 13\"") != std::string::npos);
    CHECK_EQ(terminal_font::guess_postscript_name("Cascadia Mono"), std::string("CascadiaMono-Regular"));
}

TEST(windows_terminal_fragment_profile_guid_matches_the_documented_example) {
    // the example of Microsoft's "JSON fragment extensions" page: fragment "Git", profile "Git Bash"
    CHECK_EQ(terminal_font::wt_fragment_profile_guid("Git", "Git Bash"), std::string("{2ece5bfe-50ed-5f3a-ab87-5cd4baafed2b}"));
}

TEST(terminal_font_settings_default_to_cascadia_and_round_trip) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK(!s.terminal_default_font);
    CHECK_EQ(s.terminal_font, std::string("Cascadia Mono"));
    CHECK(set_config_value("TerminalDefaultFont", "true"));
    CHECK(set_config_value("TerminalFont", "Menlo"));
    s = load_settings();
    CHECK(s.terminal_default_font);
    CHECK_EQ(s.terminal_font, std::string("Menlo"));
    save_settings(s);
    CHECK_EQ(load_settings().terminal_font, std::string("Menlo"));
}
