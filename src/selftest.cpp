#include "selftest.h"
#include "app_internal.h"
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <filesystem>
#include <string>
#include <vector>
#if defined(_WIN32)
#include "win_compat.h"
#endif

namespace muisc {

namespace {
// Only plain option words are passed on (no shell is used any more; this is a second line of defence): letters, digits, "-", "_", "," and ".".
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
    // No shell is involved: the interpreter is started directly with an argument list, so neither the script's
    // location (it may come from MOUSIKI_SCRIPTS_DIR) nor the command line can inject a command.
    std::vector<std::string> args;
#if defined(_WIN32)
    const std::string python = win_python_command();
    if (python.empty()) {
        std::fprintf(stderr, "mousiki --selftest-fetch: Python 3 was not found on PATH (the lyrics and metadata helpers need it)\n");
        return 2;
    }
    if (python == "py -3") { args.push_back("py"); args.push_back("-3"); }
    else args.push_back("\"" + python + "\"");   // _spawnvp does not quote: a path with spaces needs its own quotes
    args.push_back("\"" + script.string() + "\"");
#else
    args.push_back("python3");
    args.push_back(script.string());
#endif
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (!plain_arg(a)) {
            std::fprintf(stderr, "mousiki --selftest-fetch: unknown option \"%s\" (see: mousiki --selftest-fetch --help)\n", a.c_str());
            return 2;
        }
        args.push_back(a);
    }
    std::vector<char*> cargs;
    for (std::string& a : args) cargs.push_back(a.data());
    cargs.push_back(nullptr);
    std::fflush(stdout);
    std::fflush(stderr);
#if defined(_WIN32)
    const intptr_t rc = _spawnvp(_P_WAIT, cargs[0], cargs.data());
    return rc < 0 ? 2 : static_cast<int>(rc);
#else
    const pid_t pid = fork();
    if (pid < 0) return 2;
    if (pid == 0) {
        execvp(cargs[0], cargs.data());
        std::fprintf(stderr, "mousiki --selftest-fetch: python3 could not be started\n");
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return 2;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 2;
#endif
}

}  // namespace muisc
