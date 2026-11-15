// milkdrop.h
// MilkDrop visuals (the Winamp visualizer, as rebuilt by projectM: https://github.com/projectM-visualizer/projectm) for the
// visualization field, the big video view and the visuals overlay (":").
//
// Nothing of projectM, OpenGL or EGL is needed to BUILD Mousiki: the libraries are loaded when the visuals are first asked for
// (Linux: libprojectM-4, libprojectM-4-playlist, libEGL; Windows: projectM-4.dll, projectM-4-playlist.dll next to mousiki.exe,
// and the system's opengl32.dll), and without them the view is simply not offered. projectM (LGPL-2.1) is a projectM with
// projectm_create_with_opengl_load_proc() and projectm_opengl_render_frame_fbo() (4.2; today the git version). On Linux the
// user installs it and the preset packs (see the manual); on Windows setup.ps1 and the installer build / bundle them
// (tools/milkdrop-windows.ps1: projectM, the "Cream of the Crop" presets and the texture pack in milkdrop\ next to the exe).
// Linux draws offscreen through a surfaceless EGL context (Mesa; a graphics card when there is one, else llvmpipe); Windows through
// a WGL context on a hidden window (the graphics driver's OpenGL 3.3). macOS: not done yet.
//
// One render thread owns the GL context and projectM. It draws at the frame rate set (MilkDropFrameRate) into an offscreen
// framebuffer of the size the screen asks for (at most MilkDropResolution pixels across, same aspect) and reads every picture back
// as RGB. The sound comes from the audio callbacks of the player / the radio (milkdrop_feed(): a lock-free ring, nothing allocated
// there). When no picture has been asked for for a few seconds the thread stops and frees the context (no CPU, no memory).
#pragma once

#include "video_player.h"   // VideoFrame

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace muisc {

struct MilkdropGpu { std::string id, label; };   // id: the render node ("renderD129"), label: what the lists show ("NVIDIA (nvidia) renderD129")

struct MilkdropOptions {
    int hard_cut_secs = 20;       // the shortest time between two hard cuts (projectM's default 20)
    bool aspect = true;           // aspect correction: presets keep their proportions on a wide picture
    int mesh_w = 32, mesh_h = 24; // the warp mesh (projectM's default 32 x 24): finer = more detail and more work
    int variance = 0;             // seconds: how far the time of a preset may stray, at random (projectM's "easter egg"; 0 = every preset stays the same time)
    bool clean_start = false;     // a preset starts from a blank picture, not from what the one before left
    std::string gpu;              // the graphics card (MilkdropGpu::id; "" = automatic: what the system gives)
    std::string presets_dir;      // "" = the usual places (~/.local/share/projectM/presets, /usr/share/...; Windows: milkdrop\\presets next to the exe)
    int fps = 30;                 // pictures a second
    int max_width = 480;          // rendered at most this many pixels across (the picture is scaled to the screen from there)
    int preset_seconds = 25;      // a new preset after this long (0 = it stays until changed by hand)
    bool shuffle = true;          // the next preset at random (else in the order of the files)
    float cut_sens = 2.0f;        // hard cut sensitivity: the jump in volume that makes a hard cut (projectM's default 2.0; less = more cuts)
    std::string start_preset;     // the preset the visuals start with (a file; "" = one from the list)
    bool hard_cuts = true;        // a strong beat may change the preset at once
    int transition = 3;           // seconds of the blend from one preset into the next (0 = a cut)
    bool favourites_only = false; // the list is the favourites (when there are any) instead of the whole preset folder
    std::vector<std::string> favourites;   // the favourite presets (full paths, UTF-8)
    bool operator==(const MilkdropOptions& o) const {
        return presets_dir == o.presets_dir && fps == o.fps && max_width == o.max_width && preset_seconds == o.preset_seconds &&
               shuffle == o.shuffle && cut_sens == o.cut_sens && start_preset == o.start_preset && hard_cuts == o.hard_cuts && transition == o.transition &&
               favourites_only == o.favourites_only && favourites == o.favourites && gpu == o.gpu &&
               hard_cut_secs == o.hard_cut_secs && aspect == o.aspect && mesh_w == o.mesh_w && mesh_h == o.mesh_h && variance == o.variance && clean_start == o.clean_start;
    }
    bool operator!=(const MilkdropOptions& o) const { return !(*this == o); }
};

class Milkdrop;
inline std::string mesh_name(int i) {   // the warp mesh of the presets (Settings, ":" menu)
    static const char* const n[] = {"low (16 x 12)", "normal (32 x 24)", "high (64 x 48)", "very high (96 x 72)"};
    return n[i < 0 ? 0 : i > 3 ? 3 : i];
}
std::string milkdrop_gpu_label(const std::string& id);   // what the lists show for a choice: "automatic" or the card's label

class Milkdrop {
public:
    static Milkdrop& instance();

    // Can the visuals run here? Loads the libraries once and looks for presets; `why` gets the reason when not (shown in the overlay).
    bool available(std::string* why = nullptr);
    static std::vector<MilkdropGpu> gpus();     // the graphics cards that can be chosen (Linux; empty elsewhere: the system decides)
    void configure(const MilkdropOptions& o);   // taken over by the render thread at its next picture
    const MilkdropOptions& options() const { return opts_; }

    // The newest picture for a place of w x h pixels (the render thread starts here, and is told the size). `fresh`: newer than the
    // one returned before. nullptr while there is none yet (status() says why).
    const VideoFrame* frame(int w, int h, bool& fresh);
    bool running() const { return running_.load(); }

    void next_preset();
    void prev_preset();
    void random_preset();
    void play_named(const std::string& name);   // the preset of this file name (without .milk), if it is in the list (Style Option)
    void play_path(const std::string& path);    // this preset file (full path, UTF-8): its place in the list, else loaded on its own
    void set_locked(bool lock);
    bool locked() const { return locked_.load(); }

    std::string preset_name() const;         // the file name of the preset shown (without .milk)
    int preset_index() const { return preset_index_.load(); }   // 0-based place in the list
    int preset_count() const { return preset_count_.load(); }
    double since_change() const;             // seconds since the preset changed
    std::string status() const;              // "milkdrop: starting ..." / why there is no picture
    std::string renderer() const;            // what draws (the GL renderer string, e.g. "llvmpipe (LLVM 20.1.2, 256 bits)")
    std::string presets_dir() const;         // the folder the presets come from
    std::string preset_path() const;         // the full path of the preset shown (UTF-8; "" before the first)
    std::string find_presets_dir() const;    // the preset folder the options lead to (also before the visuals ran; "" = none)
    bool favourites_in_use() const { return fav_list_.load(); }   // the list is the favourites right now
    double frame_ms() const { return frame_ms_.load(); }   // the time one picture took (drawing + reading back), smoothed

    void shutdown();   // the program ends: the thread stops, the context goes
    ~Milkdrop();

private:
    Milkdrop() = default;
    void run();
    void stop_thread();

    MilkdropOptions opts_;
    mutable std::mutex mu_;           // opts_ for the thread, want_*, cmds_, the picture, names, status
    MilkdropOptions thread_opts_;     // (mu_) what the thread is to use
    bool opts_dirty_ = true;
    int want_w_ = 0, want_h_ = 0;
    std::vector<int> cmds_;           // 1 next, 2 previous, 3 random
    std::string want_named_;          // (mu_) play_named(): the preset to go to
    std::string want_path_;           // (mu_) play_path(): the preset file to go to
    std::string path_;                // (mu_) the full path of the preset shown
    std::atomic<bool> fav_list_{false};
    std::thread th_;
    std::atomic<bool> running_{false}, stop_{false};
    std::atomic<bool> locked_{false};
    std::atomic<int64_t> last_ask_ns_{0};
    VideoFrame front_;                // (mu_) the newest picture
    uint64_t front_seq_ = 0;          // (mu_)
    uint64_t taken_seq_ = 0;          // main thread
    VideoFrame shown_;                // main thread: what frame() returned
    std::string name_, status_, renderer_, dir_;   // (mu_)
    std::atomic<int> preset_index_{-1}, preset_count_{0};
    std::atomic<int64_t> changed_ns_{0};
    std::atomic<double> frame_ms_{0.0};
    int resume_index_ = -1;           // (the thread) the preset to come back to after an idle stop
    int avail_ = -1;                  // -1 not checked, 0 no, 1 yes
    std::string avail_why_;
    std::atomic<bool> broken_{false}; // the render thread could not start here (no OpenGL ...): not tried again; broken_why_ (mu_)
    std::string broken_why_;
};

// The sound for the visuals (audio threads: player, radio): interleaved stereo float, at `rate`. Does nothing while no picture is
// being drawn. Never blocks, never allocates.
void milkdrop_feed(const float* stereo, size_t frames, int rate);

} // namespace muisc
