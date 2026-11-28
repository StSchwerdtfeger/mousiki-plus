// app_gapless.cpp
// Part of class App (src/app.h): gapless playback (Settings -> GENERAL -> AUDIO -> Gapless Playback, config.txt Gapless=off|album|full).
//
// In the last seconds of a track, the one that will follow is worked out exactly as advance_track() will pick it (the queue, the play
// mode, the list), loaded ahead at the sample rate the device runs at, and handed to the player (Player::queue_next()). The audio thread
// goes on into it at the very sample where the first one ends, with the device kept open. On the next frame the main loop sees the switch
// (Player::take_switch()) and runs advance_track() as at any end of a track: it picks that very track, launch_load_async() takes the
// preloaded load instead of loading it again, and poll_pending_load() does everything a new track needs except starting the device.
// Anything that changes the prediction in between (a track queued, another play mode, a jump in the list) takes the preload back.
//
// "album" goes gapless only between tracks of the same album (album tag plus the same artist or folder; files without an album tag: the
// same folder) that have the same sample rate and channel count, so nothing is resampled; "full" goes gapless into every local track that
// follows automatically, resampled to the playing rate when it has another one. Online tracks, the radio and audio books are not covered.
#include "utf8_util.h"
#include "app.h"
#include "string_util.h"
#include "app_state_sleep.h"
#include "app_state_device.h"
#include "creator_state.h"
#include "path_utf8.h"
#include "console_log.h"
#include "process_util.h"
#include "waveform.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <random>

namespace muisc {

namespace {

constexpr double kGapLeadSeconds = 15.0;   // the next track is prepared this long before the end of the one that plays
constexpr auto kGapCheck = std::chrono::milliseconds(250);   // how often the prediction is checked again

std::string tag_key(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    s = s.substr(a);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s == "-" ? std::string() : s;
}

// The channel count of the file itself (not the decoded buffer, which follows the Stereo setting): the header through miniaudio,
// else ffprobe. 0 = not known.
int file_channels(const fs::path& p) {
    if (p.empty()) return 0;
    ma_decoder dec;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
#if defined(_WIN32)
    if (ma_decoder_init_file_w(p.c_str(), &cfg, &dec) == MA_SUCCESS) {
#else
    if (ma_decoder_init_file(p.c_str(), &cfg, &dec) == MA_SUCCESS) {
#endif
        const int ch = static_cast<int>(dec.outputChannels);
        ma_decoder_uninit(&dec);
        if (ch > 0) return ch;
    }
    const ProcResult r = run_capture({"ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=channels",
                                      "-of", "default=noprint_wrappers=1:nokey=1", path_utf8(p)});
    return r.ok() ? std::max(0, muisc::atoi_safe(r.out.c_str())) : 0;
}

} // namespace

// Gapless "album": "" when the next track may follow without a gap, else why not (runs on the preload's thread, before anything is decoded).
std::string App::gapless_album_mismatch(const LoadJob& j, const TrackMetadata& next, const fs::path& next_path, int next_rate, const fs::path& decode_from) {
    const std::string a1 = tag_key(j.cur_album), a2 = tag_key(next.album);
    std::error_code ec;
    const bool same_folder = fs::equivalent(j.cur_path.parent_path(), next_path.parent_path(), ec) || j.cur_path.parent_path() == next_path.parent_path();
    if (!a1.empty() && !a2.empty()) {
        if (a1 != a2) return "another album";
        const std::string r1 = tag_key(j.cur_artist), r2 = tag_key(next.artist);
        if (!same_folder && !r1.empty() && !r2.empty() && r1 != r2) return "an album of the same name by another artist in another folder";
    } else if (!same_folder) {
        return "no album tag, and another folder";
    }
    if (next_rate != j.force_rate) return "another sample rate (" + std::to_string(next_rate) + " Hz, " + std::to_string(j.force_rate) + " Hz plays)";
    const int c1 = file_channels(j.cur_path), c2 = file_channels(decode_from);
    if (c1 > 0 && c2 > 0 && c1 != c2) return "another channel count (" + std::to_string(c2) + ", " + std::to_string(c1) + " plays)";
    return "";
}

// The track advance_track() will start when the one that plays ends -- or false when it starts none, or one that cannot be gapless
// (an online track, the end of the queue pass, Stop, the sleep timer). Mirrors advance_track() / play_next_from_queue() /
// play_relative(); for shuffle the random pick is made here and kept, and play_relative_random() takes it.
bool App::gapless_predict(GapTarget& t) {
    t = GapTarget{};
    if (sleep_->stop_after_track) return false;
    const int pm = settings_.play_mode;
    if (pm == 3) return false;   // Stop
    if (pm == 1) {               // Repeat: the same buffer once more (a windowed one holds only its end now)
        if (!current_.is_local || !current_.pcm || current_.pcm->windowed) return false;
        t.path = path_utf8(current_.path);
        t.loop = true;
        return true;
    }
    if (queue_.empty() && pm == 0 && !qst_.locked && queue_item_playing_) return false;   // list with an unlocked queue: the queue was played through, playback stops
    if (!queue_.empty()) {
        if (pm == 2) return false;   // shuffle with titles in the queue (SQ): the title is picked at random when the track ends -- not known in advance
        size_t idx = 0;
        if (pm == 4 && qst_.locked) {
            auto first = std::find_if(queue_.begin(), queue_.end(), [](const QueueItem& q) { return !q.played; });
            if (first == queue_.end()) return false;   // the pass is over: playback stops
            idx = static_cast<size_t>(first - queue_.begin());
        }
        const QueueItem& q = queue_[idx];
        if (!q.is_local) return false;
        t.path = path_utf8(q.local_path);
        t.title = clean_tag_text(path_utf8(q.local_path.stem()));   // (as play_queue_item() names it)
        return true;
    }
    if (pm == 4) return false;   // queue then stop, with the queue empty
    if (list_source_ != ListSource::Local || local_view_.empty()) return false;
    const int len = static_cast<int>(local_view_.size());
    int next;
    if (pm == 2) {               // shuffle
        if (gap_.shuffle_pick >= 0 && gap_.shuffle_pick < len && path_utf8(local_view_[gap_.shuffle_pick].path) == gap_.shuffle_path) {
            next = gap_.shuffle_pick;
        } else {
            int base = current_track_list_index();
            if (base < 0) base = selected_;
            next = shuffle_pick_next(len, base);
            gap_.shuffle_pick = next;
            gap_.shuffle_path = path_utf8(local_view_[next].path);
        }
    } else {
        int base = current_track_list_index();
        if (base < 0) base = selected_;
        next = std::clamp(base + 1, 0, len - 1);
    }
    t.path = path_utf8(local_view_[next].path);
    t.title = local_view_[next].title;
    return true;
}

// Takes back what was prepared. A switch that happened meanwhile is handled as one.
void App::gapless_cancel() {
    if (gap_.phase == 2) {
        const int r = player_.clear_next();
        if (r == 0) return;                                  // (the player is busy: next frame)
        if (r < 0) {                                         // too late: it plays already
            // ... as the end of the track, unless another track is on its way (the user's choice): that one replaces it in a moment
            if (has_track_ && !advancing_ && !load_in_progress_.load()) { gapless_on_switch(); return; }
        }
    }
    gap_.phase = 0;
    gap_.job.reset();
    gap_.key.clear();
    gap_.path.clear();
}

// The player went on into the prepared track: the end of the track that played, handled as always -- but with the new one in already.
void App::gapless_on_switch() {
    ++gap_.switches;
    ConsoleLog::instance().log_basic("gapless: into \"" + path_utf8(fs::path(path_from_utf8(gap_.path)).filename()) + "\"");
    gap_.phase = 0;
    gap_.key.clear();
    gap_.for_pcm = nullptr;
    gap_.switched = true;
    gap_.switched_job = std::move(gap_.job);
    gap_.job.reset();
    const bool loop = settings_.play_mode == 1;
    history_force_complete_ = true;   // (the play that ends was heard to its last sample; finished() is reset already)
    advance_track();                  // picks the predicted track: launch_load_async() takes the preload (Repeat: advance_track() itself)
    if (loop) history_force_complete_ = false;
    if (gap_.switched) {
        // Not taken: advance_track() started something else (or nothing). The prepared track plays already -- a normal load replaces
        // it in a moment; with nothing to follow, the sound stops here.
        gap_.switched = false;
        gap_.switched_job.reset();
        history_force_complete_ = false;
        if (!load_in_progress_.load() && !has_track_) device_stop();
    }
}

// Gapless Playback as it works now: a crossfade needs the next track in memory early and any track (full), so it makes it "full".
int App::gapless_mode() const { return settings_.fade_cross && !audiobook_ ? 2 : settings_.gapless; }

// The AUDIO FADE OPTIONS to the player (music player only: not for audio books).
void App::apply_fade_options() {
    Player::FadeOptions o;
    o.enabled = !audiobook_;
    o.in = settings_.fade_in; o.out = settings_.fade_out; o.cross = settings_.fade_cross; o.trim = settings_.fade_trim;
    o.len_ms = settings_.fade_len_ms; o.curve = settings_.fade_curve; o.thr_db = settings_.fade_trim_db;
    player_.set_fade_options(o);
}

void App::gapless_tick() {
    apply_fade_options();   // (also follows a change of mode: no fades in the audio book player)
    player_.service_trim();
    if (gap_.phase == 2 && player_.take_switch()) { gapless_on_switch(); return; }
    const bool on = gapless_mode() != 0 && !audiobook_ && has_track_ && current_.is_local && !advancing_ && !load_in_progress_.load() &&
                    device_->play_pending_gen.load() == 0 && current_.pcm && !creator_->show && std::fabs(player_.speed() - 1.0) < 1e-3;
    if (!on || current_.pcm.get() != gap_.for_pcm) {
        if (gap_.phase != 0) { gapless_cancel(); if (gap_.phase != 0) return; }
        if (current_.pcm.get() != gap_.for_pcm) { gap_.shuffle_pick = -1; gap_.last_why.clear(); }
        gap_.for_pcm = on ? current_.pcm.get() : nullptr;
        if (!on) return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (gap_.phase == 0) {
        const double remaining = static_cast<double>(total_sec_) - player_.poll_elapsed();
        if (total_sec_ == 0 || remaining > std::max(kGapLeadSeconds, settings_.fade_cross ? settings_.fade_len_ms / 1000.0 + 6.0 : 0.0) || now < gap_.next_check) return;
        gap_.next_check = now + kGapCheck;
        GapTarget t;
        if (!gapless_predict(t)) return;   // (asked again in a moment: the queue or the mode may still change)
        gap_.key = t.path + (t.loop ? "|loop" : "");
        gap_.path = t.path;
        gap_.phase = 1;
        if (t.loop) return;                // (the same buffer: handed over below)
        auto job = std::make_shared<GapJob>();
        LoadJob j;
        j.local_path = path_from_utf8(t.path);
        j.title = t.title.empty() ? clean_tag_text(path_utf8(j.local_path.stem())) : t.title;
        j.location_label = path_utf8(j.local_path.parent_path().filename()) + "/";
        j.is_local = true;
        j.want_stereo = settings_.stereo;
        j.sid_seconds = settings_.sid_play_length;
        if (settings_.video_mode && !LocalSource::is_video_file(j.local_path) && video_own(t.path).mp4audio == 1)
            j.video_audio = video_source_for(j.local_path);
        j.gap = job;
        j.force_rate = player_.sample_rate();
        j.album_only = gapless_mode() == 1;
        j.cur_path = current_.path;
        j.cur_album = metadata_.album;
        j.cur_artist = metadata_.artist;
        gap_.job = job;
        spawn_bg("gapless preload", [this, j]() { load_job(j); });
        return;
    }
    if (gap_.phase == 3 && now < gap_.next_check) return;
    if (now >= gap_.next_check) {   // the prediction still holds?
        gap_.next_check = now + kGapCheck;
        GapTarget t;
        const bool any = gapless_predict(t);
        if (!any || t.path + (t.loop ? "|loop" : "") != gap_.key) { gapless_cancel(); return; }
    }
    if (gap_.phase != 1) return;
    if (gap_.key.size() > 5 && gap_.key.compare(gap_.key.size() - 5, 5, "|loop") == 0) {   // Repeat (the same track: "album" too)
        if (player_.queue_next(current_.pcm)) gap_.phase = 2;
        return;
    }
    if (!gap_.job) return;
    PendingLoad* pl = nullptr;
    {
        std::lock_guard<std::mutex> lk(gap_.job->mu);
        if (!gap_.job->ready) return;
        pl = &gap_.job->pl;
    }
    // (the preload thread is done with pl once ready is set; only the decode threads go on filling the buffer)
    if (!pl->success || !pl->pcm) {
        gap_.last_why = pl->error.empty() ? "the next track could not be loaded" : pl->error;
        ConsoleLog::instance().log_verbose("gapless: not into \"" + path_utf8(fs::path(path_from_utf8(gap_.path)).filename()) + "\" -- " + gap_.last_why);
        gap_.phase = 3;
        gap_.job.reset();
        return;
    }
    if (pl->pcm->sample_rate != player_.sample_rate()) {   // (the device was opened anew meanwhile, at another rate)
        gapless_cancel();
        return;
    }
    {   // handed over only once its start is decoded (a buffer the player reaches with nothing in it would play silence there)
        StreamingPcm& n = *pl->pcm;
        size_t lo = 0, hi = 0;
        n.readable(lo, hi);
        const size_t want = static_cast<size_t>(std::max(1, n.sample_rate)) * 2;
        const size_t total = n.windowed ? n.total_frames.load() : 0;
        if (!n.decode_done.load() && hi < want && !(total > 0 && hi >= total)) return;
    }
    if (player_.queue_next(pl->pcm)) gap_.phase = 2;    // (busy: the next frame tries again)
}

} // namespace muisc
