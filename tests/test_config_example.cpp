// The example config.txt in the project root (shipped with the releases) must list every setting the program writes, and nothing the
// program does not know: a new setting that is not added to it (or a key that was renamed) fails here. The values in the example may differ
// from the defaults (it shows examples, and the "## ... (default x)" lines give the defaults); only the keys are compared (case does not
// matter, as when the program reads them).
#include "test_framework.h"
#include "settings.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace muisc;

#ifdef MOUSIKI_SOURCE_DIR
namespace {
std::map<std::string, std::string> keys_of(const std::string& text) {   // lower-case key -> its line
    std::map<std::string, std::string> m;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ' ') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string k = line.substr(0, eq);
        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (k == "separator") k = "seprator";   // (the program writes the old spelling and reads both)
        m[k] = line;
    }
    return m;
}
}  // namespace

TEST(example_config_lists_every_setting_the_program_writes) {
    mt::TempHome home;   // (an empty config folder: load_settings() gives the defaults, hotkeys included)
    const auto want = keys_of(settings_to_text(load_settings()));
    std::ifstream in(std::string(MOUSIKI_SOURCE_DIR) + "/config.txt", std::ios::binary);
    CHECK(static_cast<bool>(in));
    std::stringstream ss;
    ss << in.rdbuf();
    const auto have = keys_of(ss.str());
    const std::set<std::string> example_only = {"localmusicpath"};   // (written only once a path is set; the example shows one)
    for (const auto& kv : want)
        if (!have.count(kv.first)) ::mt::fail(__FILE__, __LINE__, "config.txt lacks the setting  " + kv.second + "  (add it, with its ## comment line, next to its neighbours)");
    for (const auto& kv : have)
        if (!want.count(kv.first) && !example_only.count(kv.first)) ::mt::fail(__FILE__, __LINE__, "config.txt has  " + kv.second + "  which the program does not write (renamed or removed?)");
}
#endif
