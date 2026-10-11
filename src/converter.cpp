#include "converter.h"
#include "path_utf8.h"
#include "process_util.h"
#include "utf8_util.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <system_error>

namespace muisc {

namespace {

constexpr ConvertPreset kPresets[] = {
    {"MP3 320 kbps", ".mp3", 320},
    {"MP3 V0 (~245 kbps)", ".mp3", 245},
    {"MP3 192 kbps", ".mp3", 192},
    {"AAC 256 kbps (.m4a)", ".m4a", 256},
    {"Opus 128 kbps", ".opus", 128},
};

int to_int(const std::string& s) {
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return end == s.c_str() ? 0 : static_cast<int>(v);
}
double to_double(const std::string& s) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return end == s.c_str() ? 0.0 : v;
}

// ffmpeg command for one file. `cover` = carry the embedded picture over (MP3 / M4A only).
std::string build_command(const ConvertItem& it, int preset, bool cover) {
    const ConvertPreset& p = convert_preset(preset);
    const std::string ext = p.ext;
    std::string cmd = "ffmpeg -nostdin -v error -y -progress pipe:1 -i " + shell_quote(path_utf8(it.src));
    cmd += " -map 0:a:0";
    const bool pic = cover && (ext == ".mp3" || ext == ".m4a");
    if (pic) cmd += " -map 0:v:0? -c:v copy";
    else cmd += " -vn";
    cmd += " -map_metadata 0";
    if (ext == ".mp3") {
        cmd += " -c:a libmp3lame";
        if (preset == 1) cmd += " -q:a 0"; else cmd += " -b:a " + std::to_string(p.est_kbps) + "k";
        cmd += " -id3v2_version 3 -write_id3v1 1";
        if (it.sample_rate > 48000) cmd += (it.sample_rate % 44100 == 0) ? " -ar 44100" : " -ar 48000";
        if (it.channels > 2) cmd += " -ac 2";
    } else if (ext == ".m4a") {
        cmd += " -c:a aac -b:a " + std::to_string(p.est_kbps) + "k";
        if (pic) cmd += " -disposition:v:0 attached_pic";
        if (it.sample_rate > 48000) cmd += (it.sample_rate % 44100 == 0) ? " -ar 44100" : " -ar 48000";
        if (it.channels > 2) cmd += " -ac 2";
    } else {   // opus: libopus resamples to 48 kHz by itself
        cmd += " -c:a libopus -b:a " + std::to_string(p.est_kbps) + "k";
        if (it.channels > 2) cmd += " -ac 2";
    }
    const char* muxer = ext == ".mp3" ? "mp3" : ext == ".m4a" ? "ipod" : "opus";
    cmd += std::string(" -f ") + muxer + " " + shell_quote(path_utf8(it.dst) + ".part");
#if !defined(_WIN32)
    cmd = "exec " + cmd;   // kill() must hit ffmpeg, not the shell that started it
#endif
    return cmd;
}

bool is_progress_line(const std::string& l) {
    const size_t eq = l.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    for (size_t i = 0; i < eq; ++i) {
        const char c = l[i];
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

} // namespace

bool codec_is_lossless(const std::string& c) {
    return c == "flac" || c == "alac" || c == "wavpack" || c == "ape" || c == "tta" || c == "mlp" || c == "truehd" ||
           c.rfind("pcm_", 0) == 0 || c.rfind("dsd", 0) == 0;
}

AudioInfo probe_audio_info(const fs::path& file) {
    AudioInfo a;
    const ProcResult r = run_capture(
        "ffprobe -v error -select_streams a:0 -show_entries stream=codec_name,sample_rate,channels,bits_per_raw_sample,bits_per_sample"
        ":format=bit_rate,duration -of default=nw=1 " + shell_quote(path_utf8(file)), false);
    if (!r.ok() || r.out.empty()) return a;
    std::istringstream in(r.out);
    std::string line;
    int bits_raw = 0, bits_sample = 0;
    long long fmt_bitrate = 0;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (v == "N/A") continue;
        if (k == "codec_name") a.codec = v;
        else if (k == "sample_rate") a.sample_rate = to_int(v);
        else if (k == "channels") a.channels = to_int(v);
        else if (k == "bits_per_raw_sample") bits_raw = to_int(v);
        else if (k == "bits_per_sample") bits_sample = to_int(v);
        else if (k == "bit_rate") fmt_bitrate = static_cast<long long>(to_double(v));
        else if (k == "duration") a.duration = to_double(v);
    }
    if (a.codec.empty()) return a;
    a.bits = bits_raw > 0 ? bits_raw : bits_sample;
    a.bitrate_kbps = static_cast<int>(fmt_bitrate / 1000);
    a.lossless = codec_is_lossless(a.codec);
    a.hires = a.sample_rate > 48000 || a.bits > 16;
    a.probed = true;
    return a;
}

int convert_preset_count() { return static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0])); }
const ConvertPreset& convert_preset(int i) {
    return kPresets[std::clamp(i, 0, convert_preset_count() - 1)];
}
long long convert_estimate_bytes(int preset, double seconds) {
    return static_cast<long long>(seconds * convert_preset(preset).est_kbps * 1000.0 / 8.0);
}

Converter::~Converter() {
    cancel();
    if (th_.joinable()) th_.join();
}

bool Converter::start(std::vector<ConvertItem> items, int preset, const std::string& out_dir) {
    if (running_.load() || items.empty()) return false;
    if (th_.joinable()) th_.join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        prog_ = ConvertProgress{};
        prog_.running = true;
        prog_.total = static_cast<int>(items.size());
        prog_.out_dir = out_dir;
    }
    cancel_ = false;
    running_ = true;
    th_ = std::thread([this, items = std::move(items), preset]() mutable { run(std::move(items), preset); });
    return true;
}

void Converter::cancel() {
    if (!running_.load()) return;
    cancel_ = true;
    std::lock_guard<std::mutex> lk(mu_);
    if (child_) static_cast<ChildProcess*>(child_)->kill();
}

ConvertProgress Converter::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return prog_;
}

void Converter::run(std::vector<ConvertItem> items, int preset) {
    for (ConvertItem& it : items) {
        if (cancel_.load()) break;
        if (it.sample_rate == 0 || it.duration <= 0.0) {   // the overlay had no probe result for it yet
            const AudioInfo a = probe_audio_info(it.src);
            if (a.probed) { it.sample_rate = a.sample_rate; it.channels = a.channels; it.duration = a.duration; }
        }
        const std::string name = path_utf8(it.src.filename());
        {
            std::lock_guard<std::mutex> lk(mu_);
            prog_.current = name;
            prog_.fraction = 0.0;
        }
        std::error_code ec;
        if (fs::exists(it.dst, ec)) {
            std::lock_guard<std::mutex> lk(mu_);
            ++prog_.skipped;
            continue;
        }
        fs::create_directories(it.dst.parent_path(), ec);
        const fs::path part = path_from_utf8(path_utf8(it.dst) + ".part");

        bool ok = false;
        std::string err;
        for (int attempt = 0; attempt < 2 && !ok && !cancel_.load(); ++attempt) {
            // attempt 0 carries the cover over; when ffmpeg refuses (an odd picture stream) attempt 1 goes without it.
            std::unique_ptr<ChildProcess> child = spawn_capture(build_command(it, preset, attempt == 0), true);
            if (!child) { err = "ffmpeg could not be started (is it installed?)"; break; }
            {
                std::lock_guard<std::mutex> lk(mu_);
                child_ = child.get();
            }
            err.clear();
            std::string pending;
            char buf[4096];
            for (;;) {
                const std::ptrdiff_t n = child->read(buf, sizeof buf);
                if (n <= 0) break;
                pending.append(buf, static_cast<size_t>(n));
                size_t nl;
                while ((nl = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, nl);
                    pending.erase(0, nl + 1);
                    while (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.rfind("out_time_us=", 0) == 0 || line.rfind("out_time_ms=", 0) == 0) {
                        const double secs = to_double(line.substr(12)) / 1e6;
                        if (it.duration > 0) {
                            std::lock_guard<std::mutex> lk(mu_);
                            prog_.fraction = std::clamp(secs / it.duration, 0.0, 1.0);
                        }
                    } else if (!line.empty() && !is_progress_line(line)) {
                        err = line;
                    }
                }
            }
            const int rc = child->wait();
            {
                std::lock_guard<std::mutex> lk(mu_);
                child_ = nullptr;
            }
            ok = rc == 0 && !cancel_.load();
        }
        if (ok) {
            fs::rename(part, it.dst, ec);
            if (ec) { ok = false; err = "could not move the result into place: " + ec.message(); }
        }
        if (!ok) fs::remove(part, ec);
        std::lock_guard<std::mutex> lk(mu_);
        if (ok) ++prog_.done;
        else if (!cancel_.load()) {
            ++prog_.failed;
            if (err.empty()) err = "ffmpeg failed";
            if (prog_.errors.size() < 50) prog_.errors.push_back(name + ": " + err);
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    prog_.running = false;
    prog_.finished = true;
    prog_.cancelled = cancel_.load();
    prog_.current.clear();
    running_ = false;
}

} // namespace muisc
