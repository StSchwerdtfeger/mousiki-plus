#pragma once
// File conversion for the big conversion overlay (SHIFT+c): probes audio files for the details the overlay filters on
// and converts marked files with ffmpeg in a background thread. No UI code in here.
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// What ffprobe says about a file. `probed` stays false until the probe ran (or when ffprobe is missing / failed).
struct AudioInfo {
    bool probed = false;
    std::string codec;          // "flac", "alac", "pcm_s16le", "mp3", "aac", "opus", "vorbis", ...
    int sample_rate = 0;        // Hz
    int channels = 0;
    int bits = 0;               // bits per sample (lossless codecs; 0 = unknown / not applicable)
    int bitrate_kbps = 0;       // whole-file average
    double duration = 0.0;      // seconds
    bool lossless = false;
    bool hires = false;         // more than 16 bit or more than 48 kHz
};

AudioInfo probe_audio_info(const fs::path& file);
bool codec_is_lossless(const std::string& codec);

// The targets of the conversion menu. A handful on purpose; no custom bitrate.
struct ConvertPreset {
    const char* label;   // "MP3 320 kbps"
    const char* ext;     // ".mp3"
    int est_kbps;        // for the size estimate (V0 = about 245)
};
int convert_preset_count();
const ConvertPreset& convert_preset(int index);
// Estimated size in bytes of `seconds` of audio in the preset.
long long convert_estimate_bytes(int preset, double seconds);

struct ConvertItem {
    fs::path src;
    fs::path dst;            // the final file; written as "<dst>.part" first
    double duration = 0.0;   // seconds (for the progress of the running file)
    int sample_rate = 0;
    int channels = 0;
};

struct ConvertProgress {
    bool running = false;
    bool finished = false;   // a run has ended (kept until the next start)
    bool cancelled = false;
    int total = 0;
    int done = 0;            // converted
    int skipped = 0;         // the target file existed already
    int failed = 0;
    std::string current;     // file name being converted
    double fraction = 0.0;   // of the current file, 0..1
    std::vector<std::string> errors;   // "name: what ffmpeg said"
    std::string out_dir;
};

class Converter {
public:
    Converter() = default;
    ~Converter();
    Converter(const Converter&) = delete;
    Converter& operator=(const Converter&) = delete;

    // false while a run is going. The thread starts at once.
    bool start(std::vector<ConvertItem> items, int preset, const std::string& out_dir);
    void cancel();                       // kills the running ffmpeg, removes its .part file, ends the run
    bool running() const { return running_.load(); }
    ConvertProgress snapshot() const;

private:
    void run(std::vector<ConvertItem> items, int preset);
    mutable std::mutex mu_;
    ConvertProgress prog_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_{false};
    void* child_ = nullptr;              // the running ChildProcess (guarded by mu_)
};

} // namespace muisc
