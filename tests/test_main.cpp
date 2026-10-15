#include "test_framework.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace mt {

int g_failures = 0;

std::vector<Case>& registry() { static std::vector<Case> r; return r; }

void fail(const char* file, int line, const std::string& what) {
    ++g_failures;
    std::cerr << "    FAIL " << file << ":" << line << "  " << what << "\n";
}

TempHome::TempHome() {
    const char* h = std::getenv("HOME");
    had_home_ = h != nullptr;
    if (h) old_home_ = h;
    std::mt19937_64 rng(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    dir_ = std::filesystem::temp_directory_path() / ("mousiki-test-" + std::to_string(rng()));
    std::filesystem::create_directories(dir_);
#if defined(_WIN32)
    _putenv_s("HOME", dir_.string().c_str());
#else
    setenv("HOME", dir_.string().c_str(), 1);
#endif
}

TempHome::~TempHome() {
#if defined(_WIN32)
    _putenv_s("HOME", had_home_ ? old_home_.c_str() : "");
#else
    if (had_home_) setenv("HOME", old_home_.c_str(), 1); else unsetenv("HOME");
#endif
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
}

} // namespace mt

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int ran = 0, bad = 0;
    for (const mt::Case& c : mt::registry()) {
        if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
        const int before = mt::g_failures;
        std::cerr << "[ RUN  ] " << c.name << "\n";
        try { c.fn(); }
        catch (const std::exception& e) { mt::fail(__FILE__, __LINE__, std::string("exception: ") + e.what()); }
        catch (...) { mt::fail(__FILE__, __LINE__, "unknown exception"); }
        ++ran;
        if (mt::g_failures != before) { ++bad; std::cerr << "[ FAIL ] " << c.name << "\n"; }
        else std::cerr << "[  OK  ] " << c.name << "\n";
    }
    std::cerr << "\n" << ran << " tests, " << bad << " failed\n";
    if (ran == 0) { std::cerr << "no test matches '" << filter << "'\n"; return 2; }
    return bad == 0 ? 0 : 1;
}
