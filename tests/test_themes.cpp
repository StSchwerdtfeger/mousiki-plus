// Colour themes: the built-in set, the user's themes.txt, export / import.
#include "test_framework.h"
#include "themes.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace {
std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
}

TEST(themes_built_in_set) {
    mt::TempHome home;
    const auto all = themes::load_all();
    CHECK(all.size() >= 11);
    CHECK(all[0].name.find("Default") != std::string::npos);   // index 0 is the default theme
    CHECK(all[0].builtin);
    std::set<std::string> names;
    for (const auto& t : all) {
        CHECK(names.insert(lower(t.name)).second);                 // every name is unique (case does not matter)
        CHECK(t.name.size() <= static_cast<size_t>(themes::kMaxNameLen));
        if (!t.builtin) continue;
        CHECK(t.get("ColorBorderTop", false) != nullptr);          // a built-in theme defines the basics
        CHECK(t.get("ColorBorderBottom", false) != nullptr);
        CHECK(t.get("ColorHeader", false) != nullptr);
    }
    for (const char* n : {"Catppuccin Mocha", "Tokyo Night", "Rose Pine", "Everforest", "Kanagawa", "Vaporwave", "Outrun",
                          "Ember", "Glacier", "Lavender Haze"})
        CHECK(themes::find_name(all, n) >= 0);
}

TEST(themes_every_colour_is_a_palette_number) {
    mt::TempHome home;
    for (const auto& t : themes::load_all()) {
        for (const auto& [k, v] : t.kv) {
            if (k.find("Color") == std::string::npos) continue;
            if (v.empty()) continue;
            bool digits = true;
            for (char c : v) digits = digits && c >= '0' && c <= '9';
            CHECK(digits);                                         // "141", not "red"
            if (digits) CHECK(std::stoi(v) >= 0 && std::stoi(v) <= 255);
        }
    }
}

TEST(themes_save_and_delete_user_theme) {
    mt::TempHome home;
    themes::Theme t;
    t.name = "My Test";
    t.set("ColorBorderTop", "21");
    t.set("ColorBorderBottom", "196");
    std::string err;
    CHECK(themes::save_theme(t, err));
    auto all = themes::load_all();
    const int at = themes::find_name(all, "my test");              // names compare case-insensitively
    CHECK(at >= 0);
    if (at >= 0) {
        CHECK(!all[static_cast<size_t>(at)].builtin);
        CHECK(all[static_cast<size_t>(at)].get("ColorBorderTop", false) && *all[static_cast<size_t>(at)].get("ColorBorderTop", false) == "21");
    }
    CHECK(themes::delete_theme("My Test", err));
    CHECK(themes::find_name(themes::load_all(), "My Test") < 0);
}

TEST(themes_built_in_cannot_be_overwritten_or_deleted) {
    mt::TempHome home;
    themes::Theme t;
    t.name = "Tokyo Night";
    t.set("ColorBorderTop", "1");
    std::string err;
    CHECK(!themes::save_theme(t, err));
    CHECK(!err.empty());
    err.clear();
    CHECK(!themes::delete_theme("Tokyo Night", err));
}

TEST(themes_export_import_roundtrip) {
    mt::TempHome home;
    themes::Theme t;
    t.name = "Round Trip";
    t.set("ColorBorderTop", "33");
    t.set("ColorHeader", "220");
    const auto file = home.path() / "rt.theme";
    std::string err;
    CHECK(themes::write_theme_file(file, t, err));
    const auto back = themes::read_theme_file(file);
    CHECK_EQ(back.size(), static_cast<size_t>(1));
    if (back.size() == 1) {
        CHECK_EQ(back[0].name, std::string("Round Trip"));
        CHECK(back[0].get("ColorHeader", false) && *back[0].get("ColorHeader", false) == "220");
    }
    // importing a name that already exists gets a free one
    std::string e2;
    const std::string first = themes::import_theme(back[0], e2);
    const std::string second = themes::import_theme(back[0], e2);
    CHECK_EQ(first, std::string("Round Trip"));
    CHECK_EQ(second, std::string("Round Trip (2)"));
}

TEST(themes_names_are_sanitized) {
    const std::string s = themes::sanitize_name("  a very long theme name that goes on and on and on  ");
    CHECK(s.size() <= static_cast<size_t>(themes::kMaxNameLen));
    CHECK(!s.empty());
}

// Since v4.2.0 the built-in themes only use the 240 fixed colours (16-255) and give every text a colour: the 16 base
// colours and the "terminal's own" text colour change with the terminal's colour scheme (10 is green in CMD, yellow elsewhere).
TEST(themes_built_ins_look_the_same_in_every_terminal) {
    mt::TempHome home;
    for (const themes::Theme& t : themes::load_all()) {
        if (!t.builtin) continue;
        for (const auto& [k, v] : t.kv) {
            const bool bg = k.size() > 2 && k.compare(k.size() - 2, 2, "Bg") == 0;
            if (k == "ColorVizCenter" || k.find("Radio.") == 0) continue;
            if (!bg) CHECK(!v.empty());                                    // no text left to the terminal's own colour
            if (!v.empty()) {
                const int n = std::atoi(v.c_str());
                if (n < 16) { std::cerr << "    " << t.name << ": " << k << "=" << v << "\n"; }
                CHECK(n >= 16 && n <= 255);
            }
        }
    }
}

TEST(themes_renewed_built_ins_pair_old_and_new) {
    const auto pairs = themes::renewed_builtins();
    CHECK(pairs.size() >= 30);
    const themes::Theme& def_old = pairs[0].first;
    const themes::Theme& def_new = pairs[0].second;
    CHECK_EQ(def_new.name, std::string("SeriousMeowerShell(Default)"));
    CHECK_EQ(*def_old.get("ColorHeader", false), std::string("10"));      // what v4.2.0 first shipped
    CHECK_EQ(*def_new.get("ColorHeader", false), std::string("227"));     // the yellow it was made with
    CHECK_EQ(*def_new.get("ColorLegend", false), std::string("104"));
    CHECK_EQ(*def_new.get("ColorListInactiveFg", false), std::string("75"));
}
