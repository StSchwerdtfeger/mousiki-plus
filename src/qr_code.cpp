#include "qr_code.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace muisc::qr {

namespace {

constexpr int kMaxVersion = 10;
// error correction level M: ECC codewords per block and number of blocks, versions 1-10 (index 0 unused)
constexpr int kEccPerBlock[kMaxVersion + 1] = {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
constexpr int kNumBlocks[kMaxVersion + 1] = {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
constexpr int kFormatBitsM = 0;   // level M in the format information

int raw_data_modules(int ver) {
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        const int align = ver / 7 + 2;
        r -= (25 * align - 10) * align - 55;
        if (ver >= 7) r -= 36;
    }
    return r;
}
int data_codewords(int ver) { return raw_data_modules(ver) / 8 - kEccPerBlock[ver] * kNumBlocks[ver]; }

uint8_t gf_mul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<uint8_t>(z);
}

std::vector<uint8_t> rs_divisor(int degree) {
    std::vector<uint8_t> r(static_cast<size_t>(degree), 0);
    r[static_cast<size_t>(degree - 1)] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (size_t j = 0; j < r.size(); ++j) {
            r[j] = gf_mul(r[j], root);
            if (j + 1 < r.size()) r[j] ^= r[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
    return r;
}

std::vector<uint8_t> rs_remainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& div) {
    std::vector<uint8_t> r(div.size(), 0);
    for (uint8_t b : data) {
        const uint8_t factor = b ^ r[0];
        r.erase(r.begin());
        r.push_back(0);
        for (size_t i = 0; i < r.size(); ++i) r[i] ^= gf_mul(div[i], factor);
    }
    return r;
}

std::vector<int> alignment_positions(int ver) {
    if (ver == 1) return {};
    const int n = ver / 7 + 2;
    const int size = ver * 4 + 17;
    const int step = (ver == 32) ? 26 : (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
    std::vector<int> r = {6};
    for (int pos = size - 7, k = 0; k < n - 1; ++k, pos -= step) r.insert(r.begin() + 1, pos);
    return r;
}

struct Grid {
    int size;
    std::vector<std::vector<bool>> m, fn;   // modules and "is a function module"
    explicit Grid(int s) : size(s), m(static_cast<size_t>(s), std::vector<bool>(static_cast<size_t>(s), false)), fn(m) {}
    void set(int x, int y, bool dark) { m[static_cast<size_t>(y)][static_cast<size_t>(x)] = dark; fn[static_cast<size_t>(y)][static_cast<size_t>(x)] = true; }
};

void draw_format(Grid& g, int mask) {
    const int data = kFormatBitsM << 3 | mask;
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = (data << 10 | rem) ^ 0x5412;
    auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
    for (int i = 0; i <= 5; ++i) g.set(8, i, bit(i));
    g.set(8, 7, bit(6));
    g.set(8, 8, bit(7));
    g.set(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) g.set(14 - i, 8, bit(i));
    for (int i = 0; i < 8; ++i) g.set(g.size - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) g.set(8, g.size - 15 + i, bit(i));
    g.set(8, g.size - 8, true);   // the dark module
}

void draw_version(Grid& g, int ver) {
    if (ver < 7) return;
    int rem = ver;
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    const long bits = static_cast<long>(ver) << 12 | rem;
    for (int i = 0; i < 18; ++i) {
        const bool b = ((bits >> i) & 1) != 0;
        const int a = g.size - 11 + i % 3, c = i / 3;
        g.set(a, c, b);
        g.set(c, a, b);
    }
}

void draw_function_patterns(Grid& g, int ver) {
    for (int i = 0; i < g.size; ++i) { g.set(6, i, i % 2 == 0); g.set(i, 6, i % 2 == 0); }   // timing
    auto finder = [&](int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                const int d = std::max(std::abs(dx), std::abs(dy)), x = cx + dx, y = cy + dy;
                if (x >= 0 && x < g.size && y >= 0 && y < g.size) g.set(x, y, d != 2 && d != 4);
            }
    };
    finder(3, 3);
    finder(g.size - 4, 3);
    finder(3, g.size - 4);
    const std::vector<int> al = alignment_positions(ver);
    const size_t n = al.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j) {
            if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) g.set(al[i] + dx, al[j] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
        }
    draw_format(g, 0);   // reserved now, the real bits after the mask is chosen
    draw_version(g, ver);
}

void draw_codewords(Grid& g, const std::vector<uint8_t>& data) {
    size_t i = 0;
    for (int right = g.size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < g.size; ++vert)
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? g.size - 1 - vert : vert;
                if (!g.fn[static_cast<size_t>(y)][static_cast<size_t>(x)] && i < data.size() * 8) {
                    g.m[static_cast<size_t>(y)][static_cast<size_t>(x)] = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                    ++i;
                }
            }
    }
}

bool mask_bit(int mask, int x, int y) {
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return x * y % 2 + x * y % 3 == 0;
        case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
        default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

void apply_mask(Grid& g, int mask) {
    for (int y = 0; y < g.size; ++y)
        for (int x = 0; x < g.size; ++x)
            if (!g.fn[static_cast<size_t>(y)][static_cast<size_t>(x)] && mask_bit(mask, x, y))
                g.m[static_cast<size_t>(y)][static_cast<size_t>(x)] = !g.m[static_cast<size_t>(y)][static_cast<size_t>(x)];
}

// The standard's penalty score (lower is better): runs, 2x2 blocks, finder-like patterns, dark/light balance.
long penalty(const Grid& g) {
    const int n = g.size;
    auto at = [&](int x, int y) { return g.m[static_cast<size_t>(y)][static_cast<size_t>(x)]; };
    long p = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 0; a < n; ++a) {
            int run = 1;
            for (int b = 1; b <= n; ++b) {
                const bool same = b < n && (pass == 0 ? at(b, a) == at(b - 1, a) : at(a, b) == at(a, b - 1));
                if (same) { ++run; continue; }
                if (run >= 5) p += 3 + (run - 5);
                run = 1;
            }
        }
    for (int y = 0; y + 1 < n; ++y)
        for (int x = 0; x + 1 < n; ++x)
            if (at(x, y) == at(x + 1, y) && at(x, y) == at(x, y + 1) && at(x, y) == at(x + 1, y + 1)) p += 3;
    static const bool pat[11] = {true, false, true, true, true, false, true, false, false, false, false};
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 0; a < n; ++a)
            for (int b = 0; b + 11 <= n; ++b) {
                bool fwd = true, back = true;
                for (int k = 0; k < 11; ++k) {
                    const bool v = pass == 0 ? at(b + k, a) : at(a, b + k);
                    if (v != pat[k]) fwd = false;
                    if (v != pat[10 - k]) back = false;
                }
                if (fwd) p += 40;
                if (back) p += 40;
            }
    long dark = 0;
    for (const auto& row : g.m) for (bool v : row) dark += v;
    const long total = static_cast<long>(n) * n;
    const long k = (std::labs(dark * 20 - total * 10) + total - 1) / total - 1;
    p += std::max(0L, k) * 10;
    return p;
}

} // namespace

std::vector<std::vector<bool>> encode(const std::string& text) {
    const int len = static_cast<int>(text.size());
    int ver = 1;
    for (; ver <= kMaxVersion; ++ver) {
        const int bits = 4 + (ver <= 9 ? 8 : 16) + 8 * len;
        if (bits <= data_codewords(ver) * 8) break;
    }
    if (ver > kMaxVersion) return {};

    // the data bits: byte mode, the length, the bytes, then the terminator and the pad bytes
    std::vector<bool> bits;
    auto put = [&](unsigned v, int n) { for (int i = n - 1; i >= 0; --i) bits.push_back(((v >> i) & 1) != 0); };
    put(0x4, 4);
    put(static_cast<unsigned>(len), ver <= 9 ? 8 : 16);
    for (unsigned char c : text) put(c, 8);
    const size_t cap = static_cast<size_t>(data_codewords(ver)) * 8;
    put(0, static_cast<int>(std::min<size_t>(4, cap - bits.size())));
    while (bits.size() % 8) bits.push_back(false);
    for (unsigned pad = 0xEC; bits.size() < cap; pad ^= 0xEC ^ 0x11) put(pad, 8);
    std::vector<uint8_t> data(bits.size() / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i) if (bits[i]) data[i >> 3] |= static_cast<uint8_t>(1 << (7 - (i & 7)));

    // split into blocks, add the error correction, interleave
    const int blocks = kNumBlocks[ver], ecc_len = kEccPerBlock[ver];
    const int raw = raw_data_modules(ver) / 8;
    const int short_blocks = blocks - raw % blocks, short_len = raw / blocks;
    const std::vector<uint8_t> div = rs_divisor(ecc_len);
    std::vector<std::vector<uint8_t>> bl;
    for (int i = 0, k = 0; i < blocks; ++i) {
        const int n = short_len - ecc_len + (i < short_blocks ? 0 : 1);
        std::vector<uint8_t> dat(data.begin() + k, data.begin() + k + n);
        k += n;
        const std::vector<uint8_t> ecc = rs_remainder(dat, div);
        if (i < short_blocks) dat.push_back(0);
        dat.insert(dat.end(), ecc.begin(), ecc.end());
        bl.push_back(dat);
    }
    std::vector<uint8_t> all;
    for (size_t i = 0; i < bl[0].size(); ++i)
        for (size_t j = 0; j < bl.size(); ++j)
            if (i != static_cast<size_t>(short_len - ecc_len) || static_cast<int>(j) >= short_blocks) all.push_back(bl[j][i]);

    Grid base(ver * 4 + 17);
    draw_function_patterns(base, ver);
    draw_codewords(base, all);
    Grid best = base;
    long best_p = -1;
    for (int mask = 0; mask < 8; ++mask) {
        Grid g = base;
        apply_mask(g, mask);
        draw_format(g, mask);
        const long p = penalty(g);
        if (best_p < 0 || p < best_p) { best_p = p; best = g; }
    }
    return best.m;
}

std::vector<std::string> render_half_blocks(const std::vector<std::vector<bool>>& modules, int quiet) {
    std::vector<std::string> out;
    if (modules.empty()) return out;
    const int n = static_cast<int>(modules.size()), total = n + 2 * quiet;
    auto dark = [&](int x, int y) {
        x -= quiet; y -= quiet;
        return x >= 0 && y >= 0 && x < n && y < n && modules[static_cast<size_t>(y)][static_cast<size_t>(x)];
    };
    for (int y = 0; y < total; y += 2) {
        std::string line;
        for (int x = 0; x < total; ++x) {
            const bool top = dark(x, y), bottom = y + 1 < total && dark(x, y + 1);
            // upper half block: foreground = the upper module, background = the lower one (black 16 / white 231)
            line += std::string("\x1b[38;5;") + (top ? "16" : "231") + ";48;5;" + (bottom ? "16" : "231") + "m▀";
        }
        line += "\x1b[0m";
        out.push_back(line);
    }
    return out;
}

} // namespace muisc::qr
