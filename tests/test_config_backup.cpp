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
