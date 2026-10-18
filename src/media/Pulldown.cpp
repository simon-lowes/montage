#include "Pulldown.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "Decoder.h"

namespace montage {

std::vector<PulldownFrame> pulldownFrames(const std::vector<PulldownStretch>& stretches, int64_t fileFrames) {
    std::vector<PulldownFrame> out;
    if (fileFrames <= 0) return out;
    out.reserve(size_t(fileFrames * 4 / 5 + 4));
    for (size_t k = 0; k < stretches.size(); ++k) {
        const PulldownStretch& st = stretches[k];
        const int64_t s = std::min(st.start, fileFrames);
        const int64_t e = k + 1 < stretches.size() ? std::min(stretches[k + 1].start, fileFrames) : fileFrames;
        const bool tf = st.topFirst;
        for (int64_t i = s; i < e; ++i) {
            if (st.position < 0) {
                // Video with no pulldown: every fifth frame left out, so it keeps time with the rest.
                if ((i - s) % 5 != 4) out.push_back({i, i, tf});
                continue;
            }
            const int j = int((st.position + (i - s)) % 5);
            switch (j) {
                case 0:
                case 1:
                case 4: out.push_back({i, i, tf}); break;  // A A, B B, D D
                case 2:
                    // B C: B's first field again (left out), C's second. At the start of a stretch B has only this field
                    // here; at its end C has only this one.
                    if (i == s) out.push_back({i, -1, tf});
                    if (i == e - 1) out.push_back({-1, i, tf});
                    break;
                case 3:
                    // C D: C's first field, with its second from the frame before; D's second (D D follows).
                    out.push_back({i, i > s ? i - 1 : -1, tf});
                    break;
            }
        }
    }
    return out;
}

std::string pulldownStartingAt(int position, bool topFirst) { return pulldownValue({{0, std::clamp(position, 0, 4), topFirst}}); }

Rational pulldownRate(Rational fileFps) {
    if (fileFps == Rational{30000, 1001}) return Rational{24000, 1001};
    if (fileFps == Rational{30, 1}) return Rational{24, 1};
    return Rational{fileFps.num * 4, fileFps.den * 5};
}

bool pulldownRateValid(Rational fileFps) {
    return fileFps.valid() && (std::fabs(fileFps.toDouble() - 30000.0 / 1001.0) < 1e-3 || std::fabs(fileFps.toDouble() - 30.0) < 1e-3);
}

AVFrame* weaveFields(const AVFrame* first, const AVFrame* second, bool topFirst) {
    const AVFrame* any = first ? first : second;
    if (!any || (first && second && (first->width != second->width || first->height != second->height || first->format != second->format)))
        return nullptr;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(any->format));
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM)) || any->height < 2) return nullptr;
    AVFrame* out = av_frame_alloc();
    if (!out) return nullptr;
    out->format = any->format;
    out->width = any->width;
    out->height = any->height;
    if (av_frame_get_buffer(out, 0) < 0 || av_frame_copy_props(out, any) < 0) {
        av_frame_free(&out);
        return nullptr;
    }
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 7, 100)
    out->flags &= ~(AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST);
#else
    out->interlaced_frame = 0;
    out->top_field_first = 0;
#endif
    // The first field's lines are the even ones when the top field is first; each plane's lines alternate the same way
    // (4:2:0 chroma too, as FFmpeg's telecine weaves it).
    const int firstParity = topFirst ? 0 : 1;
    const bool wide = d->comp[0].depth > 8;
    for (int p = 0; p < 4 && any->data[p]; ++p) {
        const int bytes = av_image_get_linesize(AVPixelFormat(any->format), any->width, p);
        if (bytes <= 0) continue;
        const int rows = (p == 1 || p == 2) ? AV_CEIL_RSHIFT(any->height, d->log2_chroma_h) : any->height;
        for (int y = 0; y < rows; ++y) {
            const bool isFirst = (y & 1) == firstParity;
            const AVFrame* from = isFirst ? first : second;
            uint8_t* dst = out->data[p] + size_t(y) * size_t(out->linesize[p]);
            if (from) {
                std::memcpy(dst, from->data[p] + size_t(y) * size_t(from->linesize[p]), size_t(bytes));
                continue;
            }
            // A missing field's line: the average of its neighbours from the field there is (the nearest at the edges).
            const AVFrame* other = isFirst ? second : first;
            const int above = y > 0 ? y - 1 : y + 1, below = y + 1 < rows ? y + 1 : y - 1;
            if (above < 0 || above >= rows || below < 0 || below >= rows) {
                std::memset(dst, 0, size_t(bytes));
                continue;
            }
            const uint8_t* a = other->data[p] + size_t(above) * size_t(other->linesize[p]);
            const uint8_t* b = other->data[p] + size_t(below) * size_t(other->linesize[p]);
            if (wide && d->comp[0].step >= 2) {
                const auto* a16 = reinterpret_cast<const uint16_t*>(a);
                const auto* b16 = reinterpret_cast<const uint16_t*>(b);
                auto* d16 = reinterpret_cast<uint16_t*>(dst);
                for (int x = 0; x < bytes / 2; ++x) d16[x] = uint16_t((uint32_t(a16[x]) + b16[x] + 1) / 2);
            } else {
                for (int x = 0; x < bytes; ++x) dst[x] = uint8_t((unsigned(a[x]) + b[x] + 1) / 2);
            }
        }
    }
    return out;
}

namespace {

// A frame's picture as luma, row by row (Rec. 709 weights on the decoded RGB).
struct Luma {
    int w = 0, h = 0;
    std::vector<float> y;
    float at(int x, int r) const { return y[size_t(r) * size_t(w) + size_t(x)]; }
};

Luma lumaOf(const Frame16& f) {
    Luma l;
    l.w = f.width;
    l.h = f.height;
    l.y.resize(size_t(l.w) * size_t(l.h));
    const float k = 255.0f / 65535.0f;
    for (size_t i = 0; i < l.y.size(); ++i) {
        const uint16_t* p = &f.px[i * 4];
        l.y[i] = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) * k;
    }
    return l;
}

// Combing of the frame woven from `top`'s top field and `bottom`'s bottom: how far each line stands out from the two
// beside it (from the other field), on average.
double combing(const Luma& top, const Luma& bottom) {
    const int w = top.w, h = top.h;
    if (h < 3) return 0;
    double sum = 0;
    auto line = [&](int r) -> const Luma& { return (r & 1) ? bottom : top; };
    for (int r = 1; r + 1 < h; ++r) {
        const Luma& c = line(r);
        const Luma& o = line(r - 1);  // (r - 1 and r + 1 are of the other field)
        for (int x = 0; x < w; ++x) sum += std::fabs(c.at(x, r) - 0.5f * (o.at(x, r - 1) + o.at(x, r + 1)));
    }
    return sum / (double(w) * double(h - 2));
}

// The mean difference between one field (parity 0 top, 1 bottom) of two frames.
double fieldDifference(const Luma& a, const Luma& b, int parity) {
    double sum = 0;
    int64_t n = 0;
    for (int r = parity; r < a.h; r += 2)
        for (int x = 0; x < a.w; ++x, ++n) sum += std::fabs(a.at(x, r) - b.at(x, r));
    return n ? sum / double(n) : 0.0;
}

// What each frame's fields say about the cadence.
struct Fields {
    double own = 0;              // its own two fields woven
    double topWithPrev = 0;      // its top field with the frame before's bottom
    double bottomWithPrev = 0;   // its bottom field with the frame before's top
    double topRepeat = 0;        // its top field against the frame before's
    double bottomRepeat = 0;     // its bottom field against the frame before's
};

}  // namespace

bool detectPulldown(const std::string& path, std::vector<PulldownStretch>& out, const std::function<void(double)>& progress,
                    const std::atomic<bool>* cancel, std::string* error) {
    out.clear();
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    // Read as the file is, its fields left as they are (not deinterlaced), narrower but with every line.
    Interpretation in;
    in.fields = "progressive";
    VideoDecoder dec;
    if (!dec.open(interpretedPath(uninterpretedPath(path), in), error)) return false;
    if (dec.isStill()) return fail("Pulldown is removed from video, not stills");
    const double fps = dec.fps();
    const int64_t frames = int64_t(std::floor(dec.duration() * fps + 1e-6));
    if (frames < 5) return fail("It is too short to find its cadence");
    const int w = std::max(16, std::min(480, dec.displayWidth())), h = dec.displayHeight();
    if (h < 8) return fail("The picture is too small to find its cadence");
    std::vector<Fields> f;
    f.reserve(size_t(frames));
    Luma prev;
    for (int64_t i = 0; i < frames; ++i) {
        if (cancel && cancel->load()) return fail("Cancelled");
        Frame16Ptr fr = dec.frameAt((double(i) + 0.25) / fps, w, h);
        if (!fr) break;
        Luma cur = lumaOf(*fr);
        Fields m;
        m.own = combing(cur, cur);
        if (i > 0 && prev.w == cur.w && prev.h == cur.h) {
            m.topWithPrev = combing(cur, prev);
            m.bottomWithPrev = combing(prev, cur);
            m.topRepeat = fieldDifference(cur, prev, 0);
            m.bottomRepeat = fieldDifference(cur, prev, 1);
        } else {
            m.topWithPrev = m.bottomWithPrev = m.own;
            m.topRepeat = m.bottomRepeat = std::numeric_limits<double>::quiet_NaN();
        }
        f.push_back(m);
        prev = std::move(cur);
        if (progress && i % 30 == 0) progress(double(i) / double(frames));
    }
    const int64_t n = int64_t(f.size());
    if (n < 5) return fail("It is too short to find its cadence");

    // A field repeated from the frame before, as a share of the motion round it (0 repeated .. 1 not); 0.5 where nothing
    // moves (no evidence either way).
    auto repeatScore = [&](int64_t i, bool top) {
        auto value = [&](int64_t k) { return top ? f[size_t(k)].topRepeat : f[size_t(k)].bottomRepeat; };
        const double r = value(i);
        if (!(r == r)) return 0.5;
        std::array<double, 5> near{};
        int c = 0;
        for (int64_t k = std::max<int64_t>(1, i - 2); k <= std::min(n - 1, i + 2); ++k)
            if (value(k) == value(k)) near[size_t(c++)] = value(k);
        std::sort(near.begin(), near.begin() + c);
        const double motion = c ? near[size_t(c / 2)] : 0.0;
        if (motion < 0.4) return 0.5;  // still: a repeated field looks like any other
        return std::min(1.0, r / motion);
    };
    // Of the frame as it is (c) and its first field with the frame before's second (p), how much more one is combed:
    // 0 when c is the clean one, 1 when p is, 0.5 when they are alike.
    auto cleanerP = [&](int64_t i, bool top) {
        const double a = f[size_t(i)].own, b = top ? f[size_t(i)].topWithPrev : f[size_t(i)].bottomWithPrev;
        const double lo = std::min(a, b), hi = std::max(a, b);
        if (hi < 0.3 || hi < lo * 1.1) return 0.5;
        return a / (a + b);  // high when the frame itself is the combed one
    };
    // How combed the frame itself is against the clean frames round it (the median of nine, of which at most four are
    // combed in a cadence): 0 clean .. 1 combed; 0.5 where nothing moves.
    std::vector<double> combed(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        std::array<double, 9> near{};
        int c = 0;
        for (int64_t k = std::max<int64_t>(0, i - 4); k <= std::min(n - 1, i + 4); ++k) near[size_t(c++)] = f[size_t(k)].own;
        std::sort(near.begin(), near.begin() + c);
        const double base = near[size_t((c - 1) / 3)], own = f[size_t(i)].own;
        combed[size_t(i)] = std::max(own, base) < 0.3 ? 0.5 : std::clamp(own / std::max(base, 1e-9) - 1.0, 0.0, 1.0);
    }
    // States: (order, cadence position) for positions 0..4, then video with no pulldown (one per order, alike).
    constexpr int kStates = 12;
    auto order = [](int s) { return s < 5 ? true : s < 10 ? false : s == 10; };
    auto position = [](int s) { return s < 10 ? s % 5 : -1; };
    // What the frame itself says: B C and C D are of two pictures, the others (and video's frames, each like the
    // frames round it) of one.
    auto ownCost = [&](int64_t i, int s) {
        const int j = position(s);
        return j == 2 || j == 3 ? 1 - combed[size_t(i)] : combed[size_t(i)];
    };
    // What it says with the frame before, when the cadence goes on from it (at a break the frame before is of another
    // stretch, so says nothing).
    auto linkCost = [&](int64_t i, int s) {
        const bool tf = order(s);
        const int j = position(s);
        const double p = cleanerP(i, tf);  // (the first field: top when the top is first)
        double cost = 0;
        if (j < 0) return 0.15 + 0.8 * p;  // video: its own fields always belong together (a little dearer, so a still
                                           // stretch stays as it was)
        if (j == 0 || j == 1) cost += p;
        else if (j == 2 || j == 3) cost += 1 - p;
        else cost += std::fabs(p - 0.5) * 2;  // D D: the frame and its first field with C D's second are the same
        if (j == 2) cost += repeatScore(i, tf);   // B's first field again
        if (j == 4) cost += repeatScore(i, !tf);  // D's second field again
        return cost;
    };
    auto next = [&](int s) {
        if (s >= 10) return s;
        return (s / 5) * 5 + (s % 5 + 1) % 5;
    };
    constexpr double kBreak = 4.0;  // the cost of the cadence changing
    std::vector<std::array<uint8_t, kStates>> from(static_cast<size_t>(n));
    std::array<double, kStates> cost{};
    for (int s = 0; s < kStates; ++s) cost[size_t(s)] = ownCost(0, s) + (position(s) < 0 ? 0.15 : 0.0);
    for (int64_t i = 1; i < n; ++i) {
        if (cancel && cancel->load()) return fail("Cancelled");
        int best = 0;
        for (int s = 1; s < kStates; ++s)
            if (cost[size_t(s)] < cost[size_t(best)]) best = s;
        std::array<double, kStates> now{};
        for (int s = 0; s < kStates; ++s) {
            // Continuing the cadence (the state before this one), or breaking it from the best state.
            int cont = -1;
            for (int q = 0; q < kStates; ++q)
                if (next(q) == s) cont = q;  // (one predecessor each)
            double c = cont >= 0 ? cost[size_t(cont)] + linkCost(i, s) : std::numeric_limits<double>::infinity();
            int pick = cont;
            const double broken = cost[size_t(best)] + kBreak + (position(s) < 0 ? 0.15 : 0.0);
            if (broken < c) c = broken, pick = best;
            now[size_t(s)] = c + ownCost(i, s);
            from[size_t(i)][size_t(s)] = uint8_t(pick);
        }
        cost = now;
    }
    int s = 0;
    for (int k = 1; k < kStates; ++k)
        if (cost[size_t(k)] < cost[size_t(s)]) s = k;
    std::vector<int> best(static_cast<size_t>(n));
    for (int64_t i = n - 1; i >= 0; --i) {
        best[size_t(i)] = s;
        if (i > 0) s = from[size_t(i)][size_t(s)];
    }
    bool any = false;
    for (int64_t i = 0; i < n; ++i) {
        const int st = best[size_t(i)];
        const bool continues = i > 0 && next(best[size_t(i - 1)]) == st;
        if (!continues) {
            // (video is one stretch whichever field the states call first)
            if (i > 0 && st >= 10 && best[size_t(i - 1)] >= 10) continue;
            out.push_back({i, position(st), order(st)});
        }
        any |= st < 10;
    }
    if (progress) progress(1.0);
    if (!any) {
        out.clear();
        return fail("No 2:3 pulldown was found in it");
    }
    return true;
}

}  // namespace montage
