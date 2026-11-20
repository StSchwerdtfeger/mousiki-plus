// Not a test: with DUMP_DEFAULTS=<file> the default settings are written to that file as config.txt would hold them. tools/defaults_overview.py
// turns that file into the "Default values" chapters of the manual and the wizard (see the comment there). Without the variable it does nothing.
#include "test_framework.h"
#include "settings.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace muisc;

TEST(dump_default_settings_when_asked) {
    const char* target = std::getenv("DUMP_DEFAULTS");
    if (!target) return;
    mt::TempHome home;
    Settings s = load_settings();   // no file: the defaults, with the default hotkeys filled in
    save_settings(s);
    std::ifstream in(config_path());
    std::stringstream ss;
    ss << in.rdbuf();
    std::ofstream(target) << ss.str();
}
