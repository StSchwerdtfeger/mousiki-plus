// Share Profiles: with it on, the radio uses the player's oscilloscope / spectrogram settings; its own values wait in
// radio_config.txt and come back when it is turned off. Colours are never shared.
#include "test_framework.h"
#include "radio_settings.h"
#include "settings.h"

#include <fstream>
#include <sstream>

using namespace muisc;

namespace {
std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
}

TEST(share_profiles_radio_follows_the_player_and_gets_its_own_back) {
    mt::TempHome home;
    // the player: its own look, saved to config.txt
    Settings player = load_settings();
    player.osci_set[1].decay = 0.33f;
    player.osci_style = 1;
    player.spectro.span = 45;
    save_settings(player);
    // the radio: a different look, saved to radio_config.txt
    radio::RadioSettings r = radio::load_radio_settings();
    r.osci_set[1].decay = 0.55f;
    r.osci_style = 0;
    r.spectro.span = 10;
    r.scope_grid_color = "77";
    CHECK(radio::save_radio_settings(r));

    // not shared: the radio keeps its own
    r = radio::load_radio_settings();
    CHECK(!r.share_profiles);
    CHECK_NEAR(r.osci_set[1].decay, 0.55, 0.001);

    // turn it on: the player's values are used, the radio's own are kept as the backup
    radio::radio_share_profiles(r, true);
    CHECK(r.share_profiles);
    CHECK_NEAR(r.osci_set[1].decay, 0.33, 0.001);
    CHECK_EQ(r.osci_style, 1);
    CHECK_EQ(r.spectro.span, 45);
    CHECK_EQ(r.scope_grid_color, std::string("77"));            // the grid colour stays the radio's
    CHECK_NEAR(r.own_viz.osci_set[1].decay, 0.55, 0.001);
    CHECK(radio::save_radio_settings(r));
    CHECK(radio::player_share_profiles());                       // the flag is in the player's config.txt
    CHECK(read_share_profiles());

    // a restart of the radio: the same
    r = radio::load_radio_settings();
    CHECK(r.share_profiles);
    CHECK_NEAR(r.osci_set[1].decay, 0.33, 0.001);
    CHECK_NEAR(r.own_viz.osci_set[1].decay, 0.55, 0.001);        // radio_config.txt still holds the radio's own value

    // the radio changes a shared value: it lands in the player's config.txt, the radio's backup is untouched
    r.osci_set[1].decay = 0.71f;
    r.spectro.span = 30;
    CHECK(radio::save_radio_settings(r));
    const Settings p2 = load_settings();
    CHECK_NEAR(p2.osci_set[1].decay, 0.71, 0.001);
    CHECK_EQ(p2.spectro.span, 30);
    const std::string file = read_file(home.config_dir() / "radio_config.txt");
    CHECK(file.find("OsciImageDecay=0.55") != std::string::npos);

    // the change counts as a change for the settings screen (it compares texts)
    radio::RadioSettings before = r;
    r.osci_set[1].decay = 0.72f;
    CHECK(radio::radio_settings_text(before) != radio::radio_settings_text(r));

    // turn it off: the radio's own values come back, the player keeps what it has
    radio::radio_share_profiles(r, false);
    CHECK(!r.share_profiles);
    CHECK_NEAR(r.osci_set[1].decay, 0.55, 0.001);
    CHECK_EQ(r.osci_style, 0);
    CHECK_EQ(r.spectro.span, 10);
    CHECK(radio::save_radio_settings(r));
    CHECK(!read_share_profiles());
    r = radio::load_radio_settings();
    CHECK_NEAR(r.osci_set[1].decay, 0.55, 0.001);
    CHECK_NEAR(load_settings().osci_set[1].decay, 0.71, 0.001);   // the player keeps what the shared time left (the 0.72 was never saved)
}

TEST(share_profiles_without_a_player_config_keeps_the_radios_values) {
    mt::TempHome home;
    radio::RadioSettings r = radio::load_radio_settings();
    r.osci_set[0].decay = 0.42f;
    radio::radio_share_profiles(r, true);                         // no config.txt yet: nothing to take over
    CHECK_NEAR(r.osci_set[0].decay, 0.42, 0.001);
    radio::radio_share_profiles(r, false);
    CHECK_NEAR(r.osci_set[0].decay, 0.42, 0.001);
}

TEST(radio_settings_roundtrip_is_stable) {
    mt::TempHome home;
    radio::RadioSettings r = radio::load_radio_settings();
    r.osci_set[0].dwell = false;
    r.osci_set[1].trace = 4096;
    r.osci_mode = 2;
    r.image_detail = 1;
    CHECK(radio::save_radio_settings(r));
    const radio::RadioSettings back = radio::load_radio_settings();
    CHECK(!back.osci_set[0].dwell);
    CHECK_EQ(back.osci_set[1].trace, 4096);
    CHECK_EQ(back.osci_mode, 2);
    CHECK_EQ(back.image_detail, 1);
    CHECK_EQ(radio::radio_settings_text(back), radio::radio_settings_text(r));
}
