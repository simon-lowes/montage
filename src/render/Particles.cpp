// Montage — particles (render/Particles.h).
#include "render/Particles.h"

#include <QDateTime>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "core/Expression.h"
#include "core/ProjectIO.h"
#include "media/MediaPool.h"

namespace montage {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kKeepEvery = 16;                       // frames between kept states
constexpr size_t kCacheBudget = size_t(512) << 20;  // bytes of kept states, all generators together

// ---- Random numbers: each a hash of the seed, the particle and what it is for, so none depends on any other.

inline uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
// In [0, 1).
inline double rnd(uint64_t seed, uint64_t id, uint64_t what) {
    return double(mix64(mix64(seed ^ mix64(id)) + what) >> 11) * 0x1.0p-53;
}
inline double signedRnd(uint64_t seed, uint64_t id, uint64_t what) { return 2 * rnd(seed, id, what) - 1; }

// What each random number is for.
enum : uint64_t { kPos0 = 0, kPos1, kPos2, kPos3, kDir0, kDir1, kSpeed, kLife, kSpread0, kSpread1, kSize, kHue, kRotation, kSpin, kTumble };

// ---- Gradient noise with its derivatives (quintic interpolation, so smooth to the second derivative).

inline uint32_t hash3(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed * 0x9E3779B1u ^ uint32_t(x) * 0x85EBCA6Bu ^ uint32_t(y) * 0xC2B2AE35u ^ uint32_t(z) * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

const double kGrad[16][3] = {{1, 1, 0},  {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
                             {0, 1, 1},  {0, -1, 1}, {0, 1, -1}, {0, -1, -1}, {1, 1, 0},  {-1, 1, 0}, {0, -1, 1}, {0, -1, -1}};

double gradNoise(double x, double y, double z, uint32_t seed, double d[3]) {
    const double fx0 = std::floor(x), fy0 = std::floor(y), fz0 = std::floor(z);
    const int ix = int(fx0), iy = int(fy0), iz = int(fz0);
    const double fx = x - fx0, fy = y - fy0, fz = z - fz0;
    auto fade = [](double t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    auto dfade = [](double t) { return 30 * t * t * (t * (t - 2) + 1); };
    const double ux = fade(fx), uy = fade(fy), uz = fade(fz);
    const double dux = dfade(fx), duy = dfade(fy), duz = dfade(fz);
    const double* ga = kGrad[hash3(ix, iy, iz, seed) & 15];
    const double* gb = kGrad[hash3(ix + 1, iy, iz, seed) & 15];
    const double* gc = kGrad[hash3(ix, iy + 1, iz, seed) & 15];
    const double* gd = kGrad[hash3(ix + 1, iy + 1, iz, seed) & 15];
    const double* ge = kGrad[hash3(ix, iy, iz + 1, seed) & 15];
    const double* gf = kGrad[hash3(ix + 1, iy, iz + 1, seed) & 15];
    const double* gg = kGrad[hash3(ix, iy + 1, iz + 1, seed) & 15];
    const double* gh = kGrad[hash3(ix + 1, iy + 1, iz + 1, seed) & 15];
    auto dot = [](const double* g, double a, double b, double c) { return g[0] * a + g[1] * b + g[2] * c; };
    const double va = dot(ga, fx, fy, fz), vb = dot(gb, fx - 1, fy, fz), vc = dot(gc, fx, fy - 1, fz),
                 vd = dot(gd, fx - 1, fy - 1, fz), ve = dot(ge, fx, fy, fz - 1), vf = dot(gf, fx - 1, fy, fz - 1),
                 vg = dot(gg, fx, fy - 1, fz - 1), vh = dot(gh, fx - 1, fy - 1, fz - 1);
    const double k1 = vb - va, k2 = vc - va, k3 = ve - va, k4 = va - vb - vc + vd, k5 = va - vc - ve + vg,
                 k6 = va - vb - ve + vf, k7 = -va + vb + vc - vd + ve - vf - vg + vh;
    for (int i = 0; i < 3; ++i)
        d[i] = ga[i] + ux * (gb[i] - ga[i]) + uy * (gc[i] - ga[i]) + uz * (ge[i] - ga[i]) +
               ux * uy * (ga[i] - gb[i] - gc[i] + gd[i]) + uy * uz * (ga[i] - gc[i] - ge[i] + gg[i]) +
               uz * ux * (ga[i] - gb[i] - ge[i] + gf[i]) +
               ux * uy * uz * (-ga[i] + gb[i] + gc[i] - gd[i] + ge[i] - gf[i] - gg[i] + gh[i]);
    d[0] += dux * (k1 + k4 * uy + k6 * uz + k7 * uy * uz);
    d[1] += duy * (k2 + k5 * uz + k4 * ux + k7 * uz * ux);
    d[2] += duz * (k3 + k6 * ux + k5 * uy + k7 * ux * uy);
    return va + k1 * ux + k2 * uy + k3 * uz + k4 * ux * uy + k5 * uy * uz + k6 * uz * ux + k7 * ux * uy * uz;
}

// ---- Settings

// The settings the simulation reads every frame (blended between frames), in this order.
enum Sp {
    Rate, Life, LifeRandom, Speed, SpeedRandom, Direction, Elevation, Spread,
    EmitX, EmitY, EmitZ, EmitW, EmitH, EmitD, EmitAngle,
    Gravity, WindX, WindY, WindZ, Drag,
    AttractX, AttractY, AttractZ, Attract, AttractRadius,
    Turbulence, TurbulenceScale, TurbulenceEvolve,
    Bounce, Friction, FloorY,
    kSimParams
};
struct SimParamName {
    const char* name;
    double def;
};
const SimParamName kSimNames[kSimParams] = {
    {"rate", 60}, {"life", 2}, {"life_random", 25}, {"speed", 250}, {"speed_random", 25}, {"direction", 0},
    {"elevation", 0}, {"spread", 25},
    {"emit_x", 0}, {"emit_y", 200}, {"emit_z", 0}, {"emit_w", 400}, {"emit_h", 200}, {"emit_d", 0}, {"emit_angle", 0},
    {"gravity", 300}, {"wind_x", 0}, {"wind_y", 0}, {"wind_z", 0}, {"drag", 0},
    {"attract_x", 0}, {"attract_y", 0}, {"attract_z", 0}, {"attract", 0}, {"attract_radius", 200},
    {"turbulence", 0}, {"turbulence_scale", 250}, {"turbulence_evolve", 0.3},
    {"bounce", 50}, {"friction", 20}, {"floor_y", 100},
};
using Frame = std::array<double, kSimParams>;

// Emitters, looks and blends (the catalogue's choices, render/Effects.cpp).
enum Emitter { Point = 0, Line, Box, Sphere, Text, PictureAlpha, PictureBrightness };
enum Look { Dots = 0, SoftDiscs, Streaks, Squares, Sprites };

// The settings fixed for the clip (read at its first frame).
struct Settings {
    int emitter = Point;
    bool edge = false, outward = false, threeD = false, floor = false, edges = false;
    int steps = 4;
    uint64_t seed = 1;
    double preroll = 0;
    int burst = 0;
    size_t maxCount = 20000;
    double textSize = 160;
    bool textBold = true;
    std::string text, font, picture;
};

Settings readSettings(const Effect& g) {
    Settings s;
    s.emitter = std::clamp(int(std::lround(g.p("emitter", 0, 0))), 0, 6);
    s.edge = g.p("edge_only", 0) > 0.5;
    s.outward = g.p("outward", 0) > 0.5;
    s.threeD = g.p("mode", 0) > 0.5;
    s.floor = g.p("floor", 0) > 0.5;
    s.edges = g.p("edges", 0) > 0.5;
    s.steps = std::clamp(int(std::lround(g.p("steps", 0, 4))), 1, 32);
    s.seed = uint64_t(std::llround(std::max(0.0, g.p("seed", 0, 1))));
    s.preroll = std::clamp(g.p("preroll", 0, 0), 0.0, 120.0);
    s.burst = std::clamp(int(std::lround(g.p("burst", 0, 0))), 0, 200000);
    s.maxCount = size_t(std::clamp(std::llround(g.p("max_particles", 0, 20000)), 1LL, 200000LL));
    s.textSize = std::clamp(g.p("text_size", 0, 160), 4.0, 2000.0);
    s.textBold = g.p("text_bold", 0, 1) > 0.5;
    s.text = g.s("text", "Montage");
    s.font = g.s("font", "Sans Serif");
    s.picture = g.s("picture");
    return s;
}

// When a file was last written and its size, so what was read from it is read again once it changes (or appears).
std::string fileStamp(const std::string& path) {
    if (path.empty()) return {};
    const QFileInfo fi(QString::fromStdString(path));
    return "|" + std::to_string(fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : -1) + "|" + std::to_string(fi.exists() ? fi.size() : -1);
}

// The settings the simulation depends on (not how particles look), as the key its kept states are under.
const char* const kSettingNames[] = {"emitter", "edge_only", "outward", "mode", "floor", "edges", "steps", "seed",
                                     "preroll", "burst", "max_particles", "text_size", "text_bold"};

std::string simulationKey(const Effect& g, int sw, int sh, double fps, bool exprs) {
    Effect k;
    k.type = g.type;
    for (const SimParamName& n : kSimNames)
        if (auto it = g.params.find(n.name); it != g.params.end()) k.params[n.name] = it->second;
    for (const char* n : kSettingNames)
        if (auto it = g.params.find(n); it != g.params.end()) k.params[n] = it->second;
    for (const char* n : {"text", "font", "picture"})
        if (auto it = g.strings.find(n); it != g.strings.end()) k.strings[n] = it->second;
    char size[96];
    std::snprintf(size, sizeof size, "|%dx%d@%.12g", sw, sh, fps);
    std::string key = effectToJsonString(k) + size + fileStamp(g.s("picture"));
    // What an expression gives can depend on its clip (its sound, its place): states are kept per clip.
    if (exprs)
        if (const Clip* c = currentExpressionContext().clip) key += "|clip" + std::to_string(c->id);
    return key;
}

// ---- Emitter masks: where in a title's letters or a picture particles are born.

struct EmitMask {
    int w = 0, h = 0;                // pixels (a title's at sequence size or less; a picture's fitted to the emitter's box)
    double unit = 1;                 // a title's sequence pixels per pixel
    std::vector<uint32_t> pixels;    // y * w + x of each pixel that emits
    std::vector<double> cumulative;  // their weights, summed
    bool empty() const { return pixels.empty() || cumulative.back() <= 0; }
};

// Pixels of `weight` (w x h, 0..1) that emit: all in proportion to their weight, or (edge) those inside (over a half)
// next to one outside, evenly.
std::shared_ptr<EmitMask> maskFrom(const std::vector<float>& weight, int w, int h, bool edge) {
    auto m = std::make_shared<EmitMask>();
    m->w = w, m->h = h;
    double sum = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = weight[size_t(y) * size_t(w) + size_t(x)];
            double take = 0;
            if (edge) {
                auto inside = [&](int xx, int yy) {
                    return xx >= 0 && yy >= 0 && xx < w && yy < h && weight[size_t(yy) * size_t(w) + size_t(xx)] >= 0.5f;
                };
                if (v >= 0.5f && (!inside(x - 1, y) || !inside(x + 1, y) || !inside(x, y - 1) || !inside(x, y + 1))) take = 1;
            } else {
                take = v;
            }
            if (take <= 0) continue;
            sum += take;
            m->pixels.push_back(uint32_t(y) * uint32_t(w) + uint32_t(x));
            m->cumulative.push_back(sum);
        }
    return m;
}

std::shared_ptr<const EmitMask> textMask(const Settings& s) {
    QFont f(QString::fromStdString(s.font));
    f.setPixelSize(std::max(1, int(std::lround(s.textSize))));
    f.setBold(s.textBold);
    f.setHintingPreference(QFont::PreferNoHinting);
    QPainterPath path;
    const QStringList lines = QString::fromStdString(s.text).split('\n');
    const double lineStep = s.textSize * 1.15;
    for (int i = 0; i < lines.size(); ++i) {
        QPainterPath line;
        line.addText(0, 0, f, lines[i]);
        const QRectF b = line.boundingRect();
        // Each line centred.
        path.addPath(line.translated(-b.center().x(), i * lineStep));
    }
    const QRectF b = path.boundingRect();
    if (b.isEmpty()) return std::make_shared<EmitMask>();
    // At most 2048 pixels across (a long title in big letters is drawn smaller and scaled back up).
    const double k = std::min(1.0, 2044.0 / std::max(b.width(), b.height()));
    const int w = int(std::ceil(b.width() * k)) + 4, h = int(std::ceil(b.height() * k)) + 4;
    QImage img(w, h, QImage::Format_Grayscale8);
    img.fill(0);
    {
        QPainter pa(&img);
        pa.setRenderHint(QPainter::Antialiasing);
        pa.translate(2, 2);
        pa.scale(k, k);
        pa.translate(-b.left(), -b.top());
        pa.fillPath(path, Qt::white);
    }
    std::vector<float> weight(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y) {
        const uchar* r = img.constScanLine(y);
        for (int x = 0; x < w; ++x) weight[size_t(y) * size_t(w) + size_t(x)] = r[x] / 255.0f;
    }
    auto m = maskFrom(weight, w, h, s.edge);
    m->unit = 1 / k;
    return m;
}

std::shared_ptr<const EmitMask> pictureMask(const Settings& s) {
    if (s.picture.empty()) return std::make_shared<EmitMask>();
    int pw = 0, ph = 0;
    MediaPool& pool = MediaPool::instance();
    if (!pool.videoSize(s.picture, pw, ph) || pw <= 0 || ph <= 0) return std::make_shared<EmitMask>();
    const double k = std::min(1.0, 1024.0 / std::max(pw, ph));
    const int w = std::max(1, int(std::lround(pw * k))), h = std::max(1, int(std::lround(ph * k)));
    Frame16Ptr f = pool.videoFrame(s.picture, 0, w, h, true);
    if (!f || f->width != w || f->height != h) return std::make_shared<EmitMask>();
    std::vector<float> weight(size_t(w) * size_t(h));
    for (size_t i = 0; i < weight.size(); ++i) {
        const float a = f->unit(i * 4 + 3);
        weight[i] = s.emitter == PictureAlpha ? a : a * (0.2126f * f->unit(i * 4) + 0.7152f * f->unit(i * 4 + 1) + 0.0722f * f->unit(i * 4 + 2));
    }
    return maskFrom(weight, w, h, s.edge);
}

std::shared_ptr<const EmitMask> emitMask(const Settings& s) {
    if (s.emitter != Text && s.emitter != PictureAlpha && s.emitter != PictureBrightness) return nullptr;
    char head[128];
    std::snprintf(head, sizeof head, "%d|%d|%.9g|%d|", s.emitter, int(s.edge), s.textSize, int(s.textBold));
    const std::string key = head + (s.emitter == Text ? s.text + "|" + s.font : s.picture + fileStamp(s.picture));
    static std::mutex m;
    static std::list<std::pair<std::string, std::shared_ptr<const EmitMask>>> kept;  // the latest first
    {
        std::lock_guard lock(m);
        for (auto it = kept.begin(); it != kept.end(); ++it)
            if (it->first == key) {
                kept.splice(kept.begin(), kept, it);
                return it->second;
            }
    }
    std::shared_ptr<const EmitMask> made = s.emitter == Text ? textMask(s) : pictureMask(s);
    std::lock_guard lock(m);
    kept.emplace_front(key, made);
    if (kept.size() > 8) kept.pop_back();
    return made;
}

// ---- The simulation

struct V3 {
    double x = 0, y = 0, z = 0;
    double& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    double operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 normalized(V3 a) {
    const double l = std::sqrt(dot(a, a));
    return l > 0 ? a * (1 / l) : V3{0, -1, 0};
}

// A plane particles bounce off: they are inside while sign * (p[axis] - at) <= 0.
struct Wall {
    int axis;
    double at;
    double sign;
    uint64_t bit;  // in Particle::outside
};

constexpr double kMostRate = 100000;  // births a second (the catalogue's most)

class Simulation {
public:
    Simulation(const Settings& st, const std::vector<Frame>& frames, int64_t f0, double fps, int sw, int sh, const EmitMask* mask)
        : st_(st), frames_(frames), f0_(f0), h_(1.0 / (fps * st.steps)), sw_(sw), sh_(sh), mask_(mask) {}

    void step(ParticleState& s) const {
        const double t0 = double(s.step) * h_;
        const Frame mid = at(s.step, 0.5);
        for (Particle& q : s.live) {
            advance(q, h_, mid, t0);
            q.age += h_;
        }
        // Born in this step: the burst at the very start, then each particle the rate makes due, at its moment.
        const bool first = s.step == f0_ * st_.steps;
        const size_t before = s.live.size();
        if (first)
            for (int i = 0; i < st_.burst; ++i) spawn(s, 0);
        // (Within the catalogue's range, whatever an expression gives.)
        const double due = s.due + std::clamp(mid[Rate], 0.0, kMostRate) * h_;
        while (double(s.rateBorn + 1) <= due + 1e-9) {
            if (s.live.size() >= st_.maxCount) {
                // Full: the rest due in this step are not born, their numbers used up all the same.
                const uint64_t n = uint64_t(std::floor(due + 1e-9)) - s.rateBorn;
                s.rateBorn += n;
                s.nextId += n;
                break;
            }
            const double phase = due > s.due ? std::clamp((double(s.rateBorn + 1) - s.due) / (due - s.due), 0.0, 1.0) : 1.0;
            ++s.rateBorn;
            spawn(s, phase);
        }
        s.due = due;
        for (size_t i = before; i < s.live.size(); ++i) {
            Particle& q = s.live[i];
            const double phase = q.age / h_;  // (spawn keeps the birth's phase in age until here)
            const double left = h_ - q.age;
            q.age = 0;
            if (left > 0) advance(q, left, at(s.step, 0.5 * (1 + phase)), t0 + phase * h_);
            q.age = left;
        }
        const double far = 4.0 * std::max(sw_, sh_) + std::max({std::abs(mid[EmitX]), std::abs(mid[EmitY]), std::abs(mid[EmitZ])});
        std::erase_if(s.live, [&](const Particle& q) {
            return q.age >= q.life || std::abs(q.x) > far || std::abs(q.y) > far || std::abs(q.z) > far;
        });
        ++s.step;
    }

private:
    // The settings `frac` of the way through step `step`, blended between frames (the place worked out from whole numbers,
    // so a step ending on a frame never reaches past it).
    Frame at(int64_t step, double frac) const {
        const double ph = (double(step) + frac) / st_.steps;
        const double fl = std::floor(ph);
        const int64_t last = f0_ + int64_t(frames_.size()) - 1;
        const int64_t f = std::clamp<int64_t>(int64_t(fl), f0_, last);
        const int64_t g = std::min(f + 1, last);
        const double a = std::clamp(ph - double(f), 0.0, 1.0);
        const Frame& A = frames_[size_t(f - f0_)];
        const Frame& B = frames_[size_t(g - f0_)];
        if (a == 0 || f == g) return A;
        Frame out;
        for (int i = 0; i < kSimParams; ++i) out[size_t(i)] = A[size_t(i)] + (B[size_t(i)] - A[size_t(i)]) * a;
        return out;
    }

    // A new particle born `phase` of the way through the step (its age holds the phase until step() moves it).
    void spawn(ParticleState& s, double phase) const {
        const uint64_t id = s.nextId++;
        if (s.live.size() >= st_.maxCount) return;
        const Frame P = at(s.step, phase);
        const uint64_t seed = st_.seed;
        auto r = [&](uint64_t what) { return rnd(seed, id, what); };
        const double W = std::max(0.0, P[EmitW]), H = std::max(0.0, P[EmitH]), D = st_.threeD ? std::max(0.0, P[EmitD]) : 0.0;
        V3 local;
        switch (st_.emitter) {
            case Line: local.x = (r(kPos0) - 0.5) * W; break;
            case Box:
                if (!st_.edge) {
                    local = {(r(kPos0) - 0.5) * W, (r(kPos1) - 0.5) * H, (r(kPos2) - 0.5) * D};
                } else if (!st_.threeD) {
                    // Round the rectangle's outline, evenly.
                    double u = r(kPos0) * 2 * (W + H);
                    if (u < W) local = {u - W / 2, -H / 2, 0};
                    else if ((u -= W) < H) local = {W / 2, u - H / 2, 0};
                    else if ((u -= H) < W) local = {W / 2 - u, H / 2, 0};
                    else local = {-W / 2, H / 2 - (u - W), 0};
                } else {
                    // Over the box's faces, evenly.
                    const double a[3] = {W * H, W * D, H * D}, total = 2 * (a[0] + a[1] + a[2]);
                    const double u = r(kPos0) * total, side = r(kPos3) < 0.5 ? -0.5 : 0.5;
                    const double p = r(kPos1) - 0.5, q = r(kPos2) - 0.5;
                    if (u < 2 * a[0]) local = {p * W, q * H, side * D};
                    else if (u < 2 * (a[0] + a[1])) local = {p * W, side * H, q * D};
                    else local = {side * W, p * H, q * D};
                }
                break;
            case Sphere: {
                if (!st_.threeD) {
                    const double ang = 2 * kPi * r(kPos0), rad = st_.edge ? 1.0 : std::sqrt(r(kPos1));
                    local = {std::cos(ang) * rad * W / 2, std::sin(ang) * rad * H / 2, 0};
                } else {
                    const double zc = 2 * r(kPos0) - 1, ang = 2 * kPi * r(kPos1), rr = std::sqrt(std::max(0.0, 1 - zc * zc));
                    const double rad = st_.edge ? 1.0 : std::cbrt(r(kPos2));
                    local = {std::cos(ang) * rr * rad * W / 2, std::sin(ang) * rr * rad * H / 2, zc * rad * D / 2};
                }
                break;
            }
            case Text:
            case PictureAlpha:
            case PictureBrightness: {
                if (!mask_ || mask_->empty()) return;
                const double total = mask_->cumulative.back();
                const size_t i = std::min(mask_->pixels.size() - 1,
                                          size_t(std::upper_bound(mask_->cumulative.begin(), mask_->cumulative.end(), r(kPos0) * total) -
                                                 mask_->cumulative.begin()));
                const double px = double(mask_->pixels[i] % uint32_t(mask_->w)) + r(kPos1);
                const double py = double(mask_->pixels[i] / uint32_t(mask_->w)) + r(kPos2);
                // A title at its size; a picture fitted into the emitter's box.
                const double k = st_.emitter == Text ? mask_->unit : std::min(W / mask_->w, H / mask_->h);
                local = {(px - mask_->w / 2.0) * k, (py - mask_->h / 2.0) * k, (r(kPos3) - 0.5) * D};
                break;
            }
            default: break;
        }
        const double ea = P[EmitAngle] * kPi / 180, ce = std::cos(ea), se = std::sin(ea);
        local = {local.x * ce - local.y * se, local.x * se + local.y * ce, local.z};

        // Its direction: the set one (0 up, 90 right, elevation towards the viewer), or away from the emitter's centre,
        // spread round it.
        V3 axis;
        const double az = P[Direction] * kPi / 180, el = st_.threeD ? P[Elevation] * kPi / 180 : 0.0;
        axis = {std::sin(az) * std::cos(el), -std::cos(az) * std::cos(el), std::sin(el)};
        if (st_.outward) {
            if (dot(local, local) > 1e-18) {
                axis = normalized(local);
            } else if (!st_.threeD) {
                const double a = 2 * kPi * r(kDir0);
                axis = {std::cos(a), std::sin(a), 0};
            } else {
                const double zc = 2 * r(kDir0) - 1, a = 2 * kPi * r(kDir1), rr = std::sqrt(std::max(0.0, 1 - zc * zc));
                axis = {std::cos(a) * rr, std::sin(a) * rr, zc};
            }
        }
        const double spread = std::clamp(P[Spread], 0.0, 180.0) * kPi / 180;
        V3 dir;
        if (!st_.threeD) {
            const double a = spread * signedRnd(seed, id, kSpread0);
            dir = {axis.x * std::cos(a) - axis.y * std::sin(a), axis.x * std::sin(a) + axis.y * std::cos(a), 0};
        } else {
            // Evenly over the cone's cap.
            const double cz = 1 - r(kSpread0) * (1 - std::cos(spread)), sz = std::sqrt(std::max(0.0, 1 - cz * cz));
            const double a = 2 * kPi * r(kSpread1);
            const V3 other = std::abs(axis.x) < 0.9 ? V3{1, 0, 0} : V3{0, 1, 0};
            const V3 u = normalized(cross(axis, other)), v = cross(axis, u);
            dir = axis * cz + u * (sz * std::cos(a)) + v * (sz * std::sin(a));
        }
        const double speed = P[Speed] * std::max(0.0, 1 + P[SpeedRandom] / 100 * signedRnd(seed, id, kSpeed));
        Particle q;
        q.id = id;
        q.x = P[EmitX] + local.x, q.y = P[EmitY] + local.y, q.z = st_.threeD ? P[EmitZ] + local.z : 0.0;
        q.vx = dir.x * speed, q.vy = dir.y * speed, q.vz = st_.threeD ? dir.z * speed : 0.0;
        q.life = std::max(h_, P[Life] * std::max(0.0, 1 + P[LifeRandom] / 100 * signedRnd(seed, id, kLife)));
        q.age = phase * h_;
        // The walls it starts outside of (an emitter above the frame) do not hold it until it comes inside.
        std::vector<Wall> ws;
        walls(P, ws);
        for (const Wall& w : ws)
            if (w.sign * ((w.axis == 0 ? q.x : q.y) - w.at) > 0) q.outside |= w.bit;
        s.live.push_back(q);
    }

    // The forces but air resistance (worked out exactly in advance()).
    V3 acceleration(const V3& p, const Frame& P) const {
        V3 a{P[WindX], P[Gravity] + P[WindY], st_.threeD ? P[WindZ] : 0.0};
        if (P[Attract] != 0) {
            const V3 d = V3{P[AttractX], P[AttractY], st_.threeD ? P[AttractZ] : 0.0} - p;
            const double dist = std::sqrt(dot(d, d));
            if (dist > 1e-9) {
                const double r = std::max(1e-6, P[AttractRadius]);
                a = a + d * (P[Attract] / (dist * (1 + (dist / r) * (dist / r))));
            }
        }
        if (!st_.threeD) a.z = 0;
        return a;
    }

    V3 flow(const V3& p, double t, const Frame& P) const {
        if (P[Turbulence] == 0) return {};
        const double scale = std::max(1.0, P[TurbulenceScale]);
        const uint32_t seed = uint32_t(st_.seed * 2654435761u + 17u);
        if (!st_.threeD) {
            double o[2];
            curlNoise2(p.x, p.y, t, scale, P[TurbulenceEvolve], seed, o);
            return {o[0] * P[Turbulence], o[1] * P[Turbulence], 0};
        }
        double o[3];
        curlNoise3(p.x, p.y, p.z, t, scale, P[TurbulenceEvolve], seed, o);
        return {o[0] * P[Turbulence], o[1] * P[Turbulence], o[2] * P[Turbulence]};
    }

    void walls(const Frame& P, std::vector<Wall>& out) const {
        out.clear();
        if (st_.floor) out.push_back({1, (P[FloorY] / 100 - 0.5) * sh_, 1, 1});
        if (st_.edges) {
            out.push_back({0, -sw_ / 2.0, -1, 2});
            out.push_back({0, sw_ / 2.0, 1, 4});
            out.push_back({1, -sh_ / 2.0, -1, 8});
        }
    }

    // When c + w t + a t^2 / 2 (inside at 0 when c <= 0) first goes outside within [0, len]; -1 if it does not.
    static double exitTime(double c, double w, double a, double len) {
        if (c > 0) return -1;
        double roots[2];
        int n = 0;
        if (std::abs(a) < 1e-12) {
            if (w > 0) roots[n++] = -c / w;
        } else {
            const double disc = w * w - 2 * a * c;
            if (disc >= 0) {
                const double sq = std::sqrt(disc);
                // (The stable forms of the two roots.)
                const double q = -(w + std::copysign(sq, w));
                double r0 = q != 0 ? q / a : 0, r1 = q != 0 ? 2 * c / q : 0;
                if (r0 > r1) std::swap(r0, r1);
                roots[n++] = r0;
                roots[n++] = r1;
            }
        }
        for (int i = 0; i < n; ++i) {
            const double t = roots[i];
            if (t < 0 || t > len) continue;
            if (w + a * t > 0 || (w + a * t == 0 && a > 0)) return t;  // leaving, not arriving
        }
        return -1;
    }

    // Moves q on by dt from time t: the forces exactly while they are steady (velocity Verlet, air resistance solved
    // exactly), the turbulence carrying it along, bouncing off the walls at the moment it reaches them (friction taking
    // its share of the speed along the wall at each bounce, and while it rests that share every tenth of a second, as
    // more air resistance).
    void advance(Particle& q, double dt, const Frame& P, double t) const {
        std::vector<Wall> ws;
        ws.reserve(4);
        walls(P, ws);
        const double e = std::clamp(P[Bounce] / 100, 0.0, 1.0), keep = 1 - std::clamp(P[Friction] / 100, 0.0, 1.0);
        double k = std::max(0.0, P[Drag]);
        const double restingDrag = keep > 0 ? -10 * std::log(keep) : 1e6;
        // Over a time len: what is left of the speed (e^-kt), how far it carries (integral of that) and how far a steady
        // force carries from rest (integral of the last).
        auto drag = [&k](double len, double& ek, double& gk, double& hk) {
            if (k * len < 1e-4) {
                ek = 1 - k * len + 0.5 * (k * len) * (k * len);
                gk = len * (1 - 0.5 * k * len + (k * len) * (k * len) / 6);
                hk = len * len * (0.5 - k * len / 6);
            } else {
                ek = std::exp(-k * len);
                gk = -std::expm1(-k * len) / k;
                hk = (len - gk) / k;
            }
        };
        bool pinned[3] = {false, false, false};
        double left = dt;
        for (int round = 0; left > 0 && round < 8; ++round) {
            // Walls it was born outside of hold it once it is in; one it is just outside of (a floor that rose, a
            // rounding at a corner) puts it back.
            for (const Wall& w : ws) {
                double& c = w.axis == 0 ? q.x : q.y;
                double& vc = w.axis == 0 ? q.vx : q.vy;
                if (q.outside & w.bit) {
                    if (w.sign * (c - w.at) <= 0) q.outside &= ~w.bit;
                } else if (w.sign * (c - w.at) > 0) {
                    c = w.at;
                    if (w.sign * vc > 0) vc = 0;
                }
            }
            const V3 x{q.x, q.y, q.z};
            V3 v{q.vx, q.vy, q.vz};
            V3 a0 = acceleration(x, P), u = flow(x, t, P);
            for (int i = 0; i < 3; ++i)
                if (pinned[i]) a0[i] = 0, u[i] = 0, v[i] = 0;
            double ek, gk, hk;
            drag(left, ek, gk, hk);
            const V3 x1 = x + v * gk + u * left + a0 * hk;
            V3 a1 = acceleration(x1, P);
            for (int i = 0; i < 3; ++i)
                if (pinned[i]) a1[i] = 0;
            const V3 v1 = v * ek + (a0 + a1) * (0.5 * gk);
            int hit = -1;
            double when = left;
            for (int n = 0; n < int(ws.size()); ++n) {
                const Wall& w = ws[size_t(n)];
                if (pinned[w.axis] || (q.outside & w.bit) || w.sign * (x1[w.axis] - w.at) <= 0) continue;
                // (When, with the air's resistance taken as steady over the step.)
                double tc = exitTime(w.sign * (x[w.axis] - w.at), w.sign * (v[w.axis] + u[w.axis]), w.sign * (a0[w.axis] - k * v[w.axis]), left);
                if (tc < 0) tc = left;
                if (hit < 0 || tc < when) hit = n, when = tc;
            }
            if (hit < 0) {
                q.x = x1.x, q.y = x1.y, q.z = st_.threeD ? x1.z : 0.0;
                q.vx = v1.x, q.vy = v1.y, q.vz = st_.threeD ? v1.z : 0.0;
                break;
            }
            const Wall& w = ws[size_t(hit)];
            drag(when, ek, gk, hk);
            V3 xh = x + v * gk + u * when + a0 * hk;
            V3 vh = v * ek + a0 * gk;
            xh[w.axis] = w.at;
            // Too slow to leave the wall again within a step: it rests on it for the rest of this one.
            const double rest = std::abs(a0[w.axis]) * h_ + 1e-9;
            const bool impact = std::abs(vh[w.axis]) > rest;
            vh[w.axis] = -e * vh[w.axis];
            if (impact)
                for (int i = 0; i < 3; ++i)
                    if (i != w.axis) vh[i] *= keep;
            if (std::abs(vh[w.axis]) <= rest) {
                vh[w.axis] = 0;
                if (!pinned[0] && !pinned[1] && !pinned[2]) k += restingDrag;
                pinned[w.axis] = true;
            }
            q.x = xh.x, q.y = xh.y, q.z = st_.threeD ? xh.z : 0.0;
            q.vx = vh.x, q.vy = vh.y, q.vz = st_.threeD ? vh.z : 0.0;
            left -= when;
            t += when;
        }
    }

    const Settings& st_;
    const std::vector<Frame>& frames_;
    int64_t f0_;
    double h_;
    int sw_, sh_;
    const EmitMask* mask_;
};

// ---- Kept states

struct Kept {
    uint64_t hash = 0;  // of the settings every frame up to it
    std::shared_ptr<const ParticleState> state;
};
struct CacheEntry {
    std::map<int64_t, Kept> states;  // every 16 frames, by frame
    int64_t lastFrame = 0;
    Kept last;                       // the latest frame worked out (where playing carries on from)
    // The settings each frame and their running hashes, when no expression is among them (else worked out each time).
    std::shared_ptr<const std::vector<Frame>> frames;
    std::shared_ptr<const std::vector<uint64_t>> hashes;
    size_t bytes = 0;
    uint64_t used = 0;
};
std::mutex gCacheMutex;
std::unordered_map<std::string, CacheEntry> gCache;
size_t gCacheBytes = 0;
uint64_t gCacheClock = 0;

size_t stateBytes(const ParticleState& s) { return sizeof(ParticleState) + s.live.size() * sizeof(Particle) + 64; }

uint64_t hashFrame(uint64_t h, const Frame& f) {
    unsigned char bytes[sizeof(Frame)];
    std::memcpy(bytes, f.data(), sizeof bytes);
    for (unsigned char b : bytes) h = (h ^ b) * 0x100000001B3ull;
    return h;
}

// (With gCacheMutex held.)
void dropBytes(CacheEntry& e, size_t n) {
    e.bytes -= n;
    gCacheBytes -= n;
}

// Keeps the state at `frame`: one of the every-16-frames ones, or (checkpoint false) as the latest.
void keep(const std::string& key, int64_t frame, uint64_t hash, std::shared_ptr<const ParticleState> st, bool checkpoint) {
    std::lock_guard lock(gCacheMutex);
    CacheEntry& e = gCache[key];
    e.used = ++gCacheClock;
    const size_t bytes = stateBytes(*st);
    if (!checkpoint) {
        if (e.last.state) dropBytes(e, stateBytes(*e.last.state));
        e.last = {};
    } else if (auto it = e.states.find(frame); it != e.states.end()) {
        dropBytes(e, stateBytes(*it->second.state));
        e.states.erase(it);
    }
    // Over budget: the generators used longest ago go first, then every other one of this one's own.
    while (gCacheBytes + bytes > kCacheBudget) {
        auto oldest = gCache.end();
        for (auto it = gCache.begin(); it != gCache.end(); ++it)
            if (it->first != key && it->second.bytes > 0 && (oldest == gCache.end() || it->second.used < oldest->second.used)) oldest = it;
        if (oldest != gCache.end()) {
            gCacheBytes -= oldest->second.bytes;
            gCache.erase(oldest);
            continue;
        }
        if (e.states.size() < 2) break;
        bool odd = false;
        for (auto it = e.states.begin(); it != e.states.end();) {
            if ((odd = !odd)) {
                ++it;
                continue;
            }
            dropBytes(e, stateBytes(*it->second.state));
            it = e.states.erase(it);
        }
    }
    if (gCacheBytes + bytes > kCacheBudget) return;
    if (checkpoint) e.states[frame] = Kept{hash, std::move(st)};
    else e.last = Kept{hash, std::move(st)}, e.lastFrame = frame;
    e.bytes += bytes;
    gCacheBytes += bytes;
}

}  // namespace

void curlNoise2(double x, double y, double t, double scale, double evolve, uint32_t seed, double out[2]) {
    double d[3];
    gradNoise(x / scale, y / scale, t * evolve, seed, d);
    out[0] = d[1];
    out[1] = -d[0];
}

void curlNoise3(double x, double y, double z, double t, double scale, double evolve, uint32_t seed, double out[3]) {
    const double X = x / scale, Y = y / scale, Z = z / scale + t * evolve;
    double a[3], b[3], c[3];
    gradNoise(X, Y, Z, seed, a);
    gradNoise(X + 31.416, Y - 47.853, Z + 12.793, seed ^ 0x5bd1e995u, b);
    gradNoise(X - 23.137, Y + 19.371, Z - 41.719, seed ^ 0x27d4eb2fu, c);
    // curl (a, b, c): the potential's three components.
    out[0] = c[1] - b[2];
    out[1] = a[2] - c[0];
    out[2] = b[0] - a[1];
}

void clearParticleCache() {
    std::lock_guard lock(gCacheMutex);
    gCache.clear();
    gCacheBytes = 0;
}

ParticleState particleState(const Effect& g, FrameTime t, int sw, int sh, double fps) {
    fps = fps > 0 ? fps : 30;
    sw = std::max(1, sw), sh = std::max(1, sh);
    const Settings st = readSettings(g);
    const int64_t f0 = -int64_t(std::llround(st.preroll * fps));
    t = std::max<FrameTime>(t, f0);

    // The settings every frame from the start: worked out once each when they change, else read once.
    bool exprs = false, varies[kSimParams];
    Frame still;
    for (int i = 0; i < kSimParams; ++i) {
        auto it = g.params.find(kSimNames[i].name);
        varies[i] = it != g.params.end() && it->second.varies();
        exprs = exprs || (it != g.params.end() && !it->second.expr.empty());
        still[size_t(i)] = it != g.params.end() ? it->second.at(0) : kSimNames[i].def;
    }
    const std::string key = simulationKey(g, sw, sh, fps, exprs);
    const size_t need = size_t(t - f0 + 1);
    std::shared_ptr<const std::vector<Frame>> frames;
    std::shared_ptr<const std::vector<uint64_t>> hashes;
    if (!exprs) {
        std::lock_guard lock(gCacheMutex);
        if (auto e = gCache.find(key); e != gCache.end() && e->second.frames && e->second.frames->size() >= need)
            frames = e->second.frames, hashes = e->second.hashes;
    }
    if (!frames) {
        // (Without expressions, a few seconds more than asked for, for playing on.)
        const size_t n = exprs ? need : need + size_t(std::llround(10 * fps));
        auto fr = std::make_shared<std::vector<Frame>>(n, still);
        auto hs = std::make_shared<std::vector<uint64_t>>(n);
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t k = 0; k < n; ++k) {
            for (int i = 0; i < kSimParams; ++i)
                if (varies[i]) (*fr)[k][size_t(i)] = g.params.at(kSimNames[i].name).at(f0 + int64_t(k));
            h = hashFrame(h, (*fr)[k]);
            (*hs)[k] = h;
        }
        frames = fr, hashes = hs;
        if (!exprs) {
            std::lock_guard lock(gCacheMutex);
            CacheEntry& e = gCache[key];
            e.frames = frames, e.hashes = hashes;
        }
    }

    // The nearest kept state at or before t that the same settings led to.
    ParticleState s;
    s.step = f0 * st.steps;
    int64_t from = f0;
    {
        std::lock_guard lock(gCacheMutex);
        if (auto e = gCache.find(key); e != gCache.end()) {
            e->second.used = ++gCacheClock;
            for (auto it = e->second.states.upper_bound(t); it != e->second.states.begin();) {
                --it;
                if (it->first < f0 || it->second.hash != (*hashes)[size_t(it->first - f0)]) continue;
                s = *it->second.state;
                from = it->first;
                break;
            }
            const Kept& last = e->second.last;
            const int64_t lf = e->second.lastFrame;
            if (last.state && lf > from && lf <= t && last.hash == (*hashes)[size_t(lf - f0)]) {
                s = *last.state;
                from = lf;
            }
        }
    }
    const std::shared_ptr<const EmitMask> mask = emitMask(st);
    const Simulation sim(st, *frames, f0, fps, sw, sh, mask.get());
    for (int64_t f = from; f < t; ++f) {
        for (int i = 0; i < st.steps; ++i) sim.step(s);
        const int64_t done = f + 1;
        if ((done - f0) % kKeepEvery == 0) keep(key, done, (*hashes)[size_t(done - f0)], std::make_shared<const ParticleState>(s), true);
    }
    if (t > from && (t - f0) % kKeepEvery != 0) keep(key, t, (*hashes)[size_t(t - f0)], std::make_shared<const ParticleState>(s), false);
    return s;
}

// ---- Drawing

namespace {

struct Mark {
    double x, y;    // output pixels
    double tx, ty;  // a streak's tail
    double r;       // radius
    float rgba[4];  // premultiplied
    double angle;   // radians
    double tumble;  // a square's turn about its long axis (radians)
    double depth;  // the lens's magnification there: smaller is farther
    uint64_t id;
    int x0, y0, x1, y1;  // the pixels it can touch (x1, y1 past the end)
};

void hueRotate(double deg, double rgb[3]) {
    if (deg == 0) return;
    const double a = deg * kPi / 180, c = std::cos(a), s = std::sin(a), k = (1 - c) / 3, q = std::sqrt(1.0 / 3) * s;
    const double r = rgb[0], g = rgb[1], b = rgb[2];
    rgb[0] = std::clamp((c + k) * r + (k - q) * g + (k + q) * b, 0.0, 1.0);
    rgb[1] = std::clamp((k + q) * r + (c + k) * g + (k - q) * b, 0.0, 1.0);
    rgb[2] = std::clamp((k - q) * r + (k + q) * g + (c + k) * b, 0.0, 1.0);
}

// A sprite's picture (premultiplied, at most 256 pixels across) at clip time `seconds`.
std::shared_ptr<const Image> spritePicture(const std::string& path, double seconds) {
    if (path.empty()) return nullptr;
    MediaPool& pool = MediaPool::instance();
    int w = 0, h = 0;
    if (!pool.videoSize(path, w, h) || w <= 0 || h <= 0) return nullptr;
    const double k = std::min(1.0, 256.0 / std::max(w, h));
    const int sw = std::max(1, int(std::lround(w * k))), sh = std::max(1, int(std::lround(h * k)));
    Frame16Ptr f = pool.videoFrame(path, std::max(0.0, seconds), sw, sh, true);
    if (!f || f->width <= 0 || f->height <= 0) return nullptr;
    return std::make_shared<const Image>(toImage(*f));
}

}  // namespace

Image renderParticles(const Effect& g, FrameTime t, int w, int h, int sw, int sh, double fps, int bands) {
    Image img(w, h);
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return img;
    const double kx = double(w) / sw, ky = double(h) / sh, scale = std::sqrt(kx * ky);
    const ParticleState s = particleState(g, t, sw, sh, fps);
    if (s.live.empty()) return img;
    const Settings st = readSettings(g);
    const int look = std::clamp(int(std::lround(g.p("look", t, SoftDiscs))), 0, 4);
    const bool add = g.p("blend", t) > 0.5;
    // (Within the catalogue's ranges, whatever an expression gives.)
    const double size = std::clamp(g.p("size", t, 12), 0.0, 2000.0), sizeRandom = std::clamp(g.p("size_random", t, 30) / 100, 0.0, 1.0),
                 sizeEnd = std::clamp(g.p("size_end", t, 100) / 100, 0.0, 20.0);
    const double c0[3] = {g.p("color.r", t, 1), g.p("color.g", t, 1), g.p("color.b", t, 1)};
    const double c1[3] = {g.p("color_end.r", t, 1), g.p("color_end.g", t, 1), g.p("color_end.b", t, 1)};
    const double hueRandom = g.p("hue_random", t), opacity = std::clamp(g.p("opacity", t, 100) / 100, 0.0, 1.0);
    const double fadeIn = g.p("fade_in", t, 10) / 100, fadeOut = g.p("fade_out", t, 30) / 100;
    const double rotation = g.p("rotation", t), rotationRandom = g.p("rotation_random", t), spin = g.p("spin", t),
                 spinRandom = g.p("spin_random", t) / 100;
    const double shutter = std::clamp(g.p("streak", t, 0.5), 0.0, 10.0) / (fps > 0 ? fps : 30);
    const uint64_t seed = st.seed;
    std::shared_ptr<const Image> sprite = look == Sprites ? spritePicture(g.s("sprite"), double(t) / (fps > 0 ? fps : 30)) : nullptr;

    // The 3D titles' lens: the frame's plane (z = 0) at its true size, the camera turned about the frame's centre.
    const double fov = std::clamp(g.p("fov", t, 35), 5.0, 120.0) * kPi / 180;
    const double focal = (sh / 2.0) / std::tan(fov / 2);
    const double tilt = g.p("cam_tilt", t) * kPi / 180, turn = g.p("cam_turn", t) * kPi / 180;
    const double ct = std::cos(tilt), stl = std::sin(tilt), cy = std::cos(turn), sy = std::sin(turn);
    auto project = [&](double x, double y, double z, double& ox, double& oy, double& k) {
        if (!st.threeD) {
            ox = w / 2.0 + x * kx, oy = h / 2.0 + y * ky, k = 1;
            return true;
        }
        const double x1 = cy * x + sy * z, z1 = -sy * x + cy * z;
        const double y2 = ct * y - stl * z1, z2 = stl * y + ct * z1;
        const double d = focal - z2;
        if (d < focal * 0.02) return false;
        k = focal / d;
        ox = w / 2.0 + x1 * k * kx, oy = h / 2.0 + y2 * k * ky;
        return true;
    };

    std::vector<Mark> marks;
    marks.reserve(s.live.size());
    for (const Particle& q : s.live) {
        const double a = std::clamp(q.age / std::max(1e-9, q.life), 0.0, 1.0);
        double op = opacity;
        if (fadeIn > 0) op *= std::min(1.0, a / fadeIn);
        if (fadeOut > 0) op *= std::min(1.0, (1 - a) / fadeOut);
        if (op <= 0) continue;
        Mark m{};
        double k;
        if (!project(q.x, q.y, q.z, m.x, m.y, k)) continue;
        m.depth = k;  // (smaller farther)
        double diameter = size * std::max(0.0, 1 + sizeRandom * signedRnd(seed, q.id, kSize)) * (1 + (sizeEnd - 1) * a);
        m.r = diameter / 2 * k * scale;
        if (m.r <= 0) continue;
        m.tx = m.x, m.ty = m.y;
        if (look == Streaks) {
            double tk;
            if (!project(q.x - q.vx * shutter, q.y - q.vy * shutter, q.z - q.vz * shutter, m.tx, m.ty, tk)) m.tx = m.x, m.ty = m.y;
        }
        double rgb[3];
        for (int i = 0; i < 3; ++i) rgb[i] = c0[i] + (c1[i] - c0[i]) * a;
        hueRotate(hueRandom * signedRnd(seed, q.id, kHue), rgb);
        // Too small to cover a pixel: as faint as it is small instead.
        const double least = look == SoftDiscs || look == Sprites ? 1.0 : 0.5;
        if (m.r < least) {
            op *= (m.r / least) * (m.r / least);
            m.r = least;
        }
        for (int i = 0; i < 3; ++i) m.rgba[i] = float(rgb[i] * op);
        m.rgba[3] = float(op);
        const double turnRate = spin * (1 + spinRandom * signedRnd(seed, q.id, kSpin));
        m.angle = (rotation + rotationRandom * signedRnd(seed, q.id, kRotation) + turnRate * q.age) * kPi / 180;
        m.tumble = m.angle * 1.3 + rnd(seed, q.id, kTumble) * 2 * kPi;
        m.id = q.id;
        double reach = m.r;
        if (look == Sprites && sprite) reach = m.r * std::hypot(1.0, double(sprite->height) / sprite->width);
        else if (look == Squares) reach = m.r * 1.5;
        m.x0 = int(std::floor(std::min(m.x, m.tx) - reach - 1));
        m.x1 = int(std::ceil(std::max(m.x, m.tx) + reach + 1));
        m.y0 = int(std::floor(std::min(m.y, m.ty) - reach - 1));
        m.y1 = int(std::ceil(std::max(m.y, m.ty) + reach + 1));
        if (m.x1 <= 0 || m.y1 <= 0 || m.x0 >= w || m.y0 >= h) continue;
        m.x0 = std::max(0, m.x0), m.y0 = std::max(0, m.y0), m.x1 = std::min(w, m.x1), m.y1 = std::min(h, m.y1);
        marks.push_back(m);
    }
    // In 3D the farthest first; flat, in the order they were born (the newest on top).
    if (st.threeD)
        std::sort(marks.begin(), marks.end(), [](const Mark& a, const Mark& b) { return a.depth != b.depth ? a.depth < b.depth : a.id < b.id; });

    auto draw = [&](int y0, int y1) {
        for (const Mark& m : marks) {
            const int ya = std::max(y0, m.y0), yb = std::min(y1, m.y1);
            if (ya >= yb) continue;
            const double ca = std::cos(m.angle), sa = std::sin(m.angle);
            const double sx = m.tx - m.x, sy2 = m.ty - m.y, len2 = sx * sx + sy2 * sy2;
            for (int y = ya; y < yb; ++y) {
                float* row = img.row(y);
                const double py = y + 0.5;
                for (int x = m.x0; x < m.x1; ++x) {
                    const double px = x + 0.5, dx = px - m.x, dy = py - m.y;
                    double cover = 0;
                    float src[4] = {m.rgba[0], m.rgba[1], m.rgba[2], m.rgba[3]};
                    switch (look) {
                        case Dots: cover = std::clamp(m.r + 0.5 - std::sqrt(dx * dx + dy * dy), 0.0, 1.0); break;
                        case Streaks: {
                            if (len2 < 0.25) {
                                cover = std::clamp(m.r + 0.5 - std::sqrt(dx * dx + dy * dy), 0.0, 1.0);
                                break;
                            }
                            // From the tail (faint) to the head.
                            const double tx = px - m.tx, ty = py - m.ty;
                            const double along = std::clamp((tx * -sx + ty * -sy2) / len2, 0.0, 1.0);
                            const double ex = tx + sx * along, ey = ty + sy2 * along;
                            cover = std::clamp(m.r + 0.5 - std::sqrt(ex * ex + ey * ey), 0.0, 1.0) * (0.15 + 0.85 * along);
                            break;
                        }
                        case Squares: {
                            const double lx = ca * dx + sa * dy, ly = -sa * dx + ca * dy;
                            const double hy = m.r * 0.6 * std::abs(std::cos(m.tumble)) + 0.35;
                            cover = std::clamp(std::min(m.r - std::abs(lx), hy - std::abs(ly)) + 0.5, 0.0, 1.0);
                            break;
                        }
                        case Sprites:
                            if (sprite) {
                                const double lx = ca * dx + sa * dy, ly = -sa * dx + ca * dy;
                                const double u = (lx / (2 * m.r) + 0.5) * sprite->width - 0.5;
                                const double v = (ly / (2 * m.r) + 0.5 * double(sprite->height) / sprite->width) * sprite->width - 0.5;
                                const int iu = int(std::floor(u)), iv = int(std::floor(v));
                                const double fu = u - iu, fv = v - iv;
                                float acc[4] = {0, 0, 0, 0};
                                for (int j = 0; j < 2; ++j)
                                    for (int i = 0; i < 2; ++i) {
                                        const int xx = iu + i, yy = iv + j;
                                        if (xx < 0 || yy < 0 || xx >= sprite->width || yy >= sprite->height) continue;
                                        const double wgt = (i ? fu : 1 - fu) * (j ? fv : 1 - fv);
                                        const float* p = sprite->at(xx, yy);
                                        for (int c = 0; c < 4; ++c) acc[c] += float(p[c] * wgt);
                                    }
                                // Tinted by the particle's colour and opacity.
                                const float op = m.rgba[3], tint[3] = {op > 0 ? m.rgba[0] / op : 0, op > 0 ? m.rgba[1] / op : 0, op > 0 ? m.rgba[2] / op : 0};
                                for (int c = 0; c < 3; ++c) src[c] = acc[c] * tint[c] * op;
                                src[3] = acc[3] * op;
                                cover = 1;
                                break;
                            }
                            [[fallthrough]];
                        case SoftDiscs:
                        default: {
                            const double q2 = (dx * dx + dy * dy) / (m.r * m.r);
                            cover = q2 < 1 ? (1 - q2) * (1 - q2) : 0;
                            break;
                        }
                    }
                    if (cover <= 0) continue;
                    float* d = row + size_t(x) * 4;
                    const float k = float(cover);
                    if (add) {
                        for (int c = 0; c < 3; ++c) d[c] += src[c] * k;
                        d[3] += src[3] * k * (1 - d[3]);
                    } else {
                        const float keepBelow = 1 - src[3] * k;
                        for (int c = 0; c < 4; ++c) d[c] = src[c] * k + d[c] * keepBelow;
                    }
                }
            }
        }
    };
    if (bands <= 0) {
        parallelRows(h, draw);
    } else if (bands == 1) {
        draw(0, h);
    } else {
        std::vector<std::thread> threads;
        for (int b = 0; b < bands; ++b) threads.emplace_back(draw, int(int64_t(h) * b / bands), int(int64_t(h) * (b + 1) / bands));
        for (std::thread& th : threads) th.join();
    }
    return img;
}

}  // namespace montage
