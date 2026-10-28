// Microphone input for the visualisations: a lock-free stereo ring buffer that the capture thread
// fills and the visualisers read.
#include "mic_capture.h"

#include <algorithm>
#include <cstring>

#include "miniaudio.h"

namespace muisc {

StereoRing::StereoRing(size_t frames) : buf_((frames + 1) * 2, 0.0f), cap_(frames + 1) {}

size_t StereoRing::available() const {
    const size_t w = w_.load(std::memory_order_acquire), r = r_.load(std::memory_order_acquire);
    return (w + cap_ - r) % cap_;
}

size_t StereoRing::write(const float* lr, size_t frames) {
    const size_t w = w_.load(std::memory_order_relaxed), r = r_.load(std::memory_order_acquire);
    const size_t free_frames = (r + cap_ - w - 1) % cap_;
    const size_t n = std::min(frames, free_frames);
    if (n < frames) dropped_.fetch_add(frames - n, std::memory_order_relaxed);
    const size_t first = std::min(n, cap_ - w);
    std::memcpy(&buf_[w * 2], lr, first * 2 * sizeof(float));
    if (n > first) std::memcpy(&buf_[0], lr + first * 2, (n - first) * 2 * sizeof(float));
    w_.store((w + n) % cap_, std::memory_order_release);
    return n;
}

size_t StereoRing::read(float* lr, size_t max_frames) {
    const size_t r = r_.load(std::memory_order_relaxed), w = w_.load(std::memory_order_acquire);
    const size_t avail = (w + cap_ - r) % cap_;
    const size_t n = std::min(max_frames, avail);
    const size_t first = std::min(n, cap_ - r);
    std::memcpy(lr, &buf_[r * 2], first * 2 * sizeof(float));
    if (n > first) std::memcpy(lr + first * 2, &buf_[0], (n - first) * 2 * sizeof(float));
    r_.store((r + n) % cap_, std::memory_order_release);
    return n;
}

struct MicCapture::Impl {
    StereoRing ring{1u << 17};      // about 3 s at 44.1 kHz
    ma_device device{};
    bool started = false;
    int rate = 44100;
    std::string name;
    static void callback(ma_device* dev, void*, const void* input, ma_uint32 frames) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        if (input && frames) self->ring.write(static_cast<const float*>(input), frames);
    }
};

MicCapture::MicCapture() : impl_(new Impl) {}
MicCapture::~MicCapture() { stop(); }

bool MicCapture::start(int rate, std::string& error) {
    stop();
    ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 2;                 // a mono microphone is duplicated by miniaudio
    cfg.sampleRate = static_cast<ma_uint32>(std::clamp(rate, 8000, 96000));
    cfg.dataCallback = Impl::callback;
    cfg.pUserData = impl_.get();
    const ma_result init = ma_device_init(nullptr, &cfg, &impl_->device);
    if (init != MA_SUCCESS) {
        error = std::string("no microphone could be opened (") + ma_result_description(init) + ")";
        return false;
    }
    const ma_result run = ma_device_start(&impl_->device);
    if (run != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        error = std::string("the microphone could not be started (") + ma_result_description(run) + ")";
        return false;
    }
    impl_->started = true;
    impl_->rate = static_cast<int>(impl_->device.sampleRate);
    impl_->name = impl_->device.capture.name;
    float drain[512];
    while (impl_->ring.read(drain, 256) > 0) {}   // nothing from before
    return true;
}

void MicCapture::stop() {
    if (impl_ && impl_->started) {
        ma_device_uninit(&impl_->device);
        impl_->started = false;
    }
}

bool MicCapture::running() const { return impl_ && impl_->started; }
int MicCapture::rate() const { return impl_->rate; }
std::string MicCapture::device_name() const { return impl_->name; }

size_t MicCapture::read(std::vector<float>& out, size_t max_frames) {
    const size_t n = std::min(max_frames, impl_->ring.available());
    if (n == 0) return 0;
    const size_t at = out.size();
    out.resize(at + n * 2);
    return impl_->ring.read(out.data() + at, n);
}

} // namespace muisc
