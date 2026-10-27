#include "ShotGroups.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

#include "core/EditOps.h"
#include "media/Faces.h"
#include "render/ColorSpace.h"
#include "render/Compositor.h"

namespace montage {

namespace {

constexpr int kFrameW = 64, kFrameH = 36;
constexpr int kSamples = 5;  // frames read across the shot for its median

// A code value's light (the frame as decoded, read as sRGB-like video: what matters is that shots compare alike).
double codeToLinear(float v) {
    if (!std::isfinite(v)) return 0;
    const double x = std::clamp(double(v), 0.0, 1.0);
    return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
}

// The frame's L*a*b*, kFrameW x kFrameH x 3 (the picture fitted into it).
std::vector<float> labOf(const Image& img) {
    std::vector<float> out(size_t(kFrameW) * kFrameH * 3, 0.0f);
    if (img.empty()) return out;
    for (int y = 0; y < kFrameH; ++y)
        for (int x = 0; x < kFrameW; ++x) {
            const float* px = img.at(std::min(img.width - 1, x * img.width / kFrameW), std::min(img.height - 1, y * img.height / kFrameH));
            const double rgb[3] = {codeToLinear(px[0]), codeToLinear(px[1]), codeToLinear(px[2])};
            double lab[3];
            linearToLab(Primaries::Bt709, rgb, lab);
            float* o = &out[(size_t(y) * kFrameW + size_t(x)) * 3];
            for (int k = 0; k < 3; ++k) o[k] = float(lab[k]);
        }
    return out;
}

// Each channel's histogram (L* 0..100, a* and b* -64..64), each value shared between its two nearest bins.
std::array<float, 3 * kShotHistBins> histogramOf(const std::vector<float>& lab) {
    std::array<float, 3 * kShotHistBins> h{};
    const size_t n = lab.size() / 3;
    if (!n) return h;
    for (size_t i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            const double lo = k == 0 ? 0.0 : -64.0, hi = k == 0 ? 100.0 : 64.0;
            const double at = std::clamp((double(lab[i * 3 + size_t(k)]) - lo) / (hi - lo), 0.0, 1.0) * (kShotHistBins - 1);
            const int b = std::min(kShotHistBins - 2, int(at));
            const double f = at - b;
            h[size_t(k * kShotHistBins + b)] += float(1 - f);
            h[size_t(k * kShotHistBins + b + 1)] += float(f);
        }
    for (float& v : h) v /= float(n);
    return h;
}

// 1 minus the Bhattacharyya coefficient, averaged over the three channels.
double histogramDistance(const std::array<float, 3 * kShotHistBins>& a, const std::array<float, 3 * kShotHistBins>& b) {
    double bc = 0;
    for (size_t i = 0; i < a.size(); ++i) bc += std::sqrt(double(a[i]) * double(b[i]));
    return std::clamp(1 - bc / 3, 0.0, 1.0);
}

}  // namespace

bool keptOutOfGrouping(const Sequence& s, const Clip& c) {
    for (const ColorGroup* g : colorGroupsOf(s, c))
        if (g->kind.empty() || g->locked) return true;
    return false;
}

std::vector<ShotTraits> shotTraits(const Project& p, const Sequence& s, const std::vector<Id>& clips, bool visualIndex,
                                           const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    std::vector<const Clip*> shots;
    for (const Track& t : s.videoTracks)
        for (const Clip& c : t.clips) {
            const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
            if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image) || c.duration <= 0) continue;
            if (clips.empty() ? keptOutOfGrouping(s, c) : std::find(clips.begin(), clips.end(), c.id) == clips.end()) continue;
            shots.push_back(&c);
        }
    std::stable_sort(shots.begin(), shots.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
    const bool faces = faceSearchAvailable() && faceModel().installed();
    const double fps = s.fpsValue();
    std::vector<ShotTraits> out;
    for (size_t i = 0; i < shots.size(); ++i) {
        if (cancel && cancel->load()) return {};
        const Clip& c = *shots[i];
        const MediaItem& m = *p.findMedia(c.mediaId);
        ShotTraits d;
        d.clip = c.id;
        d.media = m.id;
        d.start = c.start;
        auto secondsAt = [&](double share) {
            const FrameTime t = c.start + std::clamp(FrameTime(std::floor(double(c.duration) * share)), FrameTime(0), c.duration - 1);
            return m.kind == MediaKind::Video ? std::max(0.0, c.sourceFrameAt(t) / fps) : 0.0;
        };
        d.sourceSeconds = secondsAt(0.5);
        // The middle frame, and each pixel's median over frames across the shot (a still: its one picture).
        std::vector<std::vector<float>> frames;
        Image middle;
        for (int k = 0; k < (m.kind == MediaKind::Video ? kSamples : 1); ++k) {
            const double share = m.kind == MediaKind::Video ? (k + 0.5) / kSamples : 0.5;
            Image img = renderMediaFrame(p, m, secondsAt(share), kFrameW * 2, kFrameH * 2);
            if (img.empty()) d.seen = false;
            frames.push_back(labOf(img));
            if (k == (m.kind == MediaKind::Video ? kSamples / 2 : 0)) middle = std::move(img);
        }
        const std::vector<float>& mid = frames[frames.size() / 2];
        std::vector<float> median(mid.size());
        std::vector<float> values(frames.size());
        for (size_t v = 0; v < mid.size(); ++v) {
            for (size_t f = 0; f < frames.size(); ++f) values[f] = frames[f][v];
            std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(values.size() / 2), values.end());
            median[v] = values[values.size() / 2];
        }
        d.histogram = histogramOf(mid);
        d.medianHistogram = histogramOf(median);
        // The layout: the median frame at half size.
        d.layout.assign(size_t(kShotLayoutW) * kShotLayoutH * 3, 0.0f);
        for (int y = 0; y < kShotLayoutH; ++y)
            for (int x = 0; x < kShotLayoutW; ++x)
                for (int k = 0; k < 3; ++k) {
                    double sum = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) sum += median[(size_t(y * 2 + dy) * kFrameW + size_t(x * 2 + dx)) * 3 + size_t(k)];
                    d.layout[(size_t(y) * kShotLayoutW + size_t(x)) * 3 + size_t(k)] = float(sum / 4);
                }
        for (const char* key : {"camera", "tape", "lens", "iso", "wb"})
            if (const auto it = m.metadata.find(key); it != m.metadata.end() && !it->second.empty()) d.camera[key] = it->second;
        if (const auto it = m.metadata.find("scene"); it != m.metadata.end()) d.scene = it->second;
        if (faces && !middle.empty()) {
            const Image big = renderMediaFrame(p, m, d.sourceSeconds, 640, 360);
            if (const auto found = cachedFaces(big)) {
                std::vector<FaceBox> list = *found;
                std::sort(list.begin(), list.end(), [](const FaceBox& a, const FaceBox& b) { return a.x < b.x; });
                d.faces = int(list.size());
                for (const FaceBox& f : list) d.facePlaces.insert(d.facePlaces.end(), {f.x + f.w / 2, f.y + f.h / 2, f.h});
            }
        }
        if (visualIndex && m.visual && !m.visual->samples.empty()) {
            // The sample nearest the middle frame.
            const auto& samples = m.visual->samples;
            const auto near = std::min_element(samples.begin(), samples.end(), [&](const auto& a, const auto& b) {
                return std::fabs(a.time - d.sourceSeconds) < std::fabs(b.time - d.sourceSeconds);
            });
            double norm = 0;
            for (int8_t v : near->values) {
                d.embedding.push_back(float(v) * near->scale);
                norm += double(d.embedding.back()) * d.embedding.back();
            }
            if (norm > 0) {
                for (float& v : d.embedding) v = float(v / std::sqrt(norm));
                d.embeddingModel = m.visual->model;
            } else {
                d.embedding.clear();
            }
        }
        out.push_back(std::move(d));
        if (progress) progress(double(i + 1) / double(shots.size()));
    }
    return out;
}

double shotDistance(const ShotTraits& a, const ShotTraits& b) {
    double sum = 0, weights = 0;
    auto add = [&](double d, double w) { sum += (std::isfinite(d) ? std::clamp(d, 0.0, 1.0) : 1.0) * w, weights += w; };
    // Colour: the middle frames' and the median frames' histograms (not for a shot whose frames could not be read).
    if (a.seen && b.seen)
        add((histogramDistance(a.histogram, b.histogram) + histogramDistance(a.medianHistogram, b.medianHistogram)) / 2, 1.0);
    // Layout: the mean L*a*b* difference of the thumbnails, 40 counting as wholly unalike.
    if (a.seen && b.seen && a.layout.size() == b.layout.size() && !a.layout.empty()) {
        double diff = 0;
        for (size_t i = 0; i < a.layout.size(); i += 3) {
            const double dl = a.layout[i] - b.layout[i], da = a.layout[i + 1] - b.layout[i + 1], db = a.layout[i + 2] - b.layout[i + 2];
            diff += std::sqrt(dl * dl + da * da + db * db);
        }
        add(diff / double(a.layout.size() / 3) / 40.0, 2.0);
    }
    // Camera metadata: another camera or lens is another angle; the same says less.
    for (const auto& [key, value] : a.camera)
        if (const auto it = b.camera.find(key); it != b.camera.end()) add(it->second == value ? 0.25 : 1.0, key == std::string("camera") ? 1.5 : 0.5);
    // The same file: the same set-up, near enough.
    if (a.media && a.media == b.media) add(0.0, 1.5);
    // Faces: how many, and where.
    if (a.faces >= 0 && b.faces >= 0) {
        double d = std::min(1.0, std::abs(a.faces - b.faces) / 2.0) * 0.5;
        if (a.faces == b.faces && a.faces > 0) {
            double place = 0;
            for (size_t i = 0; i < a.facePlaces.size() && i < b.facePlaces.size(); ++i) place += std::fabs(a.facePlaces[i] - b.facePlaces[i]);
            d += std::min(1.0, place / double(a.faces) * 2.0) * 0.5;
        }
        add(d, 0.75);
    }
    // What the visual index sees.
    if (!a.embedding.empty() && a.embedding.size() == b.embedding.size() && a.embeddingModel == b.embeddingModel) {
        double dot = 0;
        for (size_t i = 0; i < a.embedding.size(); ++i) dot += double(a.embedding[i]) * b.embedding[i];
        add((1 - dot) * 2.5, 1.5);
    }
    return weights > 0 ? sum / weights : 1.0;
}

std::vector<int> clusterShots(const std::vector<std::vector<double>>& distance, double threshold) {
    const size_t n = distance.size();
    std::vector<int> label(n, -1);
    if (n == 0) return label;
    // Average linkage by the nearest-neighbour chain (exact for it: the merges are those of the greedy algorithm).
    std::vector<std::vector<double>> d = distance;
    for (auto& row : d)
        for (double& v : row)
            if (!std::isfinite(v)) v = 1;  // (unknown: unalike)
    std::vector<size_t> size(n, 1);
    std::vector<char> active(n, 1);
    struct Merge {
        double height;
        size_t a, b;
    };
    std::vector<Merge> merges;
    std::vector<size_t> chain;
    size_t left = n;
    while (left > 1) {
        if (chain.empty())
            for (size_t i = 0; i < n; ++i)
                if (active[i]) {
                    chain.push_back(i);
                    break;
                }
        const size_t a = chain.back();
        const size_t prev = chain.size() > 1 ? chain[chain.size() - 2] : n;
        // a's nearest (the previous one in the chain on a tie, so the chain ends).
        size_t b = prev;
        double best = prev < n ? d[a][prev] : INFINITY;
        for (size_t j = 0; j < n; ++j)
            if (active[j] && j != a && d[a][j] < best) best = d[a][j], b = j;
        if (b == prev) {
            chain.pop_back();
            chain.pop_back();
            const size_t keep = std::min(a, b), gone = std::max(a, b);
            merges.push_back({best, keep, gone});
            for (size_t j = 0; j < n; ++j)
                if (active[j] && j != keep && j != gone) {
                    const double v = (d[keep][j] * double(size[keep]) + d[gone][j] * double(size[gone])) / double(size[keep] + size[gone]);
                    d[keep][j] = d[j][keep] = v;
                }
            size[keep] += size[gone];
            active[gone] = 0;
            --left;
        } else {
            chain.push_back(b);
        }
    }
    // The merges at or under the threshold, lowest first.
    std::stable_sort(merges.begin(), merges.end(), [](const Merge& x, const Merge& y) { return x.height < y.height; });
    std::vector<size_t> parent(n);
    std::iota(parent.begin(), parent.end(), size_t(0));
    std::function<size_t(size_t)> root = [&](size_t i) { return parent[i] == i ? i : parent[i] = root(parent[i]); };
    for (const Merge& m : merges) {
        if (m.height > threshold) break;
        parent[root(m.b)] = root(m.a);
    }
    std::map<size_t, int> numbers;
    for (size_t i = 0; i < n; ++i) {
        const auto [it, added] = numbers.try_emplace(root(i), int(numbers.size()));
        label[i] = it->second;
    }
    return label;
}

double groupingThreshold(double sensitivity) {
    const double s = std::clamp(sensitivity, 0.0, 1.0);
    // (0.4 at the middle: shots of one set-up with someone moving through them come out about 0.2 to 0.45 apart)
    return 0.1 + 0.6 * (1 - s);
}

std::vector<int> scenesOfAngles(const std::vector<int>& cut, int angles) {
    std::vector<int> parent(size_t(std::max(0, angles)));
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> root = [&](int i) { return parent[size_t(i)] == i ? i : parent[size_t(i)] = root(parent[size_t(i)]); };
    auto join = [&](int a, int b) { parent[size_t(root(b))] = root(a); };
    // Cuts between each pair of angles (either way).
    std::map<std::pair<int, int>, int> between;
    for (size_t i = 1; i < cut.size(); ++i)
        if (cut[i] != cut[i - 1]) ++between[{std::min(cut[i], cut[i - 1]), std::max(cut[i], cut[i - 1])}];
    for (const auto& [pair, n] : between)
        if (n >= 2) join(pair.first, pair.second);
    // An angle seen in one run only, between two shots of one scene (an insert), joins that scene.
    std::vector<int> runs(parent.size(), 0);
    for (size_t i = 0; i < cut.size(); ++i)
        if (i == 0 || cut[i] != cut[i - 1]) ++runs[size_t(cut[i])];
    for (size_t i = 1; i + 1 < cut.size(); ++i) {
        const int a = cut[i];
        if (runs[size_t(a)] != 1 || cut[i - 1] == a) continue;
        size_t j = i;
        while (j + 1 < cut.size() && cut[j + 1] == a) ++j;
        if (j + 1 < cut.size() && root(cut[i - 1]) == root(cut[j + 1]) && root(a) == a) {
            bool alone = true;
            for (int k = 0; k < angles; ++k) alone = alone && (k == a || root(k) != a);
            if (alone) join(cut[i - 1], a);
        }
    }
    // Numbered by first appearance in the cut.
    std::vector<int> scene(parent.size(), -1);
    std::map<int, int> numbers;
    for (int a : cut) {
        const auto [it, added] = numbers.try_emplace(root(a), int(numbers.size()));
        scene[size_t(a)] = it->second;
    }
    for (int a = 0; a < angles; ++a)
        if (scene[size_t(a)] < 0) {
            const auto [it, added] = numbers.try_emplace(root(a), int(numbers.size()));
            scene[size_t(a)] = it->second;
        }
    return scene;
}

double adjustedRandIndex(const std::vector<int>& a, const std::vector<int>& b) {
    const size_t n = std::min(a.size(), b.size());
    if (n < 2) return 1.0;
    std::map<std::pair<int, int>, double> both;
    std::map<int, double> rows, cols;
    for (size_t i = 0; i < n; ++i) ++both[{a[i], b[i]}], ++rows[a[i]], ++cols[b[i]];
    auto pairs = [](double x) { return x * (x - 1) / 2; };
    double index = 0, sa = 0, sb = 0;
    for (const auto& [k, v] : both) index += pairs(v);
    for (const auto& [k, v] : rows) sa += pairs(v);
    for (const auto& [k, v] : cols) sb += pairs(v);
    const double expected = sa * sb / pairs(double(n)), most = (sa + sb) / 2;
    return most == expected ? 1.0 : (index - expected) / (most - expected);
}

std::vector<std::pair<FrameTime, Id>> keptShots(const Sequence& s) {
    std::vector<std::pair<FrameTime, Id>> out;
    for (const Track& t : s.videoTracks)
        for (const Clip& c : t.clips)
            if (const ColorGroup* g = colorGroupOf(s, c); g && g->kind == "angle" && keptOutOfGrouping(s, c)) out.push_back({c.start, g->id});
    std::sort(out.begin(), out.end());
    return out;
}

ShotGrouping planShotGroups(const std::vector<ShotTraits>& shots, const GroupShotsOptions& o, const std::vector<std::pair<FrameTime, Id>>& kept,
                            const Sequence* s) {
    ShotGrouping g;
    const size_t n = shots.size();
    if (!n) return g;
    std::vector<std::vector<double>> distance(n, std::vector<double>(n, 0.0));
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) distance[i][j] = distance[j][i] = shotDistance(shots[i], shots[j]);
    // (the shots are in timeline order, so the angles are numbered as they first appear)
    const std::vector<int> angle = clusterShots(distance, groupingThreshold(o.sensitivity));
    const int angles = n ? *std::max_element(angle.begin(), angle.end()) + 1 : 0;
    g.angles.resize(size_t(angles));
    for (size_t i = 0; i < n; ++i) g.angles[size_t(angle[i])].clips.push_back(shots[i].clip);
    if (!o.scenes) return g;
    // The media's own scenes when every shot has one (each angle in its shots' commonest), else from the cut.
    if (std::all_of(shots.begin(), shots.end(), [](const ShotTraits& d) { return !d.scene.empty(); })) {
        std::map<std::string, int> sceneNumbers;
        for (const ShotTraits& d : shots)
            if (sceneNumbers.try_emplace(d.scene, int(sceneNumbers.size())).second) g.scenes.push_back("Scene " + d.scene);
        for (int a = 0; a < angles; ++a) {
            std::map<int, int> votes;
            for (size_t i = 0; i < n; ++i)
                if (angle[i] == a) ++votes[sceneNumbers[shots[i].scene]];
            g.angles[size_t(a)].scene = std::max_element(votes.begin(), votes.end(), [](const auto& x, const auto& y) { return x.second < y.second; })->first;
        }
        return g;
    }
    // The kept shots take part as angles of their own (their groups), in their places in the cut.
    std::vector<std::pair<FrameTime, int>> cut;
    for (size_t i = 0; i < n; ++i) cut.push_back({shots[i].start, angle[i]});
    std::map<Id, int> keptAngles;
    for (const auto& [start, group] : kept) {
        const auto [it, added] = keptAngles.try_emplace(group, angles + int(keptAngles.size()));
        cut.push_back({start, it->second});
    }
    std::stable_sort(cut.begin(), cut.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    std::vector<int> order;
    for (const auto& [start, a] : cut) order.push_back(a);
    const std::vector<int> scene = scenesOfAngles(order, angles + int(keptAngles.size()));
    // Scenes numbered by the new angles' first appearance; a scene of kept groups only is no scene of theirs.
    std::map<int, int> numbers;
    for (int a : order)
        if (a < angles) numbers.try_emplace(scene[size_t(a)], int(numbers.size()));
    g.scenes.assign(numbers.size(), std::string());
    g.sceneGroups.assign(numbers.size(), 0);
    for (int a = 0; a < angles; ++a) g.angles[size_t(a)].scene = numbers.at(scene[size_t(a)]);
    if (s)
        for (const auto& [group, a] : keptAngles) {
            const auto it = numbers.find(scene[size_t(a)]);
            const ColorGroup* cg = findColorGroup(*s, group);
            const ColorGroup* around = cg ? findColorGroup(*s, cg->parent) : nullptr;
            if (it != numbers.end() && around && around->kind == "scene" && !g.sceneGroups[size_t(it->second)]) g.sceneGroups[size_t(it->second)] = around->id;
        }
    return g;
}

}  // namespace montage
