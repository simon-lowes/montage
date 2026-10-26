#include "LearnedEffect.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage::learn {

namespace {

constexpr float kLeak = 0.1f;
constexpr int kChunk = 16;  // output pixels a convolution works out together (vectorised)

int roundUp(int v, int m) { return (v + m - 1) / m * m; }

// Four floats worked on together (SSE, NEON); the same sums in the same order as one at a time.
#if defined(__GNUC__) || defined(__clang__)
typedef float V4 __attribute__((vector_size(16)));
#else
struct V4 {
    float v[4];
    float operator[](int i) const { return v[i]; }
};
inline V4 operator*(const V4& a, const V4& b) { return V4{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}; }
inline V4& operator+=(V4& a, const V4& b) {
    for (int i = 0; i < 4; ++i) a.v[i] += b.v[i];
    return a;
}
#endif
inline V4 load4(const float* p) {
    V4 r;
    std::memcpy(&r, p, sizeof r);
    return r;
}
inline V4 splat(float x) { return V4{x, x, x, x}; }

// ---- Threads ---------------------------------------------------------------------------------------------------------

// A fixed set of threads handing out numbered tasks. Each task writes its own outputs, so what a task computes does not
// depend on how many threads there are or which one takes it.
class Workers {
public:
    explicit Workers(int n) {
        for (int i = 1; i < n; ++i) threads_.emplace_back([this] { loop(); });
    }
    ~Workers() {
        {
            std::lock_guard lock(m_);
            quit_ = true;
        }
        wake_.notify_all();
        for (std::thread& t : threads_) t.join();
    }
    int size() const { return int(threads_.size()) + 1; }
    void run(int tasks, const std::function<void(int)>& fn) {
        if (tasks <= 0) return;
        if (threads_.empty() || tasks == 1) {
            for (int i = 0; i < tasks; ++i) fn(i);
            return;
        }
        std::lock_guard one(running_);  // (one job at a time)
        {
            std::lock_guard lock(m_);
            fn_ = &fn;
            tasks_ = tasks;
            // (Handed out a run of tasks at a time: a few runs for each thread.)
            chunk_ = std::max(1, tasks / (size() * 6));
            next_ = 0;
            done_ = 0;
            ++job_;
        }
        wake_.notify_all();
        work();
        std::unique_lock lock(m_);
        finished_.wait(lock, [&] { return done_ == tasks_; });
        fn_ = nullptr;
    }

private:
    void work() {
        for (;;) {
            int i, end;
            const std::function<void(int)>* fn;
            {
                std::lock_guard lock(m_);
                if (!fn_ || next_ >= tasks_) return;
                i = next_;
                end = next_ = std::min(tasks_, next_ + chunk_);
                fn = fn_;
            }
            for (int j = i; j < end; ++j) (*fn)(j);
            std::lock_guard lock(m_);
            if ((done_ += end - i) == tasks_) finished_.notify_all();
        }
    }
    void loop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock(m_);
                wake_.wait(lock, [&] { return quit_ || job_ != seen; });
                if (quit_) return;
                seen = job_;
            }
            work();
        }
    }
    std::vector<std::thread> threads_;
    std::mutex m_, running_;
    std::condition_variable wake_, finished_;
    const std::function<void(int)>* fn_ = nullptr;
    int tasks_ = 0, chunk_ = 1, next_ = 0, done_ = 0;
    uint64_t job_ = 0;
    bool quit_ = false;
};

Workers& workers(int threads) {
    if (threads <= 0) threads = int(std::max(1u, std::thread::hardware_concurrency()));
    threads = std::min(threads, 64);
    static std::mutex m;
    static auto* pools = new std::map<int, std::unique_ptr<Workers>>();  // (kept for the session)
    std::lock_guard lock(m);
    auto& w = (*pools)[threads];
    if (!w) w = std::make_unique<Workers>(threads);
    return *w;
}

// ---- Layers ----------------------------------------------------------------------------------------------------------

struct Conv {
    int cin, cout, k;
    size_t w, b;  // offsets into the weights
};

// The convolutions, top to bottom: each level's two going down, each level's two coming up (from the second lowest),
// and the last 1x1.
std::vector<Conv> layers(const NetShape& s) {
    std::vector<Conv> out;
    size_t at = 0;
    auto add = [&](int cin, int cout, int k) {
        out.push_back({cin, cout, k, at, at + size_t(cout) * size_t(cin) * size_t(k) * size_t(k)});
        at = out.back().b + size_t(cout);
    };
    auto ch = [&](int l) { return s.base << l; };
    for (int l = 0; l < s.levels; ++l) {
        add(l == 0 ? s.channels : ch(l - 1), ch(l), 3);
        add(ch(l), ch(l), 3);
    }
    for (int l = s.levels - 2; l >= 0; --l) {
        add(ch(l + 1) + ch(l), ch(l), 3);
        add(ch(l), ch(l), 3);
    }
    add(ch(0), s.channels, 1);
    return out;
}

size_t weightCount(const std::vector<Conv>& ls) { return ls.empty() ? 0 : ls.back().b + size_t(ls.back().cout); }

// Planes zero-padded by `p` on each side, each row PW wide (the width rounded up to a chunk, plus the padding).
struct Padded {
    std::vector<float> px;
    int c = 0, w = 0, h = 0, p = 0, pw = 0;
    const float* row(int ch, int y) const { return px.data() + (size_t(ch) * size_t(h + 2 * p) + size_t(y)) * size_t(pw); }
};

void pad(const float* in, int c, int w, int h, int p, Padded& out, Workers& pool) {
    out.c = c, out.w = w, out.h = h, out.p = p;
    out.pw = roundUp(w, kChunk) + 2 * p;
    out.px.assign(size_t(c) * size_t(h + 2 * p) * size_t(out.pw), 0.0f);
    pool.run(c, [&](int ch) {
        for (int y = 0; y < h; ++y)
            std::memcpy(out.px.data() + (size_t(ch) * size_t(h + 2 * p) + size_t(y + p)) * size_t(out.pw) + size_t(p),
                        in + (size_t(ch) * size_t(h) + size_t(y)) * size_t(w), size_t(w) * sizeof(float));
    });
}

// out[o][y][x] = bias[o] + sum over c, ky, kx of wt[o][c][ky][kx] * in[c][y + ky - p][x + kx - p], four outputs and
// eight pixels of a row at a time (held in registers), the four outputs' weights side by side.
void convolve(const Padded& in, int k, const float* wt, const float* bias, int cout, float* out, Workers& pool) {
    const int cin = in.c, w = in.w, h = in.h, kk = k * k;
    const int blocks = (cout + 3) / 4;
    std::vector<float> packed(size_t(blocks) * size_t(cin) * size_t(kk) * 4, 0.0f);
    for (int o = 0; o < cout; ++o)
        for (int c = 0; c < cin; ++c)
            for (int j = 0; j < kk; ++j)
                packed[((size_t(o / 4) * size_t(cin) + size_t(c)) * size_t(kk) + size_t(j)) * 4 + size_t(o % 4)] =
                    wt[(size_t(o) * size_t(cin) + size_t(c)) * size_t(kk) + size_t(j)];
    pool.run(blocks * h, [&](int task) {
        const int b = task / h, o = b * 4, y = task % h, n = std::min(4, cout - o);
        float base[4] = {};
        for (int j = 0; j < n; ++j) base[j] = bias ? bias[o + j] : 0.0f;
        for (int xc = 0; xc < w; xc += 8) {
            V4 a00 = splat(base[0]), a01 = a00, a10 = splat(base[1]), a11 = a10, a20 = splat(base[2]), a21 = a20, a30 = splat(base[3]), a31 = a30;
            const float* wb = packed.data() + size_t(b) * size_t(cin) * size_t(kk) * 4;
            for (int c = 0; c < cin; ++c)
                for (int ky = 0; ky < k; ++ky) {
                    const float* row = in.row(c, y + ky) + xc;
                    for (int kx = 0; kx < k; ++kx, wb += 4) {
                        const V4 s0 = load4(row + kx), s1 = load4(row + kx + 4);
                        const V4 w0 = splat(wb[0]), w1 = splat(wb[1]), w2 = splat(wb[2]), w3 = splat(wb[3]);
                        a00 += w0 * s0, a01 += w0 * s1;
                        a10 += w1 * s0, a11 += w1 * s1;
                        a20 += w2 * s0, a21 += w2 * s1;
                        a30 += w3 * s0, a31 += w3 * s1;
                    }
                }
            const int m = std::min(8, w - xc);
            const V4 acc[4][2] = {{a00, a01}, {a10, a11}, {a20, a21}, {a30, a31}};
            for (int j = 0; j < n; ++j) {
                float px[8];
                std::memcpy(px, &acc[j][0], sizeof(V4));
                std::memcpy(px + 4, &acc[j][1], sizeof(V4));
                std::memcpy(out + (size_t(o + j) * size_t(h) + size_t(y)) * size_t(w) + size_t(xc), px, size_t(m) * sizeof(float));
            }
        }
    });
}

void conv(const Conv& L, const float* weights, const float* in, int w, int h, float* out, Workers& pool) {
    Padded p;
    pad(in, L.cin, w, h, L.k / 2, p, pool);
    convolve(p, L.k, weights + L.w, weights + L.b, L.cout, out, pool);
}

void leaky(float* x, size_t n) {
    for (size_t i = 0; i < n; ++i) x[i] = x[i] < 0 ? x[i] * kLeak : x[i];
}
// The gradient through a leaky ReLU, from its output.
void leakyBack(const float* y, float* g, size_t n) {
    for (size_t i = 0; i < n; ++i) g[i] = y[i] < 0 ? g[i] * kLeak : g[i];
}

void poolDown(const float* in, int c, int w, int h, float* out) {
    const int ow = w / 2, oh = h / 2;
    for (int ch = 0; ch < c; ++ch)
        for (int y = 0; y < oh; ++y) {
            const float* a = in + (size_t(ch) * size_t(h) + size_t(2 * y)) * size_t(w);
            const float* b = a + w;
            float* o = out + (size_t(ch) * size_t(oh) + size_t(y)) * size_t(ow);
            for (int x = 0; x < ow; ++x) o[x] = 0.25f * (a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1]);
        }
}
// Adds the gradient through poolDown to `g`.
void poolDownBack(const float* gout, int c, int w, int h, float* g) {
    const int ow = w / 2, oh = h / 2;
    for (int ch = 0; ch < c; ++ch)
        for (int y = 0; y < h; ++y) {
            const float* o = gout + (size_t(ch) * size_t(oh) + size_t(y / 2)) * size_t(ow);
            float* r = g + (size_t(ch) * size_t(h) + size_t(y)) * size_t(w);
            for (int x = 0; x < w; ++x) r[x] += 0.25f * o[x / 2];
        }
}
// Nearest-neighbour up (w, h the size below).
void upNearest(const float* in, int c, int w, int h, float* out) {
    const int ow = w * 2, oh = h * 2;
    for (int ch = 0; ch < c; ++ch)
        for (int y = 0; y < oh; ++y) {
            const float* s = in + (size_t(ch) * size_t(h) + size_t(y / 2)) * size_t(w);
            float* o = out + (size_t(ch) * size_t(oh) + size_t(y)) * size_t(ow);
            for (int x = 0; x < ow; ++x) o[x] = s[x / 2];
        }
}
// Adds the gradient through upNearest to `g` (w, h the size below).
void upNearestBack(const float* gout, int c, int w, int h, float* g) {
    const int ow = w * 2;
    for (int ch = 0; ch < c; ++ch)
        for (int y = 0; y < h; ++y) {
            const float* a = gout + (size_t(ch) * size_t(h * 2) + size_t(2 * y)) * size_t(ow);
            const float* b = a + ow;
            float* r = g + (size_t(ch) * size_t(h) + size_t(y)) * size_t(w);
            for (int x = 0; x < w; ++x) r[x] += a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1];
        }
}

// Back through a convolution: its weights' and biases' gradients added to `grad`, and (when `gin`) the gradient of its
// input written there.
void convBack(const Conv& L, const float* weights, const float* in, const float* gout, int w, int h, float* grad, float* gin, Workers& pool) {
    const int k = L.k, p = k / 2, kk = k * k;
    Padded P;
    pad(in, L.cin, w, h, p, P, pool);
    // The output's gradient with each row's tail zero (to a whole chunk), lined up with P.
    const int wq = roundUp(w, kChunk);
    std::vector<float> D(size_t(L.cout) * size_t(h) * size_t(wq), 0.0f);
    for (int o = 0; o < L.cout; ++o)
        for (int y = 0; y < h; ++y)
            std::memcpy(D.data() + (size_t(o) * size_t(h) + size_t(y)) * size_t(wq), gout + (size_t(o) * size_t(h) + size_t(y)) * size_t(w),
                        size_t(w) * sizeof(float));
    pool.run(L.cout * L.cin, [&](int task) {
        const int o = task / L.cin, c = task % L.cin;
        V4 acc[9];
        for (V4& a : acc) a = splat(0);
        for (int y = 0; y < h; ++y) {
            const float* d = D.data() + (size_t(o) * size_t(h) + size_t(y)) * size_t(wq);
            const float* rows[3] = {P.row(c, y), k > 1 ? P.row(c, y + 1) : nullptr, k > 2 ? P.row(c, y + 2) : nullptr};
            for (int xc = 0; xc < wq; xc += 4) {
                const V4 dv = load4(d + xc);
                for (int ky = 0; ky < k; ++ky)
                    for (int kx = 0; kx < k; ++kx) acc[ky * k + kx] += dv * load4(rows[ky] + xc + kx);
            }
        }
        float* gw = grad + L.w + (size_t(o) * size_t(L.cin) + size_t(c)) * size_t(kk);
        for (int j = 0; j < kk; ++j) gw[j] += (acc[j][0] + acc[j][1]) + (acc[j][2] + acc[j][3]);
    });
    for (int o = 0; o < L.cout; ++o) {
        float lanes[kChunk] = {};
        for (int y = 0; y < h; ++y) {
            const float* d = D.data() + (size_t(o) * size_t(h) + size_t(y)) * size_t(wq);
            for (int xc = 0; xc < wq; xc += kChunk)
                for (int i = 0; i < kChunk; ++i) lanes[i] += d[xc + i];
        }
        float sum = 0;
        for (int i = 0; i < kChunk; ++i) sum += lanes[i];
        grad[L.b + size_t(o)] += sum;
    }
    if (!gin) return;
    // The input's gradient: the output's, convolved with the weights turned round (in and out swapped, flipped).
    std::vector<float> flipped(size_t(L.cin) * size_t(L.cout) * size_t(kk));
    const float* wt = weights + L.w;
    for (int o = 0; o < L.cout; ++o)
        for (int c = 0; c < L.cin; ++c)
            for (int j = 0; j < kk; ++j)
                flipped[(size_t(c) * size_t(L.cout) + size_t(o)) * size_t(kk) + size_t(kk - 1 - j)] = wt[(size_t(o) * size_t(L.cin) + size_t(c)) * size_t(kk) + size_t(j)];
    Padded G;
    pad(gout, L.cout, w, h, p, G, pool);
    convolve(G, k, flipped.data(), nullptr, L.cin, gin, pool);
}

// ---- The network -----------------------------------------------------------------------------------------------------

// What a pass leaves for the way back: each layer's input and output, by level.
struct Pass {
    std::vector<std::vector<float>> encIn, encA, encE;  // a level's input, its first and second convolutions' outputs
    std::vector<std::vector<float>> decCat, decB, decD;  // indexed by level (the bottom level's unused)
    std::vector<float> out;
};

// w, h: the top level's size.
void run(const Net& net, const float* in, int w, int h, Pass& pass, Workers& pool) {
    const NetShape& s = net.shape;
    const std::vector<Conv> ls = layers(s);
    const float* W = net.weights.data();
    const int L = s.levels;
    pass.encIn.assign(size_t(L), {});
    pass.encA.assign(size_t(L), {});
    pass.encE.assign(size_t(L), {});
    pass.decCat.assign(size_t(L), {});
    pass.decB.assign(size_t(L), {});
    pass.decD.assign(size_t(L), {});
    auto size = [&](int l) { return size_t(w >> l) * size_t(h >> l); };
    pass.encIn[0].assign(in, in + size_t(s.channels) * size(0));
    for (int l = 0; l < L; ++l) {
        const int lw = w >> l, lh = h >> l;
        if (l > 0) {
            pass.encIn[size_t(l)].resize(size_t(ls[size_t(2 * l)].cin) * size(l));
            poolDown(pass.encE[size_t(l - 1)].data(), ls[size_t(2 * l)].cin, lw * 2, lh * 2, pass.encIn[size_t(l)].data());
        }
        const Conv& a = ls[size_t(2 * l)];
        const Conv& b = ls[size_t(2 * l + 1)];
        pass.encA[size_t(l)].resize(size_t(a.cout) * size(l));
        conv(a, W, pass.encIn[size_t(l)].data(), lw, lh, pass.encA[size_t(l)].data(), pool);
        leaky(pass.encA[size_t(l)].data(), pass.encA[size_t(l)].size());
        pass.encE[size_t(l)].resize(size_t(b.cout) * size(l));
        conv(b, W, pass.encA[size_t(l)].data(), lw, lh, pass.encE[size_t(l)].data(), pool);
        leaky(pass.encE[size_t(l)].data(), pass.encE[size_t(l)].size());
    }
    const std::vector<float>* below = &pass.encE[size_t(L - 1)];
    for (int l = L - 2; l >= 0; --l) {
        const int lw = w >> l, lh = h >> l;
        const size_t i = size_t(2 * L + 2 * (L - 2 - l));
        const Conv& a = ls[i];
        const Conv& b = ls[i + 1];
        const int up = s.base << (l + 1), skip = s.base << l;
        std::vector<float>& cat = pass.decCat[size_t(l)];
        cat.resize(size_t(up + skip) * size(l));
        upNearest(below->data(), up, lw / 2, lh / 2, cat.data());
        std::copy(pass.encE[size_t(l)].begin(), pass.encE[size_t(l)].end(), cat.begin() + std::ptrdiff_t(size_t(up) * size(l)));
        pass.decB[size_t(l)].resize(size_t(a.cout) * size(l));
        conv(a, W, cat.data(), lw, lh, pass.decB[size_t(l)].data(), pool);
        leaky(pass.decB[size_t(l)].data(), pass.decB[size_t(l)].size());
        pass.decD[size_t(l)].resize(size_t(b.cout) * size(l));
        conv(b, W, pass.decB[size_t(l)].data(), lw, lh, pass.decD[size_t(l)].data(), pool);
        leaky(pass.decD[size_t(l)].data(), pass.decD[size_t(l)].size());
        below = &pass.decD[size_t(l)];
    }
    const Conv& last = ls.back();
    pass.out.resize(size_t(s.channels) * size(0));
    conv(last, W, below->data(), w, h, pass.out.data(), pool);
    for (size_t i = 0; i < pass.out.size(); ++i) pass.out[i] += in[i];  // (it adds to the picture)
}

// (At most four levels, whose reach the tiles' context covers, of up to 32 channels at the top: a file under 32 MB.)
bool validShape(const NetShape& s) { return s.levels >= 1 && s.levels <= 4 && s.base >= 4 && s.base % 4 == 0 && s.base <= 32 && s.channels == 4; }

// ---- Random numbers (the same on every platform) -----------------------------------------------------------------

uint64_t next(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
double uniform(uint64_t& state) { return double(next(state) >> 11) * (1.0 / 9007199254740992.0); }
int below(uint64_t& state, int n) { return n <= 1 ? 0 : int(next(state) % uint64_t(n)); }
double gaussian(uint64_t& state) {
    const double u = std::max(uniform(state), 1e-300), v = uniform(state);
    return std::sqrt(-2 * std::log(u)) * std::cos(6.283185307179586 * v);
}

// ---- Bytes -------------------------------------------------------------------------------------------------------

struct Writer {
    std::vector<unsigned char> b;
    template <class T>
    void put(const T& v) {
        const auto* p = reinterpret_cast<const unsigned char*>(&v);
        b.insert(b.end(), p, p + sizeof(T));
    }
    void floats(const std::vector<float>& v) {
        put(uint64_t(v.size()));
        const auto* p = reinterpret_cast<const unsigned char*>(v.data());
        b.insert(b.end(), p, p + v.size() * sizeof(float));
    }
    void bytes(const std::vector<unsigned char>& v) {
        put(uint64_t(v.size()));
        b.insert(b.end(), v.begin(), v.end());
    }
};
struct Reader {
    const std::vector<unsigned char>& b;
    size_t at = 0;
    bool ok = true;
    template <class T>
    T get() {
        T v{};
        if (at + sizeof(T) > b.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, b.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::vector<float> floats(size_t limit) {
        const uint64_t n = get<uint64_t>();
        if (!ok || n > limit || at + n * sizeof(float) > b.size()) {
            ok = false;
            return {};
        }
        std::vector<float> v(n);
        std::memcpy(v.data(), b.data() + at, n * sizeof(float));
        at += n * sizeof(float);
        return v;
    }
    std::vector<unsigned char> bytes() {
        const uint64_t n = get<uint64_t>();
        if (!ok || n > b.size() - at) {
            ok = false;
            return {};
        }
        std::vector<unsigned char> v(b.begin() + std::ptrdiff_t(at), b.begin() + std::ptrdiff_t(at + n));
        at += n;
        return v;
    }
};

// ---- Pictures as planes --------------------------------------------------------------------------------------------

// Reflected about the edge pixels (-1 is 1).
int reflect(int i, int n) {
    if (n == 1) return 0;
    const int period = 2 * (n - 1);
    i %= period;
    if (i < 0) i += period;
    return i < n ? i : period - i;
}

// The tiles a picture is worked through: cores with context round them where the picture goes on, so the tiles join as
// one pass over the whole picture would, and none at its edges, where the network sees the edge as it did a crop's in
// training. (Each tile is made up to the levels' grid by reflection past the right and bottom.)
constexpr int kTileCore = 384;
constexpr int kTileContext = 64;

template <class Fn>
bool tiled(const Image& in, int multiple, Image& out, const std::atomic<bool>* cancel, Fn&& net) {
    out = Image(in.width, in.height, Image::Uninitialized{});
    const int W = in.width, H = in.height;
    const int context = roundUp(kTileContext, multiple);
    std::vector<float> plane, result;
    for (int ty = 0; ty < H; ty += kTileCore)
        for (int tx = 0; tx < W; tx += kTileCore) {
            if (cancel && cancel->load()) return false;
            const int x0 = std::max(0, tx - context), y0 = std::max(0, ty - context);
            const int tw = roundUp(std::min(W, tx + kTileCore + context) - x0, multiple);
            const int th = roundUp(std::min(H, ty + kTileCore + context) - y0, multiple);
            const size_t n = size_t(tw) * size_t(th);
            plane.resize(n * 4);
            for (int y = 0; y < th; ++y) {
                const int sy = reflect(y0 + y, H);
                for (int x = 0; x < tw; ++x) {
                    const float* p = in.at(reflect(x0 + x, W), sy);
                    for (int c = 0; c < 4; ++c) plane[size_t(c) * n + size_t(y) * size_t(tw) + size_t(x)] = p[c];
                }
            }
            if (!net(plane.data(), tw, th, result)) return false;
            for (int y = 0; y < std::min(kTileCore, H - ty); ++y)
                for (int x = 0; x < std::min(kTileCore, W - tx); ++x) {
                    float* o = out.at(tx + x, ty + y);
                    const size_t i = size_t(ty - y0 + y) * size_t(tw) + size_t(tx - x0 + x);
                    for (int c = 0; c < 4; ++c) o[c] = result[size_t(c) * n + i];
                }
        }
    return true;
}

}  // namespace

size_t parameterCount(const NetShape& s) { return validShape(s) ? weightCount(layers(s)) : 0; }

void Net::init(const NetShape& s, uint64_t seed) {
    shape = s;
    const std::vector<Conv> ls = layers(s);
    weights.assign(weightCount(ls), 0.0f);
    uint64_t r = seed;
    for (size_t i = 0; i + 1 < ls.size(); ++i) {
        const Conv& L = ls[i];
        // He: variance 2 / fan-in (for the leaky ReLU, 2 / (1 + leak^2)).
        const double sd = std::sqrt(2.0 / ((1 + double(kLeak) * kLeak) * L.cin * L.k * L.k));
        for (size_t j = L.w; j < L.b; ++j) weights[j] = float(gaussian(r) * sd);
    }
    // (The last layer zero: the picture as it was.)
}

void forward(const Net& net, const float* in, int w, int h, float* out, int threads) {
    Pass pass;
    run(net, in, w, h, pass, workers(threads));
    std::copy(pass.out.begin(), pass.out.end(), out);
}

double lossAndGradient(const Net& net, const float* in, const float* target, int w, int h, std::vector<float>& grad, int threads) {
    Workers& pool = workers(threads);
    const NetShape& s = net.shape;
    const std::vector<Conv> ls = layers(s);
    const float* W = net.weights.data();
    const int L = s.levels;
    if (grad.size() != net.weights.size()) grad.assign(net.weights.size(), 0.0f);
    Pass pass;
    run(net, in, w, h, pass, pool);
    auto size = [&](int l) { return size_t(w >> l) * size_t(h >> l); };
    // Mean squared error, and its gradient.
    const size_t n = pass.out.size();
    std::vector<float> g(n);
    double loss = 0;
    for (size_t i = 0; i < n; ++i) {
        const double e = double(pass.out[i]) - double(target[i]);
        loss += e * e;
        g[i] = float(2 * e / double(n));
    }
    loss /= double(n);
    float* G = grad.data();
    // The last layer.
    const std::vector<float>& top = L > 1 ? pass.decD[0] : pass.encE[0];
    std::vector<float> gBelow(size_t(ls.back().cin) * size(0));
    convBack(ls.back(), W, top.data(), g.data(), w, h, G, gBelow.data(), pool);
    // Up the levels, top to bottom: the gradient for each level's skip kept for the way down.
    std::vector<std::vector<float>> gSkip(static_cast<size_t>(L));
    for (int l = 0; l <= L - 2; ++l) {
        const int lw = w >> l, lh = h >> l;
        const size_t i = size_t(2 * L + 2 * (L - 2 - l));
        const Conv& a = ls[i];
        const Conv& b = ls[i + 1];
        leakyBack(pass.decD[size_t(l)].data(), gBelow.data(), gBelow.size());
        std::vector<float> gB(size_t(b.cin) * size(l));
        convBack(b, W, pass.decB[size_t(l)].data(), gBelow.data(), lw, lh, G, gB.data(), pool);
        leakyBack(pass.decB[size_t(l)].data(), gB.data(), gB.size());
        std::vector<float> gCat(size_t(a.cin) * size(l));
        convBack(a, W, pass.decCat[size_t(l)].data(), gB.data(), lw, lh, G, gCat.data(), pool);
        const int up = s.base << (l + 1), skip = s.base << l;
        gSkip[size_t(l)].assign(gCat.begin() + std::ptrdiff_t(size_t(up) * size(l)), gCat.end());
        (void)skip;
        // The level below's output gets the upsampled part.
        std::vector<float> gNext(size_t(up) * size(l + 1), 0.0f);
        upNearestBack(gCat.data(), up, lw / 2, lh / 2, gNext.data());
        gBelow = std::move(gNext);
    }
    // Down the levels, bottom to top: each level's output gradient is what came from above plus its skip's.
    std::vector<float> gE = std::move(gBelow);
    for (int l = L - 1; l >= 0; --l) {
        const int lw = w >> l, lh = h >> l;
        if (!gSkip[size_t(l)].empty())
            for (size_t i = 0; i < gE.size(); ++i) gE[i] += gSkip[size_t(l)][i];
        const Conv& a = ls[size_t(2 * l)];
        const Conv& b = ls[size_t(2 * l + 1)];
        leakyBack(pass.encE[size_t(l)].data(), gE.data(), gE.size());
        std::vector<float> gA(size_t(b.cin) * size(l));
        convBack(b, W, pass.encA[size_t(l)].data(), gE.data(), lw, lh, G, gA.data(), pool);
        leakyBack(pass.encA[size_t(l)].data(), gA.data(), gA.size());
        if (l == 0) {
            convBack(a, W, pass.encIn[0].data(), gA.data(), lw, lh, G, nullptr, pool);
            break;
        }
        std::vector<float> gIn(size_t(a.cin) * size(l));
        convBack(a, W, pass.encIn[size_t(l)].data(), gA.data(), lw, lh, G, gIn.data(), pool);
        std::vector<float> gAbove(size_t(a.cin) * size(l - 1), 0.0f);
        poolDownBack(gIn.data(), a.cin, lw * 2, lh * 2, gAbove.data());
        gE = std::move(gAbove);
    }
    return loss;
}

Image applyNet(const Net& net, const Image& in, int threads, const std::atomic<bool>* cancel) {
    Image out;
    if (in.empty() || !validShape(net.shape)) return in;
    Workers& pool = workers(threads);
    const bool ok = tiled(in, 1 << (net.shape.levels - 1), out, cancel, [&](const float* plane, int w, int h, std::vector<float>& result) {
        Pass pass;
        run(net, plane, w, h, pass, pool);
        result = std::move(pass.out);
        return true;
    });
    return ok ? out : Image();
}

// ---- ONNX ------------------------------------------------------------------------------------------------------------

namespace {

// ONNX's protocol buffers, written by hand.
struct Pb {
    std::string b;
    void varint(uint64_t v) {
        while (v >= 0x80) {
            b += char((v & 0x7f) | 0x80);
            v >>= 7;
        }
        b += char(v);
    }
    void key(int field, int wire) { varint(uint64_t(field) << 3 | uint64_t(wire)); }
    void u(int field, uint64_t v) { key(field, 0), varint(v); }
    void s(int field, const std::string& v) { key(field, 2), varint(v.size()), b += v; }
    void f(int field, float v) {
        key(field, 5);
        char c[4];
        std::memcpy(c, &v, 4);
        b.append(c, 4);
    }
};

std::string attrInts(const std::string& name, const std::vector<int64_t>& v) {
    Pb a;
    a.s(1, name);
    for (int64_t x : v) a.u(8, uint64_t(x));
    a.u(20, 7);  // INTS
    return a.b;
}
std::string attrInt(const std::string& name, int64_t v) {
    Pb a;
    a.s(1, name);
    a.u(3, uint64_t(v));
    a.u(20, 2);  // INT
    return a.b;
}
std::string attrFloat(const std::string& name, float v) {
    Pb a;
    a.s(1, name);
    a.f(2, v);
    a.u(20, 1);  // FLOAT
    return a.b;
}
std::string attrString(const std::string& name, const std::string& v) {
    Pb a;
    a.s(1, name);
    a.s(4, v);
    a.u(20, 3);  // STRING
    return a.b;
}
std::string node(const std::string& op, const std::vector<std::string>& in, const std::string& out, const std::vector<std::string>& attrs = {}) {
    Pb n;
    for (const std::string& i : in) n.s(1, i);
    n.s(2, out);
    n.s(3, out);
    n.s(4, op);
    for (const std::string& a : attrs) n.s(5, a);
    return n.b;
}
std::string tensor(const std::string& name, const std::vector<int64_t>& dims, const float* data, size_t count) {
    Pb t;
    for (int64_t d : dims) t.u(1, uint64_t(d));
    t.u(2, 1);  // FLOAT
    t.s(8, name);
    t.s(9, std::string(reinterpret_cast<const char*>(data), count * sizeof(float)));
    return t.b;
}
std::string valueInfo(const std::string& name, int channels) {
    Pb shape;
    auto dim = [&](int64_t v, const char* param) {
        Pb d;
        if (param) d.s(2, param);
        else d.u(1, uint64_t(v));
        shape.s(1, d.b);
    };
    dim(1, nullptr), dim(channels, nullptr), dim(0, "height"), dim(0, "width");
    Pb tt;
    tt.u(1, 1);  // FLOAT
    tt.s(2, shape.b);
    Pb type;
    type.s(1, tt.b);
    Pb v;
    v.s(1, name);
    v.s(2, type.b);
    return v.b;
}

}  // namespace

std::vector<unsigned char> onnxModel(const Net& net) {
    const NetShape& s = net.shape;
    if (!validShape(s)) return {};
    const std::vector<Conv> ls = layers(s);
    Pb graph;
    std::vector<std::string> inits;
    int count = 0;
    auto convNode = [&](const Conv& L, const std::string& in, bool act) {
        const std::string id = std::to_string(count++);
        inits.push_back(tensor("w" + id, {L.cout, L.cin, L.k, L.k}, net.weights.data() + L.w, L.b - L.w));
        inits.push_back(tensor("b" + id, {L.cout}, net.weights.data() + L.b, size_t(L.cout)));
        const int p = L.k / 2;
        graph.s(1, node("Conv", {in, "w" + id, "b" + id}, "conv" + id, {attrInts("kernel_shape", {L.k, L.k}), attrInts("pads", {p, p, p, p})}));
        if (!act) return "conv" + id;
        graph.s(1, node("LeakyRelu", {"conv" + id}, "act" + id, {attrFloat("alpha", kLeak)}));
        return "act" + id;
    };
    const float scales[4] = {1, 1, 2, 2};
    inits.push_back(tensor("scales", {4}, scales, 4));
    std::vector<std::string> enc(size_t(s.levels));
    std::string x = "input";
    for (int l = 0; l < s.levels; ++l) {
        if (l > 0) {
            const std::string p = "pool" + std::to_string(l);
            graph.s(1, node("AveragePool", {enc[size_t(l - 1)]}, p, {attrInts("kernel_shape", {2, 2}), attrInts("strides", {2, 2})}));
            x = p;
        }
        x = convNode(ls[size_t(2 * l)], x, true);
        x = convNode(ls[size_t(2 * l + 1)], x, true);
        enc[size_t(l)] = x;
    }
    for (int l = s.levels - 2; l >= 0; --l) {
        const size_t i = size_t(2 * s.levels + 2 * (s.levels - 2 - l));
        const std::string up = "up" + std::to_string(l), cat = "cat" + std::to_string(l);
        graph.s(1, node("Resize", {x, "", "scales"}, up,
                        {attrString("mode", "nearest"), attrString("coordinate_transformation_mode", "asymmetric"), attrString("nearest_mode", "floor")}));
        graph.s(1, node("Concat", {up, enc[size_t(l)]}, cat, {attrInt("axis", 1)}));
        x = convNode(ls[i], cat, true);
        x = convNode(ls[i + 1], x, true);
    }
    x = convNode(ls.back(), x, false);
    graph.s(1, node("Add", {"input", x}, "output"));
    graph.s(2, "montage-learned-effect");
    for (const std::string& t : inits) graph.s(5, t);
    graph.s(11, valueInfo("input", s.channels));
    graph.s(12, valueInfo("output", s.channels));
    Pb opset;
    opset.s(1, "");
    opset.u(2, 13);
    Pb model;
    model.u(1, 8);
    model.s(2, "Montage");
    model.s(7, graph.b);
    model.s(8, opset.b);
    return std::vector<unsigned char>(model.b.begin(), model.b.end());
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool onnxRuntimeAvailable() { return false; }
bool runOnnx(const std::vector<unsigned char>&, const float*, int, int, int, std::vector<float>&, std::string* error) {
    if (error) *error = "This build of Montage was built without ONNX Runtime";
    return false;
}
bool applyNetOrt(const Net&, const Image&, Image&, std::string* error, const std::atomic<bool>*) {
    if (error) *error = "This build of Montage was built without ONNX Runtime";
    return false;
}

#else

bool onnxRuntimeAvailable() { return true; }

namespace {

Ort::Env& env() {
    static auto* e = new Ort::Env(ORT_LOGGING_LEVEL_ERROR, "montage-learned");  // (kept for the session)
    return *e;
}

uint64_t hashBytes(const std::vector<unsigned char>& b) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : b) h = (h ^ c) * 1099511628211ULL;
    return h ^ b.size();
}

// The last few models opened, by their bytes.
std::shared_ptr<OrtModel> session(const std::vector<unsigned char>& model, std::string* error) {
    if (!ortUsable(error)) return nullptr;
    const uint64_t key = hashBytes(model);
    static std::mutex m;
    static auto* open = new std::vector<std::pair<uint64_t, std::shared_ptr<OrtModel>>>();
    std::lock_guard lock(m);
    for (const auto& [k, s] : *open)
        if (k == key) return s;
    try {
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        auto s = std::make_shared<OrtModel>(env(), model, so, ModelRuns::Anywhere);
        open->insert(open->begin(), {key, s});
        if (open->size() > 3) open->pop_back();
        return s;
    } catch (const std::exception& e) {
        if (error) *error = std::string("The learned effect's network could not be opened: ") + e.what();
        return nullptr;
    }
}

bool runSession(OrtModel& model, const float* in, int channels, int w, int h, std::vector<float>& out, std::string* error) {
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char* inNames[] = {"input"};
        const char* outNames[] = {"output"};
        const size_t n = size_t(channels) * size_t(w) * size_t(h);
        const int64_t shape[4] = {1, channels, h, w};
        Ort::Value input = Ort::Value::CreateTensor<float>(cpu, const_cast<float*>(in), n, shape, 4);
        auto result = model.Run(Ort::RunOptions{nullptr}, inNames, &input, 1, outNames, 1);
        const float* y = result[0].GetTensorData<float>();
        out.assign(y, y + n);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string("The learned effect's network failed: ") + e.what();
        return false;
    }
}

}  // namespace

bool runOnnx(const std::vector<unsigned char>& model, const float* in, int channels, int w, int h, std::vector<float>& out, std::string* error) {
    auto s = session(model, error);
    return s && runSession(*s, in, channels, w, h, out, error);
}

bool applyNetOrt(const Net& net, const Image& in, Image& out, std::string* error, const std::atomic<bool>* cancel) {
    if (in.empty()) {
        out = in;
        return true;
    }
    auto s = session(onnxModel(net), error);
    if (!s) return false;
    const bool ok = tiled(in, 1 << (net.shape.levels - 1), out, cancel, [&](const float* plane, int w, int h, std::vector<float>& result) {
        return runSession(*s, plane, net.shape.channels, w, h, result, error);
    });
    if (!ok && error && error->empty()) *error = "Cancelled";
    return ok;
}

#endif

// ---- Training ----------------------------------------------------------------------------------------------------

Trainer::Trainer(const NetShape& shape, std::vector<Pair> pairs, const TrainOptions& options)
    : shape_(shape), pairs_(std::move(pairs)), options_(options) {
    net_.init(shape_, options_.seed);
    m_.assign(net_.weights.size(), 0.0f);
    v_.assign(net_.weights.size(), 0.0f);
    random_ = options_.seed ^ 0x5DEECE66DULL;
}

bool Trainer::run(int steps, const std::atomic<bool>* cancel, const std::function<void(int, double)>& progress) {
    if (pairs_.empty() || !validShape(shape_)) return false;
    const int multiple = 1 << (shape_.levels - 1);
    const int batch = std::max(1, options_.batch);
    std::vector<float> grad, in, target;
    for (int s = 0; s < steps; ++s) {
        if (cancel && cancel->load()) return false;
        grad.assign(net_.weights.size(), 0.0f);
        double loss = 0;
        for (int b = 0; b < batch; ++b) {
            const Pair& p = pairs_[size_t(below(random_, int(pairs_.size())))];
            const int W = std::min(p.before.width, p.after.width), H = std::min(p.before.height, p.after.height);
            const int cw = std::max(multiple, std::min(options_.crop, W) / multiple * multiple);
            const int ch = std::max(multiple, std::min(options_.crop, H) / multiple * multiple);
            const int x0 = below(random_, std::max(1, W - cw + 1)), y0 = below(random_, std::max(1, H - ch + 1));
            const bool flipX = next(random_) & 1, flipY = next(random_) & 1;
            const size_t n = size_t(cw) * size_t(ch);
            in.resize(n * 4);
            target.resize(n * 4);
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x) {
                    const int sx = std::min(W - 1, x0 + (flipX ? cw - 1 - x : x)), sy = std::min(H - 1, y0 + (flipY ? ch - 1 - y : y));
                    const float* a = p.before.at(sx, sy);
                    const float* t = p.after.at(sx, sy);
                    for (int c = 0; c < 4; ++c) {
                        in[size_t(c) * n + size_t(y) * size_t(cw) + size_t(x)] = a[c];
                        target[size_t(c) * n + size_t(y) * size_t(cw) + size_t(x)] = t[c];
                    }
                }
            loss += lossAndGradient(net_, in.data(), target.data(), cw, ch, grad, options_.threads);
        }
        // Adam.
        ++step_;
        const double b1 = 0.9, b2 = 0.999, eps = 1e-8;
        const double c1 = 1 - std::pow(b1, step_), c2 = 1 - std::pow(b2, step_);
        const double decayed = options_.decay > 0 ? options_.rate / (1 + double(step_ - 1) / options_.decay) : options_.rate;
        const float rate = float(decayed * std::sqrt(c2) / c1);
        for (size_t i = 0; i < grad.size(); ++i) {
            const float g = grad[i] / float(batch);
            m_[i] = float(b1) * m_[i] + float(1 - b1) * g;
            v_[i] = float(b2) * v_[i] + float(1 - b2) * g * g;
            net_.weights[i] -= rate * m_[i] / (std::sqrt(v_[i]) + float(eps));
        }
        loss_ = loss / batch;
        if (progress) progress(step_, loss_);
    }
    return true;
}

std::vector<unsigned char> Trainer::state() const {
    Writer w;
    w.put(uint32_t(0x4D4C5354));  // "MLST"
    w.put(int32_t(shape_.levels)), w.put(int32_t(shape_.base)), w.put(int32_t(shape_.channels));
    w.put(int32_t(step_)), w.put(loss_), w.put(random_);
    w.floats(net_.weights), w.floats(m_), w.floats(v_);
    return w.b;
}

int Trainer::stepsIn(const std::vector<unsigned char>& state) {
    Reader r{state};
    if (r.get<uint32_t>() != 0x4D4C5354) return 0;
    for (int i = 0; i < 3; ++i) (void)r.get<int32_t>();  // (the shape)
    const int step = r.get<int32_t>();
    return r.ok ? std::max(0, step) : 0;
}

bool Trainer::restore(const std::vector<unsigned char>& state) {
    Reader r{state};
    if (r.get<uint32_t>() != 0x4D4C5354) return false;
    NetShape s;
    s.levels = r.get<int32_t>(), s.base = r.get<int32_t>(), s.channels = r.get<int32_t>();
    if (!r.ok || !(s == shape_)) return false;
    const int step = r.get<int32_t>();
    const double loss = r.get<double>();
    const uint64_t random = r.get<uint64_t>();
    const size_t n = net_.weights.size();
    std::vector<float> w = r.floats(n), m = r.floats(n), v = r.floats(n);
    if (!r.ok || step < 0 || w.size() != n || m.size() != n || v.size() != n) return false;
    net_.weights = std::move(w), m_ = std::move(m), v_ = std::move(v);
    step_ = step, loss_ = loss, random_ = random;
    return true;
}

// ---- Files ---------------------------------------------------------------------------------------------------------

bool save(const std::string& path, const Saved& s, std::string* error) {
    Writer w;
    w.put(uint32_t(0x544E4C4D));  // "MLNT"
    w.put(uint32_t(1));
    w.put(int32_t(s.net.shape.levels)), w.put(int32_t(s.net.shape.base)), w.put(int32_t(s.net.shape.channels));
    w.floats(s.net.weights);
    w.put(uint64_t(s.frames.size()));
    for (int64_t f : s.frames) w.put(f);
    w.put(int32_t(s.options.crop)), w.put(int32_t(s.options.batch)), w.put(s.options.rate), w.put(s.options.decay), w.put(s.options.seed);
    w.bytes(std::vector<unsigned char>(s.source.begin(), s.source.end()));
    w.bytes(s.state);
    QSaveFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly) || f.write(reinterpret_cast<const char*>(w.b.data()), qint64(w.b.size())) != qint64(w.b.size()) || !f.commit()) {
        if (error) *error = "Could not write " + path + ": " + f.errorString().toStdString();
        return false;
    }
    return true;
}

bool load(const std::string& path, Saved& s, std::string* error) {
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Could not read " + path;
        return false;
    }
    if (f.size() > (qint64(64) << 20)) {  // (the largest network with its trainer's state is about 31 MB)
        if (error) *error = path + " is not a learned effect";
        return false;
    }
    const QByteArray data = f.readAll();
    const std::vector<unsigned char> bytes(data.begin(), data.end());
    Reader r{bytes};
    if (r.get<uint32_t>() != 0x544E4C4D || r.get<uint32_t>() != 1) {
        if (error) *error = path + " is not a learned effect";
        return false;
    }
    Saved out;
    out.net.shape.levels = r.get<int32_t>(), out.net.shape.base = r.get<int32_t>(), out.net.shape.channels = r.get<int32_t>();
    const size_t n = validShape(out.net.shape) ? parameterCount(out.net.shape) : 0;
    out.net.weights = r.floats(n);
    const uint64_t frames = r.get<uint64_t>();
    if (!r.ok || n == 0 || out.net.weights.size() != n || frames > 100000 ||
        !std::all_of(out.net.weights.begin(), out.net.weights.end(), [](float w) { return std::isfinite(w); })) {
        if (error) *error = path + " is damaged";
        return false;
    }
    for (uint64_t i = 0; i < frames; ++i) out.frames.push_back(r.get<int64_t>());
    out.options.crop = r.get<int32_t>(), out.options.batch = r.get<int32_t>(), out.options.rate = r.get<double>(), out.options.decay = r.get<double>(), out.options.seed = r.get<uint64_t>();
    const std::vector<unsigned char> source = r.bytes();
    out.source.assign(source.begin(), source.end());
    out.state = r.bytes();
    const TrainOptions& o = out.options;
    if (!r.ok || o.crop < 8 || o.crop > 4096 || o.batch < 1 || o.batch > 256 || !std::isfinite(o.rate) || !std::isfinite(o.decay)) {
        if (error) *error = path + " is damaged";
        return false;
    }
    s = std::move(out);
    return true;
}

std::shared_ptr<const Saved> cachedNet(const std::string& path, std::string* error) {
    const QFileInfo info(QString::fromStdString(path));
    if (!info.exists()) {
        if (error) *error = "The learned effect's file is missing: " + path;
        return nullptr;
    }
    struct Entry {
        qint64 size;
        QDateTime time;
        std::shared_ptr<const Saved> net;
    };
    static std::mutex m;
    static auto* cache = new std::map<std::string, Entry>();
    std::lock_guard lock(m);
    if (auto it = cache->find(path); it != cache->end() && it->second.size == info.size() && it->second.time == info.lastModified())
        return it->second.net;
    auto s = std::make_shared<Saved>();
    if (!load(path, *s, error)) return nullptr;
    s->state.clear();  // (only training needs it)
    (*cache)[path] = {info.size(), info.lastModified(), s};
    return s;
}

bool applySaved(const std::string& file, const Image& in, Image& out, std::string* error, const std::atomic<bool>* cancel, bool run) {
    const std::shared_ptr<const Saved> saved = cachedNet(file, error);
    if (!saved) return false;
    uint64_t key = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ULL; };
    mix(uint64_t(reinterpret_cast<uintptr_t>(saved.get()))), mix(uint64_t(in.width)), mix(uint64_t(in.height));
    for (const float& v : in.px) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        mix(bits);
    }
    struct Result {
        uint64_t key;
        std::shared_ptr<const Saved> net;  // (held, so its address is not reused while the result is kept)
        std::shared_ptr<const Image> image;
    };
    static std::mutex m;
    static auto* results = new std::deque<Result>();
    auto find = [&] {
        std::lock_guard lock(m);
        for (const Result& r : *results)
            if (r.key == key && r.net == saved && r.image->width == in.width && r.image->height == in.height) {
                out = *r.image;
                return true;
            }
        return false;
    };
    if (find()) return true;
    if (!run) return false;
    // One run at a time, so two views asking for the same frame wait for the first rather than both running it.
    static std::mutex running;
    std::lock_guard one(running);
    if (find()) return true;
    Image made;
    std::string why;
    if (!onnxRuntimeAvailable() || !applyNetOrt(saved->net, in, made, &why, cancel)) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        made = applyNet(saved->net, in, 0, cancel);
        if (made.empty() && !in.empty()) {
            if (error) *error = why.empty() ? "Cancelled" : why;
            return false;
        }
    }
    std::lock_guard lock(m);
    results->push_front({key, saved, std::make_shared<const Image>(made)});
    size_t bytes = 0, keep = 0;
    for (const Result& r : *results) {
        bytes += r.image->px.size() * sizeof(float);
        if (keep >= 1 && bytes > (size_t(384) << 20)) break;
        ++keep;
    }
    results->resize(keep);
    out = std::move(made);
    return true;
}

double psnr(const Image& a, const Image& b) {
    if (a.width != b.width || a.height != b.height || a.empty()) return 0;
    double sum = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) {
            const double d = std::clamp(double(a.px[i + size_t(c)]), 0.0, 1.0) - std::clamp(double(b.px[i + size_t(c)]), 0.0, 1.0);
            sum += d * d;
            ++n;
        }
    const double mse = sum / double(std::max<size_t>(1, n));
    return mse <= 1e-12 ? 120.0 : 10 * std::log10(1 / mse);
}

}  // namespace montage::learn
