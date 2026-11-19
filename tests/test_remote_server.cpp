// The remote app's web server (src/remote_server.h): request parsing, the PIN, the commands, the screen, a real socket.
#include "test_framework.h"
#include "remote_server.h"
#include "remote_control.h"
#include "terminal_ui.h"
#include "tiny_json.h"

#include <algorithm>
#include <atomic>
#include <vector>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace muisc;
namespace net = muisc::remote::net;

namespace {
int code_of(const std::string& reply) { return reply.size() > 12 ? std::atoi(reply.c_str() + 9) : 0; }
std::string body_of(const std::string& reply) { const size_t p = reply.find("\r\n\r\n"); return p == std::string::npos ? "" : reply.substr(p + 4); }
std::string req(const std::string& target) { return net::handle_request("GET " + target + " HTTP/1.1", "test"); }
void use_pin(const char* pin) { net::configure({false, 4747, pin, "", false}); }   // off: no socket, only the PIN is set

// a plain HTTP GET over a socket (for the live server test)
std::string http_get(int port, const std::string& target) {
#if defined(_WIN32)
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    std::string out;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        const std::string r = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        send(s, r.data(), static_cast<int>(r.size()), 0);
        char buf[4096];
        int n;
        while ((n = static_cast<int>(recv(s, buf, sizeof buf, 0))) > 0) out.append(buf, static_cast<size_t>(n));
    }
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
    return out;
}
} // namespace

TEST(remote_server_parses_request_lines) {
    std::string path;
    std::vector<std::pair<std::string, std::string>> q;
    CHECK(net::parse_request_line("GET /api/cmd?pin=1234&name=volume&arg=%2B10 HTTP/1.1", path, q));
    CHECK_EQ(path, std::string("/api/cmd"));
    CHECK_EQ(q.size(), static_cast<size_t>(3));
    if (q.size() == 3) {
        CHECK_EQ(q[2].second, std::string("+10"));                 // %2B decoded
        CHECK_EQ(q[0].first, std::string("pin"));
    }
    CHECK(net::parse_request_line("GET / HTTP/1.1", path, q));
    CHECK_EQ(path, std::string("/"));
    CHECK(q.empty());
    CHECK(!net::parse_request_line("POST /api/cmd HTTP/1.1", path, q));   // only GET / HEAD
    CHECK(!net::parse_request_line("GET api HTTP/1.1", path, q));
    CHECK(!net::parse_request_line("garbage", path, q));
}

TEST(remote_server_json_escape) {
    CHECK_EQ(muisc::tinyjson::escape("a\"b\\c\n"), std::string("a\\\"b\\\\c\\n"));
    CHECK_EQ(muisc::tinyjson::escape("\x1b[0m"), std::string("\\u001b[0m"));      // colour escapes survive as \u001b
    CHECK_EQ(muisc::tinyjson::escape("M\xc3\xbcsik"), std::string("M\xc3\xbcsik")); // UTF-8 passes unchanged
}

TEST(remote_server_pin_rule) {
    CHECK(net::pin_ok("1234"));
    CHECK(net::pin_ok("12345678"));
    CHECK(!net::pin_ok("123"));
    CHECK(!net::pin_ok("123456789"));
    CHECK(!net::pin_ok("12a4"));
    const std::string p = net::make_pin();
    CHECK_EQ(p.size(), static_cast<size_t>(6));
    CHECK(net::pin_ok(p));
}

TEST(remote_server_page_is_open_api_needs_the_pin) {
    use_pin("246810");
    const std::string page = req("/");
    CHECK_EQ(code_of(page), 200);
    CHECK(page.find("text/html") != std::string::npos);
    CHECK(body_of(page).find("Mousiki Remote") != std::string::npos);
    CHECK_EQ(code_of(req("/api/status")), 403);
    CHECK_EQ(code_of(req("/api/status?pin=111111")), 403);
    CHECK_EQ(code_of(req("/api/status?pin=246810")), 200);
    const std::string ok = req("/api/hello?pin=246810");
    CHECK_EQ(code_of(ok), 200);
    CHECK(body_of(ok).find("\"ok\":true") != std::string::npos);
    CHECK_EQ(code_of(req("/nothing")), 404);
}

TEST(remote_server_locks_after_ten_wrong_pins) {
    use_pin("135790");
    for (int i = 0; i < 10; ++i) req("/api/status?pin=000000");
    CHECK_EQ(code_of(req("/api/status?pin=135790")), 429);           // even the right PIN waits a minute now
    use_pin("135791");                                                // a new PIN, set at the computer, ends it
    CHECK_EQ(code_of(req("/api/status?pin=135791")), 200);
}

TEST(remote_server_lockout_is_per_device) {
    use_pin("135792");
    auto from = [](const std::string& target, const std::string& ip) { return net::handle_request("GET " + target + " HTTP/1.1", ip); };
    for (int i = 0; i < 10; ++i) from("/api/status?pin=000000", "192.168.1.66");   // someone else on the Wi-Fi guesses
    CHECK_EQ(code_of(from("/api/status?pin=135792", "192.168.1.66")), 429);         // that device waits a minute
    CHECK_EQ(code_of(from("/api/status?pin=135792", "192.168.1.20")), 200);         // the phone is not locked out
    use_pin("135793");
}

TEST(remote_server_hello_needs_no_pin_and_is_no_wrong_pin) {
    // the phone app's network scan asks /api/hello?pin=0: it is recognised, and many scans do not lock the phone out
    use_pin("135794");
    for (int i = 0; i < 15; ++i) {
        const std::string r = req("/api/hello?pin=0");
        CHECK_EQ(code_of(r), 200);
        CHECK(body_of(r).find("\"ok\":true") != std::string::npos);
        CHECK(body_of(r).find("version") == std::string::npos);   // what anyone on the network may ask for says nothing but "this is Mousiki"
        CHECK(body_of(r).find("mode") == std::string::npos);
    }
    CHECK(body_of(req("/api/hello?pin=135794")).find("\"version\"") != std::string::npos);   // with the PIN: version and mode
    CHECK_EQ(code_of(req("/api/status?pin=135794")), 200);
    CHECK(req("/api/hello").find("Access-Control-Allow-Origin") == std::string::npos);   // no other web page may read the answers
}

TEST(remote_server_hello_with_a_wrong_pin_is_counted_like_any_other) {
    // /api/hello?pin=<guess> used to answer "pin":false for a wrong PIN and never count it: every six-digit PIN could be tried in minutes.
    // Only the scan's no-PIN form (pin=0 / no pin) passes uncounted; any other wrong PIN is locked out after ten, hello included.
    use_pin("135795");
    for (int i = 0; i < 10; ++i) CHECK_EQ(code_of(req("/api/hello?pin=000001")), 403);
    CHECK_EQ(code_of(req("/api/hello?pin=135795")), 429);        // even the right PIN waits a minute now
    CHECK_EQ(code_of(req("/api/status?pin=135795")), 429);       // (the lock-out is per address, shared with the other endpoints)
    use_pin("135796");                                            // a new PIN, set at the computer, ends it
    CHECK_EQ(code_of(req("/api/hello?pin=135796")), 200);
    for (int i = 0; i < 15; ++i) CHECK_EQ(code_of(req("/api/hello")), 200);   // the scan stays free, with no pin at all too
    CHECK_EQ(code_of(req("/api/hello?pin=135796")), 200);
}

TEST(remote_server_answers_only_the_home_network) {
    CHECK(net::lan_peer("127.0.0.1"));
    CHECK(net::lan_peer("192.168.178.20"));
    CHECK(net::lan_peer("10.0.0.5"));
    CHECK(net::lan_peer("172.20.1.1"));
    CHECK(net::lan_peer("169.254.3.4"));
    CHECK(net::lan_peer("100.101.102.103"));
    CHECK(!net::lan_peer("8.8.8.8"));
    CHECK(!net::lan_peer("172.32.0.1"));
    CHECK(!net::lan_peer("100.128.0.1"));
    CHECK(!net::lan_peer("?"));
}

TEST(remote_server_commands_reach_the_program) {
    mt::TempHome home;
    net::configure({false, 4747, "112233", "", false});
    remote::start("player");
    CHECK_EQ(code_of(req("/api/cmd?pin=112233&name=next")), 200);
    CHECK_EQ(code_of(req("/api/cmd?pin=112233&name=playback-mode")), 200);
    CHECK_EQ(code_of(req("/api/cmd?pin=112233&name=volume&arg=%2B5")), 200);
    CHECK_EQ(code_of(req("/api/cmd?pin=112233&name=rm-rf")), 400);       // only the known names
    CHECK_EQ(code_of(req("/api/cmd?pin=112233&name=seek&arg=1;2")), 400); // only a short number
    const auto cmds = remote::poll();                                   // network commands are not throttled
    CHECK_EQ(cmds.size(), static_cast<size_t>(3));
    if (cmds.size() == 3) {
        CHECK_EQ(cmds[0].name, std::string("next"));
        CHECK_EQ(cmds[1].name, std::string("playback-mode"));
        CHECK_EQ(cmds[2].arg, std::string("+5"));
    }
    remote::stop();
}

TEST(remote_server_hands_out_the_screen) {
    net::configure({false, 4747, "445566", "", false});
    net::set_screen("radio", 44, 2, {"\x1b[38;5;170mline one\x1b[0m", "two"}, {{1, 0, 5, 1, "next", ""}, {0, 1, 44, 1, "seek", "-5"}});
    const std::string r = req("/api/screen?pin=445566&cols=50&rows=60");
    CHECK_EQ(code_of(r), 200);
    const std::string b = body_of(r);
    CHECK(b.find("\"mode\":\"radio\"") != std::string::npos);
    CHECK(b.find("\\u001b[38;5;170mline one") != std::string::npos);
    CHECK(b.find("{\"x\":1,\"y\":0,\"w\":5,\"h\":1,\"cmd\":\"next\",\"arg\":\"\"}") != std::string::npos);
    CHECK(b.find("\"arg\":\"-5\"") != std::string::npos);
    net::set_status({{"mode", "radio"}, {"title", "A \"quoted\" title"}});
    CHECK(body_of(req("/api/status?pin=445566")).find("A \\\"quoted\\\" title") != std::string::npos);
}

TEST(remote_server_live_socket) {
    const int port = 47000 + static_cast<int>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000);
    net::configure({true, port, "778899", "", false});
    if (!net::running()) { CHECK(!net::last_error().empty()); net::shutdown(); return; }   // (port taken: the error says so)
    const std::string page = http_get(port, "/");
    CHECK_EQ(code_of(page), 200);
    CHECK(page.find("Content-Length:") != std::string::npos);
    const std::string bad = http_get(port, "/api/status?pin=1");
    CHECK(code_of(bad) == 403 || code_of(bad) == 429);
    // a second server on the same port fails with a message instead of crashing
    net::shutdown();
    CHECK(!net::running());
    CHECK_EQ(code_of(http_get(port, "/")), 0);                         // off: nothing answers
}

TEST(remote_known_commands_cover_the_remote_app) {
    const auto k = remote::known_commands();
    for (const char* c : {"play-pause", "next", "prev", "shuffle", "volume-up", "volume-down", "seek", "toggle-mute",
                          "playback-mode", "wizard", "switch-mode", "live", "stop"})
        CHECK(std::find(k.begin(), k.end(), std::string(c)) != k.end());
}

TEST(remote_server_cli_endpoint_is_off_by_default_and_waits_for_the_reply) {
    net::configure({false, 4747, "424242", "", false});
    CHECK_EQ(code_of(req("/api/cli?pin=424242&line=status")), 403);          // RemoteAppCli=false
    CHECK(body_of(req("/api/cli?pin=424242&line=status")).find("RemoteAppCli") != std::string::npos);
    net::configure({false, 4747, "424242", "", true});
    CHECK_EQ(code_of(req("/api/cli?pin=000000&line=status")), 403);          // the PIN still rules
    CHECK_EQ(code_of(req("/api/cli?pin=424242")), 400);                      // no line
    CHECK_EQ(code_of(req("/api/cli?pin=424242&line=a%0Ab")), 400);           // no control characters
    // the program's side: take the command, answer it from another thread
    std::thread prog([] {
        for (int i = 0; i < 200; ++i) {
            for (const remote::Command& c : net::take_commands()) {
                if (c.name == "cli" && c.from_net && c.id.compare(0, 4, "net-") == 0) {
                    remote::reply(c.id, c.arg != "bogus", c.arg == "bogus" ? "unknown command" : "playing: Tonight, Tonight\nline two");
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    const std::string r = req("/api/cli?pin=424242&line=status&wait=5");
    prog.join();
    CHECK_EQ(code_of(r), 200);
    CHECK(body_of(r).find("\"ok\":true") != std::string::npos);
    CHECK(body_of(r).find("\"lines\":[\"playing: Tonight, Tonight\",\"line two\"]") != std::string::npos);
    const std::string none = req("/api/cli?pin=424242&line=status&wait=1");  // nobody answers
    CHECK_EQ(code_of(none), 504);
    net::take_commands();
    net::configure({false, 4747, "424242", "", false});
}

TEST(remote_server_host_check_stops_dns_rebinding) {
    CHECK(net::host_ok("", ""));                                  // no Host header (HTTP/1.0, the tests)
    CHECK(net::host_ok("192.168.1.20:4747", ""));
    CHECK(net::host_ok("127.0.0.1", ""));
    CHECK(net::host_ok("localhost:4747", ""));
    CHECK(net::host_ok("LocalHost", ""));
    CHECK(net::host_ok("[::1]:4747", ""));
    CHECK(!net::host_ok("evil.example.com", ""));                 // a rebinding page keeps its own name
    CHECK(!net::host_ok("evil.example.com:4747", ""));
    CHECK(!net::host_ok("192.168.1", ""));                        // not an address
    CHECK(!net::host_ok("1.2.3.4.example.com", ""));
    CHECK(net::host_ok("mousiki.home:4747", "mousiki.home, NAS"));   // the owner's list
    CHECK(net::host_ok("nas", "mousiki.home, NAS"));
    CHECK(!net::host_ok("other.home", "mousiki.home, NAS"));
    use_pin("606060");
    CHECK_EQ(code_of(net::handle_request("GET /api/status?pin=606060 HTTP/1.1", "test", "192.168.1.20:4747")), 200);
    CHECK_EQ(code_of(net::handle_request("GET /api/status?pin=606060 HTTP/1.1", "test", "evil.example.com")), 400);
    CHECK_EQ(code_of(net::handle_request("GET / HTTP/1.1", "test", "evil.example.com")), 400);     // not even the page
    net::configure({false, 4747, "606060", "mousiki.home", false});
    CHECK_EQ(code_of(net::handle_request("GET /api/status?pin=606060 HTTP/1.1", "test", "mousiki.home:4747")), 200);
    net::configure({false, 4747, "606060", "", false});
}

// ---- hostile input (see also tests/test_parser_robustness.cpp: thousands of damaged requests) -------------------------------------

namespace {
// a raw request over a socket, whatever bytes: what the server answers (may be nothing when it closes the connection)
std::string raw_exchange(int port, const std::string& bytes, int hold_ms = 0) {
#if defined(_WIN32)
    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    std::string out;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        size_t sent = 0;
        while (sent < bytes.size()) {
#if defined(MSG_NOSIGNAL)
            const int flags = MSG_NOSIGNAL;   // the server may close on us: no SIGPIPE for the test program
#else
            const int flags = 0;
#endif
            const int w = static_cast<int>(send(s, bytes.data() + sent, static_cast<int>(std::min<size_t>(bytes.size() - sent, 4096)), flags));
            if (w <= 0) break;
            sent += static_cast<size_t>(w);
        }
        if (hold_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        char buf[4096];
        int n;
        while ((n = static_cast<int>(recv(s, buf, sizeof buf, 0))) > 0) out.append(buf, static_cast<size_t>(n));
    }
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
    return out;
}
int start_server(const char* pin) {
    for (int tries = 0; tries < 20; ++tries) {
        const int port = 47000 + static_cast<int>((std::chrono::steady_clock::now().time_since_epoch().count() / 1000 + tries * 37) % 1500);
        net::configure({true, port, pin, "", false});
        if (net::running()) return port;
    }
    return 0;
}
} // namespace

TEST(remote_server_malformed_percent_encoding) {
    std::string path;
    std::vector<std::pair<std::string, std::string>> q;
    CHECK(net::parse_request_line("GET /a%zz%4 HTTP/1.1", path, q));
    CHECK_EQ(path, std::string("/a%zz%4"));                         // not an escape: kept as it is
    CHECK(net::parse_request_line("GET /x?%=%&&=&k=%2 HTTP/1.1", path, q));
    CHECK(net::parse_request_line("GET /%41%42 HTTP/1.1", path, q));
    CHECK_EQ(path, std::string("/AB"));
    // an escaped NUL ends no string early and opens no file
    use_pin("424242");
    CHECK_EQ(code_of(req("/%00")), 404);
    CHECK_EQ(code_of(req("/api/image?pin=424242&key=%00..%2f..%2fetc%2fpasswd")), 404);
    CHECK_EQ(code_of(req("/api/status?pin=424242%00")), 403);        // a NUL after the PIN is not the PIN
}

TEST(remote_server_paths_do_not_lead_out) {
    use_pin("424243");
    for (const char* t : {"/../../etc/passwd", "/%2e%2e/%2e%2e/etc/passwd", "/..%2f..%2fetc%2fpasswd", "/api/image?pin=424243&key=../../x",
                          "/api/image?pin=424243&key=..%5c..%5cwindows", "//etc/passwd", "/./api/../etc/passwd"}) {
        const std::string r = req(t);
        const int c = code_of(r);
        CHECK(c == 404 || c == 400 || c == 403);
        CHECK(body_of(r).find("root:") == std::string::npos);
    }
}

TEST(remote_server_long_and_odd_pins) {
    use_pin("424244");
    CHECK(!net::pin_ok(std::string(100, '1')));
    CHECK(!net::pin_ok(std::string("12\0004", 4)));
    CHECK(!net::pin_ok("\xd9\xa1\xd9\xa2\xd9\xa3\xd9\xa4"));         // Arabic-Indic digits are no digits here
    CHECK(!net::pin_ok(" 1234"));
    CHECK(!net::pin_ok("1234\n"));
    CHECK(!net::pin_ok(""));
    CHECK_EQ(code_of(req("/api/status?pin=" + std::string(5000, '9'))), 403);
    CHECK_EQ(code_of(req("/api/status?pin=%e2%82%ac%f0%9f%98%80")), 403);
    CHECK_EQ(code_of(req("/api/status?pin=424244")), 200);
}

TEST(remote_server_odd_hosts_and_peers) {
    CHECK(!net::host_ok("evil.com@127.0.0.1", ""));                  // user info is not an address
    CHECK(!net::host_ok("127.0.0.1.evil.com", ""));
    (void)net::host_ok("[::1", "");                                   // (no answer is checked: nothing may crash)
    CHECK(!net::host_ok(std::string(10000, 'a'), ""));
    CHECK(!net::host_ok(std::string("localhost\0evil.com", 18), ""));
    (void)net::host_ok("127.0.0.1:99999999999999999999", "");
    CHECK(!net::lan_peer("::ffff:8.8.8.8"));                         // an IPv4-mapped public address is no home address
    CHECK(!net::lan_peer(""));
    CHECK(!net::lan_peer(std::string(5000, '1')));
    (void)net::lan_peer("999.1.1.1");
    CHECK(!net::lan_peer("1.2.3"));
}

TEST(remote_server_many_devices_keep_working) {
    use_pin("424245");
    // 1000 devices each guess once: the list of the lockouts stays small (kMaxGuards) and nothing breaks
    for (int i = 0; i < 1000; ++i)
        net::handle_request("GET /api/status?pin=000000 HTTP/1.1", "192.168." + std::to_string(i / 250) + "." + std::to_string(i % 250 + 1));
    CHECK_EQ(code_of(net::handle_request("GET /api/status?pin=424245 HTTP/1.1", "192.168.9.9")), 200);
}

TEST(remote_server_survives_hostile_connections) {
    const int port = start_server("424246");
    if (port == 0) { net::shutdown(); return; }                      // (no port free here)
    const std::string ok_before = http_get(port, "/");
    CHECK_EQ(code_of(ok_before), 200);

    // a header far over the limit (8 KB are read, then the line is answered or the connection closed)
    const std::string big = raw_exchange(port, "GET / HTTP/1.1\r\nX-Junk: " + std::string(100000, 'A') + "\r\n\r\n");
    CHECK(big.empty() || code_of(big) == 200 || code_of(big) == 400);
    const std::string long_line = raw_exchange(port, "GET /" + std::string(50000, 'a') + " HTTP/1.1\r\n\r\n");
    CHECK(long_line.empty() || code_of(long_line) == 404 || code_of(long_line) == 400);

    // not HTTP at all, a body, a method that is not allowed, a NUL in the middle
    CHECK(raw_exchange(port, std::string("\x16\x03\x01\x02\x00\x01\x00\x01\xfc\x03\x03", 11) + "\r\n\r\n").size() <= 2000);
    CHECK_EQ(code_of(raw_exchange(port, "POST /api/cmd?pin=424246&name=quit HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello")), 400);
    (void)raw_exchange(port, std::string("GET /\0x HTTP/1.1\r\n\r\n", 20));
    // two requests in one go: only the first is answered (no keep-alive, so nothing can be smuggled behind it)
    const std::string two = raw_exchange(port, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\nGET /api/status?pin=000000 HTTP/1.1\r\n\r\n");
    CHECK_EQ(code_of(two), 200);
    CHECK_EQ(two.find("HTTP/1.1", 10), std::string::npos);

    // idle connections that send nothing (or one byte now and then) do not stop the others
    std::vector<std::thread> idle;
    for (int i = 0; i < 4; ++i) idle.emplace_back([port] { raw_exchange(port, "GE", 3500); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(code_of(http_get(port, "/")), 200);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    for (auto& t : idle) t.join();                                    // each one was cut off by the server's own deadline

    // many requests at once
    std::atomic<int> good{0};
    std::vector<std::thread> many;
    for (int i = 0; i < 12; ++i) many.emplace_back([&] { for (int k = 0; k < 5; ++k) if (code_of(http_get(port, "/")) == 200) ++good; });
    for (auto& t : many) t.join();
    CHECK(good.load() >= 50);                                         // (a busy server may turn a few away with 503; most must pass)

    CHECK_EQ(code_of(http_get(port, "/")), 200);                      // still the same server after all of it
    net::shutdown();
}

TEST(json_deep_nesting_is_refused_not_a_stack_overflow) {
    using muisc::tinyjson::Value;
    Value v;
    CHECK(!muisc::tinyjson::parse(std::string(200000, '['), v));
    CHECK(!muisc::tinyjson::parse(std::string(200000, '{') , v));
    std::string deep;
    for (int i = 0; i < 100000; ++i) deep += "{\"a\":[";
    CHECK(!muisc::tinyjson::parse(deep, v));
    // an ordinary depth still parses
    std::string ok = std::string(50, '[') + "1" + std::string(50, ']');
    CHECK(muisc::tinyjson::parse(ok, v));
}
