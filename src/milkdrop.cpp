// milkdrop.cpp -- see milkdrop.h.
#include "milkdrop.h"
#include "console_log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace muisc {

namespace {

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- the sound: one ring for all audio threads (only one plays at a time), read by the render thread ----
constexpr uint32_t kRingFrames = 1u << 15;   // ~0.7 s at 44.1 kHz
struct PcmRing {
    float buf[2 * kRingFrames] = {};
    std::atomic<uint32_t> w{0};   // frames written so far (wraps)
    std::atomic<bool> on{false};  // the render thread runs
    std::atomic<int> rate{44100};
};
PcmRing& ring() { static PcmRing r; return r; }

} // namespace

void milkdrop_feed(const float* stereo, size_t frames, int rate) {
    PcmRing& r = ring();
    if (!r.on.load(std::memory_order_relaxed) || !stereo || frames == 0) return;
    // projectM expects about 44.1 / 48 kHz: above 64 kHz every second frame is enough
    const size_t step = rate > 64000 ? 2 : 1;
    uint32_t w = r.w.load(std::memory_order_relaxed);
    for (size_t i = 0; i < frames; i += step) {
        const uint32_t k = (w % kRingFrames) * 2;
        r.buf[k] = stereo[2 * i];
        r.buf[k + 1] = stereo[2 * i + 1];
        ++w;
    }
    r.rate.store(rate > 64000 ? rate / 2 : rate, std::memory_order_relaxed);
    r.w.store(w, std::memory_order_release);
}

Milkdrop& Milkdrop::instance() {
    static Milkdrop m;
    return m;
}

Milkdrop::~Milkdrop() { stop_thread(); }

void Milkdrop::shutdown() { stop_thread(); }

void Milkdrop::stop_thread() {
    stop_.store(true);
    if (th_.joinable()) th_.join();
    running_.store(false);
    ring().on.store(false);
}

void Milkdrop::configure(const MilkdropOptions& o) {
    if (o == opts_) return;
    const bool dir_changed = o.presets_dir != opts_.presets_dir;
    if ((o.gpu != opts_.gpu || o.extra_dirs != opts_.extra_dirs) && th_.joinable()) {   // another graphics card: the context is made anew (the preset shown comes back)
        std::string keep;
        { std::lock_guard<std::mutex> lk(mu_); keep = path_; }
        stop_thread();
        std::lock_guard<std::mutex> lk(mu_);
        if (!keep.empty()) want_path_ = keep;
    }
    opts_ = o;
    std::lock_guard<std::mutex> lk(mu_);
    thread_opts_ = o;
    opts_dirty_ = true;
    if (dir_changed) avail_ = -1;   // (looked for again)
}

void Milkdrop::next_preset() { std::lock_guard<std::mutex> lk(mu_); cmds_.push_back(1); }
void Milkdrop::prev_preset() { std::lock_guard<std::mutex> lk(mu_); cmds_.push_back(2); }
void Milkdrop::random_preset() { std::lock_guard<std::mutex> lk(mu_); cmds_.push_back(3); }
void Milkdrop::play_named(const std::string& name) { std::lock_guard<std::mutex> lk(mu_); want_named_ = name; }
void Milkdrop::play_path(const std::string& path) { std::lock_guard<std::mutex> lk(mu_); want_path_ = path; }
std::string Milkdrop::preset_path() const { std::lock_guard<std::mutex> lk(mu_); return path_; }
void Milkdrop::set_locked(bool lock) { locked_.store(lock); std::lock_guard<std::mutex> lk(mu_); opts_dirty_ = true; }

std::string Milkdrop::preset_name() const { std::lock_guard<std::mutex> lk(mu_); return name_; }
std::string Milkdrop::status() const { std::lock_guard<std::mutex> lk(mu_); return status_; }
std::string Milkdrop::renderer() const { std::lock_guard<std::mutex> lk(mu_); return renderer_; }
std::string Milkdrop::presets_dir() const { std::lock_guard<std::mutex> lk(mu_); return dir_; }
double Milkdrop::since_change() const {
    const int64_t c = changed_ns_.load();
    return c == 0 ? 1e9 : static_cast<double>(now_ns() - c) / 1e9;
}

const VideoFrame* Milkdrop::frame(int w, int h, bool& fresh) {
    fresh = false;
    if (w < 2 || h < 2 || !available()) return nullptr;
    last_ask_ns_.store(now_ns());
    {
        std::lock_guard<std::mutex> lk(mu_);
        want_w_ = w; want_h_ = h;
        if (front_seq_ != taken_seq_ && front_.w > 0) {
            shown_ = front_;   // (a copy: the thread draws the next one meanwhile)
            taken_seq_ = front_seq_;
            fresh = true;
        }
    }
    ensure_thread();
    return shown_.w > 0 ? &shown_ : nullptr;
}

void Milkdrop::ensure_thread() {
    std::lock_guard<std::mutex> sk(start_mu_);
    if (running_.load()) return;
    if (th_.joinable()) th_.join();   // (an idle stop before)
    stop_.store(false);
    running_.store(true);
    { std::lock_guard<std::mutex> lk(mu_); status_ = "milkdrop: starting ..."; opts_dirty_ = true; }
    th_ = std::thread([this] { run(); });
}

bool Milkdrop::take_window_frame(int w, int h, std::vector<uint8_t>& rgb, int& ow, int& oh) {
    if (w < 2 || h < 2 || !available()) return false;
    last_ask_ns_.store(now_ns());
    bool got = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        want_w_ = w; want_h_ = h;
        if (front_seq_ != win_taken_seq_ && front_.w > 0) {
            ow = front_.w; oh = front_.h;
            rgb = front_.rgb;
            win_taken_seq_ = front_seq_;
            got = true;
        }
    }
    ensure_thread();
    return got;
}

#if !defined(__linux__) && !defined(_WIN32) && !defined(__APPLE__)

bool Milkdrop::available(std::string* why) {
    if (why) *why = "MilkDrop visuals need Linux, macOS or Windows";
    return false;
}
void Milkdrop::run() { running_.store(false); }
std::string Milkdrop::find_presets_dir() const { return {}; }
std::vector<std::string> Milkdrop::extra_preset_dirs() const { return {}; }
std::vector<MilkdropGpu> Milkdrop::gpus() { return {}; }
std::string milkdrop_gpu_label(const std::string& id) { return id.empty() ? "automatic" : id; }

#else

namespace {

// ---- the few EGL / WGL / GL / projectM declarations used (no headers of them needed to build) ----
#if defined(_WIN32)
#define MD_GLAPI __stdcall   // (OpenGL's calling convention on Windows; the same as the default on x64)
#else
#define MD_GLAPI
#endif

#if defined(__linux__)
using EGLDisplay = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLConfig = void*;
using EGLint = int32_t;
using EGLBoolean = unsigned int;
using EGLenum = unsigned int;
constexpr EGLint kEGL_NONE = 0x3038, kEGL_RENDERABLE_TYPE = 0x3040, kEGL_OPENGL_BIT = 0x0008, kEGL_SURFACE_TYPE = 0x3033,
                 kEGL_PBUFFER_BIT = 0x0001, kEGL_WIDTH = 0x3057, kEGL_HEIGHT = 0x3056, kEGL_CONTEXT_MAJOR_VERSION = 0x3098,
                 kEGL_CONTEXT_MINOR_VERSION = 0x30FB, kEGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD, kEGL_CONTEXT_OPENGL_CORE_PROFILE_BIT = 0x1;
constexpr EGLenum kEGL_OPENGL_API = 0x30A2, kEGL_PLATFORM_SURFACELESS_MESA = 0x31DD, kEGL_PLATFORM_DEVICE_EXT = 0x313F;
constexpr EGLint kEGL_DRM_RENDER_NODE_FILE_EXT = 0x3377;
using PFN_eglQueryDevicesEXT = EGLBoolean (*)(EGLint, void**, EGLint*);
using PFN_eglQueryDeviceStringEXT = const char* (*)(void*, EGLint);

using PFN_eglGetProcAddress = void* (*)(const char*);
using PFN_eglGetDisplay = EGLDisplay (*)(void*);
using PFN_eglGetPlatformDisplayEXT = EGLDisplay (*)(EGLenum, void*, const EGLint*);
using PFN_eglInitialize = EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*);
using PFN_eglTerminate = EGLBoolean (*)(EGLDisplay);
using PFN_eglBindAPI = EGLBoolean (*)(EGLenum);
using PFN_eglChooseConfig = EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
using PFN_eglCreateContext = EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
using PFN_eglDestroyContext = EGLBoolean (*)(EGLDisplay, EGLContext);
using PFN_eglMakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
using PFN_eglCreatePbufferSurface = EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint*);
using PFN_eglDestroySurface = EGLBoolean (*)(EGLDisplay, EGLSurface);
#endif

using GLuint = unsigned int;
using GLenum = unsigned int;
using GLsizei = int;
using GLint = int;
constexpr GLenum kGL_FRAMEBUFFER = 0x8D40, kGL_RENDERBUFFER = 0x8D41, kGL_RGBA8 = 0x8058, kGL_COLOR_ATTACHMENT0 = 0x8CE0,
                 kGL_FRAMEBUFFER_COMPLETE = 0x8CD5, kGL_RGBA = 0x1908, kGL_UNSIGNED_BYTE = 0x1401, kGL_PACK_ALIGNMENT = 0x0D05,
                 kGL_RENDERER = 0x1F01;
using PFN_glGenFramebuffers = void (MD_GLAPI*)(GLsizei, GLuint*);
using PFN_glDeleteFramebuffers = void (MD_GLAPI*)(GLsizei, const GLuint*);
using PFN_glBindFramebuffer = void (MD_GLAPI*)(GLenum, GLuint);
using PFN_glGenRenderbuffers = void (MD_GLAPI*)(GLsizei, GLuint*);
using PFN_glDeleteRenderbuffers = void (MD_GLAPI*)(GLsizei, const GLuint*);
using PFN_glBindRenderbuffer = void (MD_GLAPI*)(GLenum, GLuint);
using PFN_glRenderbufferStorage = void (MD_GLAPI*)(GLenum, GLenum, GLsizei, GLsizei);
using PFN_glFramebufferRenderbuffer = void (MD_GLAPI*)(GLenum, GLenum, GLenum, GLuint);
using PFN_glCheckFramebufferStatus = GLenum (MD_GLAPI*)(GLenum);
using PFN_glReadPixels = void (MD_GLAPI*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
using PFN_glPixelStorei = void (MD_GLAPI*)(GLenum, GLint);
using PFN_glGetString = const unsigned char* (MD_GLAPI*)(GLenum);
// (the read-back through two pixel buffer objects: the GPU copies picture n while the CPU takes picture n - 1)
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;
using GLbitfield = unsigned int;
constexpr GLenum kGL_PIXEL_PACK_BUFFER = 0x88EB, kGL_STREAM_READ = 0x88E1;
constexpr GLbitfield kGL_MAP_READ_BIT = 0x0001;
using PFN_glGenBuffers = void (MD_GLAPI*)(GLsizei, GLuint*);
using PFN_glDeleteBuffers = void (MD_GLAPI*)(GLsizei, const GLuint*);
using PFN_glBindBuffer = void (MD_GLAPI*)(GLenum, GLuint);
using PFN_glBufferData = void (MD_GLAPI*)(GLenum, GLsizeiptr, const void*, GLenum);
using PFN_glMapBufferRange = void* (MD_GLAPI*)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
using PFN_glUnmapBuffer = unsigned char (MD_GLAPI*)(GLenum);

using pm_handle = void*;
using pm_playlist = void*;
using pm_load_proc = void* (*)(const char*, void*);
using pm_switched_cb = void (*)(bool, unsigned int, void*);
struct PM {
    pm_handle (*create_with_opengl_load_proc)(pm_load_proc, void*) = nullptr;
    void (*load_preset_file)(pm_handle, const char*, bool) = nullptr;
    void (*pl_clear)(pm_playlist) = nullptr;
    bool (*pl_add_preset)(pm_playlist, const char*, bool) = nullptr;
    void (*destroy)(pm_handle) = nullptr;
    void (*set_window_size)(pm_handle, size_t, size_t) = nullptr;
    void (*opengl_render_frame_fbo)(pm_handle, uint32_t) = nullptr;
    void (*pcm_add_float)(pm_handle, const float*, unsigned int, int) = nullptr;
    unsigned int (*pcm_get_max_samples)() = nullptr;
    void (*set_texture_search_paths)(pm_handle, const char**, size_t) = nullptr;
    void (*set_preset_duration)(pm_handle, double) = nullptr;
    void (*set_hard_cut_sensitivity)(pm_handle, float) = nullptr;
    // (optional: left out where this projectM lacks them)
    void (*set_hard_cut_duration)(pm_handle, double) = nullptr;
    void (*set_aspect_correction)(pm_handle, bool) = nullptr;
    void (*set_mesh_size)(pm_handle, size_t, size_t) = nullptr;
    void (*set_easter_egg)(pm_handle, float) = nullptr;
    void (*set_preset_start_clean)(pm_handle, bool) = nullptr;
    void (*set_hard_cut_enabled)(pm_handle, bool) = nullptr;
    void (*set_soft_cut_duration)(pm_handle, double) = nullptr;
    void (*set_preset_locked)(pm_handle, bool) = nullptr;
    void (*set_fps)(pm_handle, int32_t) = nullptr;
    pm_playlist (*pl_create)(pm_handle) = nullptr;
    void (*pl_destroy)(pm_playlist) = nullptr;
    uint32_t (*pl_add_path)(pm_playlist, const char*, bool, bool) = nullptr;
    void (*pl_set_shuffle)(pm_playlist, bool) = nullptr;
    uint32_t (*pl_play_next)(pm_playlist, bool) = nullptr;
    uint32_t (*pl_play_previous)(pm_playlist, bool) = nullptr;
    uint32_t (*pl_set_position)(pm_playlist, uint32_t, bool) = nullptr;
    uint32_t (*pl_get_position)(pm_playlist) = nullptr;
    uint32_t (*pl_size)(pm_playlist) = nullptr;
    char* (*pl_item)(pm_playlist, uint32_t) = nullptr;
    void (*pl_free_string)(char*) = nullptr;
    void (*pl_set_switched_cb)(pm_playlist, pm_switched_cb, void*) = nullptr;
};
PM g_pm;

// ---- loading a library and its functions ----
#if defined(_WIN32)
template <class F> bool sym(void* lib, const char* name, F& out) {
    out = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name)));
    return out != nullptr;
}
#else
template <class F> bool sym(void* lib, const char* name, F& out) {
    out = reinterpret_cast<F>(dlsym(lib, name));
    return out != nullptr;
}
[[maybe_unused]] void* open_first(std::initializer_list<const char*> names) {
    for (const char* n : names) if (void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return h;
    return nullptr;
}
#endif

// The projectM functions (the same on every system). "" = all there.
std::string load_projectm(void* pm, void* pl) {
    const bool ok =
         sym(pm, "projectm_create_with_opengl_load_proc", g_pm.create_with_opengl_load_proc) &&
         sym(pm, "projectm_destroy", g_pm.destroy) && sym(pm, "projectm_set_window_size", g_pm.set_window_size) &&
         sym(pm, "projectm_load_preset_file", g_pm.load_preset_file) &&
         sym(pl, "projectm_playlist_clear", g_pm.pl_clear) && sym(pl, "projectm_playlist_add_preset", g_pm.pl_add_preset) &&
         sym(pm, "projectm_opengl_render_frame_fbo", g_pm.opengl_render_frame_fbo) &&
         sym(pm, "projectm_pcm_add_float", g_pm.pcm_add_float) && sym(pm, "projectm_pcm_get_max_samples", g_pm.pcm_get_max_samples) &&
         sym(pm, "projectm_set_texture_search_paths", g_pm.set_texture_search_paths) &&
         sym(pm, "projectm_set_preset_duration", g_pm.set_preset_duration) &&
         sym(pm, "projectm_set_hard_cut_sensitivity", g_pm.set_hard_cut_sensitivity) &&
         sym(pm, "projectm_set_hard_cut_enabled", g_pm.set_hard_cut_enabled) &&
         sym(pm, "projectm_set_soft_cut_duration", g_pm.set_soft_cut_duration) &&
         sym(pm, "projectm_set_preset_locked", g_pm.set_preset_locked) && sym(pm, "projectm_set_fps", g_pm.set_fps) &&
         sym(pl, "projectm_playlist_create", g_pm.pl_create) && sym(pl, "projectm_playlist_destroy", g_pm.pl_destroy) &&
         sym(pl, "projectm_playlist_add_path", g_pm.pl_add_path) && sym(pl, "projectm_playlist_set_shuffle", g_pm.pl_set_shuffle) &&
         sym(pl, "projectm_playlist_play_next", g_pm.pl_play_next) && sym(pl, "projectm_playlist_play_previous", g_pm.pl_play_previous) &&
         sym(pl, "projectm_playlist_set_position", g_pm.pl_set_position) && sym(pl, "projectm_playlist_get_position", g_pm.pl_get_position) &&
         sym(pl, "projectm_playlist_size", g_pm.pl_size) && sym(pl, "projectm_playlist_item", g_pm.pl_item) &&
         sym(pl, "projectm_playlist_free_string", g_pm.pl_free_string) &&
         sym(pl, "projectm_playlist_set_preset_switched_event_callback", g_pm.pl_set_switched_cb);
    sym(pm, "projectm_set_hard_cut_duration", g_pm.set_hard_cut_duration);
    sym(pm, "projectm_set_aspect_correction", g_pm.set_aspect_correction);
    sym(pm, "projectm_set_mesh_size", g_pm.set_mesh_size);
    sym(pm, "projectm_set_easter_egg", g_pm.set_easter_egg);
    sym(pm, "projectm_set_preset_start_clean", g_pm.set_preset_start_clean);
    // (4.1.x, the releases so far, lack the first and the fourth: they draw only into the window's own framebuffer)
    return ok ? std::string() : std::string("this projectM is too old (4.2 is needed: today the git version, see the manual)");
}

#if defined(__linux__)

struct EGL {
    PFN_eglGetProcAddress GetProcAddress = nullptr;
    PFN_eglGetDisplay GetDisplay = nullptr;
    PFN_eglInitialize Initialize = nullptr;
    PFN_eglTerminate Terminate = nullptr;
    PFN_eglBindAPI BindAPI = nullptr;
    PFN_eglChooseConfig ChooseConfig = nullptr;
    PFN_eglCreateContext CreateContext = nullptr;
    PFN_eglDestroyContext DestroyContext = nullptr;
    PFN_eglMakeCurrent MakeCurrent = nullptr;
    PFN_eglCreatePbufferSurface CreatePbufferSurface = nullptr;
    PFN_eglDestroySurface DestroySurface = nullptr;
};
EGL g_egl;
void* g_gl_lib = nullptr;

fs::path exe_dir() {   // (the program's own folder, the real one when it was started through a link: /usr/bin/mousiki -> /opt/mousiki/app/mousiki)
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p.parent_path();
}
std::string home_dir() { const char* h = std::getenv("HOME"); return h ? h : ""; }

// projectM where setup.sh (~/.local/lib) or the installer (next to the program) put it, then wherever the system finds it.
std::vector<fs::path> lib_places() {
    std::vector<fs::path> v;
    const fs::path d = exe_dir();
    if (!d.empty()) { v.push_back(d); v.push_back(d / "lib"); }
    const std::string home = home_dir();
    if (!home.empty()) v.push_back(fs::path(home) / ".local/lib");
    return v;
}
void* open_first_in(const std::vector<fs::path>& dirs, std::initializer_list<const char*> names) {
    for (const fs::path& d : dirs)
        for (const char* n : names) {
            std::error_code ec;
            const fs::path f = d / n;
            if (fs::exists(f, ec))
                if (void* h = dlopen(f.c_str(), RTLD_NOW | RTLD_LOCAL)) return h;
        }
    return open_first(names);
}

// Loads the libraries once (they stay loaded). "" = fine, else why not.
std::string load_libraries() {
    static std::string result = [] {
        void* egl = open_first({"libEGL.so.1", "libEGL.so"});
        if (!egl) return std::string("libEGL is not installed (Mesa's EGL)");
        bool ok = sym(egl, "eglGetProcAddress", g_egl.GetProcAddress) && sym(egl, "eglGetDisplay", g_egl.GetDisplay) &&
                  sym(egl, "eglInitialize", g_egl.Initialize) && sym(egl, "eglTerminate", g_egl.Terminate) &&
                  sym(egl, "eglBindAPI", g_egl.BindAPI) && sym(egl, "eglChooseConfig", g_egl.ChooseConfig) &&
                  sym(egl, "eglCreateContext", g_egl.CreateContext) && sym(egl, "eglDestroyContext", g_egl.DestroyContext) &&
                  sym(egl, "eglMakeCurrent", g_egl.MakeCurrent) && sym(egl, "eglCreatePbufferSurface", g_egl.CreatePbufferSurface) &&
                  sym(egl, "eglDestroySurface", g_egl.DestroySurface);
        if (!ok) return std::string("libEGL lacks functions Mousiki needs");
        g_gl_lib = open_first({"libGL.so.1", "libOpenGL.so.0", "libGL.so"});   // (glReadPixels & co. are found here or through EGL)
        const std::vector<fs::path> places = lib_places();
        void* pm = open_first_in(places, {"libprojectM-4.so.4", "libprojectM-4.so"});
        if (!pm) return std::string("projectM 4 is not installed (libprojectM-4: run setup.sh again, or the installer)");
        void* pl = open_first_in(places, {"libprojectM-4-playlist.so.4", "libprojectM-4-playlist.so"});
        if (!pl) return std::string("projectM's playlist library is not installed (libprojectM-4-playlist)");
        return load_projectm(pm, pl);
    }();
    return result;
}

void* gl_proc(const char* name, void*) {
    void* p = g_egl.GetProcAddress ? g_egl.GetProcAddress(name) : nullptr;
    if (!p && g_gl_lib) p = dlsym(g_gl_lib, name);
    return p;
}

// The OpenGL context of the render thread: EGL, offscreen (no window, no surface; a tiny pbuffer where that is not possible).
struct GlContext {
    EGLDisplay dpy = nullptr;
    EGLContext ctx = nullptr;
    EGLSurface surf = nullptr;
};

// Makes the context and makes it current. "" = fine, else why not.
std::string gl_open(GlContext& c, const std::string& gpu) {
    EGLint maj = 0, min = 0;
    if (!gpu.empty()) {   // the graphics card of the choice (Settings -> MilkDrop GPU): its EGL device; if that fails the usual way below is tried
        auto qd = reinterpret_cast<PFN_eglQueryDevicesEXT>(g_egl.GetProcAddress("eglQueryDevicesEXT"));
        auto qs = reinterpret_cast<PFN_eglQueryDeviceStringEXT>(g_egl.GetProcAddress("eglQueryDeviceStringEXT"));
        auto gpd = reinterpret_cast<PFN_eglGetPlatformDisplayEXT>(g_egl.GetProcAddress("eglGetPlatformDisplayEXT"));
        void* devs[32] = {};
        EGLint n = 0;
        if (qd && qs && gpd && qd(32, devs, &n))
            for (EGLint i = 0; i < n && !c.dpy; ++i) {
                const char* node = qs(devs[i], kEGL_DRM_RENDER_NODE_FILE_EXT);
                if (!node || fs::path(node).filename().string() != gpu) continue;
                EGLDisplay d = gpd(kEGL_PLATFORM_DEVICE_EXT, devs[i], nullptr);
                if (d && g_egl.Initialize(d, &maj, &min)) c.dpy = d;
            }
    }
    if (!c.dpy)
        if (auto gpd = reinterpret_cast<PFN_eglGetPlatformDisplayEXT>(g_egl.GetProcAddress("eglGetPlatformDisplayEXT")))
            c.dpy = gpd(kEGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
    if (c.dpy && maj == 0 && !g_egl.Initialize(c.dpy, &maj, &min)) c.dpy = nullptr;
    if (!c.dpy) {
        c.dpy = g_egl.GetDisplay(nullptr);
        if (!c.dpy || !g_egl.Initialize(c.dpy, &maj, &min)) { c.dpy = nullptr; return "no EGL display (no offscreen OpenGL here)"; }
    }
    g_egl.BindAPI(kEGL_OPENGL_API);
    const EGLint cattr[] = {kEGL_RENDERABLE_TYPE, kEGL_OPENGL_BIT, kEGL_SURFACE_TYPE, kEGL_PBUFFER_BIT, kEGL_NONE};
    EGLConfig cfg = nullptr;
    EGLint ncfg = 0;
    g_egl.ChooseConfig(c.dpy, cattr, &cfg, 1, &ncfg);
    const EGLint xattr[] = {kEGL_CONTEXT_MAJOR_VERSION, 3, kEGL_CONTEXT_MINOR_VERSION, 3,
                            kEGL_CONTEXT_OPENGL_PROFILE_MASK, kEGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, kEGL_NONE};
    c.ctx = g_egl.CreateContext(c.dpy, ncfg > 0 ? cfg : nullptr, nullptr, xattr);
    if (!c.ctx) { g_egl.Terminate(c.dpy); c.dpy = nullptr; return "could not make an OpenGL 3.3 context"; }
    if (!g_egl.MakeCurrent(c.dpy, nullptr, nullptr, c.ctx)) {   // (no surfaceless contexts: a tiny pbuffer)
        const EGLint pattr[] = {kEGL_WIDTH, 16, kEGL_HEIGHT, 16, kEGL_NONE};
        if (ncfg > 0) c.surf = g_egl.CreatePbufferSurface(c.dpy, cfg, pattr);
        if (!c.surf || !g_egl.MakeCurrent(c.dpy, c.surf, c.surf, c.ctx)) {
            if (c.surf) g_egl.DestroySurface(c.dpy, c.surf);
            g_egl.DestroyContext(c.dpy, c.ctx); g_egl.Terminate(c.dpy);
            c = GlContext{};
            return "could not use the OpenGL context";
        }
    }
    return {};
}

void gl_close(GlContext& c) {
    if (!c.dpy) return;
    g_egl.MakeCurrent(c.dpy, nullptr, nullptr, nullptr);
    if (c.surf) g_egl.DestroySurface(c.dpy, c.surf);
    if (c.ctx) g_egl.DestroyContext(c.dpy, c.ctx);
    g_egl.Terminate(c.dpy);
    c = GlContext{};
}

void gl_pump(GlContext&) {}

// The usual preset folders, in the order they are tried (next to the program first: the installers put them there).
std::vector<fs::path> preset_places() {
    const std::string home = home_dir();
    std::vector<fs::path> c;
    if (!exe_dir().empty()) c.push_back(exe_dir() / "milkdrop" / "presets");
    if (!home.empty()) {
        c.push_back(fs::path(home) / ".local/share/projectM/presets");
        c.push_back(fs::path(home) / ".projectM/presets");
    }
    c.push_back("/usr/local/share/projectM/presets");
    c.push_back("/usr/share/projectM/presets");
    c.push_back("/usr/local/share/projectm/presets");
    c.push_back("/usr/share/projectm/presets");
    return c;
}
std::vector<fs::path> texture_places() {
    std::vector<fs::path> c;
    if (!exe_dir().empty()) c.push_back(exe_dir() / "milkdrop" / "textures");
    c.push_back(fs::path(home_dir()) / ".local/share/projectM/textures");
    c.push_back("/usr/local/share/projectM/textures");
    c.push_back("/usr/share/projectM/textures");
    return c;
}
const char* kNoPresetsWhere = "milkdrop/presets next to mousiki, ~/.local/share/projectM/presets, /usr/share/projectM/presets";

fs::path from_utf8(const std::string& s) { return fs::path(s); }
std::string to_utf8(const fs::path& p) { return p.string(); }
// A path as projectM's char* functions want it ("" = it cannot be said that way).
std::string native_path(const fs::path& p) { return p.string(); }
// A name projectM gave back (char*), as UTF-8.
std::string utf8_from_native(const char* s) { return s ? std::string(s) : std::string(); }

#elif defined(__APPLE__)

// ---- macOS: CGL (the OpenGL of the system, deprecated but there on every Mac) -- a core-profile context without a window; the
// pictures go into an offscreen framebuffer like on the other systems. Everything is found at run time (dlopen / dlsym): the build needs
// neither the OpenGL framework's headers nor projectM.
using CGLContextObj = void*;
using CGLPixelFormatObj = void*;
using CGLError = int;
using PFN_CGLChoosePixelFormat = CGLError (*)(const int*, CGLPixelFormatObj*, GLint*);
using PFN_CGLDestroyPixelFormat = CGLError (*)(CGLPixelFormatObj);
using PFN_CGLCreateContext = CGLError (*)(CGLPixelFormatObj, CGLContextObj, CGLContextObj*);
using PFN_CGLDestroyContext = CGLError (*)(CGLContextObj);
using PFN_CGLSetCurrentContext = CGLError (*)(CGLContextObj);
constexpr int kCGLPFAColorSize = 8, kCGLPFADepthSize = 12, kCGLPFAAccelerated = 73, kCGLPFAOpenGLProfile = 99,
              kCGLPFAAllowOfflineRenderers = 96;
constexpr int kCGLOGLPVersion_3_2_Core = 0x3200, kCGLOGLPVersion_GL4_Core = 0x4100;
struct CGL {
    PFN_CGLChoosePixelFormat ChoosePixelFormat = nullptr;
    PFN_CGLDestroyPixelFormat DestroyPixelFormat = nullptr;
    PFN_CGLCreateContext CreateContext = nullptr;
    PFN_CGLDestroyContext DestroyContext = nullptr;
    PFN_CGLSetCurrentContext SetCurrentContext = nullptr;
};
CGL g_cgl;
void* g_gl_lib = nullptr;

fs::path exe_dir() {
    // (the program's own folder: a portable copy keeps projectM's libraries and milkdrop/ next to mousiki)
    std::string buf(4096, '\0');
    uint32_t n = static_cast<uint32_t>(buf.size());
    if (_NSGetExecutablePath(&buf[0], &n) != 0) return {};
    buf.resize(std::strlen(buf.c_str()));
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(fs::path(buf), ec);
    return (ec ? fs::path(buf) : p).parent_path();
}
std::string home_dir() { const char* h = std::getenv("HOME"); return h ? h : ""; }

void* open_first_in(const std::vector<fs::path>& dirs, std::initializer_list<const char*> names) {
    for (const fs::path& d : dirs)
        for (const char* n : names) {
            std::error_code ec;
            const fs::path f = d / n;
            if (fs::exists(f, ec))
                if (void* h = dlopen(f.c_str(), RTLD_NOW | RTLD_LOCAL)) return h;
        }
    for (const char* n : names) if (void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return h;
    return nullptr;
}

// Where setup.sh / the installer / Homebrew put projectM (Apple silicon and Intel).
std::vector<fs::path> lib_places() {
    std::vector<fs::path> v;
    const fs::path d = exe_dir();
    if (!d.empty()) { v.push_back(d); v.push_back(d / "lib"); v.push_back(d.parent_path() / "lib"); v.push_back(d.parent_path() / "Frameworks"); }
    const std::string home = home_dir();
    if (!home.empty()) v.push_back(fs::path(home) / ".local/lib");
    v.push_back("/opt/homebrew/lib");
    v.push_back("/usr/local/lib");
    return v;
}

std::string load_libraries() {
    static std::string result = [] {
        g_gl_lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW | RTLD_LOCAL);
        if (!g_gl_lib) return std::string("the system's OpenGL could not be loaded");
        const bool ok = sym(g_gl_lib, "CGLChoosePixelFormat", g_cgl.ChoosePixelFormat) && sym(g_gl_lib, "CGLDestroyPixelFormat", g_cgl.DestroyPixelFormat) &&
                        sym(g_gl_lib, "CGLCreateContext", g_cgl.CreateContext) && sym(g_gl_lib, "CGLDestroyContext", g_cgl.DestroyContext) &&
                        sym(g_gl_lib, "CGLSetCurrentContext", g_cgl.SetCurrentContext);
        if (!ok) return std::string("the system's OpenGL lacks functions Mousiki needs");
        const std::vector<fs::path> places = lib_places();
        void* pm = open_first_in(places, {"libprojectM-4.4.dylib", "libprojectM-4.dylib"});
        if (!pm) return std::string("projectM 4 is not installed (libprojectM-4.dylib: run setup.sh again, or the installer)");
        void* pl = open_first_in(places, {"libprojectM-4-playlist.4.dylib", "libprojectM-4-playlist.dylib"});
        if (!pl) return std::string("projectM's playlist library is not installed (libprojectM-4-playlist.dylib)");
        return load_projectm(pm, pl);
    }();
    return result;
}

void* gl_proc(const char* name, void*) { return g_gl_lib ? dlsym(g_gl_lib, name) : nullptr; }

struct GlContext { CGLContextObj ctx = nullptr; };

std::string gl_open(GlContext& c, const std::string&) {
    // OpenGL 4.1 core where the Mac has it (every Mac since 2013), else 3.2 core; projectM needs 3.3, which is the same family
    for (const int profile : {kCGLOGLPVersion_GL4_Core, kCGLOGLPVersion_3_2_Core}) {
        const int attr[] = {kCGLPFAOpenGLProfile, profile, kCGLPFAColorSize, 24, kCGLPFADepthSize, 16, kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers, 0};
        CGLPixelFormatObj pf = nullptr;
        GLint n = 0;
        if (g_cgl.ChoosePixelFormat(attr, &pf, &n) != 0 || !pf || n <= 0) continue;
        CGLContextObj ctx = nullptr;
        const CGLError e = g_cgl.CreateContext(pf, nullptr, &ctx);
        g_cgl.DestroyPixelFormat(pf);
        if (e != 0 || !ctx) continue;
        if (g_cgl.SetCurrentContext(ctx) != 0) { g_cgl.DestroyContext(ctx); continue; }
        c.ctx = ctx;
        return {};
    }
    return "could not make an OpenGL 3.3 core context (is a graphics driver there?)";
}

void gl_close(GlContext& c) {
    if (!c.ctx) return;
    g_cgl.SetCurrentContext(nullptr);
    g_cgl.DestroyContext(c.ctx);
    c = GlContext{};
}

void gl_pump(GlContext&) {}

// The usual preset folders: next to the program, the user's own, Homebrew's, the system's.
std::vector<fs::path> preset_places() {
    std::vector<fs::path> c;
    const fs::path d = exe_dir();
    if (!d.empty()) { c.push_back(d / "milkdrop" / "presets"); c.push_back(d.parent_path() / "Resources" / "milkdrop" / "presets"); }
    const std::string home = home_dir();
    if (!home.empty()) {
        c.push_back(fs::path(home) / ".local/share/projectM/presets");
        c.push_back(fs::path(home) / "Library/Application Support/projectM/presets");
    }
    c.push_back("/opt/homebrew/share/projectM/presets");
    c.push_back("/usr/local/share/projectM/presets");
    return c;
}
std::vector<fs::path> texture_places() {
    std::vector<fs::path> c;
    const fs::path d = exe_dir();
    if (!d.empty()) { c.push_back(d / "milkdrop" / "textures"); c.push_back(d.parent_path() / "Resources" / "milkdrop" / "textures"); }
    const std::string home = home_dir();
    if (!home.empty()) {
        c.push_back(fs::path(home) / ".local/share/projectM/textures");
        c.push_back(fs::path(home) / "Library/Application Support/projectM/textures");
    }
    c.push_back("/opt/homebrew/share/projectM/textures");
    c.push_back("/usr/local/share/projectM/textures");
    return c;
}
const char* kNoPresetsWhere = "milkdrop/presets next to mousiki, ~/.local/share/projectM/presets, /opt/homebrew/share/projectM/presets";

fs::path from_utf8(const std::string& s) { return fs::path(s); }
std::string to_utf8(const fs::path& p) { return p.string(); }
std::string native_path(const fs::path& p) { return p.string(); }
std::string utf8_from_native(const char* s) { return s ? std::string(s) : std::string(); }

#else   // _WIN32

using PFN_wglCreateContext = HGLRC (WINAPI*)(HDC);
using PFN_wglDeleteContext = BOOL (WINAPI*)(HGLRC);
using PFN_wglMakeCurrent = BOOL (WINAPI*)(HDC, HGLRC);
using PFN_wglGetProcAddress = PROC (WINAPI*)(LPCSTR);
using PFN_wglCreateContextAttribsARB = HGLRC (WINAPI*)(HDC, HGLRC, const int*);
struct WGL {
    HMODULE gl = nullptr;   // opengl32.dll (the system's: it hands the work to the graphics driver)
    PFN_wglCreateContext CreateContext = nullptr;
    PFN_wglDeleteContext DeleteContext = nullptr;
    PFN_wglMakeCurrent MakeCurrent = nullptr;
    PFN_wglGetProcAddress GetProcAddress = nullptr;
};
WGL g_wgl;

fs::path exe_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size() - 1) { buf.resize(n); return fs::path(buf).parent_path(); }
        buf.resize(buf.size() * 2);
    }
}

std::wstring env_w(const wchar_t* name) {
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return n > 0 && n < 2048 ? std::wstring(buf, n) : std::wstring();
}

std::string wide_to(UINT cp, const std::wstring& w, bool* lossy = nullptr) {
    if (w.empty()) return {};
    BOOL bad = FALSE;
    const DWORD flags = cp == CP_UTF8 ? 0 : WC_NO_BEST_FIT_CHARS;
    BOOL* pbad = cp == CP_UTF8 ? nullptr : &bad;
    const int n = WideCharToMultiByte(cp, flags, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, pbad);
    if (n <= 0) { if (lossy) *lossy = true; return {}; }
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(cp, flags, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, pbad);
    if (lossy) *lossy = bad != FALSE;
    return s;
}
std::wstring wide_from(UINT cp, const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

fs::path from_utf8(const std::string& s) { return fs::path(wide_from(CP_UTF8, s)); }
std::string to_utf8(const fs::path& p) { return wide_to(CP_UTF8, p.wstring()); }
// projectM takes char* paths in the system's ANSI code page (std::filesystem's narrow strings on Windows). A folder whose name has
// letters outside it (e.g. a user name in another script) goes by its short 8.3 name instead.
std::string native_path(const fs::path& p) {
    bool lossy = false;
    std::string s = wide_to(CP_ACP, p.wstring(), &lossy);
    if (!lossy && !s.empty()) return s;
    std::wstring shortp(4096, L'\0');
    const DWORD n = GetShortPathNameW(p.c_str(), &shortp[0], static_cast<DWORD>(shortp.size()));
    if (n == 0 || n >= shortp.size()) return {};
    shortp.resize(n);
    s = wide_to(CP_ACP, shortp, &lossy);
    return lossy ? std::string() : s;
}
std::string utf8_from_native(const char* s) { return s ? wide_to(CP_UTF8, wide_from(CP_ACP, s)) : std::string(); }

// projectM next to mousiki.exe (where setup.ps1 and the installer put it), else wherever Windows finds it.
HMODULE load_dll(const fs::path& dir, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* n : names) {
        if (!dir.empty()) {
            const fs::path full = dir / n;
            // (LOAD_WITH_ALTERED_SEARCH_PATH: the DLLs it needs -- the playlist library needs projectM's -- come from its folder too)
            if (HMODULE h = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return h;
        }
        if (HMODULE h = LoadLibraryW(n)) return h;
    }
    return nullptr;
}

std::string load_libraries() {
    static std::string result = [] {
        g_wgl.gl = LoadLibraryW(L"opengl32.dll");
        if (!g_wgl.gl) return std::string("opengl32.dll could not be loaded");
        const bool ok = sym(g_wgl.gl, "wglCreateContext", g_wgl.CreateContext) && sym(g_wgl.gl, "wglDeleteContext", g_wgl.DeleteContext) &&
                        sym(g_wgl.gl, "wglMakeCurrent", g_wgl.MakeCurrent) && sym(g_wgl.gl, "wglGetProcAddress", g_wgl.GetProcAddress);
        if (!ok) return std::string("opengl32.dll lacks functions Mousiki needs");
        const fs::path dir = exe_dir();
        HMODULE pm = load_dll(dir, {L"projectM-4.dll", L"libprojectM-4.dll"});
        if (!pm) return std::string("projectM is not installed (projectM-4.dll next to mousiki.exe: run setup.ps1 again, or the installer)");
        HMODULE pl = load_dll(dir, {L"projectM-4-playlist.dll", L"libprojectM-4-playlist.dll"});
        if (!pl) return std::string("projectM's playlist library is missing (projectM-4-playlist.dll next to mousiki.exe)");
        return load_projectm(pm, pl);
    }();
    return result;
}

void* gl_proc(const char* name, void*) {
    void* p = g_wgl.GetProcAddress ? reinterpret_cast<void*>(g_wgl.GetProcAddress(name)) : nullptr;
    const intptr_t v = reinterpret_cast<intptr_t>(p);
    if (v >= -1 && v <= 3)   // (wglGetProcAddress: 0, 1, 2, 3 or -1 = no; the OpenGL 1.1 functions come from opengl32.dll itself)
        p = g_wgl.gl ? reinterpret_cast<void*>(GetProcAddress(g_wgl.gl, name)) : nullptr;
    return p;
}

// The OpenGL context of the render thread: WGL on a window that is never shown (Windows has no context without one); the
// pictures go into an offscreen framebuffer, so the window's own size does not matter.
struct GlContext {
    HWND wnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
};

void gl_close(GlContext& c) {
    if (c.rc) { g_wgl.MakeCurrent(nullptr, nullptr); g_wgl.DeleteContext(c.rc); }
    if (c.dc) ReleaseDC(c.wnd, c.dc);
    if (c.wnd) DestroyWindow(c.wnd);
    c = GlContext{};
}

std::string gl_open(GlContext& c, const std::string&) {
    const HINSTANCE hi = GetModuleHandleW(nullptr);
    const wchar_t* cls = L"MousikiMilkDropGL";
    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hi;
    wc.lpszClassName = cls;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return "could not make the window for OpenGL";
    c.wnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"Mousiki MilkDrop", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, hi, nullptr);
    if (!c.wnd) return "could not make the window for OpenGL";
    c.dc = GetDC(c.wnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int pf = c.dc ? ChoosePixelFormat(c.dc, &pfd) : 0;
    if (!pf || !SetPixelFormat(c.dc, pf, &pfd)) { gl_close(c); return "no OpenGL pixel format (is a graphics driver installed?)"; }
    // a plain context first: only with one current does Windows hand out wglCreateContextAttribsARB (for the 3.3 core context)
    HGLRC tmp = g_wgl.CreateContext(c.dc);
    if (!tmp || !g_wgl.MakeCurrent(c.dc, tmp)) {
        if (tmp) g_wgl.DeleteContext(tmp);
        gl_close(c);
        return "no OpenGL here (is a graphics driver installed?)";
    }
    auto cca = reinterpret_cast<PFN_wglCreateContextAttribsARB>(reinterpret_cast<void*>(g_wgl.GetProcAddress("wglCreateContextAttribsARB")));
    const int attr[] = {0x2091 /*WGL_CONTEXT_MAJOR_VERSION_ARB*/, 3, 0x2092 /*MINOR*/, 3,
                        0x9126 /*WGL_CONTEXT_PROFILE_MASK_ARB*/, 0x1 /*CORE_PROFILE_BIT*/, 0};
    c.rc = cca ? cca(c.dc, nullptr, attr) : nullptr;
    g_wgl.MakeCurrent(nullptr, nullptr);
    g_wgl.DeleteContext(tmp);
    if (!c.rc) { gl_close(c); return "could not make an OpenGL 3.3 context (the graphics driver is too old, or a remote desktop)"; }
    if (!g_wgl.MakeCurrent(c.dc, c.rc)) { gl_close(c); return "could not use the OpenGL context"; }
    return {};
}

// The window belongs to the render thread: its messages are taken here (a window that never answers would hold up programs
// that send a message to all windows).
void gl_pump(GlContext&) {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
}

std::vector<fs::path> preset_places() {
    std::vector<fs::path> c;
    const fs::path dir = exe_dir();
    if (!dir.empty()) c.push_back(dir / "milkdrop" / "presets");
    for (const wchar_t* v : {L"LOCALAPPDATA", L"APPDATA"}) {
        const std::wstring base = env_w(v);
        if (!base.empty()) c.push_back(fs::path(base) / "projectM" / "presets");
    }
    return c;
}
std::vector<fs::path> texture_places() {
    std::vector<fs::path> c;
    const fs::path dir = exe_dir();
    if (!dir.empty()) c.push_back(dir / "milkdrop" / "textures");
    const std::wstring base = env_w(L"LOCALAPPDATA");
    if (!base.empty()) c.push_back(fs::path(base) / "projectM" / "textures");
    return c;
}
const char* kNoPresetsWhere = "milkdrop\\presets next to mousiki.exe, %LOCALAPPDATA%\\projectM\\presets";

#endif

bool has_presets(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    int seen = 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
        const fs::path ext = it->path().extension();   // (compared as paths: no conversion that could throw on Windows)
        if (ext == ".milk" || ext == ".prjm") return true;
        if (++seen > 20000) break;
    }
    return false;
}

// The preset folder: the one set, else the usual places.
fs::path find_presets(const std::string& set) {
    if (!set.empty()) { const fs::path p = from_utf8(set); return has_presets(p) ? p : fs::path(); }
    for (const auto& p : preset_places()) if (has_presets(p)) return p;
    return {};
}

// The folder the list starts from: the one set / the usual places, else (when none has presets) the first extra folder that has some.
fs::path find_presets_extra(const std::string& set, const std::vector<std::string>& extra) {
    fs::path p = find_presets(set);
    if (!p.empty()) return p;
    for (const std::string& e : extra) { const fs::path q = from_utf8(e); if (!e.empty() && has_presets(q)) return q; }
    return {};
}

std::string stem_of(const char* path) {
    if (!path) return {};
    return to_utf8(from_utf8(utf8_from_native(path)).stem());
}

} // namespace

std::string Milkdrop::find_presets_dir() const {
    { std::lock_guard<std::mutex> lk(mu_); if (!dir_.empty()) return dir_; }
    const fs::path p = find_presets_extra(opts_.presets_dir, opts_.extra_dirs);
    return p.empty() ? std::string() : to_utf8(p);
}

std::vector<std::string> Milkdrop::extra_preset_dirs() const {
    std::vector<std::string> out;
    const std::string main = find_presets_dir();
    for (const std::string& e : opts_.extra_dirs)
        if (!e.empty() && e != main && has_presets(from_utf8(e)) && std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
    return out;
}

std::string milkdrop_gpu_label(const std::string& id) {
    if (id.empty()) return "automatic";
    for (const MilkdropGpu& g : Milkdrop::gpus()) if (g.id == id) return g.label;
    return id + " (not found: automatic)";
}

// The graphics cards MilkDrop can draw with. Linux: the EGL devices that are DRM render nodes (a laptop with an integrated and a dedicated
// card lists both); named from /sys/class/drm. Windows / macOS: the system decides (Windows: Settings > Graphics; macOS: automatic).
std::vector<MilkdropGpu> Milkdrop::gpus() {
    static std::vector<MilkdropGpu> out;   // (asked once: the graphics cards do not come and go while the program runs)
    static bool done = false;
    if (done) return out;
#if defined(__linux__)
    if (!load_libraries().empty()) return out;
    auto qd = reinterpret_cast<PFN_eglQueryDevicesEXT>(g_egl.GetProcAddress("eglQueryDevicesEXT"));
    auto qs = reinterpret_cast<PFN_eglQueryDeviceStringEXT>(g_egl.GetProcAddress("eglQueryDeviceStringEXT"));
    void* devs[32] = {};
    EGLint n = 0;
    if (!qd || !qs || !qd(32, devs, &n)) return out;
    auto slurp = [](const fs::path& f) {
        std::ifstream in(f);
        std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return t;
    };
    for (EGLint i = 0; i < n; ++i) {
        const char* node = qs(devs[i], kEGL_DRM_RENDER_NODE_FILE_EXT);
        if (!node || !*node) continue;   // (the software device has no render node)
        MilkdropGpu g;
        g.id = fs::path(node).filename().string();
        const fs::path dev = fs::path("/sys/class/drm") / g.id / "device";
        std::string vendor = slurp(dev / "vendor"), uev = slurp(dev / "uevent"), drv;
        while (!vendor.empty() && (vendor.back() == '\n' || vendor.back() == ' ')) vendor.pop_back();
        const size_t at = uev.find("DRIVER=");
        if (at != std::string::npos) { drv = uev.substr(at + 7, uev.find('\n', at) - at - 7); }
        const std::string v = vendor == "0x8086" ? "Intel" : vendor == "0x10de" ? "NVIDIA" : (vendor == "0x1002" || vendor == "0x1022") ? "AMD" : "GPU";
        g.label = v + (drv.empty() ? "" : " (" + drv + ")") + " " + g.id;
        out.push_back(std::move(g));
    }
#endif
    done = true;
    return out;
}

bool Milkdrop::available(std::string* why) {
    if (broken_.load()) {
        if (why) { std::lock_guard<std::mutex> lk(mu_); *why = broken_why_; }
        return false;
    }
    if (avail_ < 0) {
        avail_why_ = load_libraries();
        if (avail_why_.empty() && find_presets_extra(opts_.presets_dir, opts_.extra_dirs).empty())
            avail_why_ = opts_.presets_dir.empty() ? std::string("no MilkDrop presets found (") + kNoPresetsWhere + ")"
                                                   : "no MilkDrop presets in " + opts_.presets_dir;
        avail_ = avail_why_.empty() ? 1 : 0;
    }
    if (why) *why = avail_why_;
    return avail_ == 1;
}

void Milkdrop::run() {
    auto fail = [this](const std::string& why) {   // (not tried again in this run of the program: available() says why)
        { std::lock_guard<std::mutex> lk(mu_); status_ = "milkdrop: " + why; broken_why_ = why; }
        ConsoleLog::instance().log_basic("milkdrop: " + why);
        broken_.store(true);
        running_.store(false);
    };
    // --- the OpenGL context (offscreen: EGL on Linux, a hidden window's WGL context on Windows)
    GlContext glc;
    std::string o_gpu;
    { std::lock_guard<std::mutex> lk(mu_); o_gpu = thread_opts_.gpu; }
    if (const std::string why = gl_open(glc, o_gpu); !why.empty()) { fail(why); return; }
    auto glGenFramebuffers = reinterpret_cast<PFN_glGenFramebuffers>(gl_proc("glGenFramebuffers", nullptr));
    auto glDeleteFramebuffers = reinterpret_cast<PFN_glDeleteFramebuffers>(gl_proc("glDeleteFramebuffers", nullptr));
    auto glBindFramebuffer = reinterpret_cast<PFN_glBindFramebuffer>(gl_proc("glBindFramebuffer", nullptr));
    auto glGenRenderbuffers = reinterpret_cast<PFN_glGenRenderbuffers>(gl_proc("glGenRenderbuffers", nullptr));
    auto glDeleteRenderbuffers = reinterpret_cast<PFN_glDeleteRenderbuffers>(gl_proc("glDeleteRenderbuffers", nullptr));
    auto glBindRenderbuffer = reinterpret_cast<PFN_glBindRenderbuffer>(gl_proc("glBindRenderbuffer", nullptr));
    auto glRenderbufferStorage = reinterpret_cast<PFN_glRenderbufferStorage>(gl_proc("glRenderbufferStorage", nullptr));
    auto glFramebufferRenderbuffer = reinterpret_cast<PFN_glFramebufferRenderbuffer>(gl_proc("glFramebufferRenderbuffer", nullptr));
    auto glCheckFramebufferStatus = reinterpret_cast<PFN_glCheckFramebufferStatus>(gl_proc("glCheckFramebufferStatus", nullptr));
    auto glReadPixels = reinterpret_cast<PFN_glReadPixels>(gl_proc("glReadPixels", nullptr));
    auto glPixelStorei = reinterpret_cast<PFN_glPixelStorei>(gl_proc("glPixelStorei", nullptr));
    auto glGetString = reinterpret_cast<PFN_glGetString>(gl_proc("glGetString", nullptr));
    auto glGenBuffers = reinterpret_cast<PFN_glGenBuffers>(gl_proc("glGenBuffers", nullptr));
    auto glDeleteBuffers = reinterpret_cast<PFN_glDeleteBuffers>(gl_proc("glDeleteBuffers", nullptr));
    auto glBindBuffer = reinterpret_cast<PFN_glBindBuffer>(gl_proc("glBindBuffer", nullptr));
    auto glBufferData = reinterpret_cast<PFN_glBufferData>(gl_proc("glBufferData", nullptr));
    auto glMapBufferRange = reinterpret_cast<PFN_glMapBufferRange>(gl_proc("glMapBufferRange", nullptr));
    auto glUnmapBuffer = reinterpret_cast<PFN_glUnmapBuffer>(gl_proc("glUnmapBuffer", nullptr));
    // (OpenGL 3.3 has them all; without them the picture is read back the plain way, waiting for the GPU)
    const bool use_pbo = glGenBuffers && glDeleteBuffers && glBindBuffer && glBufferData && glMapBufferRange && glUnmapBuffer && !std::getenv("MOUSIKI_MILKDROP_SYNC_READ");
    GLuint pbo[2] = {0, 0};
    int pbo_next = 0;        // the buffer the next picture is copied into
    bool pbo_full = false;   // the other one holds the picture before (to be taken now)
    auto teardown_gl = [&] {
        if (pbo[0]) { glDeleteBuffers(2, pbo); pbo[0] = pbo[1] = 0; }
        gl_close(glc);
    };
    if (!glGenFramebuffers || !glBindFramebuffer || !glGenRenderbuffers || !glBindRenderbuffer || !glRenderbufferStorage ||
        !glFramebufferRenderbuffer || !glCheckFramebufferStatus || !glReadPixels || !glPixelStorei) {
        teardown_gl(); fail("the OpenGL functions are missing"); return;
    }
    {
        const unsigned char* r = glGetString ? glGetString(kGL_RENDERER) : nullptr;
        std::lock_guard<std::mutex> lk(mu_);
        renderer_ = r ? reinterpret_cast<const char*>(r) : "?";
    }
    // --- projectM and its preset list
    pm_handle pm = g_pm.create_with_opengl_load_proc(gl_proc, nullptr);
    if (!pm) { teardown_gl(); fail("projectM could not start (OpenGL 3.3 is needed)"); return; }
    MilkdropOptions o;
    { std::lock_guard<std::mutex> lk(mu_); o = thread_opts_; }
    const fs::path pdir = find_presets_extra(o.presets_dir, o.extra_dirs);
    std::vector<std::string> tex_store;
    {   // textures: a "textures" folder next to the presets, and the usual places
        std::error_code ec;
        std::vector<fs::path> places = {pdir.parent_path() / "textures", pdir / "textures"};
        for (const auto& t : texture_places()) places.push_back(t);
        for (const fs::path& t : places)
            if (fs::is_directory(t, ec)) {
                std::string n = native_path(t);
                if (!n.empty() && std::find(tex_store.begin(), tex_store.end(), n) == tex_store.end()) tex_store.push_back(std::move(n));
            }
        std::vector<const char*> tp;
        for (const auto& s : tex_store) tp.push_back(s.c_str());
        if (!tp.empty()) g_pm.set_texture_search_paths(pm, tp.data(), tp.size());
    }
    const std::string pdir_native = native_path(pdir);
    pm_playlist pl = pdir_native.empty() ? nullptr : g_pm.pl_create(pm);
    // The list: the favourites when they are asked for (and any of them can be read), else the whole folder.
    bool fav_now = false;
    std::vector<std::string> favs_now;
    auto fill_list = [&](const MilkdropOptions& a) -> uint32_t {
        g_pm.pl_clear(pl);
        uint32_t n = 0;
        fav_now = a.favourites_only;
        favs_now = a.favourites;
        if (a.favourites_only)
            for (const std::string& f : a.favourites) {
                const std::string nf = native_path(from_utf8(f));
                if (!nf.empty() && g_pm.pl_add_preset(pl, nf.c_str(), false)) ++n;
            }
        fav_list_.store(n > 0);
        if (n == 0) {
            n = g_pm.pl_add_path(pl, pdir_native.c_str(), true, false);
            for (const std::string& e : a.extra_dirs) {   // the extra folders join the list (not the one the list started from)
                const fs::path q = from_utf8(e);
                if (e.empty() || q == pdir || !has_presets(q)) continue;
                const std::string nq = native_path(q);
                if (!nq.empty()) n += g_pm.pl_add_path(pl, nq.c_str(), true, false);
            }
        }
        return n;
    };
    uint32_t count = pl ? fill_list(o) : 0;
    if (!pl || count == 0) {
        if (pl) g_pm.pl_destroy(pl);
        g_pm.destroy(pm); teardown_gl();
        fail(pdir_native.empty() ? "projectM cannot read the folder name " + to_utf8(pdir) + " (letters outside the system's code page)"
                                 : "no presets could be read from " + to_utf8(pdir));
        return;
    }
    preset_count_.store(static_cast<int>(count));
    { std::lock_guard<std::mutex> lk(mu_); dir_ = to_utf8(pdir); }
    struct Cb { Milkdrop* self; pm_playlist pl; };
    Cb cb{this, pl};
    g_pm.pl_set_switched_cb(pl, [](bool, unsigned int index, void* u) {
        Cb* c = static_cast<Cb*>(u);
        char* item = g_pm.pl_item(c->pl, index);
        const std::string n = stem_of(item);
        const std::string p = utf8_from_native(item);
        if (item) g_pm.pl_free_string(item);
        c->self->preset_index_.store(static_cast<int>(index));
        c->self->changed_ns_.store(now_ns());
        c->self->resume_index_ = static_cast<int>(index);
        std::lock_guard<std::mutex> lk(c->self->mu_);
        c->self->name_ = n;
        c->self->path_ = p;
    }, &cb);
    auto apply = [&](const MilkdropOptions& a) {
        g_pm.pl_set_shuffle(pl, a.shuffle);
        g_pm.set_preset_duration(pm, a.preset_seconds > 0 ? a.preset_seconds : 1e9);
        g_pm.set_hard_cut_sensitivity(pm, std::clamp(a.cut_sens, 0.5f, 6.0f));
        g_pm.set_hard_cut_enabled(pm, a.hard_cuts && !locked_.load());
        g_pm.set_soft_cut_duration(pm, std::max(0, a.transition));
        g_pm.set_preset_locked(pm, locked_.load());
        g_pm.set_fps(pm, std::clamp(a.fps, 5, 60));
        if (g_pm.set_hard_cut_duration) g_pm.set_hard_cut_duration(pm, std::clamp(a.hard_cut_secs, 1, 120));
        if (g_pm.set_aspect_correction) g_pm.set_aspect_correction(pm, a.aspect);
        if (g_pm.set_mesh_size) g_pm.set_mesh_size(pm, static_cast<size_t>(std::clamp(a.mesh_w, 8, 192)), static_cast<size_t>(std::clamp(a.mesh_h, 6, 144)));
        if (g_pm.set_easter_egg) g_pm.set_easter_egg(pm, static_cast<float>(std::clamp(a.variance, 0, 30)));
        if (g_pm.set_preset_start_clean) g_pm.set_preset_start_clean(pm, a.clean_start);
    };
    apply(o);
    // after an idle stop: the preset from before, found by its file (the list may be another one now: the favourites or all of them);
    // the name and path of before are not kept as the new ones
    std::string resume_path;
    { std::lock_guard<std::mutex> lk(mu_); resume_path.swap(path_); name_.clear(); }
    preset_index_.store(-1);
    int resume_at = -1;
    if (!resume_path.empty()) {
        const fs::path want = from_utf8(resume_path).lexically_normal();
        auto is_it = [&](uint32_t i) {
            char* item = g_pm.pl_item(pl, i);
            const bool hit = item && from_utf8(utf8_from_native(item)).lexically_normal() == want;
            if (item) g_pm.pl_free_string(item);
            return hit;
        };
        if (resume_index_ >= 0 && resume_index_ < static_cast<int>(count) && is_it(static_cast<uint32_t>(resume_index_))) resume_at = resume_index_;
        for (uint32_t i = 0; i < count && resume_at < 0; ++i) if (is_it(i)) resume_at = static_cast<int>(i);
    }
    std::error_code start_ec;
    if (resume_at >= 0) g_pm.pl_set_position(pl, static_cast<uint32_t>(resume_at), true);
    else if (!o.start_preset.empty() && fs::is_regular_file(from_utf8(o.start_preset), start_ec)) {   // the default preset (Settings): the loop below loads it, in the list or on its own
        std::lock_guard<std::mutex> lk(mu_);
        want_path_ = o.start_preset;
    }
    else if (o.shuffle) g_pm.pl_play_next(pl, true);   // (not in the list -- e.g. the favourites only, it is none of them: the list's way)
    else g_pm.pl_set_position(pl, 0, true);

    GLuint fbo = 0, rb = 0;
    int fw = 0, fh = 0;
    std::vector<unsigned char> rgba;
    VideoFrame back;
    const unsigned int max_samples = std::max(1u, g_pm.pcm_get_max_samples());
    std::vector<float> chunk;
    uint32_t rd = ring().w.load();
    ring().on.store(true);
    { std::lock_guard<std::mutex> lk(mu_); status_ = "milkdrop: drawing the first picture ..."; }
    auto next_t = std::chrono::steady_clock::now();
    double ms_avg = 0.0;
    while (!stop_.load()) {
        gl_pump(glc);
        // nobody asked for a picture for 3 s: stop (the context, the presets and the CPU are given back)
        if (now_ns() - last_ask_ns_.load() > 3'000'000'000LL) break;
        int ww = 0, wh = 0;
        std::vector<int> cmds;
        std::string named, shown, want_path;
        bool dirty = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (opts_dirty_) { o = thread_opts_; opts_dirty_ = false; dirty = true; }
            cmds.swap(cmds_);
            named.swap(want_named_);
            want_path.swap(want_path_);
            shown = name_;
            ww = want_w_; wh = want_h_;
        }
        if (dirty) {
            apply(o);
            if (o.favourites_only != fav_now || (o.favourites_only && o.favourites != favs_now)) {   // the list changes: favourites <-> all
                // The preset shown stays: the list only decides what comes next. (It used to go to the list's first preset -- "! Transition /
                // fast transition to black" when the favourites were switched off -- or a random one, even while locked, and also whenever a
                // favourite was added or taken out.) Only when the favourites are switched on and the preset shown is not one of them does a
                // favourite come at once (not while locked).
                std::string cur;
                { std::lock_guard<std::mutex> lk(mu_); cur = path_; }
                const uint32_t n = fill_list(o);
                if (n > 0) {
                    count = n;
                    preset_count_.store(static_cast<int>(n));
                    resume_index_ = -1;
                    int at = -1;
                    const fs::path want = from_utf8(cur).lexically_normal();
                    for (uint32_t i = 0; i < count && at < 0 && !cur.empty(); ++i) {
                        char* item = g_pm.pl_item(pl, i);
                        if (item && from_utf8(utf8_from_native(item)).lexically_normal() == want) at = static_cast<int>(i);
                        if (item) g_pm.pl_free_string(item);
                    }
                    preset_index_.store(at);
                    if (at < 0 && fav_list_.load() && !locked_.load()) {
                        if (o.shuffle) g_pm.pl_play_next(pl, false); else g_pm.pl_set_position(pl, 0, false);
                    }
                }
            }
        }
        if (!want_path.empty()) {   // play_path(): its place in the list (the list goes on from there), else the file on its own
            const fs::path want = from_utf8(want_path).lexically_normal();
            bool hit = false;
            for (uint32_t i = 0; i < count && !hit; ++i) {
                char* item = g_pm.pl_item(pl, i);
                hit = item && from_utf8(utf8_from_native(item)).lexically_normal() == want;
                if (item) g_pm.pl_free_string(item);
                if (hit) g_pm.pl_set_position(pl, i, true);
            }
            const std::string nf = hit ? std::string() : native_path(want);
            if (!hit && !nf.empty()) {
                g_pm.load_preset_file(pm, nf.c_str(), false);
                changed_ns_.store(now_ns());
                preset_index_.store(-1);   // (not a place in the list)
                std::lock_guard<std::mutex> lk(mu_);
                name_ = to_utf8(want.stem());
                path_ = to_utf8(want);
            }
        }
        for (int c : cmds) {
            if (c == 1) g_pm.pl_play_next(pl, true);
            else if (c == 2) g_pm.pl_play_previous(pl, true);
            else if (c == 3) {
                static std::mt19937 rng(std::random_device{}());
                g_pm.pl_set_position(pl, std::uniform_int_distribution<uint32_t>(0, count - 1)(rng), true);
            }
        }
        if (!named.empty() && named != shown) {   // play_named(): look the name up in the list (once a track; the names are not kept)
            for (uint32_t i = 0; i < count; ++i) {
                char* item = g_pm.pl_item(pl, i);
                const bool hit = item && stem_of(item) == named;
                if (item) g_pm.pl_free_string(item);
                if (hit) { g_pm.pl_set_position(pl, i, true); break; }
            }
        }
        // the size: the place on the screen, at most max_width across (same aspect)
        const double f = std::min(1.0, static_cast<double>(std::max(64, o.max_width)) / std::max(1, ww));
        const int tw = std::max(16, static_cast<int>(ww * f)), th = std::max(16, static_cast<int>(wh * f));
        // A place whose size moves by a few pixels (the Settings preview asked for 262 and 266 rows in turn) must not make every picture a new size: each
        // new size makes projectM and the buffers anew, and on some graphics cards that gave shifted, garbled pictures. Small changes are ignored (the
        // picture is scaled to the place by the program that draws it); a real change, more than about 6 %, follows at once.
        const bool size_moved = fw <= 0 || std::abs(tw - fw) > std::max(8, fw / 16) || std::abs(th - fh) > std::max(8, fh / 16);
        if (size_moved) {
            if (fbo) { glDeleteFramebuffers(1, &fbo); glDeleteRenderbuffers(1, &rb); fbo = rb = 0; }
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(kGL_FRAMEBUFFER, fbo);
            glGenRenderbuffers(1, &rb);
            glBindRenderbuffer(kGL_RENDERBUFFER, rb);
            glRenderbufferStorage(kGL_RENDERBUFFER, kGL_RGBA8, tw, th);
            glFramebufferRenderbuffer(kGL_FRAMEBUFFER, kGL_COLOR_ATTACHMENT0, kGL_RENDERBUFFER, rb);
            if (glCheckFramebufferStatus(kGL_FRAMEBUFFER) != kGL_FRAMEBUFFER_COMPLETE) { fail("the offscreen framebuffer could not be made"); break; }
            g_pm.set_window_size(pm, static_cast<size_t>(tw), static_cast<size_t>(th));
            // projectM makes its picture buffers anew at the new size. Some drivers (Intel integrated graphics on Windows) hand out memory that was not
            // cleared: its leftovers stood as red and blue bars along the edges, and the preset's feedback smeared them over the picture (they showed in
            // the Settings preview, where the place is no wider than the picture; the big views crop the edge off). So the current preset is started
            // again from a blank picture right after a size change (only when it is a preset of the list; the setting Clean Start is put back at once).
            if (count > 0 && g_pm.set_preset_start_clean && g_pm.pl_get_position) {
                g_pm.set_preset_start_clean(pm, true);
                g_pm.pl_set_position(pl, g_pm.pl_get_position(pl), true);
                g_pm.set_preset_start_clean(pm, o.clean_start);
            }
            fw = tw; fh = th;
            rgba.assign(static_cast<size_t>(fw) * fh * 4, 0);
            if (use_pbo) {
                if (!pbo[0]) glGenBuffers(2, pbo);
                for (GLuint b : pbo) {
                    glBindBuffer(kGL_PIXEL_PACK_BUFFER, b);
                    glBufferData(kGL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(fw) * fh * 4, nullptr, kGL_STREAM_READ);
                }
                glBindBuffer(kGL_PIXEL_PACK_BUFFER, 0);
                pbo_full = false;
            }
        }
        // the sound since the last picture (what fell out of the ring meanwhile is skipped)
        const uint32_t w = ring().w.load(std::memory_order_acquire);
        if (w - rd > kRingFrames) rd = w - kRingFrames;
        while (rd != w) {
            const uint32_t n = std::min<uint32_t>(std::min<uint32_t>(w - rd, max_samples), 2048);
            chunk.resize(static_cast<size_t>(n) * 2);
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t k = ((rd + i) % kRingFrames) * 2;
                chunk[2 * i] = ring().buf[k];
                chunk[2 * i + 1] = ring().buf[k + 1];
            }
            g_pm.pcm_add_float(pm, chunk.data(), n, 2 /*PROJECTM_STEREO*/);
            rd += n;
        }
        const auto t0 = std::chrono::steady_clock::now();
        g_pm.opengl_render_frame_fbo(pm, fbo);
        glBindFramebuffer(kGL_FRAMEBUFFER, fbo);
        glPixelStorei(kGL_PACK_ALIGNMENT, 1);
        bool have = true;
        if (use_pbo && pbo[0]) {
            // this picture: copied into one buffer by the GPU, without waiting for it; the picture before: taken from the other one (its
            // copy is long done). The CPU no longer waits for the GPU to finish drawing -- one picture later on screen (a frame period).
            if (!pbo_full) {   // (the first picture at this size: read the plain way, so a size that keeps changing still gives pictures)
                glReadPixels(0, 0, fw, fh, kGL_RGBA, kGL_UNSIGNED_BYTE, rgba.data());
                glBindBuffer(kGL_PIXEL_PACK_BUFFER, pbo[pbo_next]);
                glReadPixels(0, 0, fw, fh, kGL_RGBA, kGL_UNSIGNED_BYTE, nullptr);
                glBindBuffer(kGL_PIXEL_PACK_BUFFER, 0);
                pbo_full = true;
                pbo_next = 1 - pbo_next;
            } else {
                glBindBuffer(kGL_PIXEL_PACK_BUFFER, pbo[pbo_next]);
                glReadPixels(0, 0, fw, fh, kGL_RGBA, kGL_UNSIGNED_BYTE, nullptr);
                const int prev = 1 - pbo_next;
                glBindBuffer(kGL_PIXEL_PACK_BUFFER, pbo[prev]);
                const void* p = glMapBufferRange(kGL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(fw) * fh * 4, kGL_MAP_READ_BIT);
                if (p) { std::memcpy(rgba.data(), p, rgba.size()); glUnmapBuffer(kGL_PIXEL_PACK_BUFFER); }
                else have = false;
                glBindBuffer(kGL_PIXEL_PACK_BUFFER, 0);
                pbo_next = prev;
            }
        } else {
            glReadPixels(0, 0, fw, fh, kGL_RGBA, kGL_UNSIGNED_BYTE, rgba.data());
        }
        if (!have) { next_t = std::chrono::steady_clock::now(); continue; }   // (a buffer that could not be read: the next picture at once)
        back.w = fw; back.h = fh;
        back.rgb.resize(static_cast<size_t>(fw) * fh * 3);
        for (int y = 0; y < fh; ++y) {   // (OpenGL's rows go from the bottom up)
            const unsigned char* s = rgba.data() + static_cast<size_t>(fh - 1 - y) * fw * 4;
            unsigned char* d = back.rgb.data() + static_cast<size_t>(y) * fw * 3;
            for (int x = 0; x < fw; ++x) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d += 3; s += 4; }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ms_avg = ms_avg <= 0 ? ms : ms_avg * 0.9 + ms * 0.1;
        frame_ms_.store(ms_avg);
        {   // debugging: MOUSIKI_MD_DUMP=<folder> saves every 6th picture as it comes out of projectM (before any effect, terminal or Sixel), 300 at most
            static const char* dump_dir = std::getenv("MOUSIKI_MD_DUMP");
            static int dumped = 0, every = 0;
            if (dump_dir && *dump_dir && dumped < 300 && (every++ % 6) == 0) {
                std::string nm;
                { std::lock_guard<std::mutex> lk(mu_); nm = name_; }
                char fn[64];
                std::snprintf(fn, sizeof fn, "md_%04d_%dx%d.ppm", dumped, back.w, back.h);
                const fs::path dir = from_utf8(dump_dir);
                if (FILE* f = std::fopen((dir / fn).string().c_str(), "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", back.w, back.h);
                    std::fwrite(back.rgb.data(), 1, back.rgb.size(), f);
                    std::fclose(f);
                }
                if (FILE* l = std::fopen((dir / "md_log.txt").string().c_str(), "ab")) {
                    std::fprintf(l, "%s preset=\"%s\" locked=%d size=%dx%d\n", fn, nm.c_str(), locked_.load() ? 1 : 0, back.w, back.h);
                    std::fclose(l);
                }
                ++dumped;
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::swap(front_, back);
            ++front_seq_;
            status_.clear();
        }
        // the next picture at the frame rate (a slow preset: as fast as it goes, without piling up)
        next_t += std::chrono::microseconds(1000000 / std::clamp(o.fps, 5, 60));
        const auto now = std::chrono::steady_clock::now();
        if (next_t < now) next_t = now;
        while (!stop_.load() && std::chrono::steady_clock::now() < next_t)
            std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(next_t - std::chrono::steady_clock::now(), std::chrono::milliseconds(20)));
    }
    ring().on.store(false);
    g_pm.pl_set_switched_cb(pl, nullptr, nullptr);
    g_pm.pl_destroy(pl);
    g_pm.destroy(pm);
    if (fbo) { glDeleteFramebuffers(1, &fbo); glDeleteRenderbuffers(1, &rb); }
    teardown_gl();
    {
        std::lock_guard<std::mutex> lk(mu_);
        front_ = VideoFrame{};
        ++front_seq_;
    }
    running_.store(false);
}

#endif   // Linux / Windows

} // namespace muisc
