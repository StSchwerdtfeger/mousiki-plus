// Audio book pictures: decodes the built-in book artwork and draws it as braille dots when no cover is
// available.
#include "book_art.h"
#include <algorithm>
#include <cstring>
#include "miniz.h"
#include <map>
#include <utility>
#include <cmath>

namespace muisc {

namespace {
#include "book_art_data.inc"
// The picture of the audio book mode: Original image by Kamara Rahmat (iyaniyan) from Unsplash (https://unsplash.com/de/grafiken/young-woman-reading-a-blue-book-on-a-blanket-outdoors-brNfZ1TdBN0)
#include "book_pic_data.inc"

int hex2(const char* p) {
    auto d = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    return d(p[0]) * 16 + d(p[1]);
}
} // namespace

const BookArt& BookArt::get(Which which) {
    static const BookArt book(kBookPalette, kBookData, kBookFrames, kBookW, kBookH, 25.0);   // (the Lottie files' own frame rates)
    static const BookArt stack(kStackPalette, kStackData, kStackFrames, kStackW, kStackH, 30.0);
    return which == Which::Book ? book : stack;
}

BookArt::BookArt(const char* palette, const char* const* data, int frames, int w, int h, double fps)
    : frames_(frames), w_(w), h_(h), fps_(fps) {
    const size_t pn = std::strlen(palette) / 6;
    rgb_.assign((pn + 1) * 3, 0);
    for (size_t i = 0; i < pn; ++i)
        for (int c = 0; c < 3; ++c) rgb_[(i + 1) * 3 + static_cast<size_t>(c)] = static_cast<unsigned char>(hex2(palette + i * 6 + c * 2));
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    px_.reserve(static_cast<size_t>(frames));
    std::vector<unsigned char> prev(n, 0);
    for (int f = 0; f < frames; ++f) {
        std::vector<unsigned char> cur = prev;   // the data is a delta against the previous frame
        size_t pos = 0;
        for (const char* p = data[f]; p[0] && p[1] && p[2] && p[3]; p += 4) {
            const int idx = hex2(p), len = hex2(p + 2);
            for (int i = 0; i < len && pos < n; ++i, ++pos) if (idx != 255) cur[pos] = static_cast<unsigned char>(idx);
        }
        px_.push_back(cur);
        prev = cur;
    }
}

void BookArt::append_braille(std::string& out, unsigned char bits) {
    const unsigned cp = 0x2800u + bits;
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
}

// A dot is lit where the picture is opaque and bright enough for an ordered dither pattern (4 x 4 Bayer): pale colours
// (the pages) become solid, mid colours (covers) a pattern, dark lines (the outlines) stay empty, so the books keep
// their edges and the covers can be told apart.
std::vector<unsigned char> BookArt::braille_cells(int frame, std::vector<unsigned char>* shade) const {
    std::vector<unsigned char> out(static_cast<size_t>(cols() * rows()), 0);
    if (shade) shade->assign(out.size(), 0);
    const auto& px = px_[static_cast<size_t>(((frame % frames_) + frames_) % frames_)];
    static const int bit[4][2] = {{0, 3}, {1, 4}, {2, 5}, {6, 7}};   // braille dot numbers by row / column
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x) {
            const unsigned char i = px[static_cast<size_t>(y * w_ + x)];
            if (!i) continue;
            const double lum = (0.299 * rgb_[static_cast<size_t>(i) * 3] + 0.587 * rgb_[static_cast<size_t>(i) * 3 + 1] +
                                0.114 * rgb_[static_cast<size_t>(i) * 3 + 2]) / 255.0;
            // 0.18 .. 1.0 -> 0 .. 16: the darkest colours (outlines) never light a dot
            const double t = (lum - 0.18) / 0.82 * 16.0;
            if (t <= bayer[y & 3][x & 3] + 0.5 && lum < 0.93) continue;   // (near-white is always lit: solid pages)
            const size_t cell = static_cast<size_t>((y / 4) * cols() + x / 2);
            out[cell] |= static_cast<unsigned char>(1u << bit[y % 4][x % 2]);
            if (shade) (*shade)[cell] = std::max((*shade)[cell], static_cast<unsigned char>(lum * 255.0 + 0.5));
        }
    return out;
}

} // namespace muisc

namespace muisc {

// The picture scaled to fit max_w x max_h: the grey picture is unpacked once (zlib, hex text in the data file), an integral image of it,
// then the mean of the source box of every target pixel.
std::vector<unsigned char> BookArt::scale_grey(const std::vector<unsigned char>& src, int W, int H, int max_w, int max_h, int& out_w, int& out_h) {
    out_w = out_h = 0;
    if (W < 1 || H < 1 || src.size() < static_cast<size_t>(W) * static_cast<size_t>(H)) return {};
    const double sc = std::min(std::max(8, max_w) / static_cast<double>(W), std::max(8, max_h) / static_cast<double>(H));
    out_w = std::clamp(static_cast<int>(std::lround(W * sc)), 4, 2000);
    out_h = std::clamp(static_cast<int>(std::lround(H * sc)), 4, 2000);
    std::vector<unsigned int> integral(static_cast<size_t>(W + 1) * (H + 1), 0);
    for (int y = 0; y < H; ++y) {
        unsigned int run = 0;
        for (int x = 0; x < W; ++x) {
            run += src[static_cast<size_t>(y) * W + x];
            integral[static_cast<size_t>(y + 1) * (W + 1) + x + 1] = integral[static_cast<size_t>(y) * (W + 1) + x + 1] + run;
        }
    }
    std::vector<unsigned char> out(static_cast<size_t>(out_w) * out_h, 0);
    for (int y = 0; y < out_h; ++y) {
        const int y0 = y * H / out_h, y1 = std::max(y0 + 1, (y + 1) * H / out_h);
        for (int x = 0; x < out_w; ++x) {
            const int x0 = x * W / out_w, x1 = std::max(x0 + 1, (x + 1) * W / out_w);
            const unsigned int sum = integral[static_cast<size_t>(y1) * (W + 1) + x1] - integral[static_cast<size_t>(y0) * (W + 1) + x1] -
                                     integral[static_cast<size_t>(y1) * (W + 1) + x0] + integral[static_cast<size_t>(y0) * (W + 1) + x0];
            out[static_cast<size_t>(y) * out_w + x] = static_cast<unsigned char>(sum / static_cast<unsigned>((x1 - x0) * (y1 - y0)));
        }
    }
    return out;
}

const std::vector<unsigned char>& BookArt::picture(int max_w, int max_h, int& out_w, int& out_h) {
    static std::map<std::pair<int, int>, std::vector<unsigned char>> cache;
    static std::vector<unsigned int> integral;   // (W + 1) x (H + 1)
    const int W = kBookPicW, H = kBookPicH;
    const double sc = std::min(std::max(8, max_w) / static_cast<double>(W), std::max(8, max_h) / static_cast<double>(H));
    out_w = std::clamp(static_cast<int>(std::lround(W * sc)), 8, 2000);
    out_h = std::clamp(static_cast<int>(std::lround(H * sc)), 8, 2000);
    auto it = cache.find({out_w, out_h});
    if (it != cache.end()) return it->second;
    if (integral.empty()) {
        std::vector<unsigned char> z;
        z.reserve(static_cast<size_t>(kBookPicZLen));
        for (const char* row : kBookPicZ)
            for (const char* p = row; p[0] && p[1]; p += 2) z.push_back(static_cast<unsigned char>(hex2(p)));
        mz_ulong n = static_cast<mz_ulong>(W) * static_cast<mz_ulong>(H);
        std::vector<unsigned char> grey(static_cast<size_t>(n), 0);
        if (mz_uncompress(grey.data(), &n, z.data(), static_cast<mz_ulong>(z.size())) != MZ_OK) std::fill(grey.begin(), grey.end(), 0);
        integral.assign(static_cast<size_t>(W + 1) * (H + 1), 0);
        for (int y = 0; y < H; ++y) {
            unsigned int run = 0;
            for (int x = 0; x < W; ++x) {
                run += grey[static_cast<size_t>(y) * W + x];
                integral[static_cast<size_t>(y + 1) * (W + 1) + x + 1] = integral[static_cast<size_t>(y) * (W + 1) + x + 1] + run;
            }
        }
    }
    std::vector<unsigned char> out(static_cast<size_t>(out_w) * out_h, 0);
    for (int y = 0; y < out_h; ++y) {
        const int y0 = y * H / out_h, y1 = std::max(y0 + 1, (y + 1) * H / out_h);
        for (int x = 0; x < out_w; ++x) {
            const int x0 = x * W / out_w, x1 = std::max(x0 + 1, (x + 1) * W / out_w);
            const unsigned int sum = integral[static_cast<size_t>(y1) * (W + 1) + x1] - integral[static_cast<size_t>(y0) * (W + 1) + x1] -
                                     integral[static_cast<size_t>(y1) * (W + 1) + x0] + integral[static_cast<size_t>(y0) * (W + 1) + x0];
            out[static_cast<size_t>(y) * out_w + x] = static_cast<unsigned char>(sum / static_cast<unsigned>((x1 - x0) * (y1 - y0)));
        }
    }
    return cache.emplace(std::make_pair(out_w, out_h), std::move(out)).first->second;
}

} // namespace muisc
