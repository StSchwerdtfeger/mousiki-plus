// The audio player (see player.cpp): Player plays decoded audio, reports the position and handles
// pause, seek and volume.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "miniaudio.h"
#include "streaming_pcm.h"
#include "fft_visualizer.h"
#include "oscilloscope_visualizer.h"
#include "equalizer.h"
#include "time_stretch.h"
#include "audio_fade.h"

namespace muisc {

// The song fade options as the audio thread uses them (len in frames at the rate in use, thr linear).
struct FadeSnap { bool on = false, in = false, out = false, cross = false, trim = false; int curve = 0, thr_db = -60; long long len = 0; float thr = 0.001f; };

// Plays back a StreamingPcm buffer (mono or stereo) through a real audio device via
// miniaudio, using the backend pinned in audio_backend.h
// (PulseAudio/ALSA -> PipeWire on Linux, WASAPI on Windows, OpenSL ES on
// Android).
//
// This reads from a buffer that may STILL BE FILLING IN — play() can be
// called the moment decode starts (as soon as duration is known from
// ffprobe and the buffer's capacity is reserved), and the callback below
// just plays silence for any frame past what's been decoded so far,
// self-correcting once the decode thread catches up. That's what lets
// playback start almost immediately instead of waiting for the whole
// track to decode first.
//
// This replaced an earlier ffplay-subprocess design. ffplay's audio
// output goes through SDL, and SDL's Android backend expects to be
// running inside a proper Activity with its Java glue — a plain Termux
// CLI process has neither, so SDL_OpenAudioDevice effectively never
// succeeds there and nothing plays, silently. Talking to OpenSL ES
// directly through miniaudio sidesteps that entirely.
class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // `pcm` must stay alive for as long as playback is active — App holds
    // it via shared_ptr and only replaces it once stop() has fully torn
    // the device down. `fft_sink`, if given, gets push_samples() called
    // from the audio callback with each chunk actually played (nullptr
    // to disable — e.g. not needed for a plain smoke test).
    // `scope_sink`, if given, gets push_frames() called with the exact
    // interleaved stereo block written to the device -- the raw
    // time-domain feed for the lyrics panel's oscilloscope (independent
    // of fft_sink, so one can exist without the other).
    // `start_paused`: the device opens paused (silence from the first callback on; resume() starts it).
    bool play(std::shared_ptr<StreamingPcm> pcm, double start_sec, int volume_pct,
              FftVisualizer* fft_sink = nullptr, OscilloscopeVisualizer* scope_sink = nullptr, bool start_paused = false);

    void pause();
    void resume();
    bool is_paused() const { return paused_; }
    int sample_rate() const { return sample_rate_.load(); }   // of the stream being played (the scope overlay shows the YT time span)

    void seek_relative(double delta_sec);
    void set_volume(int volume_pct);
    // Extra gain 0..1 on top of the volume (the sleep timer's fade-out); 1 = untouched.
    void set_fade(float f) { fade_.store(std::clamp(f, 0.0f, 1.0f)); }
    int volume() const;

    // Stereo on/off. The device is always opened with two channels; a stereo
    // buffer plays as-is when this is on and is folded down to mono (L+R)/2
    // when it is off; a mono buffer plays identically either way (duplicated
    // to both speakers). Atomic, takes effect immediately. Note the buffer's
    // channel count is fixed when the track is decoded: turning stereo ON
    // while a mono-decoded track is playing only affects the next track.
    void set_stereo(bool enabled) { stereo_enabled_.store(enabled); }
    // Pause / resume with a 8 ms ramp instead of a hard cut (no click): Settings -> GENERAL -> AUDIO "Pause Fade", config.txt PauseFade=
    void set_pause_fade(bool on) { pause_fade_.store(on); }
    bool stereo_enabled() const { return stereo_enabled_.load(); }

    // Oscilloscope music mode: the scopes (terminal and window) get the decoded signal before the mono fold, the
    // equalizer, normalization, limiter and volume. What is heard does not change.
    void set_scope_raw(bool on) { scope_raw_on_.store(on, std::memory_order_relaxed); }

    // Loudness normalisation (see loudness_meter.h). Every track is measured
    // in LUFS while it decodes and played with a gain that brings it to
    // `target_lufs`, so a quiet recording and a heavily compressed one end up
    // at the same perceived level. The gain is capped at `max_boost_db` so
    // very quiet files aren't blown up into noise, and a soft limiter keeps
    // boosted peaks from clipping. Atomics only -- callable from any thread,
    // takes effect within about a second (the gain glides, it never jumps).
    void set_normalization(bool enabled, float target_lufs, float max_boost_db);
    bool normalization_enabled() const { return norm_enabled_.load(); }
    float normalization_gain_db() const { return norm_gain_db_.load(); }   // gain currently being applied
    float track_lufs() const { return track_lufs_.load(); }                // NaN until measured

    // Graphic equaliser (see equalizer.h). `gains_db` are the ten band gains.
    // Atomics only -- callable from any thread; the audio callback notices the
    // change on its next block and recomputes its coefficients, so the sound
    // follows within a few milliseconds and never clicks mid-track.
    void set_equalizer(bool enabled, const EqGains& gains_db);
    bool equalizer_enabled() const { return eq_enabled_.load(); }

    // Playback speed without a change of pitch (time_stretch.h): 1.0 = normal, clamped to 0.5 .. 3.0. Atomic, takes effect
    // within one grain (~40 ms). The audio book player uses it; the music player and the radio leave it at 1.0. The position
    // (poll_elapsed(), seek_relative()) stays in the time of the track itself.
    void set_speed(double s) { speed_.store(std::clamp(s, TimeStretch::kMinSpeed, TimeStretch::kMaxSpeed)); }
    double speed() const { return speed_.load(); }

    // Song fades (audio_fade.h; Settings -> GENERAL -> AUDIO FADE OPTIONS, the music player only: App leaves `enabled` off in the audio book
    // mode). in / out: a fade at the start / the end of every track; cross: the end of a track and the start of the next one overlap (the next
    // track is handed over by the gapless machinery, App makes it prepare it whatever Gapless says); len_ms: how long a fade lasts; curve:
    // audio_fade.h Curve; trim: the silence at the start and the end of a track is cut (the fades and the crossfade then work on the audible
    // part), thr_db: what counts as silence (dBFS). Atomics only -- callable from any thread, takes effect within a block.
    struct FadeOptions { bool enabled = false, in = false, out = false, cross = false, trim = false; int len_ms = 4000, curve = 0, thr_db = -60; };
    void set_fade_options(const FadeOptions& o);
    // Main thread, once per frame: measures the silence at the ends of the playing and the handed-over track as soon as their audio is decoded.
    void service_trim();

    double poll_elapsed() const;
    // The playback time for pictures: poll_elapsed() moves in the steps of the sound device (one block of samples, 10-50 ms), which made a
    // video's pictures come early or late by up to a block. This one goes on from the last step by the time that has passed since (at the
    // playback speed), at most a quarter of a second; paused, it is poll_elapsed().
    double smooth_elapsed() const;
    bool finished() const { return finished_.load(); }
    // Synchronously clears a stale finished flag left over from the
    // previous track. play() itself resets this too, but play() now
    // runs on a detached background thread (device init can genuinely
    // stall) — without this, there's a window where has_track_ is
    // already true for the NEW track but finished_ is still true from
    // the OLD one, and the main loop's "if (has_track_ && finished())
    // advance_track()" check fires again immediately, skipping straight
    // past the track that was just supposed to start.
    void clear_finished() { finished_.store(false); }

    void stop();

    // Gapless playback (Settings -> GENERAL -> AUDIO -> Gapless Playback). queue_next() hands over the track that follows: the audio
    // thread goes on into it at the very sample where the one that plays ends, in the same callback, with the device kept open -- no
    // gap, no reopen. It has to have the same sample rate as the one that plays (the device runs at that rate); it may be the same
    // buffer again (repeat). take_switch() is how the main thread learns that the audio thread went on into it (true once per switch;
    // from then on it is the track that plays and the old one is let go). clear_next() takes a track that was handed over back
    // (false: too late, the switch has happened -- take_switch() reports it). All three only try the lock: while play() swaps the device
    // they return false and are asked again on the next frame. Not at a playback speed other than 1 (the audio book player).
    bool queue_next(std::shared_ptr<StreamingPcm> next);
    int clear_next();
    bool take_switch();
    bool has_next() const { return next_.load(std::memory_order_acquire) != nullptr; }

    // The sound output (audio_backend.h): the name of the chosen device, "" = the system's default. set_device() only stores it;
    // the next play() opens that device, and reopen_device() moves a running track there at once (same position, pause state
    // kept; a few hundred ms on WASAPI, so not on the audio thread). A device that is not there (any more) falls back to the
    // default. active_device() is the name of the device that is open now ("" before the first track).
    void set_device(const std::string& name);
    bool reopen_device();
    std::string active_device() const;

private:
    // Guards play()/stop() (and seek_relative(), which touches pcm_) against
    // each other. Deliberately NOT taken by poll_elapsed(), volume() or
    // set_volume(): play() holds this across ma_device_uninit() and
    // ma_device_init()/start(), which on WASAPI can take a few hundred ms,
    // and the main thread calls poll_elapsed()/volume() every rendered frame
    // -- taking the lock there froze the whole UI for the duration of every
    // track change. Those three only touch atomics now.
    //
    // Original rationale: play()/stop() run
    // on App's persistent device-worker thread for the whole session, while
    // seek/volume/pause hotkeys and the shutdown path's stop() run on the
    // main thread -- two long-lived threads genuinely calling into the same
    // Player concurrently, not a short-lived ad-hoc thread that mostly
    // didn't overlap anything. NOT taken inside data_callback(): that runs
    // on miniaudio's own real-time audio thread, and ma_device_uninit() is
    // documented to block until that callback thread has fully stopped
    // before returning. By the time stop_locked() reassigns pcm_, the
    // callback that reads it is already provably not running, so
    // there is nothing there for this mutex to protect, and taking it in
    // the callback would risk an audible stall if it ever had to wait on a
    // slow device_init() elsewhere.
    mutable std::mutex mutex_;

    // stop()'s actual work, factored out so play() can call it without
    // re-locking mutex_ (std::mutex isn't recursive -- play() calling the
    // public stop() from inside its own already-held lock would deadlock).
    void stop_locked();
    bool open_device_locked();   // the device for pcm_ / sample_rate_ (the chosen one or the default); mutex_ held
    mutable std::mutex device_name_mutex_;   // guards device_name_ / active_device_ (read by the UI thread)
    std::string device_name_, active_device_;

    ma_context context_{};
    bool context_ready_ = false;
    ma_device device_{};
    bool device_ready_ = false;

    std::shared_ptr<StreamingPcm> pcm_;
    // What the audio callback plays (the callback reads only these raw pointers, never the shared_ptrs above/below, which the main
    // thread may reassign while it runs). active_ is pcm_ itself, or -- after a gapless switch, until take_switch() -- next_pcm_.
    // next_ is the handed-over track; the callback takes it with an exchange, so whoever exchanges it out of there first owns the decision.
    std::atomic<StreamingPcm*> active_{nullptr};
    std::shared_ptr<StreamingPcm> next_pcm_;   // mutex_
    std::atomic<StreamingPcm*> next_{nullptr};
    bool commit_switch_locked();                // the callback took next_: next_pcm_ becomes pcm_; mutex_ held
    bool switch_unreported_ = false;            // a switch committed (by any of the calls) that take_switch() has not reported yet; mutex_
    static void render(Player* self, StreamingPcm* pcm, float* out, ma_uint32 frame_count, bool may_finish = true);
    FadeSnap fade_snap(int sr) const;   // the fade options as one snapshot for the audio thread
    static float norm_target_for(const Player* self, const StreamingPcm& p, bool fold_to_mono);   // the loudness gain this track gets (1 without normalisation / measurement)
    std::atomic<bool> fo_enabled_{false}, fo_in_{false}, fo_out_{false}, fo_cross_{false}, fo_trim_{false};
    std::atomic<int> fo_len_ms_{4000}, fo_curve_{0}, fo_thr_db_{-60};
    bool entered_by_xf_ = false;   // audio thread: the playing track came in through a crossfade (no fade in on top of it)
    FftVisualizer* fft_sink_ = nullptr;
    OscilloscopeVisualizer* scope_sink_ = nullptr; // raw-waveform feed; same lifetime rules as fft_sink_
    std::atomic<int> sample_rate_{44100};
    std::atomic<long long> cursor_frames_{0};
    std::atomic<long long> clock_stamp_ns_{0};   // (steady clock) when the audio thread last moved cursor_frames_ on
    std::atomic<bool> finished_{false};
    std::atomic<float> gain_{0.7f};
    std::atomic<float> fade_{1.0f};
    std::atomic<bool> paused_{false};
    std::atomic<bool> pause_fade_{true};
    std::atomic<float> pause_ramp_{1.0f};   // 1 = full level, 0 = silent; follows paused_ over about 8 ms (the audio callback moves it)
    std::atomic<int> volume_pct_{70};

    std::atomic<bool> stereo_enabled_{true};
    std::vector<float> fft_mono_;   // scratch for the visualizer's mono feed; sized in play(), never in the callback
    std::vector<float> scope_raw_;  // scratch: the unprocessed stereo signal for the scopes (oscilloscope music mode)
    std::atomic<bool> scope_raw_on_{false};

    std::atomic<bool> norm_enabled_{false};
    std::atomic<float> norm_target_lufs_{-16.0f};
    std::atomic<float> norm_max_boost_db_{9.0f};
    std::atomic<float> norm_gain_db_{0.0f};
    std::atomic<float> track_lufs_{std::numeric_limits<float>::quiet_NaN()};
    float norm_cur_ = -1.0f; // smoothed linear normalisation gain; audio thread only (-1 = not started)

    // Equaliser. The *_ atomics are the hand-over from the UI thread; eq_,
    // eq_seen_gen_ and eq_sr_ belong to the audio thread alone (reset in
    // play() while the device is not running).
    std::atomic<bool> eq_enabled_{false};
    std::array<std::atomic<float>, kEqBands> eq_gains_{};
    std::atomic<unsigned> eq_gen_{1};
    Equalizer eq_;
    unsigned eq_seen_gen_ = 0;
    int eq_sr_ = 0;

    // Playback speed. speed_ is the hand-over from the UI thread; ts_, ts_active_ and ts_last_cur_ belong to the audio thread
    // (ts_ is sized in play() while the device is stopped). ts_last_cur_ is the position the callback stored last: a different
    // value at the next callback means a seek, and the stretcher starts over there.
    std::atomic<double> speed_{1.0};
    TimeStretch ts_;
    bool ts_active_ = false;
    long long ts_last_cur_ = 0;

    static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count);
    static void data_callback_impl(ma_device* device, void* output, const void* input, ma_uint32 frame_count);
};

} // namespace muisc
