#include "FrameRepair.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>
#include <optional>
#include <sstream>

#include "core/Effects.h"
#include "core/Model.h"
#include "media/Breaths.h"
#include "media/Decoder.h"
#include "media/Rife.h"
#include "render/Retime.h"

namespace montage {

namespace {

constexpr int kAnalysisWidth = 480;
constexpr double kResidual = 0.12;  // a pixel differs from what its neighbours make by more than this

struct Analysed {
    int64_t index = 0;
    Image img;               // at the analysis size
    std::vector<float> luma;
    double mean = 0, deviation = 0;
    std::vector<float> hist;  // 32 bins of luma, summing to 1
    std::vector<float> coarse;  // luma in blocks, 32 across: the picture's layout, which motion hardly changes
    double coarseDeviation = 0;
    bool damaged = false, missing = false;
};

std::vector<float> lumaOf(const Image& img) {
    std::vector<float> l(size_t(img.width) * size_t(img.height));
    for (int y = 0; y < img.height; ++y) {
        const float* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 4)
            l[size_t(y) * size_t(img.width) + size_t(x)] = std::clamp(0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2], 0.0f, 1.0f);
    }
    return l;
}

void measure(Analysed& a) {
    a.luma = lumaOf(a.img);
    double s = 0, s2 = 0;
    a.hist.assign(32, 0.0f);
    for (float v : a.luma) {
        s += v, s2 += double(v) * v;
        a.hist[size_t(std::min(31, int(v * 32)))] += 1;
    }
    const double n = std::max<size_t>(1, a.luma.size());
    a.mean = s / n;
    a.deviation = std::sqrt(std::max(0.0, s2 / n - a.mean * a.mean));
    for (float& h : a.hist) h = float(h / n);
    const int W = a.img.width, H = a.img.height, cw = 32, ch = std::max(1, int(std::lround(32.0 * H / std::max(1, W))));
    a.coarse.assign(size_t(cw) * size_t(ch), 0.0f);
    std::vector<int> count(a.coarse.size(), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t b = size_t(std::min(ch - 1, y * ch / H)) * size_t(cw) + size_t(std::min(cw - 1, x * cw / W));
            a.coarse[b] += a.luma[size_t(y) * size_t(W) + size_t(x)], ++count[b];
        }
    double cs = 0, cs2 = 0;
    for (size_t b = 0; b < a.coarse.size(); ++b) {
        a.coarse[b] /= float(std::max(1, count[b]));
        cs += a.coarse[b], cs2 += double(a.coarse[b]) * a.coarse[b];
    }
    const double cn = double(a.coarse.size()), cm = cs / cn;
    a.coarseDeviation = std::sqrt(std::max(0.0, cs2 / cn - cm * cm));
}

// 0 (the same spread of brightness) .. 1 (nothing in common): a cut between two frames shows here.
double histogramDistance(const Analysed& a, const Analysed& b) {
    double d = 0;
    for (size_t i = 0; i < a.hist.size(); ++i) d += std::fabs(a.hist[i] - b.hist[i]);
    return d / 2;
}

double correlation(const std::vector<float>& a, const std::vector<float>& b) {
    double ma = 0, mb = 0;
    for (size_t i = 0; i < a.size(); ++i) ma += a[i], mb += b[i];
    ma /= double(a.size()), mb /= double(b.size());
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double x = a[i] - ma, y = b[i] - mb;
        ab += x * y, aa += x * x, bb += y * y;
    }
    return aa > 1e-9 && bb > 1e-9 ? ab / std::sqrt(aa * bb) : 0;
}

bool blackFrame(const Analysed& a) { return a.mean < 0.03 && a.deviation < 0.03; }

// Two frames of one shot: the same spread of brightness and the same layout (a cut between two shots lit alike still
// changes where things are).
bool sameShot(const Analysed& a, const Analysed& b) {
    if (histogramDistance(a, b) >= 0.35 || a.img.width != b.img.width) return false;
    return (a.coarseDeviation < 0.02 && b.coarseDeviation < 0.02) || correlation(a.coarse, b.coarse) > 0.5;
}

}  // namespace

std::string frameDamageName(FrameDamage d) {
    switch (d) {
        case FrameDamage::Decode: return "decode";
        case FrameDamage::Hit: return "hit";
        case FrameDamage::Dropout: return "dropout";
        case FrameDamage::Flash: return "flash";
        case FrameDamage::Black: return "black";
        case FrameDamage::Missing: return "missing";
    }
    return "hit";
}

bool frameDamageFromName(const std::string& name, FrameDamage& out) {
    for (FrameDamage d : {FrameDamage::Decode, FrameDamage::Hit, FrameDamage::Dropout, FrameDamage::Flash, FrameDamage::Black, FrameDamage::Missing})
        if (frameDamageName(d) == name) {
            out = d;
            return true;
        }
    return false;
}

std::vector<DamagedFrame> findDamagedFrames(const std::string& path, int64_t first, int64_t last, const std::function<void(double)>& progress,
                                            const std::atomic<bool>* cancel, std::string* error) {
    std::vector<DamagedFrame> found;
    VideoDecoder dec;
    if (!dec.open(path, error)) return found;
    const double fps = dec.fps();
    if (dec.isStill() || fps <= 0) {
        if (error) *error = "Not a moving picture";
        return found;
    }
    const int64_t frames = int64_t(std::floor(dec.duration() * fps + 1e-6));
    first = std::clamp<int64_t>(first, 0, frames);
    last = last < 0 ? frames : std::clamp<int64_t>(last, first, frames);
    const int aw = std::max(16, std::min(kAnalysisWidth, dec.displayWidth()));
    const int ah = std::max(16, int(std::lround(double(aw) * dec.displayHeight() / std::max(1, dec.displayWidth()))));
    double lastPts = -1;
    auto read = [&](int64_t i, Analysed& a) {
        const Frame16Ptr f = dec.frameAt((double(i) + 0.25) / fps, aw, ah);
        if (!f) return false;
        a.index = i;
        a.img = toImage(*f);
        a.damaged = f->damaged;
        // (A frame whose time is missing shows as the frame before it again.)
        a.missing = lastPts >= 0 && f->pts <= lastPts + 0.25 / fps;
        lastPts = f->pts;
        measure(a);
        return true;
    };
    // The frames ahead (the one judged first) and the last good one before it.
    std::deque<Analysed> ahead;
    int64_t nextRead = std::max<int64_t>(0, first - 1);
    auto fill = [&] {
        while (ahead.size() < 5 && nextRead < std::min(frames, last + 4)) {
            Analysed a;
            if (!read(nextRead++, a)) {
                nextRead = frames;
                break;
            }
            ahead.push_back(std::move(a));
        }
    };
    fill();
    Analysed good;
    bool haveGood = false;
    double goodBeforeMean = -1;  // the mean of the good frame before `good`
    if (first > 0 && !ahead.empty() && ahead.front().index == first - 1) {
        good = std::move(ahead.front());
        ahead.pop_front();
        haveGood = !good.damaged && !blackFrame(good);
    }
    std::deque<double> usual;  // the share of pixels this shot's recent frames differ by from what their neighbours make
    int64_t judged = 0, missing = 0;
    for (int64_t n = first; n < last; ++n) {
        if (cancel && cancel->load()) break;
        fill();
        if (ahead.empty() || ahead.front().index != n) break;
        if (progress && (n - first) % 8 == 0) progress(double(n - first) / double(std::max<int64_t>(1, last - first)));
        Analysed& cur = ahead.front();
        const Analysed* next = ahead.size() > 1 ? &ahead[1] : nullptr;
        std::optional<DamagedFrame> damage;
        bool cameraFlash = false;
        ++judged;
        if (cur.missing) {
            ++missing;
            damage = DamagedFrame{n, FrameDamage::Missing, 1};
        } else if (cur.damaged) {
            damage = DamagedFrame{n, FrameDamage::Decode, 1};
        } else if (haveGood && next && n - good.index <= 3) {
            // (Further from the last good frame than that, what is made is too far off to judge by: the frame is taken.)
            // Black within a shot (not the bottom of a fade, nor black cut in between two shots: the same picture, steadily
            // lit, on both sides within a few frames).
            size_t lit = 0;
            for (size_t k = 1; k < ahead.size() && !lit; ++k)
                if (ahead[k].mean > 0.1 && ahead[k].deviation > 0.03) lit = k;
            const bool steady = (goodBeforeMean < 0 || std::fabs(good.mean - goodBeforeMean) < 0.05) &&
                                (lit + 1 >= ahead.size() || std::fabs(ahead[lit].mean - ahead[lit + 1].mean) < 0.05);
            if (!sameShot(good, *next)) usual.clear();  // (a cut: this shot's usual difference is learnt afresh)
            if (blackFrame(cur) && good.mean > 0.1 && lit && steady && sameShot(good, ahead[lit])) {
                damage = DamagedFrame{n, FrameDamage::Black, 1};
            } else if (sameShot(good, *next) && cur.img.width == good.img.width) {
                // What the good frame before and each of the next two frames make at this frame's time (two, so a broken
                // frame after this one does not make it look broken), and where this one differs from all of them.
                std::vector<std::vector<float>> made;
                std::vector<double> madeMean;
                for (size_t k = 1; k < ahead.size() && k <= 2; ++k) {
                    const Analysed& after = ahead[k];
                    if (k > 1 && !sameShot(good, after)) continue;
                    const double t = double(n - good.index) / double(after.index - good.index);
                    made.push_back(lumaOf(interpolateFrames(good.img, after.img, t)));
                    madeMean.push_back(std::accumulate(made.back().begin(), made.back().end(), 0.0) / double(std::max<size_t>(1, made.back().size())));
                }
                size_t closest = 0;
                for (size_t k = 1; k < made.size(); ++k)
                    if (std::fabs(cur.mean - madeMean[k]) < std::fabs(cur.mean - madeMean[closest])) closest = k;
                const double brighter = cur.mean - madeMean[closest];
                if (std::fabs(brighter) > 0.15) {
                    // A camera flash (or a flicker) lights the same picture; a flash frame is nothing but light.
                    if (correlation(cur.luma, made[closest]) > 0.6) cameraFlash = true;
                    else damage = DamagedFrame{n, FrameDamage::Flash, 1};
                } else {
                    const int W = cur.img.width, H = cur.img.height;
                    std::vector<int> rowBad(size_t(H), 0);
                    size_t bad = 0;
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x) {
                            const size_t i = size_t(y) * size_t(W) + size_t(x);
                            float r = 1;
                            for (const std::vector<float>& m : made) r = std::min(r, std::fabs(cur.luma[i] - m[i]));
                            if (r > kResidual) ++rowBad[size_t(y)], ++bad;
                        }
                    const double share = double(bad) / double(size_t(W) * size_t(H));
                    std::vector<double> sorted(usual.begin(), usual.end());
                    std::sort(sorted.begin(), sorted.end());
                    const double typical = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];
                    usual.push_back(share);  // (every frame's: the median keeps a broken one from counting)
                    if (usual.size() > 15) usual.pop_front();
                    if (sorted.size() >= 3 && share > std::max(0.003, 5 * typical)) {
                        // Whole lines (most of a line's width) holding most of what differs: tape drop-out.
                        size_t inLines = 0;
                        int lines = 0;
                        for (int y = 0; y < H; ++y)
                            if (rowBad[size_t(y)] > W / 2) inLines += size_t(rowBad[size_t(y)]), ++lines;
                        const bool dropout = lines > 0 && lines < H / 2 && double(inLines) > 0.6 * double(bad);
                        damage = DamagedFrame{n, dropout ? FrameDamage::Dropout : FrameDamage::Hit, share};
                    }
                }
            }
        }
        if (damage) {
            found.push_back(*damage);
        } else if (!cameraFlash && !blackFrame(cur)) {
            goodBeforeMean = haveGood ? good.mean : -1;
            good = std::move(cur);
            haveGood = true;
        }
        ahead.pop_front();
    }
    // Repeats all through the file are its own timing (a variable frame rate, or a rate given per field), not frames lost.
    if (missing > std::max<int64_t>(2, judged / 50)) std::erase_if(found, [](const DamagedFrame& d) { return d.kind == FrameDamage::Missing; });
    if (progress) progress(1.0);
    return found;
}

std::map<int64_t, FrameDamage> damagedFramesFromString(const std::string& s) {
    std::map<int64_t, FrameDamage> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream l(line);
        long long n = -1;
        std::string kind;
        FrameDamage d;
        if (l >> n >> kind && n >= 0 && frameDamageFromName(kind, d)) out[int64_t(n)] = d;
    }
    return out;
}

std::string damagedFramesToString(const std::map<int64_t, FrameDamage>& frames) {
    std::string s;
    for (const auto& [n, d] : frames) s += std::to_string(n) + " " + frameDamageName(d) + "\n";
    return s;
}

bool repairFrame(const std::map<int64_t, FrameDamage>& frames, int64_t n, Image& img, const std::function<bool(int64_t, Image&)>& fetch,
                 const std::string& cacheKey, bool show) {
    const auto it = frames.find(n);
    if (it == frames.end() || img.empty()) return false;
    // The nearest good frames either side (several bad ones in a row are filled from the frames around them all).
    constexpr int kReach = 12;
    Image before, after;
    int64_t a = -1, b = -1;
    for (int64_t i = n - 1; i >= std::max<int64_t>(0, n - kReach) && a < 0; --i)
        if (!frames.count(i) && fetch(i, before) && before.width == img.width && before.height == img.height) a = i;
    for (int64_t i = n + 1; i <= n + kReach && b < 0; ++i)
        if (!frames.count(i) && fetch(i, after) && after.width == img.width && after.height == img.height) b = i;
    if (a < 0 && b < 0) return false;
    Image made;
    if (a < 0) made = after;
    else if (b < 0) made = before;
    else {
        const double t = double(n - a) / double(b - a);
        const std::string key = cacheKey + '@' + std::to_string(a) + '-' + std::to_string(b);
        if (!(rifeAvailable() && rifeModel().installed() && cachedRife(before, after, t, key, made))) made = interpolateFrames(before, after, t, key);
    }
    if (made.width != img.width || made.height != img.height) return false;
    const int W = img.width, H = img.height;
    std::vector<float> mask(size_t(W) * size_t(H), 1.0f);
    if (localDamage(it->second)) {
        // Only where the frame differs from what is made, grown a little and softened, so the rest keeps its own detail.
        const std::vector<float> l = lumaOf(img), m = lumaOf(made);
        std::vector<float> raw(mask.size());
        for (size_t i = 0; i < raw.size(); ++i) raw[i] = std::fabs(l[i] - m[i]) > 0.08f ? 1.0f : 0.0f;
        const int r = std::max(2, W / 240);
        // A box grown then softened: horizontal then vertical maximum, then a box blur of the same radius.
        auto pass = [&](std::vector<float>& v, bool maximum) {
            std::vector<float> tmp(v.size());
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float acc = 0;
                    for (int k = -r; k <= r; ++k) {
                        const float s = v[size_t(y) * size_t(W) + size_t(std::clamp(x + k, 0, W - 1))];
                        acc = maximum ? std::max(acc, s) : acc + s;
                    }
                    tmp[size_t(y) * size_t(W) + size_t(x)] = maximum ? acc : acc / float(2 * r + 1);
                }
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    float acc = 0;
                    for (int k = -r; k <= r; ++k) {
                        const float s = tmp[size_t(std::clamp(y + k, 0, H - 1)) * size_t(W) + size_t(x)];
                        acc = maximum ? std::max(acc, s) : acc + s;
                    }
                    v[size_t(y) * size_t(W) + size_t(x)] = maximum ? acc : acc / float(2 * r + 1);
                }
        };
        pass(raw, true);
        pass(raw, false);
        mask = std::move(raw);
    }
    for (int y = 0; y < H; ++y) {
        float* d = img.row(y);
        const float* s = made.row(y);
        for (int x = 0; x < W; ++x, d += 4, s += 4) {
            const float k = mask[size_t(y) * size_t(W) + size_t(x)];
            if (k <= 0) continue;
            for (int c = 0; c < 4; ++c) d[c] += (s[c] - d[c]) * k;
            if (show) d[0] += (d[3] - d[0]) * 0.5f * k, d[2] *= 1 - 0.5f * k;  // (repaired: tinted red)
        }
    }
    return true;
}

bool damagedFramesInClip(const Project& p, const Sequence& s, const Clip& c, std::vector<DamagedFrame>& out,
                         const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    const MediaItem* m = p.findMedia(c.mediaId);
    if (!m || m->kind != MediaKind::Video || !m->fps.valid() || c.duration <= 0) {
        if (error) *error = "Not a video clip";
        return false;
    }
    // The file's frames the clip plays, whatever its speed or direction, and one either side.
    const double fps = s.fpsValue(), mf = m->fps.toDouble();
    double lo = 1e18, hi = -1e18;
    for (FrameTime f = 0; f <= c.duration; ++f) {
        const double at = c.sourceAt(double(f)) / fps;
        lo = std::min(lo, at), hi = std::max(hi, at);
    }
    const int64_t first = std::max<int64_t>(0, int64_t(std::floor(lo * mf)) - 1), last = int64_t(std::ceil(hi * mf)) + 2;
    std::string err;
    out = findDamagedFrames(m->path, first, last, progress, cancel, &err);
    if (!err.empty()) {
        if (error) *error = err;
        return false;
    }
    return true;
}

int listDamagedFrames(Project& p, Sequence& s, Clip& c, const std::vector<DamagedFrame>& frames) {
    const MediaItem* m = p.findMedia(c.mediaId);
    if (!m || !m->fps.valid()) return 0;
    const double fps = s.fpsValue(), mf = m->fps.toDouble();
    Effect* e = nullptr;
    for (Effect& x : c.effects)
        if (x.type == "frame_repair") e = &x;
    if (!e && frames.empty()) return 0;
    if (!e) {
        c.effects.insert(c.effects.begin(), makeEffect(p, "frame_repair"));
        e = &c.effects.front();
    }
    std::map<int64_t, FrameDamage> listed = damagedFramesFromString(e->s("frames"));
    int added = 0;
    for (const DamagedFrame& d : frames) added += listed.emplace(d.frame, d.kind).second ? 1 : 0;
    e->strings["frames"] = damagedFramesToString(listed);
    // Marked where the clip shows each one (Quality Check's red), those marked before over the clip replaced.
    std::erase_if(s.markers, [&](const Marker& mk) { return mk.name.rfind("Damaged: ", 0) == 0 && c.contains(mk.t); });
    for (const DamagedFrame& d : frames) {
        const auto [from, to] = timelineSpan(c, s, double(d.frame) / mf, double(d.frame + 1) / mf);
        if (from < 0) continue;
        const FrameTime t = std::clamp<FrameTime>(FrameTime(std::floor(from * fps + 1e-6)), c.start, c.end() - 1);
        s.markers.push_back(Marker{t, 0, "Damaged: " + frameDamageName(d.kind), "Frame " + std::to_string(d.frame) + " of the file", 11});
    }
    std::stable_sort(s.markers.begin(), s.markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
    return added;
}

int repairClip(Project& p, Sequence& s, Clip& c, std::vector<DamagedFrame>* found, const std::function<void(double)>& progress,
               const std::atomic<bool>* cancel, std::string* error) {
    std::vector<DamagedFrame> frames;
    if (!damagedFramesInClip(p, s, c, frames, progress, cancel, error)) return -1;
    if (found) *found = frames;
    return listDamagedFrames(p, s, c, frames);
}

}  // namespace montage
