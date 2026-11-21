// yt-dlp: the version comparison, the recommendation, the reading of `yt-dlp -U` and (on Unix, with a fake yt-dlp) the update itself.
#include "test_framework.h"
#include "ytdlp_update.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace muisc;
namespace yu = muisc::ytdlp_update;

TEST(ytdlp_version_parse_and_compare) {
    CHECK(yu::parse_version("2026.10.26") == (std::vector<int>{2026, 10, 26}));
    CHECK(yu::parse_version("2026.10.26.232815") == (std::vector<int>{2026, 10, 26, 232815}));   // a nightly build
    CHECK(yu::parse_version("v2026.01.03").size() == 3);
    CHECK(yu::parse_version("5.2.0").empty());           // Mousiki's own kind of number is no yt-dlp version
    CHECK(yu::parse_version("2026.13.01").empty());      // not a date
    CHECK(yu::parse_version("").empty());
    CHECK(yu::parse_version("latest").empty());
    CHECK(yu::is_newer("2026.10.26", "2026.07.01"));
    CHECK(yu::is_newer("2026.10.26", "2025.12.31"));
    CHECK(!yu::is_newer("2026.07.01", "2026.10.26"));
    CHECK(!yu::is_newer("2026.10.26", "2026.10.26"));
    CHECK(yu::is_newer("2026.10.26.232815", "2026.10.26"));   // a nightly of the same day is later than the release
    CHECK(!yu::is_newer("2026.10.26", "2026.10.26.232815"));
    CHECK(!yu::is_newer("", "2026.10.26"));
    CHECK(!yu::is_newer("2026.10.26", "garbage"));
}

TEST(ytdlp_days_between_and_age_text) {
    CHECK_EQ(yu::days_between("2026.10.01", "2026.10.26"), 25LL);
    CHECK_EQ(yu::days_between("2026.02.28", "2026.03.01"), 1LL);      // not a leap year
    CHECK_EQ(yu::days_between("2024.02.28", "2024.03.01"), 2LL);      // a leap year
    CHECK_EQ(yu::days_between("2025.12.31", "2026.01.01"), 1LL);
    CHECK_EQ(yu::days_between("2026.10.26", "2026.10.01"), -25LL);
    CHECK_EQ(yu::days_between("x", "2026.10.01"), -1LL);
    CHECK_EQ(yu::age_text(0), std::string("less than a day"));
    CHECK_EQ(yu::age_text(1), std::string("1 day"));
    CHECK_EQ(yu::age_text(9), std::string("9 days"));
    CHECK_EQ(yu::age_text(21), std::string("about 3 weeks"));
    CHECK_EQ(yu::age_text(120), std::string("about 4 months"));
    CHECK_EQ(yu::age_text(800), std::string("about 2 years"));
}

TEST(ytdlp_describe_and_recommendation) {
    yu::Status s;
    s.found = false;
    CHECK(yu::describe(s).find("not found") != std::string::npos);
    CHECK(!yu::recommended(s));

    s = yu::Status{};
    s.installed = "2026.07.01"; s.latest = "2026.10.26"; s.ok = true;
    CHECK(yu::recommended(s));                                         // 117 days behind
    const std::string d = yu::describe(s);
    CHECK(d.find("2026.07.01") != std::string::npos && d.find("2026.10.26") != std::string::npos && d.find("months newer") != std::string::npos);
    const std::string r = yu::recommendation(s);
    CHECK(r.find("update yt-dlp") != std::string::npos);

    s.installed = "2026.10.20";                                        // 6 days behind: not worth a notice
    CHECK(!yu::recommended(s));
    CHECK(yu::recommendation(s).empty());
    CHECK(yu::describe(s).find("is installed") != std::string::npos);   // but the comparison still says it

    s.installed = "2026.10.26";
    CHECK(yu::describe(s).find("newest release") != std::string::npos);
    s.installed = "2026.10.27.001122";
    CHECK(yu::describe(s).find("nightly") != std::string::npos);

    s = yu::Status{};
    s.installed = "2026.07.01"; s.error = "GitHub could not be reached";   // no newest version known
    CHECK(!yu::recommended(s));
    CHECK(yu::describe(s).find("could not be read") != std::string::npos);
    s = yu::Status{};
    s.error = "exit code 1";                                            // found, but it does not run
    CHECK(yu::describe(s).find("does not run") != std::string::npos);
}

TEST(ytdlp_state_round_trip) {
    yu::State s;
    s.checked = 1790000000; s.latest = "2026.10.26"; s.notified = "2026.10.26";
    const yu::State b = yu::state_from_text(yu::state_to_text(s));
    CHECK_EQ(b.checked, s.checked);
    CHECK_EQ(b.latest, s.latest);
    CHECK_EQ(b.notified, s.notified);
    CHECK_EQ(yu::state_from_text("garbage\nchecked=\n").checked, 0LL);
}

TEST(ytdlp_reads_what_dash_U_says) {
    using K = yu::Kind;
    CHECK(yu::read_update_output("yt-dlp is up to date (stable@2026.10.26 from yt-dlp/yt-dlp)").kind == K::UpToDate);
    CHECK(yu::read_update_output("Updated yt-dlp to stable@2026.10.26 from yt-dlp/yt-dlp").kind == K::Updated);
    const yu::Reading pip = yu::read_update_output("ERROR: You installed yt-dlp with pip or using a package manager; Use that to update");
    CHECK(pip.kind == K::Pip && pip.manager == "pip");
    const yu::Reading brew = yu::read_update_output("You installed yt-dlp from Homebrew. Use brew upgrade yt-dlp");
    CHECK(brew.kind == K::PackageManager && !brew.manager.empty());
    CHECK(yu::read_update_output("ERROR: Unable to write to /opt/yt-dlp: [Errno 13] Permission denied").kind == K::NotWritable);
    CHECK(yu::read_update_output("Access is denied.").kind == K::NotWritable);
    CHECK(yu::read_update_output("").kind == K::Unknown);
    CHECK(yu::read_update_output("something else").kind == K::Unknown);
    // a word inside another word is no package manager ("pipeline" is not pip)
    CHECK(yu::read_update_output("You installed yt-dlp through a pipeline of scripts").manager.empty());
}

TEST(ytdlp_manager_hints_name_the_command) {
    CHECK(yu::manager_hint("brew").find("brew upgrade yt-dlp") != std::string::npos);
    CHECK(yu::manager_hint("scoop").find("scoop update yt-dlp") != std::string::npos);
    CHECK(yu::manager_hint("winget").find("winget upgrade") != std::string::npos);
    CHECK(yu::manager_hint("pipx").find("pipx upgrade yt-dlp") != std::string::npos);
    CHECK(!yu::manager_hint("").empty());
}

TEST(ytdlp_command_words) {
    bool ok = true;
    CHECK(yu::run_command("bogus", "x.py", true, ok).find("update") != std::string::npos);
    CHECK(!ok);
    const std::string refused = yu::run_command("yt-dlp", "x.py", /*allow_update=*/false, ok);   // a script without `force`
    CHECK(!ok);
    CHECK(refused.find("force") != std::string::npos);
}

#if !defined(_WIN32)
namespace {
struct FakeYtdlp {   // a yt-dlp in a folder that comes first on PATH: it keeps its version in a file and "updates" it
    std::filesystem::path dir;
    std::string old_path;
    FakeYtdlp(const std::string& version, const std::string& dash_u_output, bool really_update) {
        dir = std::filesystem::temp_directory_path() / ("mousiki-fake-ytdlp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        { std::ofstream(dir / "ver") << version << "\n"; }
        {
            std::ofstream f(dir / "yt-dlp");
            f << "#!/bin/sh\nv=\"$(dirname \"$0\")/ver\"\n"
              << "case \"$1\" in\n"
              << "  --version) cat \"$v\" ;;\n"
              << "  -U) " << (really_update ? "echo 2026.10.26 > \"$v\"; " : "") << "echo '" << dash_u_output << "' ;;\n"
              << "esac\n";
        }
        std::filesystem::permissions(dir / "yt-dlp", std::filesystem::perms::owner_all);
        const char* p = std::getenv("PATH");
        old_path = p ? p : "";
        setenv("PATH", (dir.string() + ":" + old_path).c_str(), 1);
    }
    ~FakeYtdlp() { setenv("PATH", old_path.c_str(), 1); std::error_code ec; std::filesystem::remove_all(dir, ec); }
};
yu::Result wait_for_update() {
    yu::Result r;
    for (int i = 0; i < 200 && !yu::take_update(r); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return r;
}
}  // namespace

TEST(ytdlp_update_runs_and_reports_the_new_version) {
    mt::TempHome home;
    FakeYtdlp fake("2026.07.01", "Updated yt-dlp to stable@2026.10.26 from yt-dlp/yt-dlp", true);
    CHECK(yu::update_now());
    const yu::Result r = wait_for_update();
    CHECK(r.ok && r.changed);
    CHECK_EQ(r.before, std::string("2026.07.01"));
    CHECK_EQ(r.after, std::string("2026.10.26"));
    CHECK(r.message.find("2026.07.01 -> 2026.10.26") != std::string::npos);
    CHECK(!yu::busy());
}

TEST(ytdlp_update_says_when_nothing_is_newer) {
    mt::TempHome home;
    FakeYtdlp fake("2026.10.26", "yt-dlp is up to date (stable@2026.10.26 from yt-dlp/yt-dlp)", false);
    CHECK(yu::update_now());
    const yu::Result r = wait_for_update();
    CHECK(r.ok && !r.changed);
    CHECK(r.message.find("already the newest") != std::string::npos);
}

TEST(ytdlp_update_leaves_a_package_manager_copy_alone) {
    mt::TempHome home;
    FakeYtdlp fake("2026.07.01", "You installed yt-dlp from Homebrew. Use that to update", false);
    CHECK(yu::update_now());
    const yu::Result r = wait_for_update();
    CHECK(!r.ok && !r.changed);
    CHECK_EQ(r.after, std::string("2026.07.01"));                       // nothing was run to change it
    CHECK(r.message.find("brew upgrade yt-dlp") != std::string::npos);
}
#endif
