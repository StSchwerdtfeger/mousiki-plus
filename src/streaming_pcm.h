// StreamingPcm: the decoded audio of a track while it is still being decoded, so playback can start
// before the file is finished.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>
#include "loudness_meter.h"

namespace muisc {

// A growing, single-producer/single-consumer PCM buffer: one decode
// thread appends to it while the audio callback (and, once decode
// finishes, the waveform pass) read whatever's been decoded so far.
//
// Capacity is reserved up front from ffprobe's duration estimate, and
// append() DELIBERATELY REFUSES to grow past that reserved capacity
// (it silently clamps instead of reallocating). That's not laziness —
// it's what makes the read side safe without a lock: once `available`
// is published (release-store), every element up to that count is
// guaranteed already written AND the vector's backing pointer is
// guaranteed to never have moved, because it never reallocates. A
// reader that does `size_t n = available.load(acquire); read data[0..n)`
// is race-free by construction. The tradeoff is a duration estimate
// that's wildly wrong (rare — ffprobe reads the container header, it's
// usually right) truncates the last bit of a track rather than risking
// a use-after-free on a mid-playback reallocation touched by another
// thread. That trade is worth it here.
struct StreamingPcm {
    // Interleaved samples: frame f, channel c lives at data[f * channels + c].
    // `channels` is 1 (mono) or 2 (stereo) and must be set (via
    // reserve_for_seconds) before decoding starts; it never changes afterwards.
    // Everything else that talks about positions -- `available`, the
    // player's cursor, seeking -- counts FRAMES, so a stereo track and a mono
    // track of the same length have the same numbers.
    std::vector<float> data;
    std::atomic<size_t> available{0};   // FRAMES safe to read right now
    std::atomic<bool> decode_done{false};
    std::atomic<bool> decode_failed{false};
    std::atomic<bool> capacity_exceeded{false}; // diagnostic only
    int sample_rate = 44100;
    int channels = 1;

    // Integrated loudness (LUFS) of the track, measured while it decodes --
    // see loudness_meter.h. NaN until enough audio has been analysed
    // (kMinAnalysisSeconds), then refreshed as decoding continues and
    // frozen by finalize_loudness(). Decode decides nothing about playback
    // gain itself; Player reads this and derives the normalisation gain.
    //
    //   loudness_lufs       -- the track as it is stored (stereo if channels==2)
    //   loudness_mono_lufs  -- the same track folded down to mono, which is
    //                          what plays when "stereo" is switched off while
    //                          a stereo track is loaded. (Identical to
    //                          loudness_lufs for a mono buffer.)
    std::atomic<float> loudness_lufs{std::numeric_limits<float>::quiet_NaN()};
    std::atomic<float> loudness_mono_lufs{std::numeric_limits<float>::quiet_NaN()};
    std::atomic<bool> loudness_final{false};   // decode ended -- lufs won't change any more
    static constexpr double kMinAnalysisSeconds = 15.0;
    LoudnessMeter meter;                       // decode thread only
    LoudnessMeter meter_mono;                  // decode thread only; fed only when channels == 2

    // ---- Windowed mode (long / big tracks) ------------------------------------------------------------------------
    // A whole track as 32-bit float is 8 bytes per stereo frame: an 80-minute file at 48 kHz is 1.8 GB. Above
    // the limit (kWindowBytes unless the user changed it) the buffer instead holds a window of kWindowSeconds around the playing position (a ring): the
    // decoder stays up to 3/4 of it ahead and keeps 1/4 behind (a short step back needs no new decoding), and a seek
    // outside it moves the decoder there (stream_decode_ffmpeg). The audio itself is exactly the same -- same rate,
    // same 32-bit samples -- only less of it is held at once. Loudness and the waveform then come from a separate
    // pass over the whole file that keeps nothing but the figures (analyse_track).
    //
    // Positions stay ABSOLUTE frame numbers in both modes; in windowed mode frame f lives at ring slot f % ring_frames
    // and only [start, available) is held.
    static constexpr double kWindowBytes = 256.0 * 1024 * 1024;   // the default; Settings -> ON/OFF -> MAIN UI / AUDIO -> Track Buffer (config.txt: TrackBufferMB)
    static constexpr double kWindowSeconds = 120.0;
    bool windowed = false;
    size_t ring_frames = 0;
    std::atomic<size_t> start{0};                   // first frame held (windowed; SIZE_MAX while moving)
    std::atomic<long long> play_pos{0};             // the player's position (windowed: tells the decoder where to be)
    std::atomic<size_t> total_frames{0};            // exact length once known (0 = not yet)
    size_t est_frames = 0;                          // estimated length (duration probe)
    // waveform figures of the analysis pass (windowed): sum of squares of the mono fold per kEnvGran frames
    static constexpr size_t kEnvGran = 256;
    std::vector<float> env_sumsq;                   // written by the analysis pass; read after analysis_done
    std::atomic<bool> analysis_done{false};
    std::atomic<bool> cancelled{false};             // the file must be let go now (a cut, a rename): the decode threads end and close it
    std::atomic<long> internal_refs{0};             // shared_ptr holders that are the decode threads themselves

    // limit_bytes: the size of the decoded audio (without the headroom below) above which the window is used instead.
    void reserve_for_seconds(double seconds, int sr, int ch = 1, double limit_bytes = kWindowBytes) {
        sample_rate = sr;
        channels = ch >= 2 ? 2 : 1;
        meter.reset(sr, channels);
        meter_mono.reset(sr, 1);
        est_frames = static_cast<size_t>(std::max(1.0, seconds) * sr);
        size_t est = static_cast<size_t>(std::max(1.0, seconds) * sr * 1.25); // 25% headroom, in frames
        const double bytes = static_cast<double>(est) * channels * sizeof(float);
        // The headroom (25 %) is only for the reservation: the estimate comes from the file header and can be too low, and the
        // buffer must never grow after the fact (see the top of this struct). It is address space that is reserved, not
        // memory in use: the pages are only touched as the decoder fills them.
        const double audio_bytes = static_cast<double>(est_frames) * channels * sizeof(float);
        (void)bytes;
        if (audio_bytes > limit_bytes && static_cast<double>(est_frames) > kWindowSeconds * sr * 1.5) {
            windowed = true;
            ring_frames = static_cast<size_t>(kWindowSeconds * sr);
            data.assign(ring_frames * static_cast<size_t>(channels), 0.0f);
            return;
        }
        data.reserve(std::max<size_t>(est, static_cast<size_t>(sr) * 5) * static_cast<size_t>(channels));
    }

    // Capacity in frames (what seeking is clamped against).
    size_t capacity_frames() const { return data.capacity() / static_cast<size_t>(channels); }
    // How far the player may seek (frames).
    size_t seek_limit_frames() const {
        if (!windowed) return capacity_frames();
        const size_t t = total_frames.load(std::memory_order_acquire);
        return t > 0 ? t : est_frames + static_cast<size_t>(sample_rate) * 5;
    }
    // Sample index of frame f (interleaved, channel 0).
    size_t slot(size_t f) const { return (windowed ? f % ring_frames : f) * static_cast<size_t>(channels); }
    // Player: the frames that can be read now, [lo, hi). A seek in progress reads as nothing.
    void readable(size_t& lo, size_t& hi) const {
        if (!windowed) { lo = 0; hi = available.load(std::memory_order_acquire); return; }
        const size_t s1 = start.load(std::memory_order_acquire);
        hi = available.load(std::memory_order_acquire);
        const size_t s2 = start.load(std::memory_order_acquire);
        lo = s1;
        if (s1 != s2 || s1 == SIZE_MAX || hi < s1) lo = hi;
    }

    // Windowed decode thread: where the decoder must go next. Returns true (and the frame) when the player's position
    // is outside what is held / being filled (a seek, or the start of a track resumed somewhere in the middle).
    bool need_seek(size_t& to) const {
        const long long p = std::max(0ll, play_pos.load(std::memory_order_relaxed));
        const size_t s = start.load(std::memory_order_relaxed), a = available.load(std::memory_order_relaxed);
        const size_t t = total_frames.load(std::memory_order_relaxed);
        const size_t up = static_cast<size_t>(p);
        if (t > 0 && up >= t) return false;   // past the end: nothing to fetch
        if (up < s || up > a + static_cast<size_t>(sample_rate) * 2) { to = up; return true; }
        return false;
    }
    // Windowed decode thread: the decoder now continues at frame `p`; nothing old is held any more.
    void reposition(size_t p) {
        decode_done.store(false, std::memory_order_relaxed);
        start.store(SIZE_MAX, std::memory_order_release);
        available.store(p, std::memory_order_release);
        start.store(p, std::memory_order_release);
    }
    // Windowed decode thread: store frames at the end of the window. Waits while the window is full ahead of the
    // player. Returns false (the rest not stored) when the decoder must move (need_seek) or give up (`abandoned`).
    template <class Abandoned>
    bool write_window(const float* samples, size_t frames, const Abandoned& abandoned) {
        const size_t ch = static_cast<size_t>(channels);
        const size_t ahead = ring_frames - ring_frames / 4;
        size_t done = 0;
        while (done < frames) {
            size_t to;
            if (need_seek(to) || abandoned()) return false;
            const size_t a = available.load(std::memory_order_relaxed);
            const long long p = std::max(0ll, play_pos.load(std::memory_order_relaxed));
            const size_t room_to = static_cast<size_t>(p) + ahead;
            if (a >= room_to) { std::this_thread::sleep_for(std::chrono::milliseconds(15)); continue; }
            size_t n = std::min(frames - done, room_to - a);
            n = std::min(n, ring_frames - a % ring_frames);   // up to the end of the ring
            // the frames about to be overwritten are no longer held -- say so BEFORE writing over them
            const size_t new_end = a + n;
            if (new_end > ring_frames) {
                const size_t s = start.load(std::memory_order_relaxed);
                if (new_end - ring_frames > s) start.store(new_end - ring_frames, std::memory_order_release);
            }
            std::copy(samples + done * ch, samples + (done + n) * ch, data.begin() + static_cast<long>((a % ring_frames) * ch));
            available.store(new_end, std::memory_order_release);
            done += n;
        }
        return true;
    }
    // Analysis pass (windowed): loudness and waveform figures of a stretch of the track, in order from its start.
    void analyse(const float* samples, size_t frames) {
        feed_meters(samples, frames);
        const size_t ch = static_cast<size_t>(channels);
        for (size_t i = 0; i < frames; ++i) {
            const float m = ch == 2 ? 0.5f * (samples[2 * i] + samples[2 * i + 1]) : samples[i];
            env_acc_ += static_cast<double>(m) * m;
            if (++env_n_ == kEnvGran) { env_sumsq.push_back(static_cast<float>(env_acc_)); env_acc_ = 0; env_n_ = 0; }
        }
    }
    void analyse_end() {
        if (env_n_ > 0) env_sumsq.push_back(static_cast<float>(env_acc_));
        env_acc_ = 0; env_n_ = 0;
    }

    // Decode thread only. `frames` interleaved frames.
    void append(const float* samples, size_t frames) {
        const size_t ch = static_cast<size_t>(channels);
        const size_t room = (data.capacity() - data.size()) / ch;
        const size_t n = std::min(frames, room);
        if (n > 0) {
            const size_t before = data.size();
            data.insert(data.end(), samples, samples + n * ch);
            available.store(data.size() / ch, std::memory_order_release);
            // Measured from the stored copy; capacity is fixed so the
            // pointer is stable (see the class comment).
            feed_meters(data.data() + before, n);
        }
        if (n < frames) capacity_exceeded.store(true, std::memory_order_relaxed);
    }

    void feed_meters(const float* src0, size_t n) {
        meter.push(src0, n);
        if (channels == 2) {
            float mix[1024];
            for (size_t done = 0; done < n;) {
                const size_t m = std::min<size_t>(1024, n - done);
                const float* src = src0 + done * 2;
                for (size_t i = 0; i < m; ++i) mix[i] = 0.5f * (src[2 * i] + src[2 * i + 1]);
                meter_mono.push(mix, m);
                done += m;
            }
        }
        if (meter.take_updated() && meter.seconds_analyzed() >= kMinAnalysisSeconds) publish_loudness();
    }

    // Decode thread, once decoding has ended (success or failure): publish
    // the final figure -- including for tracks shorter than
    // kMinAnalysisSeconds, which never got an early estimate -- and tell
    // anyone waiting on it (Player::play) there is nothing more to wait for.
    void finalize_loudness() {
        publish_loudness();
        loudness_final.store(true, std::memory_order_release);
    }

private:
    double env_acc_ = 0;
    size_t env_n_ = 0;
    void publish_loudness() {
        const double l = meter.integrated_lufs();
        if (std::isnan(l)) return;
        loudness_lufs.store(static_cast<float>(l), std::memory_order_relaxed);
        const double lm = (channels == 2) ? meter_mono.integrated_lufs() : l;
        loudness_mono_lufs.store(static_cast<float>(std::isnan(lm) ? l : lm), std::memory_order_relaxed);
    }
};

} // namespace muisc
