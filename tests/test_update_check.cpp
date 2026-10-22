// The "newer version" notice (src/update_check.h): version comparison, the state file, the wording.
#include "test_framework.h"
#include "update_check.h"

#include "open_url.h"

using namespace muisc;

TEST(update_check_is_newer_compares_numbers) {
    CHECK(update_check::is_newer("v4.5.0", "4.4.0"));
    CHECK(update_check::is_newer("4.4.1", "4.4.0"));
    CHECK(update_check::is_newer("5.0", "4.9.9"));
    CHECK(update_check::is_newer("v4.10.0", "4.9.0"));   // numbers, not text
    CHECK(!update_check::is_newer("4.4.0", "4.4.0"));
    CHECK(!update_check::is_newer("v4.4.0", "4.4.0"));
    CHECK(!update_check::is_newer("4.3.9", "4.4.0"));
    CHECK(!update_check::is_newer("4.4", "4.4.0"));      // 4.4 is 4.4.0
}

TEST(update_check_is_newer_ignores_unreadable_versions) {
    CHECK(!update_check::is_newer("", "4.4.0"));
    CHECK(!update_check::is_newer("latest", "4.4.0"));
    CHECK(!update_check::is_newer("4.5.0", "dev"));      // a development build never gets the notice
    CHECK(!update_check::is_newer("4.5.0", ""));
}

TEST(update_check_state_round_trip) {
    update_check::State s;
    s.checked = 1790000000;
    s.latest = "v4.5.0";
    s.url = "https://github.com/StSchwerdtfeger/mousiki-plus/releases/tag/v4.5.0";
    s.notified = "v4.4.0";
    const update_check::State b = update_check::state_from_text(update_check::state_to_text(s));
    CHECK_EQ(b.checked, s.checked);
    CHECK_EQ(b.latest, s.latest);
    CHECK_EQ(b.url, s.url);
    CHECK_EQ(b.notified, s.notified);
}

TEST(update_check_state_survives_garbage) {
    const update_check::State b = update_check::state_from_text("nonsense\n=\nchecked=abc\nlatest=v9.9.9\r\n\n");
    CHECK_EQ(b.checked, 0LL);
    CHECK_EQ(b.latest, std::string("v9.9.9"));
    CHECK(update_check::state_from_text("").latest.empty());
}

TEST(update_check_notice_names_version_and_page) {
    const std::string n = update_check::notice_text("v4.5.0", "4.4.0", "https://example.org/r");
    CHECK(n.find("4.5.0") != std::string::npos);
    CHECK(n.find("4.4.0") != std::string::npos);
    CHECK(n.find("https://example.org/r") != std::string::npos);
    CHECK(update_check::notice_text("4.5.0", "4.4.0", "").find("github.com/StSchwerdtfeger/mousiki-plus") != std::string::npos);
}

TEST(open_url_accepts_only_plain_https_addresses) {
    CHECK(url_is_safe("https://github.com/StSchwerdtfeger/mousiki-plus/releases/tag/v4.5.0"));
    CHECK(!url_is_safe("http://github.com/x/y"));
    CHECK(!url_is_safe("file:///etc/passwd"));
    CHECK(!url_is_safe("https://example.org/a b"));
    CHECK(!url_is_safe("https://example.org/\"; rm -rf ~"));
    CHECK(!url_is_safe("https://example.org/$(id)"));
    CHECK(!url_is_safe("https://example.org/a&b"));
    CHECK(!url_is_safe(""));
}
