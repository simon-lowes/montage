#include "GradeGraph.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "LutExport.h"
#include "Processing.h"
#include "core/Effects.h"
#include "core/GradeGraph.h"

namespace montage {

namespace {

// What leaves a node: its picture and its key (empty: everywhere, 1).
struct Stream {
    Image pic;
    std::vector<float> key;
    bool hasPic = false;
};

// A key's value at pixel i (empty: 1).
inline float keyAt(const std::vector<float>& k, size_t i) { return k.empty() ? 1.0f : k[i]; }

std::string effectName(const Effect& e) {
    const EffectInfo* info = findEffectInfo(e.type);
    return info ? info->displayName : e.type;
}

}  // namespace

void applyGradeGraph(const Sequence& s, const Clip& c, FrameTime lt, Image& img, double pixelScale, double sourceSeconds,
                     const std::function<bool(const Effect&)>& skip, bool colourOnly, std::vector<std::string>* skipped) {
    const GradeGraph& g = c.grade;
    if (g.empty() || img.empty() || !validGrade(g)) return;
    const size_t N = size_t(img.width) * size_t(img.height);
    auto runEffects = [&](int key, Image& pic) {
        for (const Effect* e : gradeNodeEffects(s, c, key)) {
            if (!e->enabled || (skip && skip(*e))) continue;
            if (colourOnly && !isColorOnlyEffect(*e, lt)) {
                if (skipped) skipped->push_back(effectName(*e));
                continue;
            }
            applyVideoEffect(*e, lt, pic, pixelScale, sourceSeconds);
        }
    };
    auto nodeMasked = [&](int key) {
        const Effect* st = gradeNodeSettings(c, key);
        return st && hasMask(*st, lt);
    };
    // A plain chain with nothing limiting its nodes: each node's effects in turn, in place.
    if (serialGrade(g)) {
        bool plain = true;
        for (const GradeNode& n : g.nodes) {
            const Effect* st = gradeNodeSettings(c, n.key);
            if (st && st->enabled && (hasMask(*st, lt) || st->p("mix", lt, 100) < 100)) plain = false;
        }
        if (plain) {
            for (int key : gradeOrder(g))
                if (const GradeNode* n = findGradeNode(g, key); n && n->enabled) runEffects(key, img);
            return;
        }
    }
    const Image source = img;
    std::map<int, Stream> out;
    auto pictureFrom = [&](int from) -> const Image& {
        if (from) {
            auto it = out.find(from);
            if (it != out.end() && it->second.hasPic) return it->second.pic;
        }
        return source;
    };
    auto keyFrom = [&](const GradeLink* l) -> const std::vector<float>* {
        if (!l || !l->from) return nullptr;
        auto it = out.find(l->from);
        return it == out.end() ? nullptr : &it->second.key;
    };
    // A mixer's inputs in order.
    auto inputsOf = [&](int key) {
        std::vector<const GradeLink*> ins;
        for (const GradeLink& l : g.links)
            if (l.to == key) ins.push_back(&l);
        std::sort(ins.begin(), ins.end(), [](const GradeLink* a, const GradeLink* b) { return a->input < b->input; });
        return ins;
    };
    for (int key : gradeOrder(g)) {
        const GradeNode* n = findGradeNode(g, key);
        if (!n) continue;
        Stream st;
        if (n->kind == "corrector") {
            const GradeLink* in = gradeLinkInto(g, key, 0);
            st.pic = pictureFrom(in ? in->from : 0);
            st.hasPic = true;
            if (!n->enabled) {
                out[key] = std::move(st);
                continue;
            }
            const Effect* settings = gradeNodeSettings(c, key);
            const bool on = settings && settings->enabled;
            if (colourOnly && on && (settings->p("mask.shape", lt) > 0.5 || settings->p("mask.depth", lt) > 0.5)) {
                // (a window or depth is about where a colour is, which a LUT cannot hold)
                if (skipped) skipped->push_back(gradeNodeName(*n));
                out[key] = std::move(st);
                continue;
            }
            // Its matte: its own window, qualifier and depth, joined with its key input, by its mix.
            std::vector<float> m = on && nodeMasked(key) ? effectMatte(*settings, lt, st.pic, pixelScale, sourceSeconds) : std::vector<float>{};
            if (const GradeLink* kl = gradeLinkInto(g, key, 1)) {
                const std::vector<float>* k = keyFrom(kl);
                const float gain = on ? float(std::clamp(settings->p("key_gain", lt, 100) / 100, 0.0, 1.0)) : 1.0f;
                const bool invert = on && settings->p("key_invert", lt) > 0.5;
                const int mode = on ? int(std::lround(settings->p("key_mode", lt))) : 0;
                std::vector<float> joined(N);
                for (size_t i = 0; i < N; ++i) {
                    float kin = k ? keyAt(*k, i) : 1.0f;
                    if (invert) kin = 1 - kin;
                    kin *= gain;
                    const float own = keyAt(m, i);
                    joined[i] = mode == 1 ? std::min(1.0f, own + kin) : mode == 2 ? std::max(0.0f, (m.empty() ? 1.0f : own) - kin)
                              : mode == 3 ? kin : own * kin;
                }
                m = std::move(joined);
            }
            const double mix = on ? std::clamp(settings->p("mix", lt, 100) / 100, 0.0, 1.0) : 1.0;
            if (mix < 1) {
                if (m.empty()) m.assign(N, 1.0f);
                for (float& v : m) v *= float(mix);
            }
            if (on && settings->p("mask.show", lt) > 0.5) {
                // The node's matte as grey, as an effect's Show Mask does.
                for (size_t i = 0; i < N; ++i) {
                    float* p = &st.pic.px[i * 4];
                    p[0] = p[1] = p[2] = keyAt(m, i) * p[3];
                }
                st.key = std::move(m);
                out[key] = std::move(st);
                continue;
            }
            if (m.empty()) {
                runEffects(key, st.pic);
            } else {
                const Image before = st.pic;
                runEffects(key, st.pic);
                for (size_t i = 0; i < N; ++i) {
                    float* p = &st.pic.px[i * 4];
                    const float* o = &before.px[i * 4];
                    for (int ch = 0; ch < 4; ++ch) p[ch] = o[ch] + (p[ch] - o[ch]) * m[i];
                }
            }
            st.key = std::move(m);
        } else if (n->kind == "key_mixer") {
            // Its keys added (where any of them is); none: no key (everywhere).
            const std::vector<const GradeLink*> ins = inputsOf(key);
            std::vector<float> k(ins.empty() ? 0 : N, 0.0f);
            for (const GradeLink* l : ins) {
                const std::vector<float>* in = keyFrom(l);
                for (size_t i = 0; i < N; ++i) k[i] = std::min(1.0f, k[i] + (in ? keyAt(*in, i) : 1.0f));
            }
            st.key = std::move(k);
        } else {
            // Parallel: each branch where its key is, else their average. Layer: each over the ones before it, with its
            // blend mode, where its key is.
            const std::vector<const GradeLink*> ins = inputsOf(key);
            st.hasPic = true;
            if (ins.empty() || !n->enabled) {
                const GradeLink* first = ins.empty() ? nullptr : ins.front();
                st.pic = pictureFrom(first ? first->from : 0);
                out[key] = std::move(st);
                continue;
            }
            std::vector<const Image*> pics;
            std::vector<const std::vector<float>*> keys;
            for (const GradeLink* l : ins) {
                pics.push_back(&pictureFrom(l->from));
                auto it = l->from ? out.find(l->from) : out.end();
                keys.push_back(it == out.end() ? nullptr : &it->second.key);
            }
            st.pic = *pics.front();
            // Its key: where any branch's is (a parallel mixer's every input, a layer mixer's layers over the first),
            // for an outside node or a key mixer; a branch with no key of its own makes it everywhere.
            {
                const size_t from = n->kind == "parallel" ? 0 : 1;
                bool everywhere = from >= keys.size();
                for (size_t j = from; j < keys.size(); ++j) everywhere = everywhere || !keys[j] || keys[j]->empty();
                if (!everywhere) {
                    st.key.assign(N, 0.0f);
                    for (size_t j = from; j < keys.size(); ++j)
                        for (size_t i = 0; i < N; ++i) st.key[i] = std::min(1.0f, st.key[i] + (*keys[j])[i]);
                }
            }
            if (n->kind == "parallel") {
                for (size_t i = 0; i < N; ++i) {
                    float sum[4] = {0, 0, 0, 0}, mean[4] = {0, 0, 0, 0}, w = 0;
                    for (size_t j = 0; j < pics.size(); ++j) {
                        const float* p = &pics[j]->px[i * 4];
                        const float wj = keys[j] ? keyAt(*keys[j], i) : 1.0f;
                        for (int ch = 0; ch < 4; ++ch) sum[ch] += p[ch] * wj, mean[ch] += p[ch];
                        w += wj;
                    }
                    float* d = &st.pic.px[i * 4];
                    for (int ch = 0; ch < 4; ++ch) d[ch] = w > 1e-6f ? sum[ch] / w : mean[ch] / float(pics.size());
                }
            } else {
                for (size_t j = 1; j < pics.size(); ++j) {
                    const int mode = blendModeIndex(j - 1 < n->blend.size() ? n->blend[j - 1] : "normal");
                    for (size_t i = 0; i < N; ++i) {
                        float* d = &st.pic.px[i * 4];
                        const float* b = &pics[j]->px[i * 4];
                        const float w = keys[j] ? keyAt(*keys[j], i) : 1.0f;
                        if (w <= 0) continue;
                        // (straight colour under the picture's own alpha: the branches come from one picture)
                        const float a = d[3], ab = b[3];
                        const float ia = a > 0 ? 1 / a : 0, ib = ab > 0 ? 1 / ab : 0;
                        for (int ch = 0; ch < 3; ++ch) {
                            const float cb = d[ch] * ia, cs = b[ch] * ib;
                            d[ch] = (cb + (blendModeChannel(mode, cb, cs) - cb) * w) * a;
                        }
                    }
                }
            }
        }
        out[key] = std::move(st);
    }
    const GradeLink* last = gradeLinkInto(g, 0, 0);
    if (last && last->from) img = pictureFrom(last->from);
}

}  // namespace montage
