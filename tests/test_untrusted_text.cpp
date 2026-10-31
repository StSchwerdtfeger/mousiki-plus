// Text from outside (Radio Browser stations, stream titles, web search results, file tags) never reaches the terminal with
// control characters in it: an ESC sequence in a station name must not redraw the screen or reach the clipboard (OSC 52).
#include "test_framework.h"
#include "radio_browser.h"
#include "utf8_util.h"
#include <string>

using namespace muisc;
using namespace muisc::radio;

namespace {
bool has_control(const std::string& s) {
    for (unsigned char c : s) if (c < 0x20 || c == 0x7F) return true;
    return false;
}
} // namespace

TEST(radio_browser_results_lose_control_characters) {
    // \u001b]52;c;... is the OSC 52 "write to the clipboard" sequence; \u0007 ends it
    const std::string json =
        "[{\"stationuuid\":\"u1\",\"name\":\"Evil \\u001b]52;c;aGk=\\u0007FM\",\"url\":\"http://example.com/s\","
        "\"tags\":\"rock,\\u001b[2Jpop\",\"country\":\"Ger\\u0008many\",\"countrycode\":\"DE\",\"codec\":\"MP3\",\"bitrate\":128}]";
    bool ok = false;
    const std::vector<BrowseStation> v = parse_browse_json(json, &ok);
    CHECK(ok);
    CHECK_EQ(v.size(), static_cast<size_t>(1));
    if (v.empty()) return;
    CHECK(!has_control(v[0].name));
    CHECK(!has_control(v[0].tags));
    CHECK(!has_control(v[0].country));
    CHECK(v[0].name.find("Evil") == 0);   // the text itself stays
    CHECK(!has_control(browse_to_station(v[0]).genre));
}

TEST(clean_tag_text_removes_escape_sequences_starters) {
    const std::string t = clean_tag_text("Artist\x1b[31m - Ti\rtle\x07");
    CHECK(!has_control(t));
    CHECK(t.find("Artist") == 0);
}

TEST(radio_browser_keeps_only_web_streams) {
    // anyone can add a station: a file: path, a device or an ffmpeg option in place of the stream address is dropped
    const std::string json =
        "[{\"name\":\"Web\",\"url\":\"HTTPS://example.com/s\"},"
        "{\"name\":\"File\",\"url\":\"file:///etc/passwd\"},"
        "{\"name\":\"Option\",\"url\":\"-f lavfi\"},"
        "{\"name\":\"Resolved\",\"url\":\"http://a/\",\"url_resolved\":\"concat:/x|/y\"}]";
    bool ok = false;
    const std::vector<BrowseStation> v = parse_browse_json(json, &ok);
    CHECK(ok);
    CHECK_EQ(v.size(), static_cast<size_t>(1));
    if (!v.empty()) CHECK_EQ(v[0].name, std::string("Web"));
    CHECK(Station::web_url("http://x"));
    CHECK(!Station::web_url("httpx://x"));
    CHECK(!Station::web_url("http:"));
}
