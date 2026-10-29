#include "CompositeRender.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "GradeGraph.h"
#include "Processing.h"
#include "RenderCache.h"
#include "core/CompositeGraph.h"
#include "core/Effects.h"
#include "core/Expression.h"
#include "core/GradeGraph.h"
#include "core/ProjectIO.h"
#include "media/Faces.h"
#include "media/Matting.h"
#include "media/VideoDepth.h"

namespace montage {

namespace {

struct Out {
    Image img;
    std::shared_ptr<const DeepFrame> deep;  // the picture as deep samples, when it is deep (img its flattening)
    bool displayReferred = false;           // (graded scene-referred: mixed as the display shows it)
};
using OutPtr = std::shared_ptr<const Out>;

// The kept results, by key, the oldest dropped past the budget.
struct Kept {
    std::mutex m;
    struct Entry {
        OutPtr out;
        size_t bytes = 0;
        uint64_t used = 0;
    };
    std::unordered_map<std::string, Entry> entries;
    size_t bytes = 0;
    uint64_t clock = 0;
    std::map<Id, uint64_t> renders;
    uint64_t rendered = 0, reused = 0;
};
Kept& kept() {
    static Kept k;
    return k;
}
constexpr size_t kBudget = size_t(1) << 30;

size_t bytesOf(const Out& o) { return o.img.px.size() * sizeof(float) + (o.deep ? o.deep->bytes() : 0); }

OutPtr find(const std::string& key) {
    if (key.empty()) return nullptr;
    Kept& k = kept();
    std::lock_guard lock(k.m);
    auto it = k.entries.find(key);
    if (it == k.entries.end()) return nullptr;
    it->second.used = ++k.clock;
    ++k.reused;
    return it->second.out;
}

void keep(const std::string& key, Id node, const OutPtr& out, const RenderOptions& o) {
    Kept& k = kept();
    std::lock_guard lock(k.m);
    ++k.rendered;
    ++k.renders[node];
    // (A frame not rendered in full, or nothing to know it again by, is not kept.)
    if (key.empty() || (o.issues && o.issues->incomplete) || (o.cancel && *o.cancel)) return;
    const size_t bytes = bytesOf(*out);
    if (bytes > kBudget / 4) return;
    if (auto it = k.entries.find(key); it != k.entries.end()) {
        k.bytes -= it->second.bytes;
        k.entries.erase(it);
    }
    while (k.bytes + bytes > kBudget && !k.entries.empty()) {
        auto oldest = std::min_element(k.entries.begin(), k.entries.end(),
                                       [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        k.bytes -= oldest->second.bytes;
        k.entries.erase(oldest);
    }
    k.entries[key] = {out, bytes, ++k.clock};
    k.bytes += bytes;
}

std::string hashed(const QByteArray& data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex().toStdString(); }

// The node's settings as they decide its picture (not where it is drawn, its name or its ids).
QByteArray nodeSettings(const CompositeNode& n) {
    CompositeGraph one;
    CompositeNode k = n;
    k.key = 1;
    k.id = 0;
    k.x = k.y = 0;
    k.label.clear();
    k.generator.id = 0;
    for (Effect& e : k.effects) e.id = 0;
    one.nodes.push_back(std::move(k));
    return QByteArray::fromStdString(compositeGraphToJsonString(one));
}

// A clip for an input's media or generator: placed as a new clip would be, over the graph's clip's span.
Clip inputClip(const Clip& c, const CompositeNode& n) {
    Clip tc;
    tc.id = n.id;
    tc.start = c.start;
    tc.duration = c.duration;
    tc.motion = makeEffect("transform", 0);
    tc.timing = makeEffect("time", 0);
    if (n.source == "media") {
        tc.mediaId = n.media;
        tc.sourceIn = n.offset;
    } else {
        tc.generator = n.generator;
    }
    return tc;
}

// The clip a track shows at t (null when none or a transition runs there: then its frame is not kept).
const Clip* trackClipAt(const Sequence& seq, int track, FrameTime t, bool& transition) {
    transition = false;
    if (track < 0 || track >= int(seq.videoTracks.size())) return nullptr;
    const Track& tr = seq.videoTracks[size_t(track)];
    for (const Transition& x : tr.transitions) {
        FrameTime a, b;
        if (edit::transitionRange(tr, x, a, b) && t >= a && t < b) transition = true;
    }
    for (const Clip& c : tr.clips)
        if (c.enabled && c.contains(t)) return &c;
    return nullptr;
}

struct Renderer {
    const Project& p;
    const Sequence& seq;
    const Clip& c;
    FrameTime t, lt;
    const RenderOptions& o;
    const CompositeHost& host;
    int W, H;
    bool cacheable = true;
    std::map<int, std::string> keys;
    std::map<int, OutPtr> done;  // (this frame's, so a node feeding two others is drawn once)
    std::set<int> keying, rendering;  // (a loop a project's checks missed ends here rather than in the stack running out)

    std::vector<const CompositeLink*> inputsOf(int key) const {
        std::vector<const CompositeLink*> in;
        for (const CompositeLink& l : c.composite.links)
            if (l.to == key) in.push_back(&l);
        std::sort(in.begin(), in.end(), [](const CompositeLink* a, const CompositeLink* b) { return a->input < b->input; });
        return in;
    }

    // What decides the node's picture; "" when it cannot be known again (it is then drawn each time).
    std::string keyOf(int nodeKey) {
        if (auto it = keys.find(nodeKey); it != keys.end()) return it->second;
        if (!keying.insert(nodeKey).second) return {};
        const CompositeNode* n = findCompositeNode(c.composite, nodeKey);
        std::string key;
        if (n && cacheable) {
            QByteArray data("montage-composite-1|");
            data += QByteArray::number(o.scale, 'g', 10) + (o.useProxies ? "P" : "") + (o.highQuality ? "H" : "") + '#' +
                    QByteArray::fromStdString(o.displaySpace + '|' + o.outputSpace) + QByteArray::number(o.outputPeak) +
                    (o.sceneLight ? "|light" : "") + '|' + QByteArray::number(o.eye);
            data += QByteArray::number(seq.width) + 'x' + QByteArray::number(seq.height) + '@' + QByteArray::number(seq.fpsValue(), 'g', 10);
            data += QByteArray::fromStdString('|' + seq.colorSpace + '|' + seq.workingSpace + '|' + seq.outputTransform) +
                    QByteArray::number(seq.hdrPeakNits);
            data += "|lt" + QByteArray::number(qint64(lt)) + '|' + nodeSettings(*n);
            bool known = true;
            if (n->enabled && n->kind == "input") {
                if (n->source == "clip") {
                    Clip plain = c;
                    plain.composite = {};
                    data += "|clip" + clipLayerKey(p, seq, plain, t, o);
                } else if (n->source == "track") {
                    bool transition = false;
                    const Clip* tc = trackClipAt(seq, n->track, t, transition);
                    if (transition) known = false;
                    else if (tc) {
                        data += "|track" + clipLayerKey(p, seq, *tc, t, o);
                        // A clip with a graph of its own draws differently with which graphs are already being drawn
                        // (each is drawn plain inside itself), which its key cannot tell: not kept.
                        if (!tc->composite.empty()) known = false;
                    }
                } else {
                    data += "|input" + clipLayerKey(p, seq, inputClip(c, *n), t, o);
                }
            } else if (n->enabled && n->kind == "track") {
                if (const Clip* f = edit::clipById(seq, n->follow)) {
                    Clip k = *f;
                    k.id = 0;
                    data += "|follow" + QByteArray::fromStdString(clipToJsonString(k)) + QByteArray::number(qint64(t - f->start));
                }
            } else if (n->enabled && n->kind == "grade") {
                for (const GradeNode& g : n->grade.nodes)
                    if (const SharedNode* sn = findSharedNode(seq, g.shared))
                        for (Effect e : sn->effects) {
                            e.id = 0;
                            data += "|shared" + QByteArray::fromStdString(effectToJsonString(e));
                        }
            }
            for (const CompositeLink* l : inputsOf(nodeKey)) {
                const std::string k = keyOf(l->from);
                if (k.empty()) known = false;
                data += "|in" + QByteArray::number(l->input) + ':' + QByteArray::fromStdString(k);
            }
            if (known) key = hashed(data);
        }
        keying.erase(nodeKey);
        keys[nodeKey] = key;
        return key;
    }

    Image blank() const { return Image(W, H); }

    // A picture brought to the frame's size (an input with no picture: transparent).
    Image fitted(Image img) const {
        if (img.width == W && img.height == H) return img;
        return blank();
    }

    // Effects run on a picture of the frame, with what they need found in it (its depth, people, faces).
    void runEffects(Image& img, const std::vector<const Effect*>& fx) const {
        if (fx.empty() || img.empty()) return;
        const SceneGrading* sg = currentSceneGrading();
        std::optional<WorkingSpaceScope> working;  // (graded scene-referred, layers are in the light they are mixed in)
        if (sg) working.emplace(sg->linear);
        auto any = [&](const std::function<bool(const Effect&)>& f) { return std::any_of(fx.begin(), fx.end(), [&](const Effect* e) { return f(*e); }); };
        std::shared_ptr<const DepthMap> depth;
        if (any([&](const Effect& e) { return needsDepth(e, lt); })) depth = frameDepth(host.shown(img), std::string(), -1);
        DepthScope depthScope(depth);
        std::shared_ptr<const ValueMap> people;
        if (any([&](const Effect& e) { return needsPersonMatte(e, lt); }) && mattingAvailable() && mattingModel().installed())
            people = cachedPersonMatte(host.shown(img));
        PersonScope personScope(people);
        std::shared_ptr<const std::vector<FaceBox>> faces;
        if (any([&](const Effect& e) { return needsFaces(e); }) && faceSearchAvailable() && faceModel().installed())
            faces = cachedFaces(host.shown(img));
        FaceScope faceScope(faces);
        SlowEffectsScope slow({o.highQuality || o.slowEffects, o.cancel, o.issues});
        for (const Effect* e : fx) applyVideoEffect(*e, lt, img, o.scale);
    }
    void runEffects(Image& img, const std::vector<Effect>& fx, size_t from = 0) const {
        std::vector<const Effect*> list;
        for (size_t i = from; i < fx.size(); ++i)
            if (fx[i].enabled) list.push_back(&fx[i]);
        runEffects(img, list);
    }

    bool timed(Id media) const {
        const MediaItem* m = p.findMedia(media);
        return m && m->kind != MediaKind::Image;
    }

    OutPtr input(const CompositeNode& n) {
        auto out = std::make_shared<Out>();
        if (n.source == "track") {
            out->img = fitted(host.track(n.track, &out->displayReferred));
        } else if (n.source == "media" && n.offset < 0 && double(lt) < -n.offset && timed(n.media)) {
            out->img = blank();  // (before its media's first frame: the clip's head was drawn out past it; a still has none)
        } else {
            const Clip tc = n.source == "clip" ? Clip() : inputClip(c, n);
            const Clip& shown = n.source == "clip" ? c : tc;
            const bool model = shown.isGenerator() && shown.generator.type == "model3d";
            if (model || host.isDeep(shown)) {
                auto deep = std::make_shared<DeepFrame>(host.deep(shown));
                if (deep->width == W && deep->height == H) {
                    out->img = Image(W, H, Image::Uninitialized{});
                    const std::vector<float> rgba = flattenDeep(*deep);
                    std::copy(rgba.begin(), rgba.end(), out->img.px.begin());
                    out->deep = std::move(deep);
                } else {
                    out->img = blank();
                }
            } else {
                out->img = fitted(host.layer(shown, &out->displayReferred));
            }
        }
        if (std::any_of(n.effects.begin(), n.effects.end(), [](const Effect& e) { return e.enabled; })) {
            out->deep.reset();
            runEffects(out->img, n.effects);
        }
        return out;
    }

    OutPtr merge(const CompositeNode& n, const std::vector<std::pair<int, OutPtr>>& in) {
        auto out = std::make_shared<Out>();
        const Effect* settings = compositeSettings(n);
        const float opacity = float(std::clamp((settings ? settings->p("opacity", lt, 100) : 100) / 100.0, 0.0, 1.0));
        if (n.blend == "depth") {
            // Every input by depth: deep ones as they are, flat ones behind everything.
            std::vector<DeepFrame> made;
            std::vector<const DeepFrame*> layers;
            made.reserve(in.size());
            for (const auto& [input, x] : in) {
                if (!x) continue;
                if (x->deep) {
                    layers.push_back(x->deep.get());
                    continue;
                }
                const std::vector<float> far(size_t(W) * size_t(H), 1e9f);
                made.push_back(deepFromFlat(x->img.px.data(), W, H, far.data(), 0));
                layers.push_back(&made.back());
            }
            if (layers.empty()) {
                out->img = blank();
                return out;
            }
            auto merged = std::make_shared<DeepFrame>(mergeDeep(layers));
            out->img = Image(W, H, Image::Uninitialized{});
            const std::vector<float> rgba = flattenDeep(*merged);
            std::copy(rgba.begin(), rgba.end(), out->img.px.begin());
            out->deep = std::move(merged);
            return out;
        }
        const Out* background = !in.empty() && in.front().first == 0 ? in.front().second.get() : nullptr;
        out->img = background ? background->img : blank();
        out->displayReferred = background && background->displayReferred;
        for (const auto& [input, x] : in)
            if (input > 0 && x && opacity > 0) host.blend(out->img, x->img, n.blend, opacity, x->displayReferred);
        return out;
    }

    OutPtr mask(const CompositeNode& n, const Out* picture, const Out* matte) {
        auto out = std::make_shared<Out>();
        out->img = picture ? picture->img : blank();
        out->displayReferred = picture && picture->displayReferred;
        const Effect* e = compositeSettings(n);
        std::vector<float> m(size_t(W) * size_t(H), 1.0f);
        if (matte) {
            const int how = e ? int(std::lround(e->p("matte", lt, 0))) : 0;
            const bool luma = how == 1 || how == 3, invert = how >= 2;
            const Image shown = luma ? host.shown(matte->img) : Image();
            const Image& from = luma ? shown : matte->img;
            for (size_t i = 0; i < m.size() && i * 4 + 3 < from.px.size(); ++i) {
                const float* q = &from.px[i * 4];
                const float v = std::clamp(luma ? 0.2126f * q[0] + 0.7152f * q[1] + 0.0722f * q[2] : q[3], 0.0f, 1.0f);
                m[i] = invert ? 1 - v : v;
            }
        }
        if (e && e->enabled && hasMask(*e, lt)) {
            std::shared_ptr<const DepthMap> depth;
            if (needsDepth(*e, lt)) depth = frameDepth(host.shown(out->img), std::string(), -1);
            DepthScope depthScope(depth);
            std::shared_ptr<const ValueMap> people;
            if (needsPersonMatte(*e, lt) && mattingAvailable() && mattingModel().installed()) people = cachedPersonMatte(host.shown(out->img));
            PersonScope personScope(people);
            const std::vector<float> shape = effectMatte(*e, lt, out->img, o.scale);
            for (size_t i = 0; i < m.size() && i < shape.size(); ++i) m[i] *= shape[i];
        }
        for (size_t i = 0; i < m.size() && i * 4 + 3 < out->img.px.size(); ++i)
            for (int k = 0; k < 4; ++k) out->img.px[i * 4 + size_t(k)] *= m[i];
        return out;
    }

    OutPtr track(const CompositeNode& n, const Out* picture) {
        auto out = std::make_shared<Out>();
        out->img = picture ? picture->img : blank();
        out->displayReferred = picture && picture->displayReferred;
        const Clip* f = edit::clipById(seq, n.follow);
        const Effect* e = compositeSettings(n);
        if (!f || !e || out->img.empty()) return out;
        const int follow = int(std::lround(e->p("follow", lt, 2)));
        if (follow == 3) {
            // Through the followed clip's Corner Pin, at that clip's own time.
            for (const Effect& cp : f->effects)
                if (cp.enabled && cp.type == "corner_pin") {
                    applyVideoEffect(cp, t - f->start, out->img, o.scale);
                    break;
                }
            return out;
        }
        CanvasMove now, then;
        const FrameTime ref = c.start + FrameTime(std::lround(e->p("reference", lt, 0)));
        if (!host.placement(*f, t, now) || !host.placement(*f, ref, then)) return out;
        CanvasMove m;
        // Where the followed anchor was lands where it is now, turned and scaled about it as the clip has been since.
        m.ax = then.px, m.ay = then.py;
        m.px = now.px, m.py = now.py;
        if (follow >= 1) {
            m.sx = then.sx != 0 ? now.sx / then.sx : 1;
            m.sy = then.sy != 0 ? now.sy / then.sy : 1;
        }
        if (follow >= 2) m.rot = now.rot - then.rot;
        out->img = host.move(out->img, m);
        return out;
    }

    OutPtr grade(const CompositeNode& n, const Out* picture) {
        auto out = std::make_shared<Out>();
        out->img = picture ? picture->img : blank();
        out->displayReferred = picture && picture->displayReferred;
        if (n.grade.empty()) {
            runEffects(out->img, n.effects);
            return out;
        }
        // As a clip's grade with colour nodes runs: its effects outside the nodes in order, the nodes where the first
        // of theirs is.
        Clip graded;
        graded.id = n.id;
        graded.start = c.start;
        graded.duration = c.duration;
        graded.effects = n.effects;
        graded.grade = n.grade;
        const SceneGrading* sg = currentSceneGrading();
        std::optional<WorkingSpaceScope> working;
        if (sg) working.emplace(sg->linear);
        SlowEffectsScope slow({o.highQuality || o.slowEffects, o.cancel, o.issues});
        bool ran = false;
        auto runGraph = [&] {
            if (!ran) applyGradeGraph(seq, graded, lt, out->img, o.scale, -1);
            ran = true;
        };
        for (const Effect& e : graded.effects) {
            if (e.node) runGraph();
            else if (e.enabled) runEffects(out->img, std::vector<const Effect*>{&e});
        }
        runGraph();
        return out;
    }

    OutPtr render(int key) {
        if (auto it = done.find(key); it != done.end()) return it->second;
        const CompositeNode* n = findCompositeNode(c.composite, key);
        if (!n || rendering.count(key)) return nullptr;
        const std::string k = keyOf(key);
        if (OutPtr found = find(k)) return done[key] = found;
        std::vector<std::pair<int, OutPtr>> in;
        rendering.insert(key);
        for (const CompositeLink* l : inputsOf(key)) in.emplace_back(l->input, render(l->from));
        rendering.erase(key);
        auto inputAt = [&](int input) -> const Out* {
            for (const auto& [i, x] : in)
                if (i == input) return x.get();
            return nullptr;
        };
        OutPtr out;
        if (!n->enabled) {
            if (const Out* first = inputAt(0)) out = std::make_shared<Out>(*first);
            else {
                auto empty = std::make_shared<Out>();
                empty->img = blank();
                out = empty;
            }
        } else if (n->kind == "input") {
            out = input(*n);
        } else if (n->kind == "merge") {
            out = merge(*n, in);
        } else if (n->kind == "mask") {
            out = mask(*n, inputAt(0), inputAt(1));
        } else if (n->kind == "track") {
            out = track(*n, inputAt(0));
        } else if (n->kind == "grade") {
            out = grade(*n, inputAt(0));
        } else {
            auto x = std::make_shared<Out>();
            const Out* first = inputAt(0);
            x->img = first ? first->img : blank();
            x->displayReferred = first && first->displayReferred;
            if (n->kind == "transform") {
                if (const Effect* e = compositeSettings(*n)) x->img = host.transform(x->img, *e, lt);
                runEffects(x->img, n->effects, 1);
            } else {
                runEffects(x->img, n->effects);  // (a key's keyers, an effect node's effects)
            }
            out = x;
        }
        keep(k, n->id, out, o);
        return done[key] = out;
    }
};

}  // namespace

Image renderCompositeGraph(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, const RenderOptions& o,
                           const CompositeHost& host, bool* displayReferred) {
    const CompositeLink* last = compositeLinkInto(c.composite, 0, 0);
    if (!last) return {};
    const int W = std::max(1, int(std::lround(seq.width * o.scale))), H = std::max(1, int(std::lround(seq.height * o.scale)));
    Renderer r{p, seq, c, t, t - c.start, o, host, W, H};
    r.cacheable = !usesExpressions(p);  // (what they give can depend on what the keys do not cover)
    OutPtr out = r.render(last->from);
    if (!out) return {};
    if (displayReferred) *displayReferred = out->displayReferred;
    return out->img;
}

CompositeCacheStats compositeCacheStats() {
    Kept& k = kept();
    std::lock_guard lock(k.m);
    return {k.rendered, k.reused};
}

uint64_t compositeNodeRenders(Id node) {
    Kept& k = kept();
    std::lock_guard lock(k.m);
    auto it = k.renders.find(node);
    return it == k.renders.end() ? 0 : it->second;
}

void clearCompositeCache() {
    Kept& k = kept();
    std::lock_guard lock(k.m);
    k.entries.clear();
    k.renders.clear();
    k.bytes = 0;
    k.rendered = k.reused = 0;
}

}  // namespace montage
