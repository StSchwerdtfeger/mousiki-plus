#include "selftest.h"
#include "app_internal.h"
#include <cstdio>
#include <cctype>
#include <cstdlib>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif
#include <filesystem>
#include <string>
#if defined(_WIN32)
#include "win_compat.h"
#endif

namespace muisc {

namespace {
// Only plain option words are passed on (the command line goes through a shell): letters, digits, "-", "_", "," and ".".
bool plain_arg(const std::string& a) {
    if (a.empty()) return false;
    for (unsigned char c : a) {
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == ',' || c == '.')) return false;
    }
    return true;
}
}  // namespace

int selftest_fetch_main(int argc, char** argv) {
    const std::filesystem::path script = locate_script("selftest_fetch.py");
    if (script.empty() || !std::filesystem::exists(script)) {
        std::fprintf(stderr, "mousiki --selftest-fetch: scripts/selftest_fetch.py was not found (reinstall Mousiki, or set MOUSIKI_SCRIPTS_DIR)\n");
        return 2;
    }
#if defined(_WIN32)
    const std::string python = win_python_command();
    if (python.empty()) {
        std::fprintf(stderr, "mousiki --selftest-fetch: Python 3 was not found on PATH (the lyrics and metadata helpers need it)\n");
        return 2;
    }
    const std::string q = "\"";
#else
    const std::string python = "python3";
    const std::string q = "'";
#endif
    std::string cmd = python + " " + q + script.string() + q;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (!plain_arg(a)) {
            std::fprintf(stderr, "mousiki --selftest-fetch: unknown option \"%s\" (see: mousiki --selftest-fetch --help)\n", a.c_str());
            return 2;
        }
        cmd += " " + a;
    }
#if defined(_WIN32)
    cmd = "\"" + cmd + "\"";   // cmd.exe /c strips one pair of outer quotes
#endif
    std::fflush(stdout);
    const int rc = std::system(cmd.c_str());
#if defined(_WIN32)
    return rc;
#else
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 2;
#endif
}

}  // namespace muisc
