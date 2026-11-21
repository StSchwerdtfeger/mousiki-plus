// The audio player: plays a StreamingPcm buffer through miniaudio, with volume, pause, seeking and a
// soft limiter at the output.
#include "player.h"
#include "milkdrop.h"
#include "scope_window.h"
#include "spectrogram.h"
#include "audio_backend.h"
#include "console_log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace muisc {

namespace {

// Transparent below the knee, then eases into +/-1.0 with a continuous
// slope instead of hard-clipping. Only used while normalisation is on, where
// boosting a quiet track could otherwise push its peaks over full scale.
inline float soft_limit(float x) {
    constexpr float kKnee = 0.891f; // -1 dBFS
    const float a = std::fabs(x);
    if (a <= kKnee) return x;
    const float y = kKnee + (1.0f - kKnee) * std::tanh((a - kKnee) / (1.0f - kKnee));
    return x < 0.0f ? -y : y;
}

} // namespace

Player::Player() = default;
Player::~Player() { stop(); }

namespace {
// Where the track ends, in frames, once that is known (-1: still being decoded / the window has not reached the end yet).
long long known_end_frames(StreamingPcm& pcm) {
    if (pcm.windowed) {
        size_t lo = 0, hi = 0;
        pcm.readable(lo, hi);
        const size_t total = pcm.total_frames.load(std::memory_order_acquire);
        return total > 0 && hi >= total ? static_cast<long long>(total) : -1;
    }
    return pcm.decode_done.load() ? static_cast<long long>(pcm.available.load(std::memory_order_acquire)) : -1;
}
} // namespace

namespace {
// ---- song fades (audio_fade.h) ----
// The first frame to play / one past the last one of a track: with Trim Silence the measured ones (a windowed track: the start from the window
// at its beginning, the end from the peaks of the analysis pass), else 0 / the end of the track. -1: the end is not known yet.
long long eff_start(const FadeSnap& f, const StreamingPcm& p) {
    if (!f.on || !f.trim || p.trim_thr_db.load(std::memory_order_acquire) != f.thr_db) return 0;
    return std::max<long long>(0, p.trim_start.load(std::memory_order_acquire));
}
long long eff_end(const FadeSnap& f, StreamingPcm& p) {
    if (f.on && f.trim && p.trim_thr_db.load(std::memory_order_acquire) == f.thr_db) {
        const long long e = p.trim_end.load(std::memory_order_acquire);
        if (e >= 0) return e;
    }
    return known_end_frames(p);
}

// Measures the silence at the two ends of a decoded track (once per threshold). Any thread; reads only frames that are published.
void analyse_trim(StreamingPcm& p, const FadeSnap& f) {
    if (!f.on || !f.trim) return;
    if (p.trim_thr_db.load(std::memory_order_acquire) == f.thr_db && p.trim_start.load() >= 0 && p.trim_end.load() >= 0) return;
    if (p.trim_thr_db.load(std::memory_order_acquire) != f.thr_db) { p.trim_start.store(-1); p.trim_end.store(-1); }
    const int ch = p.channels >= 2 ? 2 : 1, rate = std::max(1, p.sample_rate);
    const size_t cap = static_cast<size_t>(fade::kMaxTrimSeconds * rate);
    if (p.windowed) {
        // A long track: its beginning is read from the window while that still holds frame 0 (if the playing position has moved on, or a
        // seek came first, the start is not cut); its end from the peaks that the analysis pass keeps, once that has been through the file.
        if (p.trim_start.load() < 0) {
            size_t lo = 0, hi = 0;
            p.readable(lo, hi);
            if (lo != 0 || p.play_pos.load(std::memory_order_relaxed) > static_cast<long long>(cap)) p.trim_start.store(0);
            else if (hi > 0) {
                const long long l = fade::scan_lead(p.data.data(), ch, std::min(hi, cap), rate, f.thr);
                size_t lo2 = 0, hi2 = 0;
                p.readable(lo2, hi2);
                if (lo2 != 0) p.trim_start.store(0);   // (the window slid while it was read)
                else if (l >= 0) p.trim_start.store(fade::trimmed_start(l, rate));
                else if (hi >= cap || p.decode_done.load(std::memory_order_acquire)) p.trim_start.store(0);
            }
        }
        if (p.trim_end.load() < 0 && p.analysis_done.load(std::memory_order_acquire)) {
            const size_t total = p.total_frames.load(std::memory_order_acquire) > 0 ? p.total_frames.load(std::memory_order_acquire)
                                                                                      : p.env_peak.size() * StreamingPcm::kEnvGran;
            const long long t = fade::scan_tail_peaks(p.env_peak.data(), p.env_peak.size(), StreamingPcm::kEnvGran, total, rate, f.thr);
            long long e = fade::trimmed_end(t, total, rate);
            const long long s0 = std::max<long long>(0, p.trim_start.load());
            if (total == 0 || e <= s0 + rate / 10) e = static_cast<long long>(total);   // (nothing left to play: not cut)
            p.trim_end.store(e);
        }
        if (p.trim_start.load() >= 0) p.trim_thr_db.store(f.thr_db, std::memory_order_release);
        return;
    }
    const size_t avail = p.available.load(std::memory_order_acquire);
    const bool done = p.decode_done.load(std::memory_order_acquire);
    if (p.trim_start.load() < 0) {
        const long long l = fade::scan_lead(p.data.data(), ch, std::min(avail, cap), rate, f.thr);
        if (l >= 0) p.trim_start.store(fade::trimmed_start(l, rate));
        else if (done || avail >= cap) p.trim_start.store(0);   // (nothing audible in that stretch: not cut)
    }
    if (p.trim_end.load() < 0 && done) {
        const long long t = fade::scan_tail(p.data.data(), ch, avail, rate, f.thr);
        long long e = fade::trimmed_end(t, avail, rate);
        const long long s0 = std::max<long long>(0, p.trim_start.load());
        if (e <= s0 + rate / 10) e = static_cast<long long>(avail);   // (nothing left to play: not cut)
        p.trim_end.store(e);
    }
    p.trim_thr_db.store(f.thr_db, std::memory_order_release);
}

// A crossfade from `A` (ends at endA) into `B`: where it starts, how long it is and where `B` starts. on = false: no crossfade.
struct XfPlan { bool on = false; long long t0 = 0, len = 0, sB = 0; };
XfPlan plan_xf(const FadeSnap& f, StreamingPcm& A, long long startA, long long endA, StreamingPcm* B) {
    XfPlan x;
    if (!f.on || !f.cross || !B || endA < 0) return x;
    const long long sB = eff_start(f, *B), endB = eff_end(f, *B);
    long long len = std::min<long long>(f.len, (endA - startA) / 2);
    if (endB >= 0) len = std::min<long long>(len, (endB - sB) / 2);
    if (len < std::max(1, A.sample_rate) / 20) return x;   // (less than 50 ms: no crossfade)
    x.on = true; x.len = len; x.t0 = endA - len; x.sB = sB;
    return x;
}
} // namespace

namespace {
// Tests only (MOUSIKI_AUDIO_TAP=<file> together with MOUSIKI_NULL_AUDIO=1): every block that goes to the (silent) device is appended to
// <file> as float32 stereo, and <file>.t gets one line per block: the steady-clock time in microseconds and the number of frames. That is
// how the UI tests measure the gap between two tracks (the samples between them, and the time the device was closed).
struct AudioTap {
    std::FILE* pcm = nullptr;
    std::FILE* times = nullptr;
    AudioTap() {
        const char* n = std::getenv("MOUSIKI_NULL_AUDIO");
        const char* p = std::getenv("MOUSIKI_AUDIO_TAP");
        if (!n || *n != '1' || !p || !*p) return;
        pcm = std::fopen(p, "wb");
        times = std::fopen((std::string(p) + ".t").c_str(), "w");
    }
    void write(const float* out, ma_uint32 n) {
        if (!pcm) return;
        std::fwrite(out, sizeof(float), 2 * static_cast<size_t>(n), pcm);
        if (times) std::fprintf(times, "%lld %u\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                       std::chrono::steady_clock::now().time_since_epoch()).count()), n);
        std::fflush(pcm);
        if (times) std::fflush(times);
    }
};
AudioTap& audio_tap() { static AudioTap t; return t; }
} // namespace

void Player::data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    data_callback_impl(device, output, input, frame_count);
    audio_tap().write(static_cast<const float*>(output), frame_count);
}

void Player::data_callback_impl(ma_device* device, void* output, const void* /*input*/, ma_uint32 frame_count) {
    Player* self = static_cast<Player*>(device->pUserData);
    float* out = static_cast<float*>(output); // interleaved stereo: frame_count * 2 floats
    StreamingPcm* pcm = self ? self->active_.load(std::memory_order_acquire) : nullptr;
    // Gapless: a track is handed over and the one that plays ends inside this block -- its last frames, then straight on into the next
    // one (from its first frame) in the same block. Not while paused, not at another playback speed.
    if (pcm && self->next_.load(std::memory_order_acquire) && !self->paused_.load() && std::fabs(self->speed_.load(std::memory_order_relaxed) - 1.0) <= 1e-3) {
        const FadeSnap fs = self->fade_snap(self->sample_rate_.load());
        const long long end = eff_end(fs, *pcm);   // (with Trim Silence: where the audible part of the track ends)
        const long long cur = self->cursor_frames_.load();
        if (end >= 0 && end - cur <= static_cast<long long>(frame_count)) {   // (<=: a block that ends exactly at the end must not report it)
            const ma_uint32 n1 = static_cast<ma_uint32>(std::clamp<long long>(end - cur, 0, frame_count));
            if (n1 > 0) render(self, pcm, out, n1, /*may_finish=*/false);   // (the main loop must not see the end: the next one follows)
            StreamingPcm* nb_peek = self->next_.load(std::memory_order_acquire);
            const XfPlan xp = plan_xf(fs, *pcm, eff_start(fs, *pcm), end, nb_peek);
            if (StreamingPcm* nx = self->next_.exchange(nullptr, std::memory_order_acq_rel)) {
                // the new track goes on where the crossfade has brought it (its first audible frame without one)
                const long long start_nx = nx == nb_peek && xp.on ? xp.sB + xp.len : eff_start(fs, *nx);
                self->cursor_frames_.store(start_nx);
                self->entered_by_xf_ = nx == nb_peek && xp.on;
                if (nx == nb_peek && xp.on) self->norm_cur_ = norm_target_for(self, *nx, nx->channels >= 2 && !self->stereo_enabled_.load());
                if (nx->windowed) nx->play_pos.store(start_nx, std::memory_order_relaxed);
                self->active_.store(nx, std::memory_order_release);
                self->finished_.store(false);
                if (frame_count > n1) render(self, nx, out + 2 * static_cast<size_t>(n1), frame_count - n1);
                return;
            }
            if (frame_count > n1) render(self, pcm, out + 2 * static_cast<size_t>(n1), frame_count - n1);   // (taken back just now)
            return;
        }
    }
    render(self, pcm, out, frame_count);
}

void Player::render(Player* self, StreamingPcm* pcm_ptr, float* out, ma_uint32 frame_count, bool may_finish) {
    // Paused: silence. With Pause Fade the level goes down over about 8 ms first (and up again on resume), so the cut does not click.
    const bool want_paused = self && self->paused_.load();
    const bool smooth = self && self->pause_fade_.load(std::memory_order_relaxed);
    float ramp = self ? self->pause_ramp_.load(std::memory_order_relaxed) : 0.0f;
    if (!self || !pcm_ptr || (want_paused && (!smooth || ramp <= 0.0f))) {
        if (self) self->pause_ramp_.store(smooth ? 0.0f : 1.0f, std::memory_order_relaxed);
        std::memset(out, 0, static_cast<size_t>(frame_count) * 2 * sizeof(float));
        return;
    }
    if (!smooth) ramp = 1.0f;

    StreamingPcm& pcm = *pcm_ptr;
    long long cur = self->cursor_frames_.load();
    float gain = self->gain_.load() * self->fade_.load();

    // Channel handling. The device is always stereo. A stereo buffer plays as
    // stereo when the option is on, otherwise it is folded to mono; a mono
    // buffer is just duplicated.
    const int pcm_ch = pcm.channels >= 2 ? 2 : 1;
    const bool play_stereo = pcm_ch == 2 && self->stereo_enabled_.load();
    const bool fold_to_mono = pcm_ch == 2 && !play_stereo;

    // Loudness normalisation: target gain from this track's measured LUFS.
    // Until the measurement exists (or with normalisation off) the target is
    // unity. The applied gain follows the target with a ~1 s time constant,
    // so toggling it, or the estimate being refined as decoding proceeds,
    // never produces a click or a sudden jump. When a stereo buffer is being
    // folded to mono the mono-mix figure is the one that applies, since that
    // is what actually reaches the speakers.
    const bool norm_on = self->norm_enabled_.load();
    const float lufs = (fold_to_mono ? pcm.loudness_mono_lufs : pcm.loudness_lufs).load(std::memory_order_relaxed);
    float norm_target = 1.0f;
    float norm_db = 0.0f;
    if (norm_on && !std::isnan(lufs)) {
        norm_db = std::clamp(self->norm_target_lufs_.load() - lufs, -30.0f, self->norm_max_boost_db_.load());
        norm_target = std::pow(10.0f, norm_db / 20.0f);
    }
    self->track_lufs_.store(lufs);
    self->norm_gain_db_.store(norm_db);
    float norm_cur = self->norm_cur_ < 0.0f ? norm_target : self->norm_cur_;
    const int sr_now = std::max(1, self->sample_rate_.load());
    const float norm_alpha = 1.0f - std::exp(-1.0f / (1.0f * static_cast<float>(sr_now)));

    // Equaliser: pick up a changed band layout (or a new sample rate) once per
    // block. configure() does ten sin/cos pairs and no allocation, so doing it
    // here on the audio thread is fine. Disabled -> bypass entirely.
    const bool eq_on = self->eq_enabled_.load(std::memory_order_relaxed);
    if (eq_on) {
        const unsigned gen = self->eq_gen_.load(std::memory_order_acquire);
        if (gen != self->eq_seen_gen_ || sr_now != self->eq_sr_) {
            EqGains g{};
            for (int b = 0; b < kEqBands; ++b) g[b] = self->eq_gains_[b].load(std::memory_order_relaxed);
            self->eq_.configure(g, sr_now);
            self->eq_seen_gen_ = gen;
            self->eq_sr_ = sr_now;
        }
    } else if (self->eq_seen_gen_ != 0) {
        self->eq_.reset();          // stale filter memory must not leak into the next enable
        self->eq_seen_gen_ = 0;
    }
    const bool eq_run = eq_on && self->eq_.active();

    // Acquire-load: pairs with the release-store in StreamingPcm::append(),
    // guaranteeing every frame below `avail` was fully written by the
    // decode thread before we read it here.
    // Windowed buffer (long tracks): only [lo, avail) is held, frame k at ring slot pcm.slot(k).
    size_t lo = 0, avail = 0;
    pcm.readable(lo, avail);
    const float* src = pcm.data.data();
    const bool windowed = pcm.windowed;
    if (windowed) pcm.play_pos.store(cur, std::memory_order_relaxed);

    const bool feed_fft = self->fft_sink_ && self->fft_mono_.size() >= frame_count;
    float* mono_out = feed_fft ? self->fft_mono_.data() : nullptr;
    // Oscilloscope music mode: the scopes get the decoded signal itself -- before the mono fold, the equalizer, the
    // normalization, the limiter and the volume -- so figures keep their exact shape and size.
    // The spectrogram analyses the decoded signal too (like Audacity analyses the file), whatever the mode.
    const bool raw_scope = self->scope_raw_on_.load(std::memory_order_relaxed);
    const bool raw_spectro = spectro_feed_active() || spectro_window_feed_active();   // terminal and / or window
    float* raw_out = (raw_scope || raw_spectro) && self->scope_raw_.size() >= 2 * static_cast<size_t>(frame_count) ? self->scope_raw_.data() : nullptr;

    // Playback speed (time_stretch.h; only the audio book player sets it): at any speed but 1.0 the frames come from the
    // stretcher, which reads the buffer itself. It starts over when the speed has just left 1.0 or the position was moved
    // from outside (a seek) since the last block.
    const double speed = self->speed_.load(std::memory_order_relaxed);
    const bool stretch = std::fabs(speed - 1.0) > 1e-3;
    if (stretch) {
        if (!self->ts_active_ || cur != self->ts_last_cur_) self->ts_.reset(static_cast<double>(cur));
        self->ts_active_ = true;
    } else {
        self->ts_active_ = false;
    }
    // Frames past the end of the track count as silence once the end is known; frames still being decoded do not.
    const size_t total_now = windowed ? pcm.total_frames.load(std::memory_order_acquire) : 0;
    const bool at_end = windowed ? (total_now > 0 && avail >= total_now) : pcm.decode_done.load();
    auto ts_avail = [&](long long i0, long long i1) {
        if (i1 > static_cast<long long>(avail) && !at_end) return false;
        if (windowed && i0 >= 0 && static_cast<size_t>(i0) < lo) return false;
        return true;
    };
    auto ts_sample = [&](long long i, int ch) -> float {
        if (i < 0 || static_cast<size_t>(i) >= avail || (windowed && static_cast<size_t>(i) < lo)) return 0.0f;
        const size_t k = pcm.slot(static_cast<size_t>(i));
        return (pcm_ch == 1 || ch == 0) ? src[k] : src[k + 1];
    };

    // Song fades (audio_fade.h): the first / last frame to play, the crossfade into the handed-over track, the gains of this block.
    // Not at another playback speed (the audio book player).
    const FadeSnap fs = self->fade_snap(sr_now);
    const bool fades_on = fs.on && !stretch;
    long long fS = 0, fE = -1;
    XfPlan xf;
    StreamingPcm* nb = nullptr;
    size_t b_lo = 0, b_avail = 0;
    float b_norm = 1.0f;
    long long fio = fs.len;   // the length of a fade in / out: a track shorter than twice the fade has each one half of it (they never overlap)
    int b_ch = 1;
    bool b_stereo = false, b_fold = false;
    if (fades_on) {
        fS = eff_start(fs, pcm);
        fE = eff_end(fs, pcm);
        if (cur < fS) {   // the silence at the start is cut: on from the first audible frame
            long long expected = cur;
            if (self->cursor_frames_.compare_exchange_strong(expected, fS)) cur = fS;
            else cur = expected;
        }
        if (fE >= 0) fio = std::max<long long>(1, std::min<long long>(fs.len, (fE - fS) / 2));
        nb = self->next_.load(std::memory_order_acquire);
        xf = plan_xf(fs, pcm, fS, fE, nb);
        if (xf.on) {
            nb->readable(b_lo, b_avail);
            b_ch = nb->channels >= 2 ? 2 : 1;
            b_stereo = b_ch == 2 && self->stereo_enabled_.load();
            b_fold = b_ch == 2 && !b_stereo;
            b_norm = norm_on ? norm_target_for(self, *nb, b_fold) : 1.0f;
        }
    }
    const bool fade_trim_end = fades_on && fs.trim && fE >= 0;
    // Windowed: the position only moves on over frames that are actually there (after a seek the decoder needs a
    // moment to get there: that moment is a short silence, not a skipped stretch of the track).
    const float ramp_step = 1.0f / std::max(8.0f, 0.008f * static_cast<float>(sr_now));   // 8 ms from full to silent
    long long played = 0;
    bool gap = false;
    for (ma_uint32 i = 0; i < frame_count; ++i) {
        long long idx = cur + played;
        if (!windowed) idx = cur + static_cast<long long>(i);
        norm_cur += (norm_target - norm_cur) * norm_alpha;
        float l = 0.0f, r = 0.0f;
        if (raw_out) raw_out[2 * i] = raw_out[2 * i + 1] = 0.0f;
        float a = 0.0f, b = 0.0f;   // the source frame (channel 0 / 1; a mono buffer gives the same value twice)
        bool have = false;
        if (stretch) {
            have = self->ts_.next(speed, ts_avail, ts_sample, a, b);
        } else if (!gap && idx >= 0 && static_cast<size_t>(idx) >= lo && static_cast<size_t>(idx) < avail) {
            ++played;
            const size_t k = pcm.slot(static_cast<size_t>(idx));   // sample index of channel 0
            a = src[k];
            b = pcm_ch == 1 ? src[k] : src[k + 1];
            have = true;
        } else if (windowed) gap = true;
        if (have && fade_trim_end && idx >= fE) have = false;   // (the silence at the end is cut)
        bool mixing = false;
        if (have) {
            if (raw_out) {
                raw_out[2 * i] = a;
                raw_out[2 * i + 1] = b;
            }
            if (pcm_ch == 1) {
                l = r = a;
            } else if (play_stereo) {
                l = a;
                r = b;
            } else {
                l = r = 0.5f * (a + b);
            }
            if (fades_on) {   // the fade in / out of this track, or the crossfade into the next one
                float fg = 1.0f;
                if (xf.on && idx >= xf.t0 && idx < fE) {
                    const float pr = static_cast<float>(idx - xf.t0) / static_cast<float>(xf.len);
                    fg = fade::xfade_out(fs.curve, pr);
                    const float gb = fade::xfade_in(fs.curve, pr) * std::min(8.0f, b_norm / std::max(0.05f, norm_cur));
                    const long long bi = xf.sB + (idx - xf.t0);
                    if (bi >= 0 && static_cast<size_t>(bi) >= b_lo && static_cast<size_t>(bi) < b_avail) {
                        const size_t kb = nb->slot(static_cast<size_t>(bi));
                        const float ba = nb->data[kb], bb = b_ch == 1 ? nb->data[kb] : nb->data[kb + 1];
                        const float lb = b_ch == 1 ? ba : b_stereo ? ba : 0.5f * (ba + bb);
                        const float rb = b_ch == 1 ? ba : b_stereo ? bb : 0.5f * (ba + bb);
                        l = l * fg + lb * gb;
                        r = r * fg + rb * gb;
                        mixing = true;
                        fg = 1.0f;
                    }
                } else if (fs.out && !xf.on && fE >= 0 && idx < fE && fE - idx < fio) {
                    fg = fade::shape(fs.curve, static_cast<float>(fE - idx) / static_cast<float>(fio));
                }
                if (fs.in && !self->entered_by_xf_ && idx >= fS && idx - fS < fio)
                    fg *= fade::shape(fs.curve, static_cast<float>(idx - fS) / static_cast<float>(fio));
                l *= fg;
                r *= fg;
            }
            if (eq_run) self->eq_.process(l, r); // before volume/normalisation: tone shaping on the raw signal
            if (smooth) {
                if (want_paused) ramp = std::max(0.0f, ramp - ramp_step);
                else if (ramp < 1.0f) ramp = std::min(1.0f, ramp + ramp_step);
            }
            const float g = norm_cur * gain * ramp;
            l *= g;
            r *= g;
        }
        if (norm_on || norm_cur > 1.001f || eq_run || mixing) { l = soft_limit(l); r = soft_limit(r); }
        out[2 * i]     = l;
        out[2 * i + 1] = r;
        if (mono_out) mono_out[i] = 0.5f * (l + r);
    }
    self->norm_cur_ = norm_cur;
    self->pause_ramp_.store(ramp, std::memory_order_relaxed);

    // The visualizer wants one channel: feed it the mono mix of what was
    // actually played (the buffer is pre-sized in play(); a callback larger
    // than that just skips the feed rather than allocating on the audio thread).
    if (feed_fft) self->fft_sink_->push_samples(mono_out, frame_count, self->sample_rate_.load());

    // The oscilloscope wants the raw time-domain signal instead: the very
    // same interleaved stereo block just written to `out` (post gain,
    // post normalisation, post limiter), left and right kept apart so it
    // can draw a stereo pair or fold to the mono mix itself.
    // push_frames() writes a fixed-size ring, so -- like the FFT feed
    // above -- this never allocates on the audio thread. While paused the
    // callback returns before any of this runs, which freezes the scope on
    // the last drawn waveform, exactly like the frozen spectrum bars.
    const float* scope_src = raw_out && raw_scope ? raw_out : out;
    if (raw_out && raw_spectro) {
        spectro_feed_push(raw_out, frame_count, self->sample_rate_.load());
        spectro_window_feed_push(raw_out, frame_count, self->sample_rate_.load());   // the spectrogram window (SHIFT+8)
    }
    if (self->scope_sink_) self->scope_sink_->push_frames(scope_src, frame_count);
    scope_feed_push(scope_src, frame_count, self->sample_rate_.load());   // the scope window (SHIFT+9); nothing while it is closed
    milkdrop_feed(scope_src, frame_count, self->sample_rate_.load());   // the MilkDrop visuals (milkdrop.h); nothing while they are not drawn

    long long new_cur = cur + static_cast<long long>(frame_count);
    if (stretch) new_cur = static_cast<long long>(std::llround(self->ts_.heard_pos()));   // the source time of what is heard
    if (windowed) {
        if (!stretch) new_cur = cur + played;
        const size_t total = pcm.total_frames.load(std::memory_order_acquire);
        if (total > 0 && new_cur >= 0 && static_cast<size_t>(new_cur) >= total) {
            new_cur = static_cast<long long>(total);
            if (may_finish) self->finished_.store(true);
        } else if (pcm.decode_failed.load() && may_finish) self->finished_.store(true);
        else if (fade_trim_end && new_cur >= fE && may_finish) self->finished_.store(true);   // (the silence at the end is cut)
    } else
    // Only truly "finished" once decode is done AND playback has caught
    // all the way up to everything it ever produced — not just the
    // current available count, which may still be growing while we play.
    if (pcm.decode_done.load() &&
        new_cur >= 0 && static_cast<size_t>(new_cur) >= pcm.available.load(std::memory_order_acquire)) {
        if (may_finish) self->finished_.store(true);
    } else if (fade_trim_end && new_cur >= fE && may_finish) {
        self->finished_.store(true);   // (the silence at the end is cut: the track is over where the audible part ends)
    }
    // a seek (seek_relative) while this block was being made wins over the position this block reached
    self->ts_last_cur_ = new_cur;
    if (self->cursor_frames_.compare_exchange_strong(cur, new_cur))
        self->clock_stamp_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
}

bool Player::play(std::shared_ptr<StreamingPcm> pcm, double start_sec, int volume_pct,
                   FftVisualizer* fft_sink, OscilloscopeVisualizer* scope_sink, bool start_paused) {
    // Give the loudness measurement a moment to exist so the track starts at
    // its final level instead of gliding into it. Decoding runs far faster
    // than real time, so this normally returns immediately; capped so a slow
    // source can't delay playback noticeably. Done BEFORE taking mutex_ so
    // the main thread's stop() is never held up behind it.
    if (pcm && norm_enabled_.load()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
        while (std::isnan(pcm->loudness_lufs.load(std::memory_order_relaxed)) &&
               !pcm->loudness_final.load(std::memory_order_acquire) &&
               !pcm->decode_failed.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::lock_guard<std::mutex> lk(mutex_);
    stop_locked();
    if (!pcm) return false;

    if (!context_ready_) {
        const char* use_null = std::getenv("MOUSIKI_NULL_AUDIO");   // headless runs (tests, CI): a silent device
        if (use_null && *use_null == '1') {
            ma_backend nb[] = { ma_backend_null };
            context_ready_ = ma_context_init(nb, 1, nullptr, &context_) == MA_SUCCESS;
        } else
        context_ready_ = init_platform_audio_context(context_);
        // Not fatal if this fails — ma_device_init(nullptr, ...) below
        // falls back to miniaudio's own default backend selection.
    }

    pcm_ = std::move(pcm);
    fft_sink_ = fft_sink;
    scope_sink_ = scope_sink;
    fft_mono_.assign(16384, 0.0f); // > any realistic device period (16384 frames = 370 ms at 44.1 kHz)
    scope_raw_.assign(2 * 16384, 0.0f);
    sample_rate_.store(pcm_->sample_rate > 0 ? pcm_->sample_rate : 44100);
    volume_pct_.store(std::clamp(volume_pct, 0, 100));
    gain_.store(volume_pct_.load() / 100.0f);
    finished_.store(false);
    paused_.store(start_paused);
    pause_ramp_.store(start_paused ? 0.0f : 1.0f);
    eq_.reset();       // device isn't running yet: safe to touch audio-thread state
    eq_seen_gen_ = 0;
    eq_sr_ = 0;
    ts_.configure(sample_rate_.load());   // the speed stretcher's buffers for this rate (allocates: only while stopped)
    ts_active_ = false;
    norm_cur_ = -1.0f; // audio device isn't running yet: safe to reset; first callback snaps to the target gain
    track_lufs_.store(std::numeric_limits<float>::quiet_NaN());
    norm_gain_db_.store(0.0f);
    cursor_frames_.store(static_cast<long long>(std::max(0.0, start_sec) * sample_rate_.load()));
    if (pcm_->windowed) pcm_->play_pos.store(cursor_frames_.load(), std::memory_order_relaxed);
    active_.store(pcm_.get(), std::memory_order_release);   // (the device is not running yet)

    if (!open_device_locked()) { active_.store(nullptr); pcm_.reset(); return false; }
    return true;
}

bool Player::commit_switch_locked() {
    if (!next_pcm_ || next_.load(std::memory_order_acquire) != nullptr) return false;   // nothing handed over, or not taken yet
    // The callback exchanged next_ out: it plays next_pcm_ now and never looks at the old buffer again (it took it after the old one's
    // last frames). The old buffer can go.
    pcm_ = std::move(next_pcm_);
    next_pcm_.reset();
    switch_unreported_ = true;
    return true;
}

bool Player::queue_next(std::shared_ptr<StreamingPcm> next) {
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    commit_switch_locked();
    if (switch_unreported_) return false;   // (a switch first: the caller learns of it through take_switch())
    if (!device_ready_ || !pcm_ || !next || next->sample_rate != sample_rate_.load()) return false;
    if (next_.exchange(nullptr, std::memory_order_acq_rel)) next_pcm_.reset();   // (one handed over before: replaced)
    next_pcm_ = std::move(next);
    next_.store(next_pcm_.get(), std::memory_order_release);
    return true;
}

int Player::clear_next() {
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (!lk.owns_lock()) return 0;
    if (switch_unreported_) return -1;
    if (!next_pcm_) return 1;
    if (next_.exchange(nullptr, std::memory_order_acq_rel)) { next_pcm_.reset(); return 1; }   // taken back before the callback got it
    commit_switch_locked();
    return -1;   // the callback took it: a switch (take_switch() reports it)
}

bool Player::take_switch() {
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    commit_switch_locked();
    const bool r = switch_unreported_;
    switch_unreported_ = false;
    return r;
}

// Opens and starts the device for the current pcm_ / sample rate: the chosen output (set_device()) when it is there, otherwise
// the system's default. mutex_ held, device not open.
bool Player::open_device_locked() {
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2; // always stereo; mono buffers are duplicated in the callback
    cfg.sampleRate = static_cast<ma_uint32>(sample_rate_.load());
    // If the device cannot take the track's rate, miniaudio converts it: with the steepest low-pass it offers instead
    // of the default (order 4), so no aliasing and no dull top end. (WASAPI shared mode uses Windows' own converter.)
    cfg.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
    cfg.dataCallback = data_callback;
    cfg.pUserData = this;
    // The device buffer: miniaudio's default is a few milliseconds, fine for a sound card, but a sound server that reaches the speakers
    // over a network-like path (WSLg's PulseAudio on Windows) runs dry with it: crackling, a sound that seems slow. There the buffer is 60 ms
    // x 4. MOUSIKI_AUDIO_BUFFER_MS=<ms> sets the length of one period everywhere (0 = miniaudio's default).
    {
        int period_ms = 0;
#if defined(__linux__)
        if (std::FILE* f = std::fopen("/proc/version", "r")) {
            char b[256] = {0};
            const size_t n = std::fread(b, 1, sizeof b - 1, f);
            std::fclose(f);
            std::string v(b, n);
            for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (v.find("microsoft") != std::string::npos) period_ms = 60;   // WSL
        }
#endif
        if (const char* e = std::getenv("MOUSIKI_AUDIO_BUFFER_MS")) period_ms = std::clamp(std::atoi(e), 0, 500);
        if (period_ms > 0) { cfg.periodSizeInMilliseconds = static_cast<ma_uint32>(period_ms); cfg.periods = 4; }
    }

    ma_context* ctx = context_ready_ ? &context_ : nullptr;
    ma_device_id chosen{};
    std::string want;
    { std::lock_guard<std::mutex> dk(device_name_mutex_); want = device_name_; }
    if (ctx && find_output_device(*ctx, want, chosen)) cfg.playback.pDeviceID = &chosen;
    else if (!want.empty()) ConsoleLog::instance().log_basic("audio: the device \"" + want + "\" is not there -- the default device plays");
    ma_result init_res = ma_device_init(ctx, &cfg, &device_);
    if (init_res != MA_SUCCESS) {
        ConsoleLog::instance().log_verbose(
            std::string("audio: ma_device_init failed: ") + ma_result_description(init_res));
        return false;
    }
    ma_result start_res = ma_device_start(&device_);
    if (start_res != MA_SUCCESS) {
        ma_device_uninit(&device_);
        ConsoleLog::instance().log_verbose(
            std::string("audio: ma_device_start failed: ") + ma_result_description(start_res));
        return false;
    }
    ConsoleLog::instance().log_basic(
        std::string("audio: device \"") + device_.playback.name + "\" started, backend=" + ma_get_backend_name(device_.pContext->backend) +
        ", track rate=" + std::to_string(sample_rate_.load()) + " Hz, device rate=" + std::to_string(device_.playback.internalSampleRate) + " Hz, buffer=" +
        std::to_string(device_.playback.internalPeriods) + " x " + std::to_string(device_.playback.internalPeriodSizeInFrames) + " frames" +
        (device_.playback.internalSampleRate != static_cast<ma_uint32>(sample_rate_.load()) ? " (converted)" : ""));

    {
        std::lock_guard<std::mutex> dk(device_name_mutex_);
        active_device_ = device_.playback.name;
    }
    device_ready_ = true;
    return true;
}

bool Player::reopen_device() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!device_ready_ || !pcm_) return true;   // nothing open: the next play() uses the new choice
    ma_device_uninit(&device_);                 // blocks until the callback has stopped; position, pause and pcm_ stay
    device_ready_ = false;
    if (open_device_locked()) return true;
    { std::lock_guard<std::mutex> dk(device_name_mutex_); device_name_.clear(); }   // the chosen one failed: back to the default
    return open_device_locked();
}

void Player::set_device(const std::string& name) {
    std::lock_guard<std::mutex> dk(device_name_mutex_);
    device_name_ = name;
}

std::string Player::active_device() const {
    std::lock_guard<std::mutex> dk(device_name_mutex_);
    return active_device_;
}

void Player::pause() { paused_.store(true); }
void Player::resume() { paused_.store(false); }

int Player::volume() const {
    // Lock-free on purpose: called every frame by the UI, and mutex_ can be
    // held for hundreds of ms by play() during a device swap.
    return volume_pct_.load();
}

void Player::seek_relative(double delta_sec) {
    // try_lock, not lock: if play()/stop() is mid device swap the old pcm_ is
    // being torn down anyway, so dropping the seek is right -- and blocking
    // the main thread on it would freeze the UI.
    std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
    if (!lk.owns_lock() || !pcm_) return;
    commit_switch_locked();   // (a gapless switch the main thread has not taken yet: the seek is in the track that plays now)
    long long delta_frames = static_cast<long long>(delta_sec * sample_rate_.load());
    long long cur = cursor_frames_.load();
    // Clamp against reserved capacity (the eventual max), not the
    // currently-decoded amount — seeking a bit ahead of what's decoded
    // so far is fine, it just plays silence until decode catches up.
    long long cap = static_cast<long long>(pcm_->seek_limit_frames());
    long long next = std::clamp<long long>(cur + delta_frames, 0, cap);
    cursor_frames_.store(next);
    if (pcm_->windowed) pcm_->play_pos.store(next, std::memory_order_relaxed);   // the decoder moves there now, even while paused
    if (next < cap) finished_.store(false);
}

void Player::set_normalization(bool enabled, float target_lufs, float max_boost_db) {
    norm_target_lufs_.store(std::clamp(target_lufs, -40.0f, 0.0f));
    norm_max_boost_db_.store(std::clamp(max_boost_db, 0.0f, 24.0f));
    norm_enabled_.store(enabled);
}

void Player::set_fade_options(const FadeOptions& o) {
    fo_in_.store(o.in); fo_out_.store(o.out); fo_cross_.store(o.cross); fo_trim_.store(o.trim);
    fo_len_ms_.store(std::clamp(o.len_ms, 500, 20000));
    fo_curve_.store(std::clamp(o.curve, 0, fade::kCurveCount - 1));
    fo_thr_db_.store(std::clamp(o.thr_db, -90, -20));
    fo_enabled_.store(o.enabled);
}

FadeSnap Player::fade_snap(int sr) const {
    FadeSnap f;
    f.in = fo_in_.load(); f.out = fo_out_.load(); f.cross = fo_cross_.load(); f.trim = fo_trim_.load();
    f.on = fo_enabled_.load() && (f.in || f.out || f.cross || f.trim);
    f.curve = fo_curve_.load();
    f.thr_db = fo_thr_db_.load();
    f.thr = fade::db_to_lin(f.thr_db);
    f.len = static_cast<long long>(fo_len_ms_.load()) * std::max(1, sr) / 1000;
    return f;
}

float Player::norm_target_for(const Player* self, const StreamingPcm& p, bool fold_to_mono) {
    if (!self->norm_enabled_.load()) return 1.0f;
    const float lufs = (fold_to_mono ? p.loudness_mono_lufs : p.loudness_lufs).load(std::memory_order_relaxed);
    if (std::isnan(lufs)) return 1.0f;
    const float db = std::clamp(self->norm_target_lufs_.load() - lufs, -30.0f, self->norm_max_boost_db_.load());
    return std::pow(10.0f, db / 20.0f);
}

// UI thread, every frame: measures the silence at the ends of the playing and the preloaded track (cheap: nothing to do once measured).
void Player::service_trim() {
    const FadeSnap f = fade_snap(std::max(1, sample_rate_.load()));
    if (!f.on || !f.trim) return;
    std::shared_ptr<StreamingPcm> a, b;
    {
        std::unique_lock<std::mutex> lk(mutex_, std::try_to_lock);
        if (!lk.owns_lock()) return;
        a = pcm_; b = next_pcm_;
    }
    if (a) analyse_trim(*a, f);
    if (b) analyse_trim(*b, f);
}

void Player::set_equalizer(bool enabled, const EqGains& gains_db) {
    for (int b = 0; b < kEqBands; ++b)
        eq_gains_[b].store(std::clamp(gains_db[b], kEqMinDb, kEqMaxDb), std::memory_order_relaxed);
    eq_gen_.fetch_add(1, std::memory_order_release); // publish AFTER the gains are stored
    eq_enabled_.store(enabled);
}

void Player::set_volume(int volume_pct) {
    // Atomics only -- see volume().
    const int v = std::clamp(volume_pct, 0, 100);
    volume_pct_.store(v);
    gain_.store(v / 100.0f);
}

double Player::poll_elapsed() const {
    // Lock-free on purpose -- see volume().
    const int sr = sample_rate_.load();
    if (sr <= 0) return 0.0;
    return static_cast<double>(cursor_frames_.load()) / sr;
}

double Player::smooth_elapsed() const {
    const double base = poll_elapsed();
    const long long stamp = clock_stamp_ns_.load(std::memory_order_relaxed);
    if (paused_.load() || stamp == 0) return base;
    const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double dt = static_cast<double>(now - stamp) / 1e9;
    if (dt < 0.0 || dt > 0.25) return base;   // (no step for a long time: the device stands, so does the clock)
    return base + dt * speed();
}

void Player::stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    stop_locked();
}

void Player::stop_locked() {
    if (device_ready_) {
        ma_device_uninit(&device_);
        device_ready_ = false;
    }
    active_.store(nullptr, std::memory_order_release);   // (the callback has stopped: uninit waited for it)
    next_.store(nullptr, std::memory_order_release);
    next_pcm_.reset();
    switch_unreported_ = false;
    pcm_.reset();
    fft_sink_ = nullptr;
    scope_sink_ = nullptr;
}

} // namespace muisc
