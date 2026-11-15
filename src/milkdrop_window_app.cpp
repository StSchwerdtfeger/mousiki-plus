// The MilkDrop window process (`mousiki --milkdrop-window`): receives finished pictures (RGB) and the title from its standard input
// (sent by scope_window.cpp) and shows them with the graphics card, at the monitor's refresh rate. The presets are drawn by the engine
// in the player (milkdrop.cpp); this window does no drawing of its own, so there is no terminal conversion / encoding / decoding on the way.
#include "app_dirs.h"
#include "scope_window.h"
#include "scope_sdl.h"
#include "string_util.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace muisc {

namespace {

struct Shared {
    std::mutex m;
    MilkWinConfig cfg;
    unsigned cfg_gen = 0;
    std::vector<uint8_t> rgb;   // the newest picture not drawn yet
    int w = 0, h = 0;
    bool fresh = false;
    std::atomic<bool> eof{false};
};

void write_error(const std::string& msg) {
    std::error_code ec;
    const std::filesystem::path dir = muisc::app_dirs::cache();
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / "milkdrop_window_error.txt") << msg << "\n";
}

bool read_exact(void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        const size_t got = std::fread(p, 1, n, stdin);
        if (got == 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

void reader_main(Shared* S) {
    std::vector<char> payload;
    for (;;) {
        uint8_t head[5];
        if (!read_exact(head, sizeof head)) break;
        const uint32_t len = static_cast<uint32_t>(head[1]) | (static_cast<uint32_t>(head[2]) << 8) |
                             (static_cast<uint32_t>(head[3]) << 16) | (static_cast<uint32_t>(head[4]) << 24);
        if (len > (64u << 20)) break;
        payload.resize(len);
        if (len > 0 && !read_exact(payload.data(), len)) break;
        if (head[0] == 1 && len == sizeof(MilkWinConfig)) {
            MilkWinConfig c;
            std::memcpy(&c, payload.data(), sizeof c);
            if (c.magic != MilkWinConfig{}.magic) continue;
            std::lock_guard<std::mutex> lk(S->m);
            S->cfg = c;
            ++S->cfg_gen;
        } else if (head[0] == 3 && len > 8) {
            uint32_t w, h;
            std::memcpy(&w, payload.data(), 4);
            std::memcpy(&h, payload.data() + 4, 4);
            if (w < 1 || h < 1 || w > 8192 || h > 8192 || static_cast<uint64_t>(w) * h * 3 + 8 != len) continue;
            std::lock_guard<std::mutex> lk(S->m);
            S->rgb.assign(reinterpret_cast<const uint8_t*>(payload.data()) + 8, reinterpret_cast<const uint8_t*>(payload.data()) + len);
            S->w = static_cast<int>(w);
            S->h = static_cast<int>(h);
            S->fresh = true;
        }
    }
    S->eof.store(true);
}

// ---- window position and size, kept between runs ----------------------------------------------------------------------
struct Geometry { int x = sdl::kPosCentered, y = sdl::kPosCentered, w = 960, h = 540; bool full = false, top = false; };

std::filesystem::path geometry_file() { return muisc::app_dirs::config() / "milkdrop_window.txt"; }

Geometry load_geometry(const sdl::Api& A) {
    Geometry g;
    std::ifstream in(geometry_file());
    int x, y, w, h, f, t;
    if (in >> x >> y >> w >> h >> f >> t) {
        g.w = std::clamp(w, 200, 8192);
        g.h = std::clamp(h, 120, 8192);
        g.full = f != 0;
        g.top = t != 0;
        const int n = A.GetNumVideoDisplays();
        for (int i = 0; i < n; ++i) {
            sdl::Rect r{};
            if (A.GetDisplayBounds(i, &r) != 0) continue;
            const int mx = x + g.w / 2, my = y + 16;
            if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) { g.x = x; g.y = y; break; }
        }
    }
    return g;
}

void save_geometry(const Geometry& g) {
    std::error_code ec;
    std::filesystem::create_directories(geometry_file().parent_path(), ec);
    std::ofstream(geometry_file()) << g.x << " " << g.y << " " << g.w << " " << g.h << " " << (g.full ? 1 : 0) << " " << (g.top ? 1 : 0) << "\n";
}

void write_ppm(const std::string& path, const std::vector<uint32_t>& px, int w, int h) {
    std::ofstream o(path, std::ios::binary);
    o << "P6\n" << w << " " << h << "\n255\n";
    for (uint32_t v : px) { const char c[3] = {static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)}; o.write(c, 3); }
}

} // namespace

int milkdrop_window_main() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    Shared& S = *new Shared;   // never freed: the reader thread may still sit in fread() while the window shuts down
    std::thread(reader_main, &S).detach();

    sdl::Api A;
    std::string err;
    if (!sdl::load(A, &err)) { write_error(err); return 2; }
    A.SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    A.SetHint("SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR", "0");
    A.SetHint("SDL_RENDER_SCALE_QUALITY", "1");
#if defined(_WIN32)
    A.SetHint("SDL_RENDER_DRIVER", "direct3d11");
#endif
    if (A.Init(sdl::kInitVideo) != 0) { write_error(std::string("MilkDrop window: SDL could not start the video system: ") + A.GetError()); return 3; }

    Geometry geo = load_geometry(A);
    sdl::Window* win = A.CreateWin("Mousiki MilkDrop", geo.x, geo.y, geo.w, geo.h, sdl::kWinResizable | sdl::kWinHighDpi | sdl::kWinShown);
    if (!win) { write_error(std::string("MilkDrop window: cannot open a window: ") + A.GetError()); A.Quit(); return 4; }
    sdl::Renderer* R = A.CreateRenderer(win, -1, sdl::kRenAccelerated | sdl::kRenVsync);
    if (!R) { A.SetHint("SDL_RENDER_DRIVER", ""); R = A.CreateRenderer(win, -1, sdl::kRenAccelerated | sdl::kRenVsync); }
    if (!R) R = A.CreateRenderer(win, -1, sdl::kRenVsync);
    if (!R) { write_error(std::string("MilkDrop window: no usable renderer: ") + A.GetError()); A.DestroyWindow(win); A.Quit(); return 5; }
    if (geo.full) A.SetWindowFullscreen(win, sdl::kWinFullscreenDesktop);
    if (geo.top && A.SetWindowAlwaysOnTop) A.SetWindowAlwaysOnTop(win, 1);

    const char* shot_path = std::getenv("MOUSIKI_MILK_SHOT");   // tests: save the Nth picture shown as a PPM picture
    const int shot_frame = std::getenv("MOUSIKI_MILK_SHOT_FRAME") ? muisc::atoi_safe(std::getenv("MOUSIKI_MILK_SHOT_FRAME")) : 3;
    const bool shot_exit = std::getenv("MOUSIKI_MILK_SHOT_EXIT") != nullptr;

    sdl::Texture* tex = nullptr;
    int tw = 0, th = 0;
    std::vector<uint8_t> rgb;
    std::vector<uint32_t> px;
    bool have_pic = false;
    MilkWinConfig cfg;
    unsigned cfg_seen = ~0u;
    std::string title_set;
    const double freq = static_cast<double>(A.GetPerformanceFrequency());
    int pics = 0;
    bool quit = false;
    alignas(8) unsigned char ev[128];

    while (!quit) {
        while (A.PollEvent(ev)) {
            uint32_t type;
            std::memcpy(&type, ev, 4);
            if (type == sdl::kEvQuit) quit = true;
            else if (type == sdl::kEvWindow) {
                if (ev[12] == sdl::kWinEvClose) quit = true;
            } else if (type == sdl::kEvKeyDown) {
                int32_t sym;
                std::memcpy(&sym, ev + 20, 4);
                // ';' / '|': the keys that open it in the terminal (by keyboard layout) close it as well
                if (sym == sdl::kKeyEsc || sym == 'q' || sym == ';' || sym == '|') quit = true;
                else if (sym == 'f' || sym == sdl::kKeyF11) {
                    const bool full = (A.GetWindowFlags(win) & sdl::kWinFullscreenDesktop) == sdl::kWinFullscreenDesktop;
                    A.SetWindowFullscreen(win, full ? 0 : sdl::kWinFullscreenDesktop);
                } else if (sym == 't' && A.SetWindowAlwaysOnTop) {
                    geo.top = !geo.top;
                    A.SetWindowAlwaysOnTop(win, geo.top ? 1 : 0);
                }
            } else if (type == sdl::kEvMouseDown) {
                if (ev[18] >= 2) {
                    const bool full = (A.GetWindowFlags(win) & sdl::kWinFullscreenDesktop) == sdl::kWinFullscreenDesktop;
                    A.SetWindowFullscreen(win, full ? 0 : sdl::kWinFullscreenDesktop);
                }
            }
        }
        if (S.eof.load()) quit = true;
        if (quit) break;
        const uint32_t fl = A.GetWindowFlags(win);
        geo.full = (fl & sdl::kWinFullscreenDesktop) == sdl::kWinFullscreenDesktop;
        if (!geo.full && !(fl & sdl::kWinMinimized)) { A.GetWindowPosition(win, &geo.x, &geo.y); A.GetWindowSize(win, &geo.w, &geo.h); }
        if (fl & (sdl::kWinMinimized | sdl::kWinHidden)) { A.Delay(30); continue; }
        const uint64_t t_now = A.GetPerformanceCounter();

        int pw = 0, ph = 0;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> lk(S.m);
            if (S.cfg_gen != cfg_seen) { cfg = S.cfg; cfg_seen = S.cfg_gen; }
            if (S.fresh) { rgb.swap(S.rgb); pw = S.w; ph = S.h; S.fresh = false; fresh = true; }
        }
        {
            std::string t = std::string(cfg.title, strnlen(cfg.title, sizeof cfg.title));
            t = t.empty() ? "Mousiki MilkDrop" : t + "  -  Mousiki MilkDrop";
            if (t != title_set) { A.SetWindowTitle(win, t.c_str()); title_set = t; }
        }
        if (fresh) {
            if (!tex || tw != pw || th != ph) {
                if (tex) A.DestroyTexture(tex);
                tex = A.CreateTexture(R, sdl::kFmtARGB8888, sdl::kTexStatic, pw, ph);
                if (!tex) { write_error(std::string("MilkDrop window: cannot create a texture: ") + A.GetError()); break; }
                A.SetTextureBlendMode(tex, sdl::kBlendNone);
                if (A.SetTextureScaleMode) A.SetTextureScaleMode(tex, sdl::kScaleLinear);
                tw = pw; th = ph;
            }
            px.resize(static_cast<size_t>(pw) * ph);
            const uint8_t* s = rgb.data();
            for (size_t i = 0; i < px.size(); ++i, s += 3) px[i] = 0xFF000000u | (static_cast<uint32_t>(s[0]) << 16) | (static_cast<uint32_t>(s[1]) << 8) | s[2];
            A.UpdateTexture(tex, nullptr, px.data(), pw * 4);
            have_pic = true;
        }
        int ow = 0, oh = 0;
        A.GetRendererOutputSize(R, &ow, &oh);
        A.SetRenderDrawColor(R, 0, 0, 0, 255);
        A.RenderClear(R);
        if (have_pic && tex && ow > 0 && oh > 0) {
            sdl::Rect dst{0, 0, ow, oh};
            if (!cfg.stretch) {   // keep the proportions: black bars
                const double k = std::min(static_cast<double>(ow) / tw, static_cast<double>(oh) / th);
                dst.w = std::max(1, static_cast<int>(tw * k));
                dst.h = std::max(1, static_cast<int>(th * k));
                dst.x = (ow - dst.w) / 2;
                dst.y = (oh - dst.h) / 2;
            }
            A.RenderCopy(R, tex, nullptr, &dst);
        }
        if (fresh && have_pic) ++pics;
        if (shot_path && fresh && pics == shot_frame) {
            std::vector<uint32_t> shot(static_cast<size_t>(ow) * oh);
            if (A.RenderReadPixels(R, nullptr, sdl::kFmtARGB8888, shot.data(), ow * 4) == 0) write_ppm(shot_path, shot, ow, oh);
            if (shot_exit) quit = true;
        }
        A.RenderPresent(R);
        const double spent = static_cast<double>(A.GetPerformanceCounter() - t_now) / freq;
        if (spent < 1.0 / 240.0) A.Delay(static_cast<uint32_t>((1.0 / 240.0 - spent) * 1000.0));
    }

    save_geometry(geo);
    if (tex) A.DestroyTexture(tex);
    A.DestroyRenderer(R);
    A.DestroyWindow(win);
    A.Quit();
    return 0;
}

} // namespace muisc
