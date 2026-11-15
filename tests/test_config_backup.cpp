// Upgrade safety: config.txt / radio_config.txt are copied to *.bak before a different version rewrites them.
#include "test_framework.h"
#include "settings.h"
#include "version.h"

#include <fstream>
#include <sstream>

using namespace muisc;
namespace fs = std::filesystem;

namespace {
std::string read_all(const fs::path& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
void write_all(const fs::path& p, const std::string& t) { fs::create_directories(p.parent_path()); std::ofstream f(p); f << t; }
}

TEST(config_backup_of_an_older_file) {
    mt::TempHome home;
    const fs::path cfg = config_path();
    write_all(cfg, "# Mousiki Configuration File\nBorderColor=99\n");   // no stamp: from before v4.2.0
    Settings s = load_settings();
    save_settings(s);
    CHECK(fs::exists(cfg.string() + ".bak"));
    CHECK(read_all(cfg.string() + ".bak").find("BorderColor=99") != std::string::npos);   // the old file, unchanged
    CHECK_EQ(config_written_by(cfg), std::string(MOUSIKI_VERSION));                     // the new one carries the stamp
}

TEST(config_backup_only_once_per_version) {
    mt::TempHome home;
    const fs::path cfg = config_path();
    write_all(cfg, "# Written by Mousiki v0.0.1\nBorderColor=12\n");
    save_settings(load_settings());
    CHECK(read_all(cfg.string() + ".bak").find("BorderColor=12") != std::string::npos);
    write_all(cfg.string() + ".bak", "keep me");
    save_settings(load_settings());                                   // the same version again: the backup stays
    CHECK_EQ(read_all(cfg.string() + ".bak"), std::string("keep me"));
}

TEST(config_backup_nothing_without_a_file) {
    mt::TempHome home;
    const fs::path cfg = config_path();
    CHECK(!backup_config_if_older(cfg));
    save_settings(load_settings());
    CHECK(!fs::exists(cfg.string() + ".bak"));                        // a first install has nothing to keep
}

TEST(remote_app_config_round_trip) {
    mt::TempHome home;
    save_settings(load_settings());
    RemoteAppConfig rc = read_remote_app();
    CHECK(!rc.enabled);                                               // off by default
    CHECK_EQ(rc.port, 4747);
    rc.enabled = true;
    CHECK(write_remote_app(rc));
    CHECK_EQ(rc.pin.size(), static_cast<size_t>(6));                  // switched on without a PIN: one is made
    const RemoteAppConfig back = read_remote_app();
    CHECK(back.enabled);
    CHECK_EQ(back.pin, rc.pin);
    const Settings s = load_settings();                               // the player reads the same lines
    CHECK(s.remote_app);
    CHECK_EQ(s.remote_pin, rc.pin);
    save_settings(s);                                                 // ... and writes them back unchanged
    CHECK_EQ(read_remote_app().pin, rc.pin);
}

TEST(config_milkdrop_options_round_trip) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK_EQ(s.milkdrop_hcd, 20);                 // projectM's own defaults
    CHECK(s.milkdrop_aspect);
    CHECK(s.milkdrop_ratio == 0);
    CHECK_EQ(s.milkdrop_mesh, 1);
    CHECK_EQ(s.milkdrop_var, 0);
    CHECK(!s.milkdrop_clean);
    CHECK_EQ(s.milkdrop_win_res, 1280);           // the extra MilkDrop window's picture size
    CHECK(s.hotkeys.count("HKeyMilkdropWindow") == 1 && !s.hotkeys["HKeyMilkdropWindow"].empty());   // ";" or "|" by keyboard layout
    s.milkdrop_win_res = 960;
    s.milkdrop_lock = true; s.milkdrop_extra_paths = {"/tmp/more presets", "/tmp/even more"};
    s.milkdrop_hcd = 45; s.milkdrop_aspect = false; s.milkdrop_ratio = 3; s.milkdrop_mesh = 3; s.milkdrop_var = 10; s.milkdrop_clean = true; s.milkdrop_gpu = "renderD129";
    save_settings(s);
    const Settings t = load_settings();
    CHECK_EQ(t.milkdrop_hcd, 45);
    CHECK(t.milkdrop_lock);                                  // Lock Preset survives a restart
    CHECK_EQ(t.milkdrop_extra_paths.size(), size_t(2));      // MILKDROP PRESET PATH: one line a folder
    CHECK_EQ(t.milkdrop_extra_paths[1], std::string("/tmp/even more"));
    CHECK(!t.milkdrop_aspect);
    CHECK(t.milkdrop_ratio == 3);
    CHECK_EQ(t.milkdrop_mesh, 3);
    CHECK_EQ(t.milkdrop_var, 10);
    CHECK(t.milkdrop_clean);
    CHECK_EQ(t.milkdrop_win_res, 960);
    CHECK_EQ(t.milkdrop_gpu, std::string("renderD129"));
    const std::string txt = read_all(config_path());
    CHECK(txt.find("MilkDropHardCutInterval=45") != std::string::npos);
    CHECK(txt.find("MilkDropMeshDetail=3") != std::string::npos);
    CHECK(txt.find("MilkDropWindowResolution=960") != std::string::npos);
}
