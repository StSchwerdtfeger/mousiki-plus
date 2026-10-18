#pragma once
// Microphone input for the creator (the ^ overlay, "Microphone" mode): the default capture device through miniaudio,
// delivered to the UI thread as interleaved stereo frames. A mono microphone arrives as the same signal on both channels.
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace muisc {

// A lock-free ring of stereo frames (interleaved L, R) for ONE producer thread (the audio callback) and ONE consumer (the UI).
// The producer never waits: when the consumer is too slow the newest frames are dropped and counted.
class StereoRing {
public:
    explicit StereoRing(size_t frames);
    size_t write(const float* lr, size_t frames);   // producer; returns the frames stored
    size_t read(float* lr, size_t max_frames);      // consumer; returns the frames copied
    size_t available() const;                       // frames waiting
    size_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    size_t capacity() const { return cap_ - 1; }
private:
    std::vector<float> buf_;
    size_t cap_;                                    // frames in buf_ (one stays free to tell full from empty)
    std::atomic<size_t> w_{0}, r_{0};
    std::atomic<size_t> dropped_{0};
};

class MicCapture {
public:
    MicCapture();
    ~MicCapture();
    MicCapture(const MicCapture&) = delete;
    MicCapture& operator=(const MicCapture&) = delete;
    // Opens the default microphone at about `rate` Hz and starts capturing. False with a short reason in `error` (no device,
    // access refused ...).
    bool start(int rate, std::string& error);
    void stop();
    bool running() const;
    int rate() const;                                // the rate of the frames read() gives (after start())
    std::string device_name() const;
    // New frames since the last call, interleaved L/R, appended to `out` (at most `max_frames`). Returns the frames added.
    size_t read(std::vector<float>& out, size_t max_frames = 1u << 16);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace muisc
