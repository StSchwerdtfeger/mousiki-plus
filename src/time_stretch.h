// Playback speed without a change of pitch (the audio book player's 0.5x - 3.0x): WSOLA, "waveform similarity overlap-add".
//
// The output is built from short overlapping pieces ("grains", about 40 ms) of the source, each faded in and out with a Hann
// window and laid over the one before by half its length. For a speed s the grains are taken s times as far apart in the
// source as they are laid down in the output, so the audio runs s times as fast, while every grain keeps its own frequencies:
// the voice keeps its pitch. To avoid the rough, phasy sound of a plain overlap-add, every grain is moved by up to ~12 ms to the
// place where it fits best onto the one before (the highest normalised cross-correlation with the natural continuation of the
// previous grain); speech then stays clear.
//
// The source is read through two functions, so this works on any buffer that can be read at random positions (the player's
// StreamingPcm, also the windowed one, and the tests' plain arrays):
//     avail(i0, i1) -> bool          every frame in [i0, i1) can be read now (frames outside the track count as silence)
//     sample(i, ch) -> float         channel ch (0 / 1) of frame i (0 outside the track)
// configure() sizes every buffer and must run while the audio device is stopped; reset() and next() never allocate, so they are
// safe on the real-time audio thread.
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace muisc {

class TimeStretch {
public:
    static constexpr double kMinSpeed = 0.5;
    static constexpr double kMaxSpeed = 3.0;

    // Grain and search sizes for this sample rate. Not on the audio thread (allocates).
    void configure(int sample_rate) {
        const int sr = std::max(8000, sample_rate);
        n_ = std::max(64, static_cast<int>(0.040 * sr) / 2 * 2);   // grain length, even
        ha_ = n_ / 2;                                               // output hop = overlap
        delta_ = std::max(8, static_cast<int>(0.012 * sr));         // how far a grain may move to fit
        win_.resize(static_cast<size_t>(n_));
        constexpr double kPi = 3.14159265358979323846;
        for (int j = 0; j < n_; ++j) win_[static_cast<size_t>(j)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * j / n_));   // periodic Hann: two at half overlap add up to 1
        ola_l_.assign(static_cast<size_t>(n_), 0.0f);
        ola_r_.assign(static_cast<size_t>(n_), 0.0f);
        nat_.assign(static_cast<size_t>(ha_), 0.0f);
        cand_.assign(static_cast<size_t>(ha_ + 2 * delta_ + 1), 0.0f);
        reset(0.0);
    }

    // Start over at source frame `src_pos` (a new track, a seek, the speed coming back from 1.0). Audio thread safe.
    void reset(double src_pos) {
        std::fill(ola_l_.begin(), ola_l_.end(), 0.0f);
        std::fill(ola_r_.begin(), ola_r_.end(), 0.0f);
        nominal_ = src_pos;
        prev_ = static_cast<long long>(std::llround(src_pos));
        have_prev_ = false;
        out_ = ha_;   // the first next() lays down a grain
        cur_speed_ = 1.0;
    }

    // One output frame (left / right) at `speed`. False (and silence) when the source frames for the next grain are not
    // there yet: nothing moves on then, the same frame is asked for again later.
    template <class Avail, class Sample>
    bool next(double speed, Avail&& avail, Sample&& sample, float& l, float& r) {
        if (n_ == 0) { l = r = 0.0f; return false; }
        if (out_ >= ha_) {
            speed = std::clamp(speed, kMinSpeed, kMaxSpeed);
            const long long target = static_cast<long long>(std::llround(nominal_));
            const long long nat = prev_ + ha_;   // where the previous grain would naturally go on
            long long lo = target - delta_, hi = target + delta_ + n_;
            if (have_prev_) { lo = std::min(lo, nat); hi = std::max(hi, nat + ha_); }
            if (!avail(lo, hi)) { l = r = 0.0f; return false; }
            const long long s = have_prev_ ? best_start(target, nat, sample) : target;
            // the second half of the last grain moves to the front; the new grain is added over it
            for (int j = 0; j < ha_; ++j) {
                ola_l_[static_cast<size_t>(j)] = ola_l_[static_cast<size_t>(j + ha_)];
                ola_r_[static_cast<size_t>(j)] = ola_r_[static_cast<size_t>(j + ha_)];
            }
            std::fill(ola_l_.begin() + ha_, ola_l_.end(), 0.0f);
            std::fill(ola_r_.begin() + ha_, ola_r_.end(), 0.0f);
            for (int j = 0; j < n_; ++j) {
                const float w = win_[static_cast<size_t>(j)];
                ola_l_[static_cast<size_t>(j)] += w * sample(s + j, 0);
                ola_r_[static_cast<size_t>(j)] += w * sample(s + j, 1);
            }
            prev_ = s;
            have_prev_ = true;
            cur_speed_ = speed;
            nominal_ += ha_ * speed;
            out_ = 0;
        }
        l = ola_l_[static_cast<size_t>(out_)];
        r = ola_r_[static_cast<size_t>(out_)];
        ++out_;
        return true;
    }

    // The source position (in frames) of what is being heard now: moves on smoothly at `speed` frames per output frame.
    double heard_pos() const { return nominal_ - ha_ * cur_speed_ + out_ * cur_speed_; }

    int grain() const { return n_; }

private:
    // The start within target +/- delta whose first half-grain looks most like the natural continuation of the previous grain.
    template <class Sample>
    long long best_start(long long target, long long nat, Sample& sample) {
        auto mono = [&](long long i) { return 0.5f * (sample(i, 0) + sample(i, 1)); };
        for (int j = 0; j < ha_; ++j) nat_[static_cast<size_t>(j)] = mono(nat + j);
        const int span = ha_ + 2 * delta_;
        for (int k = 0; k <= span; ++k) cand_[static_cast<size_t>(k)] = mono(target - delta_ + k);
        // score(d) = <nat, cand at d> / |cand at d|: the best fit in shape, whatever the loudness
        auto score = [&](int d, int step) {
            double num = 0.0, en = 1e-9;
            const float* c = cand_.data() + (d + delta_);
            for (int j = 0; j < ha_; j += step) { num += nat_[static_cast<size_t>(j)] * c[j]; en += c[j] * c[j]; }
            return num / std::sqrt(en);
        };
        constexpr int kCoarse = 4;   // first every 4th lag on every 4th sample, then the lags around the best one exactly
        int best = 0;
        double best_score = -1e300;
        for (int d = -delta_; d <= delta_; d += kCoarse) {
            const double sc = score(d, kCoarse);
            if (sc > best_score) { best_score = sc; best = d; }
        }
        const int lo = std::max(-delta_, best - kCoarse + 1), hi = std::min(delta_, best + kCoarse - 1);
        best_score = -1e300;
        int fine = best;
        for (int d = lo; d <= hi; ++d) {
            const double sc = score(d, 2);
            if (sc > best_score) { best_score = sc; fine = d; }
        }
        return target + fine;
    }

    int n_ = 0, ha_ = 0, delta_ = 0;
    std::vector<float> win_, ola_l_, ola_r_, nat_, cand_;
    double nominal_ = 0.0;      // the nominal source start of the next grain
    long long prev_ = 0;        // the actual source start of the last grain
    bool have_prev_ = false;
    int out_ = 0;               // frames of the current grain handed out
    double cur_speed_ = 1.0;    // the speed of the current grain
};

// "1.0x", "1.25x", "0.5x": the shortest way to write a speed (two decimals at most, one at least).
inline std::string speed_label(double speed) {
    const int hundredths = static_cast<int>(std::lround(speed * 100.0));
    std::string s = std::to_string(hundredths / 100) + "." + std::to_string(hundredths % 100 / 10);
    if (hundredths % 10 != 0) s += std::to_string(hundredths % 10);
    return s + "x";
}

} // namespace muisc
