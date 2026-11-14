// Gapless playback in the Player (player.h): a buffer handed over with queue_next() is played on from the very sample where the
// one that plays ends, on the same open device; take_switch() reports it once; clear_next() takes a buffer back before that.
// The device is miniaudio's null backend (MOUSIKI_NULL_AUDIO=1): it pulls blocks in real time without making a sound.
#include "test_framework.h"
#include "player.h"
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using namespace muisc;

namespace {

std::shared_ptr<StreamingPcm> tone_pcm(double seconds, float level) {
    auto p = std::make_shared<StreamingPcm>();
    p->reserve_for_seconds(seconds, 44100, 2);
    const size_t frames = static_cast<size_t>(seconds * 44100);
    std::vector<float> buf(frames * 2, level);
    p->append(buf.data(), frames);
    p->decode_done.store(true);
    return p;
}

void use_null_audio() {
#if defined(_WIN32)
    _putenv_s("MOUSIKI_NULL_AUDIO", "1");
#else
    setenv("MOUSIKI_NULL_AUDIO", "1", 1);
#endif
}

template <class F> bool wait_until(F f, double secs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(secs);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}

} // namespace

TEST(player_gapless_goes_on_into_the_next_buffer) {
    use_null_audio();
    Player p;
    auto a = tone_pcm(0.4, 0.25f), b = tone_pcm(0.8, 0.5f);
    CHECK(p.play(a, 0.0, 100));
    CHECK(wait_until([&] { return p.queue_next(b); }, 1.0));
    CHECK(p.has_next());
    bool switched = false;
    CHECK(wait_until([&] { switched = switched || p.take_switch(); return switched; }, 2.0));
    CHECK(!p.has_next());
    CHECK(!p.finished());                 // the end of the first one is never reported: the next one plays
    CHECK(p.poll_elapsed() < 0.5);        // the position is in the new buffer, from its start
    CHECK(!p.take_switch());              // reported once
    CHECK(wait_until([&] { return p.finished(); }, 2.0));   // and the second one ends as usual
    p.stop();
}

TEST(player_gapless_buffer_taken_back_is_not_played) {
    use_null_audio();
    Player p;
    auto a = tone_pcm(0.3, 0.25f), b = tone_pcm(0.5, 0.5f);
    CHECK(p.play(a, 0.0, 100));
    CHECK(wait_until([&] { return p.queue_next(b); }, 1.0));
    int r = 0;
    CHECK(wait_until([&] { r = p.clear_next(); return r != 0; }, 1.0));
    CHECK_EQ(r, 1);
    CHECK(wait_until([&] { return p.finished(); }, 2.0));   // the first one simply ends
    CHECK(!p.take_switch());
    p.stop();
}

TEST(player_gapless_needs_the_same_sample_rate) {
    use_null_audio();
    Player p;
    auto a = tone_pcm(0.5, 0.25f);
    auto b = std::make_shared<StreamingPcm>();
    b->reserve_for_seconds(0.5, 48000, 2);
    CHECK(p.play(a, 0.0, 100));
    bool ok = true;
    for (int i = 0; i < 50 && ok; ++i) { ok = !p.queue_next(b); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    CHECK(ok);                            // refused (a 48 kHz buffer on a 44.1 kHz device)
    CHECK(!p.has_next());
    p.stop();
}
