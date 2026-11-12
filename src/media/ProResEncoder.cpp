// Montage — ProRes frames coded and decoded by Montage itself (media/ProResEncoder.h).
#include "media/ProResEncoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <numeric>
#include <thread>

namespace montage::prores {

namespace {

bool failWith(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

// Slices are eight macroblocks wide; a row's last few, narrower, take the rest in halves (4, 2, 1).
constexpr int kSliceLog2 = 3;

// RDD 36's scan of an 8 x 8 block's coefficients (raster indexes, the row the vertical frequency) for progressive
// pictures; fields are scanned the same way transposed, taller than wide.
constexpr uint8_t kProgressiveScan[64] = {0,  1,  8,  9,  2,  3,  10, 11, 16, 17, 24, 25, 18, 19, 26, 27, 4,  5,  12, 20, 13, 6,
                                          7,  14, 21, 28, 29, 22, 15, 23, 30, 31, 32, 33, 40, 48, 41, 34, 35, 42, 49, 56, 57, 50,
                                          43, 36, 37, 44, 51, 58, 59, 52, 45, 38, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct Scans {
    uint8_t progressive[64], interlaced[64];
    Scans() {
        for (int i = 0; i < 64; ++i) {
            progressive[i] = kProgressiveScan[i];
            interlaced[i] = uint8_t((kProgressiveScan[i] % 8) * 8 + kProgressiveScan[i] / 8);
        }
    }
};
const Scans& scans() {
    static const Scans s;
    return s;
}

// The codebooks (rice order in bits 7 to 5, exponential Golomb order in 4 to 2, the switch between them in 1 and 0):
// the first DC's; each later DC difference's by the code before it (to 6); each run's by the run before it (to 15),
// each level's by the level before it (to 9).
constexpr uint8_t kFirstDcBook = 0xB8;
constexpr uint8_t kDcBooks[7] = {0x04, 0x28, 0x28, 0x4D, 0x4D, 0x70, 0x70};
constexpr uint8_t kRunBooks[16] = {0x06, 0x06, 0x05, 0x05, 0x04, 0x29, 0x29, 0x29, 0x29, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x4C};
constexpr uint8_t kLevelBooks[10] = {0x04, 0x0A, 0x05, 0x06, 0x04, 0x28, 0x28, 0x28, 0x28, 0x4C};

// The scale a slice's quantization index stands for: 1 to 128 as they are, then in steps of four to 512.
int qScale(int index) { return index <= 128 ? index : 128 + (index - 128) * 4; }

// Montage's quantization matrices (raster order), sent in every frame header: flat, 4 for 4444 and 2 for 4444 XQ
// (whose rate keeps detail a 4 would round away), 4 for the 4:2:2 profiles.
int matrixValue(int profile) { return profile == 6 ? 2 : 4; }

// The orthonormal 8-point DCT's basis: c[k][n] = C(k) / 2 cos((2n + 1) k pi / 16), C(0) = 1 / sqrt 2.
struct Basis {
    float c[8][8];
    Basis() {
        const double pi = 3.14159265358979323846;
        for (int k = 0; k < 8; ++k)
            for (int n = 0; n < 8; ++n) c[k][n] = float((k == 0 ? std::sqrt(0.5) : 1.0) / 2.0 * std::cos((2 * n + 1) * k * pi / 16.0));
    }
};
const Basis& basis() {
    static const Basis b;
    return b;
}

void forwardDct(const float in[64], float out[64]) {
    const Basis& b = basis();
    float rows[64];
    for (int y = 0; y < 8; ++y)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int x = 0; x < 8; ++x) s += b.c[u][x] * in[y * 8 + x];
            rows[y * 8 + u] = s;
        }
    for (int v = 0; v < 8; ++v)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int y = 0; y < 8; ++y) s += b.c[v][y] * rows[y * 8 + u];
            out[v * 8 + u] = s;
        }
}

void inverseDct(const float in[64], float out[64]) {
    const Basis& b = basis();
    float cols[64];
    for (int y = 0; y < 8; ++y)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int v = 0; v < 8; ++v) s += b.c[v][y] * in[v * 8 + u];
            cols[y * 8 + u] = s;
        }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int u = 0; u < 8; ++u) s += b.c[u][x] * cols[y * 8 + u];
            out[y * 8 + x] = s;
        }
}

// Bits most significant first; with no output, only counted.
struct BitWriter {
    std::vector<uint8_t>* out = nullptr;
    uint64_t acc = 0;
    int pending = 0;
    size_t bits = 0;
    void put(uint32_t v, int k) {
        bits += size_t(k);
        if (!out || k == 0) return;
        acc = (acc << k) | (uint64_t(v) & ((uint64_t(1) << k) - 1));
        pending += k;
        while (pending >= 8) {
            pending -= 8;
            out->push_back(uint8_t(acc >> pending));
        }
    }
    // To a whole byte, with zeros.
    void align() {
        if (out && pending) out->push_back(uint8_t(acc << (8 - pending)));
        pending = 0;
        acc = 0;
        bits = (bits + 7) / 8 * 8;
    }
    size_t bytes() const { return (bits + 7) / 8; }
};

// A value in a codebook: below the switch, a Rice code (the quotient's zeros, a one, the remainder's bits); from it,
// an exponential Golomb code of the value past the switch.
void putCode(BitWriter& w, uint8_t book, uint32_t value) {
    const int rice = book >> 5, expo = (book >> 2) & 7, sw = book & 3;
    const uint32_t threshold = uint32_t(sw + 1) << rice;
    if (value < threshold) {
        const int q = int(value >> rice);
        w.put(1, q + 1);
        if (rice) w.put(value, rice);
    } else {
        const uint32_t v = value - threshold + (uint32_t(1) << expo);
        const int nb = int(std::bit_width(v));
        w.put(0, nb - expo + sw);
        w.put(v, nb);
    }
}

uint32_t signedCode(int v) { return v >= 0 ? uint32_t(v) * 2 : uint32_t(-v) * 2 - 1; }

// One component of a slice: its blocks' quantized coefficients (raster order, 64 a block), blocks a power of two.
void codeComponent(BitWriter& w, const int32_t* levels, int blocks, const uint8_t* scan) {
    int prev = levels[0];
    putCode(w, kFirstDcBook, signedCode(prev));
    uint32_t code = 5;
    bool negative = false;
    for (int b = 1; b < blocks; ++b) {
        const int d = levels[size_t(b) * 64] - prev;
        prev = levels[size_t(b) * 64];
        const uint8_t book = kDcBooks[std::min<uint32_t>(code, 6)];
        if (d == 0) {
            code = 0;
            negative = false;
        } else {
            // (odd: the difference's sign is not the one before it; even: the same)
            const bool neg = d < 0;
            const uint32_t m = uint32_t(neg ? -d : d);
            code = neg != negative ? 2 * m - 1 : 2 * m;
            negative = neg;
        }
        putCode(w, book, code);
    }
    uint32_t prevRun = 4, prevLevel = 2, run = 0;
    for (int i = 1; i < 64; ++i)
        for (int b = 0; b < blocks; ++b) {
            const int l = levels[size_t(b) * 64 + scan[i]];
            if (!l) {
                ++run;
                continue;
            }
            const uint32_t a = uint32_t(l < 0 ? -l : l);
            putCode(w, kRunBooks[std::min<uint32_t>(prevRun, 15)], run);
            putCode(w, kLevelBooks[std::min<uint32_t>(prevLevel, 9)], a - 1);
            w.put(l < 0 ? 1 : 0, 1);
            prevRun = run;
            prevLevel = a;
            run = 0;
        }
    w.align();
}

// Alpha, a slice's sixteen lines in raster order: each value its difference from the one before (the first's from the
// top code) modulo the depth, small ones in a few bits, then whether a run of the same value follows and how long.
void codeAlpha(BitWriter& w, const uint16_t* a, int n, int bits) {
    const uint32_t mask = (uint32_t(1) << bits) - 1;
    const int small = bits == 16 ? 64 : 8, smallBits = bits == 16 ? 6 : 3;
    uint32_t prev = mask;
    auto value = [&](uint32_t cur) {
        int d = int((cur - prev) & mask);
        if (d > int(mask) / 2) d -= int(mask) + 1;
        if (d != 0 && d >= -small && d <= small) {
            w.put(0, 1);
            w.put(uint32_t((d < 0 ? -d : d) - 1), smallBits);
            w.put(d < 0 ? 1 : 0, 1);
        } else {
            w.put(1, 1);
            w.put(uint32_t(d) & mask, bits);
        }
        prev = cur;
    };
    auto runCode = [&](int r) {  // (after a value: none, or so many more of it)
        if (r == 0) return w.put(1, 1);
        w.put(0, 1);
        if (r < 16) return w.put(uint32_t(r), 4);
        w.put(0, 4);
        w.put(uint32_t(r), 11);
    };
    int i = 0;
    value(a[i++]);
    for (;;) {
        if (i >= n) {
            // (a run is read after the last value: none)
            w.put(0, 4);
            w.put(0, 11);
            break;
        }
        int r = 0;
        while (i + r < n && a[i + r] == prev && r < 2047) ++r;
        if (r) {
            w.put(0, 1);
            if (r < 16) w.put(uint32_t(r), 4);
            else {
                w.put(0, 4);
                w.put(uint32_t(r), 11);
            }
            i += r;
            if (i >= n) break;
        } else {
            runCode(0);
        }
        value(a[i++]);
    }
    w.align();
}

// The slices of one picture (a frame, or a field), in raster order.
struct SliceAt {
    int mbx = 0, mby = 0, mbs = 0;
};
std::vector<SliceAt> sliceLayout(int width, int pictureHeight) {
    const int mbw = (width + 15) / 16, mbh = (pictureHeight + 15) / 16;
    std::vector<SliceAt> out;
    for (int y = 0; y < mbh; ++y) {
        int x = 0;
        for (int size = 1 << kSliceLog2; size >= 1; size >>= 1)
            while (mbw - x >= size) {
                out.push_back({x, y, size});
                x += size;
                if (size < (1 << kSliceLog2)) break;  // (each narrower size once)
            }
    }
    return out;
}

// Runs f(i, thread) for i in [0, n) on up to `threads` threads.
template <class F>
void forEach(int n, int threads, F&& f) {
    threads = std::max(1, std::min(threads, n));
    if (threads == 1) {
        for (int i = 0; i < n; ++i) f(i, 0);
        return;
    }
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    pool.reserve(size_t(threads));
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            for (int i; (i = next.fetch_add(1)) < n;) f(i, t);
        });
    for (std::thread& t : pool) t.join();
}

void putBe16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}
void putBe32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
uint32_t be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// A block's place in its macroblock: luma left to right then down; 4:4:4 colour differences down then right; 4:2:2's
// (eight samples wide) the top then the bottom.
constexpr int kLumaBlocks[4][2] = {{0, 0}, {8, 0}, {0, 8}, {8, 8}};
constexpr int kChroma444Blocks[4][2] = {{0, 0}, {0, 8}, {8, 0}, {8, 8}};
constexpr int kChroma422Blocks[2][2] = {{0, 0}, {0, 8}};

struct Work {
    SliceAt at;
    int parity = 0;  // the field's (0 top), or 0 for a frame
    int blocks[3] = {0, 0, 0};
    std::vector<float> coef[3];
    std::vector<uint8_t> alpha;  // coded
};

}  // namespace

size_t targetFrameBytes(int profile, int width, int height) {
    static const double mbps[7] = {0, 45, 102, 147, 220, 330, 500};
    if (profile < 1 || profile > 6) return 0;
    const double bitsPerPixel = mbps[profile] * 1e6 / (30000.0 / 1001.0) / (1920.0 * 1080.0);
    return size_t(std::floor(bitsPerPixel * double(width) * double(height) / 8.0));
}

int frameRateCode(double fps) {
    static const double rates[] = {24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 50, 60000.0 / 1001, 60, 100, 120000.0 / 1001, 120};
    for (int i = 0; i < 11; ++i)
        if (std::fabs(fps - rates[i]) < 1e-3) return i + 1;
    return 0;
}

OwnEncoder::OwnEncoder() = default;
OwnEncoder::~OwnEncoder() = default;

bool OwnEncoder::open(const OwnSettings& settings, std::string* error) {
    open_ = false;
    if (settings.profile < 1 || settings.profile > 6) return failWith(error, "Unknown ProRes profile");
    if (settings.width <= 0 || settings.height <= 0 || settings.width > 8192 || settings.height > 8192)
        return failWith(error, "ProRes pictures here are up to 8192 x 8192");
    if (settings.fields < 0 || settings.fields > 2) return failWith(error, "Fields are 0 (progressive), 1 (the top first) or 2 (the bottom first)");
    if (settings.fields && settings.height % 2) return failWith(error, "Interlaced ProRes pictures here have an even height");
    if (settings.alpha && settings.profile < 5) return failWith(error, "Only ProRes 4444 and 4444 XQ carry alpha");
    s_ = settings;
    if (!s_.targetBytes) s_.targetBytes = targetFrameBytes(s_.profile, s_.width, s_.height);
    if (s_.threads <= 0) s_.threads = std::max(1, int(std::thread::hardware_concurrency()));
    open_ = true;
    return true;
}

bool OwnEncoder::encode(const PlanarPicture& pic, std::vector<uint8_t>& out, std::string* error) {
    if (!open_) return failWith(error, "The ProRes encoder is not open");
    if (!pic.y || !pic.cb || !pic.cr) return failWith(error, "A ProRes picture needs its Y', Cb and Cr");
    const int W = s_.width, H = s_.height;
    const bool full = s_.profile >= 5;
    const int cw = full ? W : (W + 1) / 2;
    const int pictures = s_.fields ? 2 : 1;
    const uint8_t* scan = s_.fields ? scans().interlaced : scans().progressive;
    const int qm = matrixValue(s_.profile);

    // Every slice of every picture, its coefficients worked out once and its alpha coded.
    std::vector<Work> work;
    std::vector<size_t> picSlices(size_t(pictures), 0);
    for (int k = 0; k < pictures; ++k) {
        // (fields: the first in time first, the top one with interlace_mode 1)
        const int parity = s_.fields == 2 ? 1 - k : k;
        const int ph = s_.fields ? (parity == 0 ? (H + 1) / 2 : H / 2) : H;
        for (const SliceAt& at : sliceLayout(W, ph)) {
            Work w;
            w.at = at;
            w.parity = s_.fields ? parity : 0;
            work.push_back(std::move(w));
            ++picSlices[size_t(k)];
        }
    }
    const int n = int(work.size());
    forEach(n, s_.threads, [&](int i, int) {
        Work& w = work[size_t(i)];
        const int ph = s_.fields ? (w.parity == 0 ? (H + 1) / 2 : H / 2) : H;
        // (a field's line r is the frame's 2r + parity; outside the picture, its last column and line again)
        auto rowOf = [&](int r) { return s_.fields ? 2 * std::min(r, ph - 1) + w.parity : std::min(r, ph - 1); };
        auto sample = [&](const uint16_t* plane, std::ptrdiff_t stride, int width, int x, int y) {
            return plane[std::ptrdiff_t(rowOf(y)) * stride + std::min(x, width - 1)];
        };
        float block[64];
        for (int c = 0; c < 3; ++c) {
            const int per = c == 0 || full ? 4 : 2;
            w.blocks[c] = per * w.at.mbs;
            w.coef[c].resize(size_t(w.blocks[c]) * 64);
            const uint16_t* plane = c == 0 ? pic.y : c == 1 ? pic.cb : pic.cr;
            const std::ptrdiff_t stride = c == 0 ? pic.yStride : pic.cStride;
            const int pw = c == 0 ? W : cw;
            const int mbWide = c == 0 || full ? 16 : 8;
            for (int m = 0; m < w.at.mbs; ++m)
                for (int b = 0; b < per; ++b) {
                    const int* off = c == 0 ? kLumaBlocks[b] : full ? kChroma444Blocks[b] : kChroma422Blocks[b];
                    const int x0 = (w.at.mbx + m) * mbWide + off[0], y0 = w.at.mby * 16 + off[1];
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 8; ++x) block[y * 8 + x] = float(sample(plane, stride, pw, x0 + x, y0 + y)) - 2048.0f;
                    forwardDct(block, &w.coef[c][(size_t(m) * size_t(per) + size_t(b)) * 64]);
                }
        }
        if (s_.alpha) {
            const int wide = w.at.mbs * 16;
            std::vector<uint16_t> a(size_t(wide) * 16, 0xffff);
            if (pic.alpha)
                for (int y = 0; y < 16; ++y)
                    for (int x = 0; x < wide; ++x) a[size_t(y) * size_t(wide) + size_t(x)] = sample(pic.alpha, pic.aStride, W, w.at.mbx * 16 + x, w.at.mby * 16 + y);
            BitWriter bw;
            bw.out = &w.alpha;
            codeAlpha(bw, a.data(), int(a.size()), 16);
        }
    });

    // A slice's colour at a quantization index: its header and its three components, counted or written.
    const size_t sliceHeader = s_.alpha ? 8 : 6;
    std::vector<std::vector<int32_t>> scratch(size_t(s_.threads));
    auto coded = [&](const Work& w, int q, int thread, std::vector<uint8_t>* into, size_t* sizes) {
        std::vector<int32_t>& levels = scratch[size_t(thread)];
        const float inverse = 1.0f / float(qm * qScale(q));
        const float most = float(32767 / (qm * qScale(q)));
        size_t total = sliceHeader;
        for (int c = 0; c < 3; ++c) {
            levels.resize(w.coef[c].size());
            // (to the nearest level, halves away from zero)
            for (size_t i = 0; i < levels.size(); ++i) {
                const float l = std::clamp(w.coef[c][i] * inverse, -most, most);
                levels[i] = int32_t(l < 0 ? l - 0.5f : l + 0.5f);
            }
            BitWriter bw;
            bw.out = into;
            codeComponent(bw, levels.data(), w.blocks[c], scan);
            if (sizes) sizes[c] = bw.bytes();
            total += bw.bytes();
        }
        return total;
    };
    // The frame's headers: its size and identifier, the frame header with both matrices, each picture's header and
    // slice table.
    size_t fixed = 8 + 148;
    for (size_t k = 0; k < picSlices.size(); ++k) fixed += 8 + 2 * picSlices[k];
    alphaBytes_ = 0;
    for (const Work& w : work) alphaBytes_ += w.alpha.size();
    const size_t budget = s_.targetBytes > fixed ? s_.targetBytes - fixed : 0;
    std::vector<size_t> bytes(static_cast<size_t>(n));
    auto colourAt = [&](int q) {
        forEach(n, s_.threads, [&](int i, int t) { bytes[size_t(i)] = coded(work[size_t(i)], q, t, nullptr, nullptr); });
        return std::accumulate(bytes.begin(), bytes.end(), size_t(0));
    };
    // The finest index one scale for the whole frame keeps within the budget (found by halving: sizes shrink as the
    // index grows), then the slices that cost least made a step finer while the budget lasts.
    std::vector<uint8_t> q(size_t(n), 1);
    if (colourAt(1) > budget) {
        // (doubling from 1 until one keeps within it, then halving the gap: a few trials for the usual fine indexes)
        int lo = 1, hi = 2;
        std::vector<size_t> atHi;
        bool fits = false;
        for (;;) {
            fits = colourAt(hi) <= budget;
            if (fits || hi == 224) break;
            lo = hi;
            hi = std::min(224, hi * 2);
        }
        atHi = bytes;  // (none keeps within it: the coarsest)
        if (fits)
            while (hi - lo > 1) {
                const int mid = (lo + hi) / 2;
                if (colourAt(mid) <= budget) {
                    hi = mid;
                    atHi = bytes;
                } else {
                    lo = mid;
                }
            }
        std::fill(q.begin(), q.end(), uint8_t(hi));
        size_t used = std::accumulate(atHi.begin(), atHi.end(), size_t(0));
        if (hi > 1 && used < budget) {
            colourAt(hi - 1);
            std::vector<int> order(static_cast<size_t>(n));
            std::iota(order.begin(), order.end(), 0);
            auto more = [&](int i) { return bytes[size_t(i)] > atHi[size_t(i)] ? bytes[size_t(i)] - atHi[size_t(i)] : size_t(0); };
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return more(a) < more(b); });
            for (int i : order) {
                if (used + more(i) > budget) break;
                used += more(i);
                q[size_t(i)] = uint8_t(hi - 1);
            }
        }
    }
    quant_ = q;

    // Each slice written at its index (coarser, for the one in a great many, while it is over the 65535 bytes its
    // size can say).
    std::vector<std::vector<uint8_t>> slices(static_cast<size_t>(n));
    forEach(n, s_.threads, [&](int i, int t) {
        const Work& w = work[size_t(i)];
        std::vector<uint8_t>& sl = slices[size_t(i)];
        for (int qi = q[size_t(i)];; ++qi) {
            sl.assign(sliceHeader, 0);
            size_t sizes[3] = {0, 0, 0};
            coded(w, qi, t, &sl, sizes);
            sl.insert(sl.end(), w.alpha.begin(), w.alpha.end());
            sl[0] = uint8_t(sliceHeader << 3);
            sl[1] = uint8_t(qi);
            putBe16(&sl[2], uint32_t(sizes[0]));
            putBe16(&sl[4], uint32_t(sizes[1]));
            if (s_.alpha) putBe16(&sl[6], uint32_t(sizes[2]));
            if ((sl.size() <= 65535 && sizes[0] <= 65535 && sizes[1] <= 65535 && sizes[2] <= 65535) || qi == 224) {
                q[size_t(i)] = uint8_t(qi);
                break;
            }
        }
    });
    quant_ = q;
    for (const auto& sl : slices)
        if (sl.size() > 65535) return failWith(error, "A ProRes slice came out too large");

    // The frame: its size, 'icpf', the frame header, then each picture.
    out.clear();
    out.resize(8 + 148);
    std::memcpy(&out[4], "icpf", 4);
    uint8_t* h = &out[8];
    putBe16(h, 148);
    h[2] = 0;
    h[3] = uint8_t(full || s_.alpha ? 1 : 0);  // bitstream_version: 1 for 4:4:4 or alpha
    std::memcpy(h + 4, "mtge", 4);
    putBe16(h + 8, uint32_t(W));
    putBe16(h + 10, uint32_t(H));
    h[12] = uint8_t((full ? 3 : 2) << 6 | (s_.fields & 3) << 2);
    h[13] = uint8_t((s_.aspectCode & 15) << 4 | (s_.frameRateCode & 15));
    h[14] = uint8_t(s_.primaries);
    h[15] = uint8_t(s_.transfer);
    h[16] = uint8_t(s_.matrix);
    h[17] = uint8_t(s_.alpha ? 2 : 0);
    h[18] = 0;
    h[19] = 3;  // both matrices follow
    std::memset(h + 20, qm, 128);
    size_t first = 0;
    for (int k = 0; k < pictures; ++k) {
        const size_t count = picSlices[size_t(k)];
        const size_t start = out.size();
        out.resize(start + 8 + 2 * count);
        out[start] = 8 << 3;
        putBe16(&out[start + 5], uint32_t(count));
        out[start + 7] = uint8_t(kSliceLog2 << 4);
        for (size_t j = 0; j < count; ++j) {
            const std::vector<uint8_t>& sl = slices[first + j];
            putBe16(&out[start + 8 + 2 * j], uint32_t(sl.size()));
            out.insert(out.end(), sl.begin(), sl.end());
        }
        putBe32(&out[start + 1], uint32_t(out.size() - start));
        first += count;
    }
    putBe32(&out[0], uint32_t(out.size()));
    return true;
}

namespace {

struct BitReader {
    const uint8_t* p = nullptr;
    size_t size = 0;  // bits
    size_t pos = 0;
    bool bit() {
        if (pos >= size) return (++pos, false);
        const bool b = (p[pos >> 3] >> (7 - (pos & 7))) & 1;
        ++pos;
        return b;
    }
    uint32_t get(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; ++i) v = v << 1 | uint32_t(bit());
        return v;
    }
    bool code(uint8_t book, uint32_t& value) {
        const int rice = book >> 5, expo = (book >> 2) & 7, sw = book & 3;
        int q = 0;
        while (pos + size_t(q) < size && !((p[(pos + q) >> 3] >> (7 - ((pos + q) & 7))) & 1)) ++q;
        if (pos + size_t(q) >= size) return false;
        if (q > sw) {
            const int total = expo - sw + 2 * q;
            if (total > 32) return false;
            const uint32_t v = get(total);
            value = v - (uint32_t(1) << expo) + (uint32_t(sw + 1) << rice);
        } else {
            pos += size_t(q) + 1;
            value = (uint32_t(q) << rice) + get(rice);
        }
        return pos <= size;
    }
};

bool decodeComponent(const uint8_t* data, size_t bytes, int blocks, const uint8_t* scan, std::vector<int32_t>& levels) {
    levels.assign(size_t(blocks) * 64, 0);
    BitReader r;
    r.p = data;
    r.size = bytes * 8;
    // (the data ends where its last one bit is: zeros after it are padding)
    size_t last = 0;
    for (size_t i = bytes; i-- > 0;)
        if (data[i]) {
            int b = 0;
            while (!((data[i] >> b) & 1)) ++b;
            last = i * 8 + size_t(7 - b) + 1;
            break;
        }
    uint32_t code = 0;
    if (!r.code(kFirstDcBook, code)) return false;
    int prev = (code & 1) ? -int((code + 1) >> 1) : int(code >> 1);
    levels[0] = prev;
    code = 5;
    bool negative = false;
    for (int b = 1; b < blocks; ++b) {
        if (!r.code(kDcBooks[std::min<uint32_t>(code, 6)], code)) return false;
        if (code == 0) negative = false;
        else if (code & 1) negative = !negative;
        const int m = int((code + 1) >> 1);
        prev += negative ? -m : m;
        levels[size_t(b) * 64] = prev;
    }
    const int log2 = [&] {
        int l = 0;
        while ((1 << l) < blocks) ++l;
        return l;
    }();
    uint32_t run = 4, level = 2;
    size_t at = size_t(blocks) - 1;
    while (r.pos < last) {
        if (!r.code(kRunBooks[std::min<uint32_t>(run, 15)], run)) return false;
        at += run + 1;
        if (at >= size_t(blocks) * 64) return false;
        if (!r.code(kLevelBooks[std::min<uint32_t>(level, 9)], level)) return false;
        ++level;
        const bool neg = r.bit();
        levels[(at & size_t(blocks - 1)) * 64 + scan[at >> log2]] = neg ? -int(level) : int(level);
    }
    return true;
}

bool decodeAlpha(const uint8_t* data, size_t bytes, int n, int bits, uint16_t* out) {
    BitReader r;
    r.p = data;
    r.size = bytes * 8;
    const uint32_t mask = (uint32_t(1) << bits) - 1;
    const int smallBits = bits == 16 ? 7 : 4;
    uint32_t a = mask;
    int i = 0;
    auto widen = [&](uint32_t v) { return uint16_t(bits == 16 ? v : v * 257); };
    do {
        do {
            int32_t d;
            if (r.bit()) {
                d = int32_t(r.get(bits));
            } else {
                const uint32_t v = r.get(smallBits);
                d = int32_t((v + 2) >> 1);
                if (v & 1) d = -d;
            }
            a = (a + uint32_t(d)) & mask;
            out[i++] = widen(a);
            if (i >= n) break;
        } while (r.bit());
        uint32_t run = r.get(4);
        if (!run) run = r.get(11);
        for (uint32_t k = 0; k < run && i < n; ++k) out[i++] = widen(a);
    } while (i < n && r.pos <= r.size);
    return i >= n;
}

}  // namespace

bool decodeOwn(const uint8_t* data, size_t size, OwnDecoded& out, std::string* error) {
    if (size < 28 || std::memcmp(data + 4, "icpf", 4) != 0) return failWith(error, "Not a ProRes frame");
    const uint8_t* h = data + 8;
    const size_t headerSize = be16(h);
    if (headerSize < 20 || 8 + headerSize > size) return failWith(error, "The ProRes frame header is cut short");
    out = OwnDecoded{};
    out.width = int(be16(h + 8));
    out.height = int(be16(h + 10));
    out.chroma = h[12] >> 6;
    out.interlace = (h[12] >> 2) & 3;
    out.alphaType = h[17] & 15;
    if (out.width <= 0 || out.height <= 0 || (out.chroma != 2 && out.chroma != 3) || out.interlace == 3 || out.alphaType > 2)
        return failWith(error, "The ProRes frame header is not one this decoder reads");
    int qmat[2][64];
    std::fill(&qmat[0][0], &qmat[0][0] + 128, 4);
    size_t m = 20;
    if (h[19] & 2) {
        if (m + 64 > headerSize) return failWith(error, "The ProRes frame header is cut short");
        for (int i = 0; i < 64; ++i) qmat[0][i] = h[m + size_t(i)];
        m += 64;
    } else {
        std::copy(&qmat[0][0], &qmat[0][0] + 64, &qmat[1][0]);
    }
    if (h[19] & 1) {
        if (m + 64 > headerSize) return failWith(error, "The ProRes frame header is cut short");
        for (int i = 0; i < 64; ++i) qmat[1][i] = h[m + size_t(i)];
    } else {
        std::copy(&qmat[0][0], &qmat[0][0] + 64, &qmat[1][0]);
    }
    const int W = out.width, H = out.height;
    const bool full = out.chroma == 3;
    const int cw = out.chromaWidth();
    out.y.assign(size_t(W) * size_t(H), 0);
    out.cb.assign(size_t(cw) * size_t(H), 0);
    out.cr.assign(size_t(cw) * size_t(H), 0);
    if (out.alphaType) out.alpha.assign(size_t(W) * size_t(H), 0xffff);
    const uint8_t* scan = out.interlace ? scans().interlaced : scans().progressive;
    size_t at = 8 + headerSize;
    std::vector<int32_t> levels;
    for (int k = 0; k < (out.interlace ? 2 : 1); ++k) {
        const int parity = out.interlace == 2 ? 1 - k : k;
        const int ph = out.interlace ? (parity == 0 ? (H + 1) / 2 : H / 2) : H;
        if (at + 8 > size) return failWith(error, "A ProRes picture is cut short");
        const uint8_t* p = data + at;
        const size_t ph8 = p[0] >> 3, picSize = be32(p + 1), count = be16(p + 5);
        const int log2 = p[7] >> 4;
        if (ph8 < 8 || picSize < ph8 + 2 * count || at + picSize > size || log2 > 3) return failWith(error, "A ProRes picture header is damaged");
        // (the slices, as the encoder lays them out for the slice width the header gives)
        std::vector<SliceAt> layout;
        {
            const int mbw = (W + 15) / 16, mbh = (ph + 15) / 16;
            for (int y = 0; y < mbh; ++y) {
                int x = 0;
                for (int s = 1 << log2; s >= 1; s >>= 1)
                    while (mbw - x >= s) {
                        layout.push_back({x, y, s});
                        x += s;
                        if (s < (1 << log2)) break;
                    }
            }
        }
        if (layout.size() != count) return failWith(error, "A ProRes picture's slice count is not its size's");
        size_t sliceAt = at + ph8 + 2 * count;
        for (size_t j = 0; j < count; ++j) {
            const size_t sliceSize = be16(p + ph8 + 2 * j);
            if (sliceAt + sliceSize > at + picSize || sliceSize < 6) return failWith(error, "A ProRes slice is damaged");
            const uint8_t* s = data + sliceAt;
            const size_t hs = s[0] >> 3;
            const int q = std::clamp<int>(s[1], 1, 224);
            if (hs < 6 || hs > sliceSize) return failWith(error, "A ProRes slice header is damaged");
            size_t sizes[3] = {be16(s + 2), be16(s + 4), 0};
            if (hs >= 8) sizes[2] = be16(s + 6);
            else if (sliceSize >= hs + sizes[0] + sizes[1]) sizes[2] = sliceSize - hs - sizes[0] - sizes[1];
            if (hs + sizes[0] + sizes[1] + sizes[2] > sliceSize) return failWith(error, "A ProRes slice's sizes are more than it holds");
            const SliceAt& sa = layout[j];
            size_t off = hs;
            for (int c = 0; c < 3; ++c) {
                const int per = c == 0 || full ? 4 : 2;
                const int blocks = per * sa.mbs;
                if (!decodeComponent(s + off, sizes[c], blocks, scan, levels)) return failWith(error, "A ProRes slice's coefficients are damaged");
                off += sizes[c];
                const int* mat = qmat[c == 0 ? 0 : 1];
                std::vector<uint16_t>& plane = c == 0 ? out.y : c == 1 ? out.cb : out.cr;
                const int pw = c == 0 ? W : cw;
                const int mbWide = c == 0 || full ? 16 : 8;
                float coef[64], px[64];
                for (int mb = 0; mb < sa.mbs; ++mb)
                    for (int b = 0; b < per; ++b) {
                        const int32_t* l = &levels[(size_t(mb) * size_t(per) + size_t(b)) * 64];
                        for (int i = 0; i < 64; ++i) coef[i] = float(l[i]) * float(mat[i] * qScale(q));
                        inverseDct(coef, px);
                        const int* o = c == 0 ? kLumaBlocks[b] : full ? kChroma444Blocks[b] : kChroma422Blocks[b];
                        const int x0 = (sa.mbx + mb) * mbWide + o[0], y0 = sa.mby * 16 + o[1];
                        for (int y = 0; y < 8; ++y) {
                            if (y0 + y >= ph) break;
                            const int row = out.interlace ? 2 * (y0 + y) + parity : y0 + y;
                            // (held to 4 ... 4091, the codes outside them being reserved for timing in serial video)
                            for (int x = 0; x < 8 && x0 + x < pw; ++x)
                                plane[size_t(row) * size_t(pw) + size_t(x0 + x)] = uint16_t(std::clamp(std::lround(px[y * 8 + x] + 2048.0f), 4L, 4091L));
                        }
                    }
            }
            if (out.alphaType) {
                const size_t rest = sliceSize - off;
                const int wide = sa.mbs * 16;
                std::vector<uint16_t> a(size_t(wide) * 16);
                if (!decodeAlpha(s + off, rest, int(a.size()), out.alphaType == 2 ? 16 : 8, a.data())) return failWith(error, "A ProRes slice's alpha is damaged");
                for (int y = 0; y < 16 && sa.mby * 16 + y < ph; ++y) {
                    const int row = out.interlace ? 2 * (sa.mby * 16 + y) + parity : sa.mby * 16 + y;
                    for (int x = 0; x < wide && sa.mbx * 16 + x < W; ++x) out.alpha[size_t(row) * size_t(W) + size_t(sa.mbx * 16 + x)] = a[size_t(y) * size_t(wide) + size_t(x)];
                }
            }
            sliceAt += sliceSize;
        }
        at += picSize;
    }
    return true;
}

}  // namespace montage::prores
