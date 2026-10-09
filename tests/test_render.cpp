// Renderer tests: blending, transforms, effects, transitions, generators.
#include <QtTest>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <tuple>

#include "core/ClipAnimation.h"
#include "core/GradeVersions.h"
#include "core/Captions.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/MaskPath.h"
#include "render/ColorSpace.h"
#include "render/Compositor.h"
#include "render/Exporter.h"
#include "render/Ocio.h"
#include "media/Tracking.h"
#include "render/Processing.h"
#include "render/QualityCheck.h"
#include "render/LutExport.h"
#include "render/Relight.h"
#include "render/Deconvolve.h"
#include "render/FilmLook.h"
#include "media/DepthMap.h"
#include "render/RenderCache.h"
#include "render/Shapes.h"
#include "render/Shorts.h"
#include "core/Transcript.h"
#include "render/VideoDenoise.h"
#include "render/VideoFx.h"
#include <random>

using namespace montage;

namespace {

Image solid(int w, int h, float r, float g, float b, float a = 1) {
    Image img(w, h);
    img.fill(r, g, b, a);
    return img;
}

bool near(float a, float b, float tol = 0.02f) { return std::fabs(a - b) <= tol; }

// Unpremultiplied colour at a pixel.
void rgb(const Image& img, int x, int y, float out[4]) {
    const float* p = img.at(x, y);
    float a = p[3];
    for (int c = 0; c < 3; ++c) out[c] = a > 0 ? p[c] / a : 0;
    out[3] = a;
}

Clip colorClip(Project& p, float r, float g, float b, FrameTime start, FrameTime len) {
    Clip c = makeGeneratorClip(p, "color", len);
    c.start = start;
    c.generator.params["color.r"] = r;
    c.generator.params["color.g"] = g;
    c.generator.params["color.b"] = b;
    return c;
}

}  // namespace

class TestRender : public QObject {
    Q_OBJECT
private slots:
    void creatorTransitions() {
        const int W = 160, H = 120;
        // A: warm with vertical stripes, B: cool with horizontal stripes.
        Image A(W, H), B(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float* a = A.at(x, y);
                a[0] = 0.8f, a[1] = 0.3f + 0.2f * float((x / 4) % 2), a[2] = 0.1f, a[3] = 1;
                float* b = B.at(x, y);
                b[0] = 0.1f, b[1] = 0.3f + 0.2f * float((y / 4) % 2), b[2] = 0.8f, b[3] = 1;
            }
        auto maxDiff = [](const Image& x, const Image& y) {
            float d = 0;
            for (size_t i = 0; i < x.px.size(); ++i) d = std::max(d, std::abs(x.px[i] - y.px[i]));
            return d;
        };
        Project p;
        for (const char* type : {"whip_pan", "zoom_blur", "spin", "glitch", "light_leak", "luma_wipe", "clock_wipe", "shape_wipe", "cube", "flip"}) {
            const EffectInfo* info = findEffectInfo(type);
            QVERIFY2(info && info->category == EffectCategory::VideoTransition, type);
            const Effect e = makeEffect(p, type);
            QVERIFY2(maxDiff(transitionMix(type, e, A, B, 0.0, W, H), A) < 1e-3f, type);
            QVERIFY2(maxDiff(transitionMix(type, e, A, B, 1.0, W, H), B) < 1e-3f, type);
            const Image mid = transitionMix(type, e, A, B, 0.5, W, H);
            QVERIFY2(maxDiff(mid, A) > 0.1f && maxDiff(mid, B) > 0.1f, type);
        }
        auto redness = [](const float* q) { return q[0] - q[2]; };
        // Whip Pan left: A on the left going out, B on the right coming in, smeared sideways.
        {
            Effect e = makeEffect(p, "whip_pan");
            const Image m = transitionMix("whip_pan", e, A, B, 0.5, W, H);
            QVERIFY(redness(m.at(20, 60)) > 0.3f && redness(m.at(140, 60)) < -0.3f);
            // A's vertical stripes are smeared away along the move.
            QVERIFY(std::abs(m.at(20, 60)[1] - m.at(22, 60)[1]) < 0.05f);
        }
        // Zoom Blur: early on, the centre still A, the edges smeared outward.
        {
            Effect e = makeEffect(p, "zoom_blur");
            const Image m = transitionMix("zoom_blur", e, A, B, 0.25, W, H);
            QVERIFY(redness(m.at(80, 60)) > 0.5f);
            QVERIFY(std::abs(m.at(4, 60)[1] - m.at(6, 60)[1]) < 0.1f);
        }
        // Spin: a mark on A moves round the centre.
        {
            Image marked = A;
            for (int y = 10; y < 20; ++y)
                for (int x = 75; x < 85; ++x) std::fill_n(marked.at(x, y), 3, 1.0f);
            Effect e = makeEffect(p, "spin");
            e.params["strength"] = Param(0.0);
            const Image m = transitionMix("spin", e, marked, B, 0.25, W, H);
            QVERIFY(m.at(80, 15)[2] < 0.5f);  // no longer where it was
        }
        // Glitch: rows thrown about halfway, and colours split.
        {
            Effect e = makeEffect(p, "glitch");
            e.params["strength"] = Param(2.0);
            const Image m = transitionMix("glitch", e, A, B, 0.4, W, H);
            int changed = 0;
            for (int y = 0; y < H; y += 5) changed += maxDiff(transitionMix("glitch", e, A, B, 0.0, W, H), m) > 0.1f;
            QVERIFY(changed > 0);
        }
        // Light Leak: brighter and warmer than a plain dissolve halfway.
        {
            Effect e = makeEffect(p, "light_leak");
            const Image m = transitionMix("light_leak", e, A, B, 0.5, W, H);
            Effect plain = makeEffect(p, "cross_dissolve");
            const Image d = transitionMix("cross_dissolve", plain, A, B, 0.5, W, H);
            double added[3] = {0, 0, 0};
            for (size_t i = 0; i < m.px.size(); i += 4)
                for (int c = 0; c < 3; ++c) added[c] += m.px[i + c] - d.px[i + c];
            QVERIFY(added[0] > 0 && added[0] > added[2] * 2);
        }
        // Shape Wipe: a circle of B growing from the middle; a diamond reaches along the axes first; a grid of them.
        {
            Effect e = makeEffect(p, "shape_wipe");
            Image m = transitionMix("shape_wipe", e, A, B, 0.5, W, H);
            QVERIFY(redness(m.at(80, 60)) < -0.5f && redness(m.at(1, 1)) > 0.5f);
            e.params["shape"] = Param(1.0);
            m = transitionMix("shape_wipe", e, A, B, 0.28, W, H);
            QVERIFY2(redness(m.at(110, 60)) < -0.5f && redness(m.at(101, 81)) > 0.1f,
                     qPrintable(QString("%1 %2").arg(redness(m.at(110, 60))).arg(redness(m.at(101, 81)))));
            e.params["shape"] = Param(0.0);
            e.params["count"] = Param(4.0);
            m = transitionMix("shape_wipe", e, A, B, 0.5, W, H);
            for (int cx : {20, 60, 100, 140}) QVERIFY(redness(m.at(cx, 20)) < -0.5f && redness(m.at(cx + 19, 39)) > 0.5f);
        }
        // 3D Cube: halfway the cube's edge is in the middle, A turning away on the left and B coming in on the right,
        // pushed back so the corners are empty.
        {
            Effect e = makeEffect(p, "cube");
            const Image m = transitionMix("cube", e, A, B, 0.5, W, H);
            QVERIFY(redness(m.at(50, 60)) > 0.5f && redness(m.at(110, 60)) < -0.5f && m.at(1, 1)[3] < 0.01f);
            e.params["direction"] = Param(1.0);  // the other way round
            const Image r = transitionMix("cube", e, A, B, 0.5, W, H);
            QVERIFY(redness(r.at(50, 60)) < -0.5f && redness(r.at(110, 60)) > 0.5f);
            e.params["direction"] = Param(2.0);  // up: B from below
            const Image up = transitionMix("cube", e, A, B, 0.5, W, H);
            QVERIFY(redness(up.at(80, 35)) > 0.5f && redness(up.at(80, 85)) < -0.5f);
        }
        // 3D Flip: A on a narrowing card, then B on its back.
        {
            Effect e = makeEffect(p, "flip");
            const Image first = transitionMix("flip", e, A, B, 0.25, W, H), second = transitionMix("flip", e, A, B, 0.75, W, H);
            QVERIFY(redness(first.at(80, 60)) > 0.5f && first.at(2, 60)[3] < 0.01f);
            QVERIFY(redness(second.at(80, 60)) < -0.5f && second.at(2, 60)[3] < 0.01f);
        }
        // Luma Wipe: B through A's dark half first; brights first reverses it.
        {
            Image split = A;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W / 2; ++x) std::fill_n(split.at(x, y), 3, 0.05f);
            Effect e = makeEffect(p, "luma_wipe");
            Image m = transitionMix("luma_wipe", e, split, B, 0.3, W, H);
            QVERIFY(redness(m.at(20, 60)) < -0.3f && redness(m.at(140, 60)) > 0.3f);
            e.params["invert"] = Param(1.0);
            m = transitionMix("luma_wipe", e, split, B, 0.6, W, H);
            QVERIFY(std::abs(redness(m.at(20, 60))) < 0.01f && redness(m.at(140, 60)) < -0.3f);
        }
        // Clock Wipe: a quarter of the way, the top right shows B, the rest A.
        {
            Effect e = makeEffect(p, "clock_wipe");
            const Image m = transitionMix("clock_wipe", e, A, B, 0.27, W, H);
            QVERIFY(redness(m.at(130, 30)) < -0.3f);
            QVERIFY(redness(m.at(30, 30)) > 0.3f && redness(m.at(30, 100)) > 0.3f && redness(m.at(130, 100)) > 0.3f);
        }
    }

    void filmLookParts() {
        auto none = [] {
            FilmLookSettings s;
            s.halation = s.bloom = s.grain = s.weave = s.vignette = s.softness = s.flicker = s.aberration = s.fade = 0;
            return s;
        };
        const int W = 400, H = 300;
        // A white square on black: halation glows red-orange round it, bloom glows neutral.
        Image square(W, H);
        square.fill(0, 0, 0, 1);
        for (int y = 120; y < 180; ++y)
            for (int x = 170; x < 230; ++x) std::fill_n(square.at(x, y), 3, 1.0f);
        FilmLookSettings s = none();
        s.halation = 1;
        Image img = square;
        filmLook(img, s, 0);
        const float* near = img.at(234, 150);
        QVERIFY2(near[0] > 0.05f && near[0] > near[1] * 2 && near[1] > near[2], qPrintable(QString("%1 %2 %3").arg(near[0]).arg(near[1]).arg(near[2])));
        QCOMPARE(img.at(10, 10)[0], 0.0f);
        s = none();
        s.bloom = 1;
        img = square;
        filmLook(img, s, 0);
        const float* glow = img.at(240, 150);
        QVERIFY2(glow[0] > 0.02f && std::abs(glow[0] - glow[2]) < glow[0] * 0.05f, qPrintable(QString("%1 %2 %3").arg(glow[0]).arg(glow[1]).arg(glow[2])));
        // Grain on flat grey: the mean kept, the spread with the gauge, the same frame the same.
        Image grey(W, H);
        grey.fill(0.5f, 0.5f, 0.5f, 1);
        auto spread = [&](int gauge, FrameTime t, double* mean = nullptr) {
            FilmLookSettings g = none();
            g.grain = 1;
            g.gauge = gauge;
            Image im = grey;
            filmLook(im, g, t);
            double sum = 0, sq = 0;
            for (size_t i = 0; i < im.px.size(); i += 4) sum += im.px[i + 1], sq += double(im.px[i + 1]) * im.px[i + 1];
            const double n = double(im.px.size() / 4), m = sum / n;
            if (mean) *mean = m;
            return std::sqrt(sq / n - m * m);
        };
        double mean = 0;
        const double s35 = spread(1, 3, &mean), s65 = spread(0, 3), s8 = spread(3, 3);
        QVERIFY2(std::abs(mean - 0.5) < 0.01, qPrintable(QString::number(mean)));
        QVERIFY(s35 > 0.01 && s65 < s35 && s8 > s35);
        QCOMPARE(spread(1, 3), s35);
        {
            FilmLookSettings g = none();
            g.grain = 1;
            Image a = grey, b = grey;
            filmLook(a, g, 3);
            filmLook(b, g, 4);
            QVERIFY(a.px != b.px);
        }
        // Gate weave: a dot wanders a little from frame to frame, and not at all without it.
        Image dot(W, H);
        dot.fill(0, 0, 0, 1);
        for (int y = 148; y < 152; ++y)
            for (int x = 198; x < 202; ++x) std::fill_n(dot.at(x, y), 3, 1.0f);
        auto centre = [&](const Image& im) {
            double sx = 0, sy = 0, sw = 0;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const double v = im.at(x, y)[1];
                    sx += v * x, sy += v * y, sw += v;
                }
            return std::pair{sx / sw, sy / sw};
        };
        s = none();
        s.weave = 1;
        double most = 0;
        for (FrameTime t : {0, 12, 24, 37, 50}) {
            Image im = dot;
            filmLook(im, s, t);
            const auto [cx, cy] = centre(im);
            most = std::max(most, std::hypot(cx - 199.5, cy - 149.5));
            QVERIFY(std::hypot(cx - 199.5, cy - 149.5) < 2.5);
        }
        QVERIFY(most > 0.1);
        // Vignette darkens the corners only; fade lifts black; softness takes the edge off.
        s = none();
        s.vignette = 1;
        img = grey;
        filmLook(img, s, 0);
        QVERIFY(img.at(2, 2)[0] < 0.3f && std::abs(img.at(200, 150)[0] - 0.5f) < 1e-4f);
        s = none();
        s.fade = 1;
        img = square;
        filmLook(img, s, 0);
        QVERIFY(img.at(10, 10)[0] > 0.05f && img.at(200, 150)[0] > 0.99f);
        s = none();
        s.softness = 1;
        img = square;
        filmLook(img, s, 0);
        QVERIFY(img.at(169, 150)[0] > 0.05f && img.at(170, 150)[0] < 0.95f);
        // Flicker moves the brightness from frame to frame, within a few percent.
        s = none();
        s.flicker = 1;
        double lo = 1, hi = 0;
        for (FrameTime t = 0; t < 10; ++t) {
            img = grey;
            filmLook(img, s, t);
            lo = std::min(lo, double(img.at(100, 100)[0])), hi = std::max(hi, double(img.at(100, 100)[0]));
        }
        QVERIFY(hi - lo > 0.01 && hi < 0.55 && lo > 0.45);
        // Aberration: red and blue apart at the edges, none at the centre.
        s = none();
        s.aberration = 1;
        img = square;
        filmLook(img, s, 0);
        const float* edge = img.at(229, 150);
        QVERIFY(std::abs(edge[0] - edge[2]) > 0.05f);
        // Through the effect, with its defaults.
        Project p;
        Effect e = makeEffect(p, "film_look");
        QCOMPARE(filmLookSettings(e, 0).gauge, 1);
        img = square;
        applyVideoEffect(e, 5, img, 1.0);
        QVERIFY(img.px != square.px);
    }

    void relightFromDepth() {
        // A dome rising out of a flat backdrop, on a mid-grey picture.
        const int W = 200, H = 160;
        DepthMap dome;
        dome.width = W, dome.height = H;
        dome.values.assign(size_t(W) * H, 0.0f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double r = std::hypot(x - 100.0, y - 80.0) / 60.0;
                dome.values[size_t(y) * W + x] = r < 1 ? float(std::sqrt(1 - r * r)) : 0.0f;
            }
        // Normals point outwards on the dome and straight at the viewer on the backdrop.
        const std::vector<float> n = depthNormals(dome, W, H, 3, 1);
        auto normal = [&](int x, int y) { return &n[(size_t(y) * W + x) * 3]; };
        QVERIFY(normal(60, 80)[0] < -0.3f);  // left side faces left
        QVERIFY(normal(140, 80)[0] > 0.3f);  // right side faces right
        QVERIFY(normal(100, 40)[1] < -0.3f);  // top faces up
        QVERIFY(std::abs(normal(5, 5)[0]) < 1e-3f && normal(5, 5)[2] > 0.999f);
        auto lit = [&](double azimuth) {
            Image img(W, H);
            img.fill(0.5f, 0.5f, 0.5f, 1.0f);
            RelightSettings s;
            s.azimuth = azimuth;
            s.color[0] = s.color[1] = s.color[2] = 1;
            s.smoothness = 1;
            relight(img, dome, s);
            return img;
        };
        // From the left (180 degrees), the dome's left side is brighter than its right; from the right, the reverse.
        const Image left = lit(180), right = lit(0);
        QVERIFY(left.at(60, 80)[0] > left.at(140, 80)[0] + 0.1f);
        QVERIFY(right.at(140, 80)[0] > right.at(60, 80)[0] + 0.1f);
        // The flat backdrop gets the same light either way.
        QVERIFY(std::abs(left.at(5, 5)[0] - right.at(5, 5)[0]) < 1e-5f);
        // Surfaces facing the camera keep their light; a short reach lights only the nearer parts.
        QCOMPARE(left.at(5, 5)[0], 0.5f);
        QVERIFY(std::abs(left.at(100, 80)[0] - 0.5f) < 0.02f);
        Image img(W, H);
        img.fill(0.5f, 0.5f, 0.5f, 1.0f);
        RelightSettings s;
        s.azimuth = 90;  // from above
        s.reach = 0.3;
        s.color[0] = s.color[1] = s.color[2] = 1;
        relight(img, dome, s);
        QVERIFY(img.at(100, 55)[0] > 0.55f);    // the upper slope, near
        QCOMPARE(img.at(100, 138)[0], 0.5f);  // the bottom edge, as far as the backdrop: unlit
    }

    void colorTransfers() {
        // Published reference values.
        QVERIFY(near(float(fromLinear(Transfer::Pq, 100 / 203.0)), 0.5081f, 0.0005f));  // 100 nits
        QVERIFY(near(float(fromLinear(Transfer::Pq, 10000 / 203.0)), 1.0f, 1e-5f));
        QVERIFY(near(float(fromLinear(Transfer::Pq, 1.0)), 0.5807f, 0.0005f));  // HDR reference white, 203 nits
        QVERIFY(near(float(fromLinear(Transfer::SLog3, 0.18) * 1023), 420.0f, 0.05f));
        QVERIFY(near(float(fromLinear(Transfer::LogC3, 0.18)), 0.3910f, 0.0005f));
        QVERIFY(near(float(fromLinear(Transfer::LogC4, 0.18)), 0.2784f, 0.0005f));
        QVERIFY(near(float(fromLinear(Transfer::VLog, 0.18)), 0.4233f, 0.0005f));
        QVERIFY(near(float(fromLinear(Transfer::CLog3, 0.18)), 0.3434f, 0.0005f));
        QVERIFY(near(float(fromLinear(Transfer::AcesCct, 0.18)), 0.4136f, 0.0005f));
        QVERIFY(near(float(fromLinear(Transfer::Hlg, 1.0 / 12)), 0.5f, 1e-5f));
        // Every curve inverts itself, through both segments.
        for (Transfer t : {Transfer::Bt1886, Transfer::Srgb, Transfer::Pq, Transfer::Hlg, Transfer::SLog3, Transfer::LogC3,
                           Transfer::LogC4, Transfer::VLog, Transfer::CLog3, Transfer::AcesCct})
            for (double v : {0.02, 0.1, 0.3, 0.6, 0.95}) QVERIFY2(std::fabs(fromLinear(t, toLinear(t, v)) - v) < 1e-6, qPrintable(QString::number(int(t))));

        // Rec.709 primaries in BT.2020 (ITU-R BT.2087).
        double m[9];
        primariesMatrix(Primaries::Bt709, Primaries::Bt2020, m);
        const double bt2087[9] = {0.6274, 0.3293, 0.0433, 0.0691, 0.9195, 0.0114, 0.0164, 0.0880, 0.8956};
        for (int i = 0; i < 9; ++i) QVERIFY(std::fabs(m[i] - bt2087[i]) < 2e-4);
        // ACES AP1 (D60 white) is adapted: its white lands on D65 white.
        primariesMatrix(Primaries::Ap1, Primaries::Bt709, m);
        for (int r = 0; r < 3; ++r) QVERIFY(std::fabs(m[r * 3] + m[r * 3 + 1] + m[r * 3 + 2] - 1) < 2e-3);

        const ColorSpace& sdr = *findColorSpace("rec709");
        const ColorSpace& pq = *findColorSpace("rec2100pq");
        const ColorSpace& hlg = *findColorSpace("rec2100hlg");
        auto through = [](float v, const ColorSpace& a, const ColorSpace& b) {
            float px[3] = {v, v, v};
            convertPixel(px, a, b);
            return px[0];
        };
        // SDR white sits at HDR reference white: 203 nits in PQ, 75 % in HLG (BT.2408).
        QVERIFY(near(through(1, sdr, pq), 0.5807f, 0.001f));
        QVERIFY(near(through(1, sdr, hlg), 0.75f, 0.002f));
        // HDR to SDR: reference white stays bright, the 1000-nit peak reaches white, and
        // the curve keeps the order of tones.
        QVERIFY(through(0.5807f, pq, sdr) > 0.94f);
        QVERIFY(through(0.75f, hlg, sdr) > 0.94f);
        QVERIFY(near(through(0.7518f, pq, sdr), 1.0f, 0.005f));
        float last = 0;
        for (float v = 0.05f; v <= 0.75f; v += 0.05f) {
            const float o = through(v, pq, sdr);
            QVERIFY(o >= last);
            last = o;
        }
        // Mid tones of SDR pass through PQ and back almost unchanged.
        QVERIFY(near(through(through(0.5f, sdr, pq), pq, sdr), 0.5f, 0.02f));
        // Pure Rec.709 red in BT.2020 is a less saturated red (BT.2087).
        float red[3] = {1, 0, 0};
        convertPixel(red, sdr, *findColorSpace("rec2020"));
        QVERIFY(near(red[0], float(std::pow(0.6274, 1 / 2.4)), 0.005f));
        QVERIFY(near(red[1], float(std::pow(0.0691, 1 / 2.4)), 0.005f));
        // Camera log gets a display rendering: 18 % grey lands in the mid tones.
        const float grey = through(float(420.0 / 1023), *findColorSpace("slog3-sgamut3cine"), sdr);
        QVERIFY2(grey > 0.36f && grey < 0.5f, qPrintable(QString::number(grey)));
        // Tags.
        QCOMPARE(colorSpaceFromTags("bt2020", "smpte2084"), std::string("rec2100pq"));
        QCOMPARE(colorSpaceFromTags("bt2020", "arib-std-b67"), std::string("rec2100hlg"));
        QCOMPARE(colorSpaceFromTags("bt2020", "bt2020-10"), std::string("rec2020"));
        QCOMPARE(colorSpaceFromTags("bt709", "iec61966-2-1"), std::string("rec709"));
        QCOMPARE(colorSpaceFromTags("unknown", "unknown"), std::string("rec709"));
    }

    void cameraLogMatchesOpenColorIO() {
        if (!ocioAvailable()) QSKIP("Built without OpenColorIO");
        // Each camera space against OCIO's reference transform to ACES, compared in XYZ D65.
        const std::pair<const char*, const char*> cases[] = {
            {"SONY_SLOG3-SGAMUT3.CINE_to_ACES2065-1", "slog3-sgamut3cine"},
            {"ARRI_ALEXA-LOGC-EI800-AWG_to_ACES2065-1", "logc3-awg3"},
            {"PANASONIC_VLOG-VGAMUT_to_ACES2065-1", "vlog-vgamut"},
            {"CANON_CLOG3-CGAMUT_to_ACES2065-1", "clog3-cinemagamut"},
            {"ACEScct_to_ACES2065-1", "acescct"},
            {"IDENTITY", "aces2065-1"}};
        const float codes[4][3] = {{0.2f, 0.4f, 0.6f}, {0.5f, 0.5f, 0.5f}, {0.7f, 0.3f, 0.15f}, {0.1f, 0.12f, 0.9f}};
        for (const auto& [style, id] : cases) {
            const ColorSpace* cs = findColorSpace(id);
            QVERIFY(cs);
            float ref[12];
            std::memcpy(ref, codes, sizeof ref);
            std::string err;
            QVERIFY2(applyOcioBuiltin(style, ref, 4, false, &err), err.c_str());
            QVERIFY2(applyOcioBuiltin("UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD", ref, 4, false, &err), err.c_str());
            double m[9];
            primariesToXyz(cs->primaries, m);
            for (int k = 0; k < 4; ++k) {
                double l[3];
                for (int j = 0; j < 3; ++j) l[j] = toLinear(cs->transfer, codes[k][j]);
                for (int r = 0; r < 3; ++r) {
                    const double mine = m[r * 3] * l[0] + m[r * 3 + 1] * l[1] + m[r * 3 + 2] * l[2];
                    QVERIFY2(std::fabs(mine - ref[k * 3 + r]) < 1e-4 + 1e-4 * std::fabs(mine),
                             qPrintable(QString("%1: %2 vs OCIO %3").arg(id).arg(mine).arg(ref[k * 3 + r])));
                }
            }
        }
        // PQ, in OCIO's units (1.0 = 100 nits).
        float pq[3] = {0.5f, 0.58f, 0.75f};
        QVERIFY(applyOcioBuiltin("CURVE - ST-2084_to_LINEAR", pq, 1));
        for (int i = 0; i < 3; ++i) QVERIFY(std::fabs(toLinear(Transfer::Pq, (i == 0 ? 0.5 : i == 1 ? 0.58 : 0.75)) * 2.03 - pq[i]) < 1e-4 * pq[i]);
    }

    void ocioTransformEffect() {
        if (!ocioAvailable()) QSKIP("Built without OpenColorIO");
        QTemporaryDir dir;
        const QString cfg = dir.filePath("test.ocio");
        QFile f(cfg);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"(ocio_profile_version: 2
roles:
  default: linear
  scene_linear: linear
file_rules:
  - !<Rule> {name: Default, colorspace: default}
displays:
  Monitor:
    - !<View> {name: Gamma, colorspace: gamma_display}
active_displays: []
active_views: []
looks:
  - !<Look>
    name: Brighter
    process_space: linear
    transform: !<MatrixTransform> {matrix: [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1]}
colorspaces:
  - !<ColorSpace>
    name: linear
  - !<ColorSpace>
    name: half
    from_scene_reference: !<MatrixTransform> {matrix: [0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 1]}
  - !<ColorSpace>
    name: gamma_display
    from_scene_reference: !<ExponentTransform> {value: [2.2, 2.2, 2.2, 1], direction: inverse}
)");
        f.close();
        Project p = makeDefaultProject();
        Effect e = makeEffect(p, "ocio");
        e.strings["config"] = cfg.toStdString();
        QCOMPARE(ocioChoices(e, "src"), (std::vector<std::string>{"linear", "half", "gamma_display"}));
        QCOMPARE(ocioChoices(e, "display"), std::vector<std::string>{"Monitor"});
        QCOMPARE(ocioChoices(e, "view"), std::vector<std::string>{"Gamma"});
        QCOMPARE(ocioChoices(e, "look"), (std::vector<std::string>{"", "Brighter"}));

        // Colour space to colour space, on premultiplied pixels.
        e.strings["src"] = "linear";
        e.strings["dst"] = "half";
        Image img = solid(4, 2, 0.8f, 0.8f, 0.8f, 0.5f);  // straight 0.8, half covered
        applyVideoEffect(e, 0, img, 1.0);
        float c[4];
        rgb(img, 1, 1, c);
        QVERIFY2(near(c[0], 0.4f, 1e-4f) && near(c[3], 0.5f, 1e-6f), qPrintable(QString("%1 %2").arg(c[0]).arg(c[3])));
        e.params["inverse"] = Param(1.0);
        img = solid(4, 2, 0.4f, 0.4f, 0.4f);
        applyVideoEffect(e, 0, img, 1.0);
        rgb(img, 0, 0, c);
        QVERIFY(near(c[0], 0.8f, 1e-4f));
        e.params["inverse"] = Param(0.0);
        // Display / view, with a look.
        e.strings["mode"] = "Display / View";
        img = solid(2, 2, 0.5f, 0.5f, 0.5f);
        applyVideoEffect(e, 0, img, 1.0);
        rgb(img, 0, 0, c);
        QVERIFY(near(c[0], float(std::pow(0.5, 1 / 2.2)), 1e-4f));
        e.strings["look"] = "Brighter";
        img = solid(2, 2, 0.25f, 0.25f, 0.25f);
        applyVideoEffect(e, 0, img, 1.0);
        rgb(img, 0, 0, c);
        QVERIFY(near(c[0], float(std::pow(0.5, 1 / 2.2)), 1e-4f));
        // A broken config leaves the picture alone.
        e.strings["config"] = dir.filePath("missing.ocio").toStdString();
        img = solid(2, 2, 0.3f, 0.3f, 0.3f);
        std::string err;
        QVERIFY(!applyOcio(e, img, &err));
        QVERIFY(!err.empty());
        rgb(img, 0, 0, c);
        QVERIFY(near(c[0], 0.3f, 1e-6f));
    }

    void colorSpaceTransformEffect() {
        Project p = makeDefaultProject();
        Effect e = makeEffect(p, "color_space_transform");
        e.strings["from"] = findColorSpace("slog3-sgamut3cine")->label;
        e.strings["to"] = findColorSpace("rec709")->label;
        const float grey = float(420.0 / 1023);
        Image img = solid(2, 2, grey, grey, grey);
        applyVideoEffect(e, 0, img, 1.0);
        float c[4];
        rgb(img, 0, 0, c);
        float px[3] = {grey, grey, grey};
        convertPixel(px, *findColorSpace("slog3-sgamut3cine"), *findColorSpace("rec709"));
        QVERIFY(near(c[0], px[0], 0.005f));
        QVERIFY(c[0] > 0.36f && c[0] < 0.5f);
    }

    void objectMaskMatte() {
        // Exact distances, checked against brute force on scattered seeds.
        const int w = 37, h = 23;
        std::vector<uint8_t> seed(size_t(w) * h, 0);
        for (int i = 0; i < 9; ++i) seed[size_t((i * 7919) % (w * h))] = 1;
        const std::vector<float> d = distanceTransform(seed, w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double best = 1e9;
                for (int k = 0; k < w * h; ++k)
                    if (seed[size_t(k)]) best = std::min(best, std::hypot(x - k % w, y - k / w));
                QVERIFY2(std::fabs(d[size_t(y) * w + size_t(x)] - best) < 1e-4, qPrintable(QString("%1,%2").arg(x).arg(y)));
            }
        QVERIFY(distanceTransform(std::vector<uint8_t>(size_t(w) * h, 0), w, h)[5] > 1e9);  // no seeds: far away

        // Logits of a disc of radius 40.3 grid cells: 100.75 px in a 640 x 360 picture (the
        // grid spans the frame, so its cells are 2.5 px wide and 1.40625 px tall), centred.
        std::vector<float> logits(size_t(kObjectGrid) * kObjectGrid);
        for (int y = 0; y < kObjectGrid; ++y)
            for (int x = 0; x < kObjectGrid; ++x)
                logits[size_t(y) * kObjectGrid + size_t(x)] = float(40.3 - std::hypot(x + 0.5 - 128, (y + 0.5 - 128) * 360 / 640.0)) * 0.6f;
        auto area = [](const std::vector<float>& m) {
            double a = 0;
            for (float v : m) a += v;
            return a;
        };
        auto radius = [&](const std::vector<float>& m) { return std::sqrt(area(m) / M_PI); };
        const std::vector<float> sharp = objectMatte(logits, 640, 360, 1, 0);
        QVERIFY2(std::fabs(radius(sharp) - 100.75) < 0.5, qPrintable(QString::number(radius(sharp))));
        // The edge is placed to a fraction of a pixel: a row's coverage is the chord's length.
        double row = 0;
        for (int x = 0; x < 640; ++x) row += sharp[size_t(180) * 640 + size_t(x)];
        QVERIFY2(std::fabs(row - 2 * std::sqrt(100.75 * 100.75 - 0.25)) < 0.3, qPrintable(QString::number(row)));
        // Expansion grows the edge outwards by that many pixels, contraction shrinks it.
        QVERIFY2(std::fabs(radius(objectMatte(logits, 640, 360, 1, 12)) - 112.75) < 0.6,
                 qPrintable(QString::number(radius(objectMatte(logits, 640, 360, 1, 12)))));
        QVERIFY(std::fabs(radius(objectMatte(logits, 640, 360, 1, -20)) - 80.75) < 0.6);
        // Feathering keeps the area but softens the edge over its width.
        const std::vector<float> soft = objectMatte(logits, 640, 360, 30, 0);
        QVERIFY2(std::fabs(radius(soft) - 100.75) < 1.5, qPrintable(QString::number(radius(soft))));
        QVERIFY(soft[size_t(180) * 640 + 320 + 100] > 0.4f && soft[size_t(180) * 640 + 320 + 100] < 0.6f);
        QVERIFY(soft[size_t(180) * 640 + 320 + 90] > 0.9f && soft[size_t(180) * 640 + 320 + 112] < 0.1f);
        // Through an effect: only frames that were segmented, inverted on request.
        Project p = makeDefaultProject();
        Effect e = makeEffect(p, "invert");
        e.params["mask.shape"] = Param(3.0);
        e.params["mask.feather"] = Param(1.0);
        auto obj = std::make_shared<ObjectMask>();
        obj->fps = 25;
        obj->frames[10] = packObjectLogits(logits.data());
        e.object = obj;
        Image img(640, 360);
        img.fill(0.2f, 0.4f, 0.6f, 1);
        QVERIFY(std::fabs(area(effectMatte(e, 0, img, 1.0, 10.2 / 25)) - M_PI * 100.75 * 100.75) < 300);
        QCOMPARE(area(effectMatte(e, 0, img, 1.0, 11.2 / 25)), 0.0);
        QCOMPARE(area(effectMatte(e, 0, img, 1.0, -1)), 0.0);  // not footage (a still or generator)
        e.params["mask.invert"] = Param(1.0);
        QVERIFY(std::fabs(area(effectMatte(e, 0, img, 1.0, 10.2 / 25)) - (640 * 360 - M_PI * 100.75 * 100.75)) < 300);
        // The effect applies inside the object only.
        e.params["mask.invert"] = Param(0.0);
        applyVideoEffect(e, 0, img, 1.0, 10.2 / 25);
        QVERIFY(std::fabs(img.at(320, 180)[0] - 0.8f) < 1e-4 && std::fabs(img.at(20, 20)[0] - 0.2f) < 1e-4);
    }

    void multicamShowsOneAngle() {
        // A multicam sequence with a red and a blue angle (the blue one on top).
        Project p = makeDefaultProject();
        Sequence mc = makeSequence(p, "MC", 64, 36, {30, 1}, 2, 0);
        mc.multicam = true;
        mc.videoTracks[0].clips.push_back(colorClip(p, 1, 0, 0, 0, 60));
        mc.videoTracks[1].clips.push_back(colorClip(p, 0, 0, 1, 0, 60));
        mc.videoTracks[0].muted = true;  // a hidden angle still shows when chosen
        MediaItem m;
        m.id = p.newId();
        m.kind = MediaKind::Sequence;
        m.sequenceId = mc.id;
        m.hasVideo = true;
        m.width = 64;
        m.height = 36;
        p.sequences.push_back(mc);
        p.media.push_back(m);
        Sequence& s = *p.active();
        s.width = 64;
        s.height = 36;
        Clip c = makeClip(p, m, TrackKind::Video, s);
        c.duration = 60;
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        RenderOptions o;
        float px[4];
        rgb(renderProgramFrame(p, s, 5, o), 32, 18, px);
        QVERIFY(near(px[0], 1) && near(px[2], 0));  // angle 0: red, not the blue track above it
        trackAt(s, {TrackKind::Video, 0})->clips[0].angle = 1;
        rgb(renderProgramFrame(p, s, 5, o), 32, 18, px);
        QVERIFY(near(px[0], 0) && near(px[2], 1));
        // Out-of-range angles fall back to the last one.
        trackAt(s, {TrackKind::Video, 0})->clips[0].angle = 7;
        rgb(renderProgramFrame(p, s, 5, o), 32, 18, px);
        QVERIFY(near(px[2], 1));
        // The same sequence nested as an ordinary compound shows every visible track.
        p.findSequence(mc.id)->multicam = false;
        rgb(renderProgramFrame(p, s, 5, o), 32, 18, px);
        QVERIFY(near(px[2], 1) && near(px[0], 0));
    }

    void colorManagedCompositing() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64;
        s.height = 36;
        edit::overwrite(p, s, {TrackKind::Video, 0}, colorClip(p, 1, 1, 1, 0, 30));
        RenderOptions o;
        float c[4];
        rgb(renderProgramFrame(p, s, 5, o), 32, 18, c);
        QVERIFY(near(c[0], 1));
        // In an HDR10 sequence graphics white is reference white, not the 10 000-nit peak.
        s.colorSpace = "rec2100pq";
        rgb(renderProgramFrame(p, s, 5, o), 32, 18, c);
        QVERIFY2(near(c[0], 0.5807f, 0.003f), qPrintable(QString::number(c[0])));
        // The viewer previews it tone mapped to SDR.
        o.displaySpace = "rec709";
        rgb(renderProgramFrame(p, s, 5, o), 32, 18, c);
        QVERIFY(c[0] > 0.94f && c[0] <= 1.0f);
        o.displaySpace.clear();
        // A compound clip keeps its parent's space; a Rec.709 one nested in HDR is converted.
        s.colorSpace = "rec709";
        const Id outerId = s.id;
        auto r = edit::makeCompound(p, s, {s.videoTracks[0].clips[0].id}, "Nested");
        QVERIFY(r.ok);
        Sequence& outer = *p.findSequence(outerId);
        const Clip& nc = outer.videoTracks[0].clips.at(0);
        const Sequence* inner = p.findSequence(p.findMedia(nc.mediaId)->sequenceId);
        QVERIFY(inner && inner != &outer);
        QCOMPARE(inner->colorSpace, std::string("rec709"));
        outer.colorSpace = "rec2100hlg";
        rgb(renderProgramFrame(p, outer, 5, o), 32, 18, c);
        QVERIFY2(near(c[0], 0.75f, 0.005f), qPrintable(QString::number(c[0])));
    }

    void drawnAndGradientMasks() {
        // Coverage is exact across a pixel: a 10.5 x 10 rectangle with its sides a quarter into pixels.
        const std::vector<float> cov = polygonCoverage({{10.25, 10}, {20.75, 10}, {20.75, 20}, {10.25, 20}}, 32, 32);
        double total = 0;
        for (float v : cov) total += v;
        QVERIFY(std::fabs(total - 105) < 1e-3);
        QVERIFY(std::fabs(cov[15 * 32 + 10] - 0.75f) < 1e-5 && std::fabs(cov[15 * 32 + 20] - 0.75f) < 1e-5);
        QCOMPARE(cov[15 * 32 + 15], 1.0f);

        // A triangle drawn on a 100 x 50 frame: (10, 5.5) (90, 5.5) (50, 45.5), so 1600 px.
        Image img = solid(100, 50, 0.2f, 0.2f, 0.2f);
        Effect e = makeEffect("invert", 1);
        setMaskPathFromFrame(e, {{0.1, 0.11}, {0.9, 0.11}, {0.5, 0.91}}, 100, 50, false);
        e.params["mask.feather"] = 0.0;
        std::vector<float> m = effectMatte(e, 0, img, 1.0);
        double area = 0;
        for (float v : m) area += v;
        QVERIFY2(std::fabs(area - 1600) < 20, qPrintable(QString::number(area)));
        QCOMPARE(m[20 * 100 + 50], 1.0f);
        QCOMPARE(m[40 * 100 + 20], 0.0f);
        QCOMPARE(m[2 * 100 + 50], 0.0f);
        QVERIFY2(std::fabs(m[5 * 100 + 50] - 0.5f) < 0.05f, qPrintable(QString::number(m[5 * 100 + 50])));  // the edge halves row 5
        QCOMPARE(m[8 * 100 + 15], 1.0f);
        Image fx = img;
        applyVideoEffect(e, 0, fx, 1.0);
        QVERIFY(std::fabs(fx.at(50, 20)[0] - 0.8f) < 0.01f);
        QVERIFY(std::fabs(fx.at(5, 40)[0] - 0.2f) < 0.01f);
        // Feathered, it fades out past the edge.
        e.params["mask.feather"] = 10.0;
        m = effectMatte(e, 0, img, 1.0);
        QCOMPARE(m[25 * 100 + 50], 1.0f);
        QVERIFY2(m[2 * 100 + 50] > 0.02f && m[2 * 100 + 50] < 0.5f, qPrintable(QString::number(m[2 * 100 + 50])));
        // The box carries the path: turned half round, the triangle points up.
        e.params["mask.feather"] = 0.0;
        e.params["mask.rotation"] = 180.0;
        m = effectMatte(e, 0, img, 1.0);
        QCOMPARE(m[8 * 100 + 15], 0.0f);
        QCOMPARE(m[42 * 100 + 15], 1.0f);
        // Fewer than three points select nothing.
        setMaskPath(e, 0, {{0, 0}, {0.5, 0.5}});
        m = effectMatte(e, 0, img, 1.0);
        QCOMPARE(*std::max_element(m.begin(), m.end()), 0.0f);

        // A gradient: all above the box, none below, half way in its middle; turned, it runs across.
        Effect g = makeEffect("invert", 1);
        g.params["mask.shape"] = 6.0;
        g.params["mask.feather"] = 0.0;
        m = effectMatte(g, 0, img, 1.0);
        QCOMPARE(m[5 * 100 + 50], 1.0f);
        QCOMPARE(m[45 * 100 + 50], 0.0f);
        QVERIFY2(std::fabs(m[25 * 100 + 50] - 0.5f) < 0.06f, qPrintable(QString::number(m[25 * 100 + 50])));
        for (int y = 1; y < 50; ++y) QVERIFY(m[size_t(y) * 100 + 50] <= m[size_t(y - 1) * 100 + 50]);
        g.params["mask.rotation"] = 90.0;
        m = effectMatte(g, 0, img, 1.0);
        QCOMPARE(m[25 * 100 + 95], 1.0f);
        QCOMPARE(m[25 * 100 + 5], 0.0f);
    }

    void colourWarper() {
        // Red, a grey and a blue: red's full-saturation point moved a third of the way round makes it green.
        const float colours[3][3] = {{1, 0, 0}, {0.5f, 0.5f, 0.5f}, {0, 0, 1}};
        Image src(3, 1);
        for (int i = 0; i < 3; ++i) {
            std::copy(colours[i], colours[i] + 3, src.at(i, 0));
            src.at(i, 0)[3] = 1;
        }
        Effect e = makeEffect("color_warper", 1);
        e.strings["mesh"] = "0,4,120,0,0";
        Image img = src;
        applyVideoEffect(e, 0, img, 1);
        QVERIFY(img.at(0, 0)[0] < 0.01f && img.at(0, 0)[1] > 0.99f && img.at(0, 0)[2] < 0.01f);
        for (int i : {1, 2})
            for (int c = 0; c < 3; ++c) QVERIFY(std::fabs(img.at(i, 0)[c] - colours[i][c]) < 1e-5f);
        // Half mixed; at red's brightness (the green as light as the red was); a point that darkens by a stop.
        e.params["mix"] = 50.0;
        img = src;
        applyVideoEffect(e, 0, img, 1);
        QVERIFY(std::fabs(img.at(0, 0)[0] - 0.5f) < 0.01f && std::fabs(img.at(0, 0)[1] - 0.5f) < 0.01f);
        e.params["mix"] = 100.0;
        e.params["preserve_luma"] = 1.0;
        img = src;
        applyVideoEffect(e, 0, img, 1);
        QVERIFY2(std::fabs(img.at(0, 0)[1] - 0.2126f / 0.7152f) < 0.005f, qPrintable(QString::number(img.at(0, 0)[1])));
        e.params["preserve_luma"] = 0.0;
        e.strings["mesh"] = "8,4,0,0,-1";  // blue's point, a stop darker
        img = src;
        applyVideoEffect(e, 0, img, 1);
        QVERIFY(std::fabs(img.at(2, 0)[2] - 0.5f) < 0.005f && std::fabs(img.at(0, 0)[0] - 1) < 1e-5f);
    }

    void rollingShutterRepair() {
        // The picture moves right by a tenth of the width a frame; read out over 80 % of a frame, a vertical bar
        // leans (lower rows were read later, so further on): 20 px a frame, 16 px from top to bottom of 200 x 100.
        CameraMotion cm;
        cm.fps = 25;
        cm.steps.resize(10);
        for (size_t i = 1; i < cm.steps.size(); ++i) cm.steps[i].tx = 0.1;
        auto leaning = [] {
            Image img(200, 100);
            for (int y = 0; y < 100; ++y) {
                const double centre = 100 + 20 * ((y + 0.5) / 100 - 0.5) * 0.8;
                for (int x = 0; x < 200; ++x) {
                    const float v = float(std::clamp(3 - std::fabs(x + 0.5 - centre), 0.0, 1.0));
                    float* p = img.at(x, y);
                    p[0] = p[1] = p[2] = v;
                    p[3] = 1;
                }
            }
            return img;
        };
        auto bar = [](const Image& img, int y) {
            double sum = 0, at = 0;
            for (int x = 0; x < img.width; ++x) {
                sum += img.at(x, y)[0];
                at += img.at(x, y)[0] * (x + 0.5);
            }
            return sum > 0 ? at / sum : -1.0;
        };
        Image img = leaning();
        QVERIFY(bar(img, 95) - bar(img, 5) > 14);
        Effect rs = makeEffect("rolling_shutter", 1);
        rs.strings["motion"] = cameraMotionToString(cm);
        rs.params["readout"] = Param(80.0);
        rs.params["framing"] = Param(1.0);  // show edges: no zoom
        applyVideoEffect(rs, 5, img, 1.0, 5 / 25.0);
        for (int y : {5, 25, 50, 75, 95}) QVERIFY2(std::fabs(bar(img, y) - 100) < 0.5, qPrintable(QString("%1: %2").arg(y).arg(bar(img, y))));
        // Too short a readout leaves some lean; Zoom to Fill enlarges just enough to hide the moved edges.
        Image part = leaning();
        rs.params["readout"] = Param(40.0);
        applyVideoEffect(rs, 5, part, 1.0, 5 / 25.0);
        QVERIFY2(std::fabs((bar(part, 95) - bar(part, 5)) - 7.2) < 0.6, qPrintable(QString::number(bar(part, 95) - bar(part, 5))));  // half of 14.4
        Image filled = leaning();
        rs.params["readout"] = Param(80.0);
        rs.params["framing"] = Param(0.0);
        applyVideoEffect(rs, 5, filled, 1.0, 5 / 25.0);
        QVERIFY(std::fabs(bar(filled, 50) - 100) < 0.5);
        QVERIFY(filled.at(0, 0)[3] > 0.99f && filled.at(199, 99)[3] > 0.99f);  // no empty corners
        // Stabilize's own Rolling Shutter: off by default, so nothing straightens.
        Effect stab = makeEffect("stabilize", 2);
        stab.strings["motion"] = cameraMotionToString(cm);
        stab.params["framing"] = Param(1.0);
        Image still = leaning();
        applyVideoEffect(stab, 5, still, 1.0, 5 / 25.0);
        QVERIFY(bar(still, 95) - bar(still, 5) > 14);
    }

    void effectMasks() {
        auto masked = [](const char* type, int shape) {
            Effect e = makeEffect(type, 1);
            e.params["mask.shape"] = double(shape);
            e.params["mask.x"] = 0.5;
            e.params["mask.y"] = 0.5;
            e.params["mask.w"] = 0.5;
            e.params["mask.h"] = 0.5;
            e.params["mask.feather"] = 1.0;
            return e;
        };
        // An ellipse limits Invert to the middle of the frame.
        Image img = solid(100, 50, 0.2f, 0.2f, 0.2f);
        Effect inv = masked("invert", 1);
        applyVideoEffect(inv, 0, img, 1.0);
        QVERIFY(std::fabs(img.at(50, 25)[0] - 0.8f) < 0.01f);
        QVERIFY(std::fabs(img.at(2, 2)[0] - 0.2f) < 0.01f);
        QVERIFY(std::fabs(img.at(50, 2)[0] - 0.2f) < 0.01f);  // above the ellipse (semi-axis 12.5 px)
        // The matte: 1 inside, 0 outside, a one-pixel edge at x = 50 + 25.
        std::vector<float> m = effectMatte(inv, 0, solid(100, 50, 0.2f, 0.2f, 0.2f), 1.0);
        QCOMPARE(m.size(), size_t(5000));
        QCOMPARE(m[25 * 100 + 50], 1.0f);
        QCOMPARE(m[0], 0.0f);
        QVERIFY(m[25 * 100 + 74] > 0.9f && m[25 * 100 + 76] < 0.1f);
        // Feather makes a gradient that falls outward.
        inv.params["mask.feather"] = 30.0;
        m = effectMatte(inv, 0, img, 1.0);
        QVERIFY(m[25 * 100 + 60] > m[25 * 100 + 70] && m[25 * 100 + 70] > m[25 * 100 + 80] && m[25 * 100 + 80] > m[25 * 100 + 95]);
        // Expansion grows the shape; invert and opacity.
        inv.params["mask.feather"] = 1.0;
        inv.params["mask.expansion"] = 10.0;
        QCOMPARE(effectMatte(inv, 0, img, 1.0)[25 * 100 + 80], 1.0f);
        inv.params["mask.expansion"] = 0.0;
        inv.params["mask.invert"] = 1.0;
        inv.params["mask.opacity"] = 50.0;
        m = effectMatte(inv, 0, img, 1.0);
        QCOMPARE(m[25 * 100 + 50], 0.0f);
        QCOMPARE(m[0], 0.5f);
        // Feather and expansion are in sequence pixels: a half-size image halves them.
        inv.params["mask.invert"] = 0.0;
        inv.params["mask.opacity"] = 100.0;
        inv.params["mask.expansion"] = 10.0;
        QCOMPARE(effectMatte(inv, 0, solid(50, 25, 0.2f, 0.2f, 0.2f), 0.5)[12 * 50 + 40], 1.0f);
        inv.params["mask.expansion"] = 0.0;

        // A rectangle turned 90 degrees: 40 px tall (clipped by the frame), 5 px wide either side.
        Effect rect = masked("invert", 2);
        rect.params["mask.w"] = 0.8;
        rect.params["mask.h"] = 0.2;
        rect.params["mask.rotation"] = 90.0;
        m = effectMatte(rect, 0, img, 1.0);
        QCOMPARE(m[3 * 100 + 50], 1.0f);
        QCOMPARE(m[25 * 100 + 53], 1.0f);
        QCOMPARE(m[25 * 100 + 70], 0.0f);

        // The HSL qualifier picks the red half; Black & White greys only that.
        Image two(100, 10);
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 100; ++x) {
                float* p = two.at(x, y);
                p[0] = x < 50 ? 0.9f : 0.1f;
                p[1] = 0.1f;
                p[2] = x < 50 ? 0.1f : 0.9f;
                p[3] = 1;
            }
        Effect bw = makeEffect("black_white", 2);
        bw.params["mask.qualify"] = 1.0;
        bw.params["mask.hue"] = 0.0;
        bw.params["mask.hue_width"] = 40.0;
        Image q = two;
        applyVideoEffect(bw, 0, q, 1.0);
        QVERIFY(std::fabs(q.at(10, 5)[0] - q.at(10, 5)[2]) < 0.02f);  // red became grey
        QCOMPARE(q.at(90, 5)[2], 0.9f);                                // blue untouched
        // Show Mask puts the matte on screen.
        bw.params["mask.show"] = 1.0;
        q = two;
        applyVideoEffect(bw, 0, q, 1.0);
        QCOMPARE(q.at(10, 5)[0], 1.0f);
        QCOMPARE(q.at(90, 5)[0], 0.0f);
        // No mask: the whole frame.
        Image all = two;
        applyVideoEffect(makeEffect("black_white", 3), 0, all, 1.0);
        QVERIFY(std::fabs(all.at(90, 5)[0] - all.at(90, 5)[2]) < 0.02f);
        QVERIFY(supportsMask("gaussian_blur"));
        QVERIFY(!supportsMask("compressor"));
    }

    void clipFrameMapping() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 1920;
        s.height = 1080;
        Clip c = makeGeneratorClip(p, "color", 30);
        c.motion.params["scale"] = 50.0;
        c.motion.params["rotation"] = 30.0;
        c.motion.params["pos_x"] = 200.0;
        c.motion.params["pos_y"] = -100.0;
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        const Clip& placed = s.videoTracks[0].clips[0];
        double x = 0, y = 0, u = 0, v = 0;
        // The frame centre sits at the clip's position.
        QVERIFY(clipFrameToSequence(p, s, placed, 5, 0.5, 0.5, x, y));
        QVERIFY(std::fabs(x - (960 + 200)) < 1e-6 && std::fabs(y - (540 - 100)) < 1e-6);
        // Round trip.
        QVERIFY(clipFrameToSequence(p, s, placed, 5, 0.9, 0.2, x, y));
        QVERIFY(sequenceToClipFrame(p, s, placed, 5, x, y, u, v));
        QVERIFY(std::fabs(u - 0.9) < 1e-9 && std::fabs(v - 0.2) < 1e-9);
        // Half scale: the right edge is half a frame width from the centre, turned 30 degrees.
        QVERIFY(clipFrameToSequence(p, s, placed, 5, 1.0, 0.5, x, y));
        QVERIFY(std::fabs(std::hypot(x - 1160, y - 440) - 480) < 1e-6);
        QVERIFY(std::fabs(std::atan2(y - 440, x - 1160) * 180 / M_PI - 30) < 1e-6);
        double w = 0, h = 0;
        QVERIFY(clipFrameSize(p, s, placed, w, h));
        QCOMPARE(w, 1920.0);
    }

    void blendModes() {
        Image dst = solid(4, 4, 0, 0, 1);
        blendOnto(dst, solid(4, 4, 1, 0, 0), "normal", 0.5f);
        float c[4];
        rgb(dst, 1, 1, c);
        QVERIFY(near(c[0], 0.5f) && near(c[1], 0) && near(c[2], 0.5f) && near(c[3], 1));

        Image m = solid(2, 2, 0.5f, 0.5f, 0.5f);
        blendOnto(m, solid(2, 2, 0.5f, 1, 0), "multiply", 1);
        rgb(m, 0, 0, c);
        QVERIFY(near(c[0], 0.25f) && near(c[1], 0.5f) && near(c[2], 0));

        Image s = solid(2, 2, 0.5f, 0.5f, 0.5f);
        blendOnto(s, solid(2, 2, 0.5f, 0.5f, 0.5f), "screen", 1);
        rgb(s, 0, 0, c);
        QVERIFY(near(c[0], 0.75f));

        Image d = solid(2, 2, 0.8f, 0.2f, 0.5f);
        blendOnto(d, solid(2, 2, 0.3f, 0.3f, 0.5f), "difference", 1);
        rgb(d, 0, 0, c);
        QVERIFY(near(c[0], 0.5f) && near(c[1], 0.1f) && near(c[2], 0));

        // Transparent source leaves the destination untouched.
        Image t = solid(2, 2, 0.1f, 0.2f, 0.3f);
        blendOnto(t, Image(2, 2), "overlay", 1);
        rgb(t, 0, 0, c);
        QVERIFY(near(c[0], 0.1f) && near(c[2], 0.3f));
    }

    void colorCorrection() {
        Project p;
        Image img = solid(8, 8, 0.8f, 0.4f, 0.2f);
        Effect cc = makeEffect(p, "color_correct");
        Image same = img;
        applyVideoEffect(cc, 0, same, 1);
        float c[4];
        rgb(same, 3, 3, c);
        QVERIFY(near(c[0], 0.8f, 0.005f) && near(c[1], 0.4f, 0.005f) && near(c[2], 0.2f, 0.005f));
        cc.params["saturation"] = 0.0;
        applyVideoEffect(cc, 0, img, 1);
        rgb(img, 3, 3, c);
        QVERIFY(near(c[0], c[1], 0.001f) && near(c[1], c[2], 0.001f));
        // Exposure +1 stop doubles linear-ish values.
        Image e = solid(2, 2, 0.2f, 0.2f, 0.2f);
        Effect ex = makeEffect(p, "color_correct");
        ex.params["exposure"] = 1.0;
        ex.params["pivot"] = 0.0;
        applyVideoEffect(ex, 0, e, 1);
        rgb(e, 0, 0, c);
        QVERIFY(near(c[0], 0.4f, 0.01f));
        // Keyframed parameters evaluate per frame.
        Effect kf = makeEffect(p, "invert");
        kf.params["amount"].addKey(0, 0);
        kf.params["amount"].addKey(10, 100);
        Image k0 = solid(2, 2, 0.2f, 0.2f, 0.2f), k10 = k0;
        applyVideoEffect(kf, 0, k0, 1);
        applyVideoEffect(kf, 10, k10, 1);
        rgb(k0, 0, 0, c);
        QVERIFY(near(c[0], 0.2f));
        rgb(k10, 0, 0, c);
        QVERIFY(near(c[0], 0.8f));
    }

    void toneControls() {
        Project p;
        // A grey ramp, 0 to 1.2 across 121 pixels (a little over white, as footage can be).
        auto ramp = [] {
            Image img(121, 1);
            for (int x = 0; x <= 120; ++x) {
                float* q = img.at(x, 0);
                q[0] = q[1] = q[2] = x / 100.0f;
                q[3] = 1;
            }
            return img;
        };
        auto at = [](const Image& img, float v) { return img.at(int(std::lround(v * 100)), 0)[1]; };
        auto toned = [&](const char* name, double v) {
            Image img = ramp();
            Effect cc = makeEffect(p, "color_correct");
            cc.params[name] = v;
            applyVideoEffect(cc, 0, img, 1);
            return img;
        };
        const Image plain = ramp();
        {
            Image same = plain;
            applyVideoEffect(makeEffect(p, "color_correct"), 0, same, 1);
            for (int x = 0; x <= 120; ++x) QVERIFY(std::fabs(same.at(x, 0)[1] - plain.at(x, 0)[1]) < 1e-6f);
        }
        // Each moves its own part of the range and leaves the far end alone.
        const Image shadowsUp = toned("shadows", 100), highlightsDown = toned("highlights", -100);
        const Image blacksDown = toned("blacks", -100), whitesUp = toned("whites", 100);
        QVERIFY2(at(shadowsUp, 0.25f) - 0.25f > 0.15f && std::fabs(at(shadowsUp, 0.9f) - 0.9f) < 0.005f,
                 qPrintable(QString("%1 %2").arg(at(shadowsUp, 0.25f)).arg(at(shadowsUp, 0.9f))));
        QVERIFY2(0.75f - at(highlightsDown, 0.75f) > 0.15f && std::fabs(at(highlightsDown, 0.1f) - 0.1f) < 0.005f,
                 qPrintable(QString("%1 %2").arg(at(highlightsDown, 0.75f)).arg(at(highlightsDown, 0.1f))));
        QVERIFY(0.05f - at(blacksDown, 0.05f) > 0.08f && std::fabs(at(blacksDown, 0.6f) - 0.6f) < 1e-4f);
        QVERIFY(at(whitesUp, 0.95f) - 0.95f > 0.08f && std::fabs(at(whitesUp, 0.4f) - 0.4f) < 1e-4f);
        // Never turns tones over, even with everything pulled against each other.
        Image fight = ramp();
        Effect cc = makeEffect(p, "color_correct");
        cc.params["shadows"] = -100.0, cc.params["blacks"] = 100.0, cc.params["highlights"] = 100.0, cc.params["whites"] = -100.0;
        applyVideoEffect(cc, 0, fight, 1);
        for (int x = 1; x <= 120; ++x) QVERIFY2(fight.at(x, 0)[1] >= fight.at(x - 1, 0)[1] - 1e-6f, qPrintable(QString::number(x)));
        // Colour holds while brightness moves: an orange in the shadows stays the same orange, lighter.
        Image orange = solid(2, 2, 0.3f, 0.15f, 0.05f);
        Effect lift = makeEffect(p, "color_correct");
        lift.params["shadows"] = 100.0;
        applyVideoEffect(lift, 0, orange, 1);
        float c[4];
        rgb(orange, 0, 0, c);
        QVERIFY2(c[0] > 0.33f && std::fabs(c[1] / c[0] - 0.5f) < 0.01f && std::fabs(c[2] / c[0] - 1 / 6.0f) < 0.01f,
                 qPrintable(QString("%1 %2 %3").arg(c[0]).arg(c[1]).arg(c[2])));
        // Vibrance lifts a muted colour much more than a saturated one.
        auto chroma = [](const float* q) { return std::max({q[0], q[1], q[2]}) - std::min({q[0], q[1], q[2]}); };
        Image muted = solid(2, 2, 0.5f, 0.45f, 0.4f), vivid = solid(2, 2, 0.8f, 0.1f, 0.05f);
        Effect vib = makeEffect(p, "color_correct");
        vib.params["vibrance"] = 100.0;
        const float m0 = chroma(muted.at(0, 0)), v0 = chroma(vivid.at(0, 0));
        applyVideoEffect(vib, 0, muted, 1);
        applyVideoEffect(vib, 0, vivid, 1);
        QVERIFY2(chroma(muted.at(0, 0)) / m0 > 1.6f && chroma(vivid.at(0, 0)) / v0 < 1.1f,
                 qPrintable(QString("%1 %2").arg(chroma(muted.at(0, 0)) / m0).arg(chroma(vivid.at(0, 0)) / v0)));
    }

    void trackMatteKey() {
        // V1 blue; V2 red through V3's matte, a small white square in the middle for the first 30 frames.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 160;
        s.height = 90;
        edit::overwrite(p, s, {TrackKind::Video, 0}, colorClip(p, 0, 0, 1, 0, 60));
        Clip fill = colorClip(p, 1, 0, 0, 0, 60);
        fill.effects.push_back(makeEffect(p, "track_matte"));
        const Id fillId = fill.id;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 1}, fill).ok);
        Clip square = colorClip(p, 1, 1, 1, 0, 30);
        square.motion.params["scale"] = 25.0;
        const Id squareId = square.id;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 2}, square).ok);
        RenderOptions o;
        float c[4];
        auto at = [&](FrameTime t, int x, int y) {
            const Image img = renderProgramFrame(p, s, t, o);
            rgb(img, x, y, c);
            return QString("%1 %2 %3").arg(c[0]).arg(c[1]).arg(c[2]);
        };
        Effect& key = edit::clipById(s, fillId)->effects.back();
        // Red only inside the square, which is not seen itself; blue round it.
        QVERIFY2((at(10, 80, 45), near(c[0], 1) && near(c[1], 0) && near(c[2], 0)), qPrintable(at(10, 80, 45)));
        QVERIFY2((at(10, 10, 10), near(c[0], 0) && near(c[2], 1)), qPrintable(at(10, 10, 10)));
        // Reversed: red round the square, blue inside it.
        key.params["reverse"] = 1.0;
        QVERIFY2((at(10, 80, 45), near(c[0], 0) && near(c[2], 1)), qPrintable(at(10, 80, 45)));
        QVERIFY2((at(10, 10, 10), near(c[0], 1) && near(c[2], 0)), qPrintable(at(10, 10, 10)));
        // No matte (the square has ended): nothing of the fill shows, or all of it reversed.
        QVERIFY2((at(40, 80, 45), near(c[0], 1) && near(c[2], 0)), qPrintable(at(40, 80, 45)));
        key.params["reverse"] = 0.0;
        QVERIFY2((at(40, 80, 45), near(c[0], 0) && near(c[2], 1)), qPrintable(at(40, 80, 45)));
        // Not hidden: the white square is drawn over it.
        key.params["hide"] = 0.0;
        QVERIFY2((at(10, 80, 45), near(c[0], 1) && near(c[1], 1) && near(c[2], 1)), qPrintable(at(10, 80, 45)));
        key.params["hide"] = 1.0;
        // Luma: a mid-grey square lets half the red through.
        Clip* sq = edit::clipById(s, squareId);
        sq->generator.params["color.r"] = sq->generator.params["color.g"] = sq->generator.params["color.b"] = 0.5;
        key.params["composite"] = 1.0;
        at(10, 80, 45);
        QVERIFY2(c[0] > 0.2f && c[0] < 0.8f && near(c[0] + c[2], 1, 0.02f), qPrintable(at(10, 80, 45)));
        // An explicit track number works the same as "the one above"; its own track turns the key off.
        key.params["composite"] = 0.0;
        key.params["track"] = 3.0;
        QVERIFY2((at(10, 10, 10), near(c[2], 1)), qPrintable(at(10, 10, 10)));
        key.params["track"] = 2.0;
        QVERIFY2((at(10, 10, 10), near(c[0], 1) && near(c[2], 0)), qPrintable(at(10, 10, 10)));
    }

    void distortAndStylize() {
        Project p;
        auto fx = [&](const char* type, std::initializer_list<std::pair<const char*, double>> params) {
            if (!findEffectInfo(type)) qFatal("no effect %s", type);
            Effect e = makeEffect(p, type);
            for (const auto& [k, v] : params) e.params[k] = v;
            return e;
        };
        Effect e;
        float c[4];
        // Wave Warp: a white line across the middle becomes a sine, a wave height off at a quarter wave.
        Image line(160, 90);
        for (int x = 0; x < 160; ++x) std::fill_n(line.at(x, 45), 4, 1.0f);
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 160; ++x) line.at(x, y)[3] = 1;
        auto rowOfLine = [](const Image& img, int x) {
            int best = 0;
            for (int y = 0; y < img.height; ++y)
                if (img.at(x, y)[0] > img.at(x, best)[0]) best = y;
            return best;
        };
        Image waved = line;
        e = fx("wave_warp", {{"height", 10.0}, {"width", 40.0}});
        applyVideoEffect(e, 0, waved, 1);
        QVERIFY2(std::abs(rowOfLine(waved, 10) - rowOfLine(waved, 30)) >= 18, qPrintable(QString("%1 %2").arg(rowOfLine(waved, 10)).arg(rowOfLine(waved, 30))));
        QVERIFY(std::abs(rowOfLine(waved, 20) - 45) <= 1);  // where the wave crosses
        // Twirl: the middle stays, a mark off-centre turns round it.
        auto spot = [](int sx, int sy) {
            Image img = solid(160, 90, 0, 0, 0);
            for (int y = sy - 2; y <= sy + 2; ++y)
                for (int x = sx - 2; x <= sx + 2; ++x) std::fill_n(img.at(x, y), 3, 1.0f);
            return img;
        };
        auto centroid = [](const Image& img, double& cx, double& cy) {
            double s = 0;
            cx = cy = 0;
            for (int y = 0; y < img.height; ++y)
                for (int x = 0; x < img.width; ++x) s += img.at(x, y)[0], cx += x * img.at(x, y)[0], cy += y * img.at(x, y)[0];
            cx /= s, cy /= s;
        };
        Image tw = spot(100, 45);
        applyVideoEffect(fx("twirl", {{"angle", 90.0}, {"radius", 100.0}}), 0, tw, 1);
        double mx, my;
        centroid(tw, mx, my);
        QVERIFY2(std::fabs(std::hypot(mx - 80, my - 45) - 20) < 2.5 && std::fabs(my - 45) > 5, qPrintable(QString("%1 %2").arg(mx).arg(my)));
        // Spherize: bulging magnifies the middle (a ramp flattens there), pinching does the opposite.
        auto ramp = [] {
            Image img(160, 90);
            for (int y = 0; y < 90; ++y)
                for (int x = 0; x < 160; ++x) {
                    float* q = img.at(x, y);
                    q[0] = q[1] = q[2] = (x + 0.5f) / 160;
                    q[3] = 1;
                }
            return img;
        };
        Image bulge = ramp(), pinch = ramp();
        applyVideoEffect(fx("spherize", {{"amount", 80.0}, {"radius", 100.0}}), 0, bulge, 1);
        applyVideoEffect(fx("spherize", {{"amount", -80.0}, {"radius", 100.0}}), 0, pinch, 1);
        const float plain = (95 + 0.5f) / 160;
        QVERIFY2(std::fabs(bulge.at(95, 45)[0] - 0.5f) < std::fabs(plain - 0.5f) * 0.7f && std::fabs(pinch.at(95, 45)[0] - 0.5f) > std::fabs(plain - 0.5f) * 1.2f,
                 qPrintable(QString("%1 %2 %3").arg(bulge.at(95, 45)[0]).arg(plain).arg(pinch.at(95, 45)[0])));
        // Ripple: rings move the ramp; frame to frame they travel.
        Image r0 = ramp(), r5 = ramp();
        e = fx("ripple", {{"amplitude", 6.0}, {"wavelength", 30.0}});
        applyVideoEffect(e, 0, r0, 1);
        applyVideoEffect(e, 5, r5, 1);
        QVERIFY(r0.px != ramp().px && r0.px != r5.px);
        // Turbulent Displace: moves pixels by about the amount; the same frame the same, evolution changes it.
        Image d1 = ramp(), d2 = ramp(), d3 = ramp();
        e = fx("turbulent_displace", {{"amount", 10.0}, {"size", 40.0}});
        applyVideoEffect(e, 3, d1, 1);
        applyVideoEffect(e, 3, d2, 1);
        e.params["evolution"] = 90.0;
        applyVideoEffect(e, 3, d3, 1);
        double moved = 0;
        for (int x = 20; x < 140; ++x) moved += std::fabs(d1.at(x, 45)[0] - ramp().at(x, 45)[0]) * 160;
        moved /= 120;
        QVERIFY2(moved > 1 && moved < 10, qPrintable(QString::number(moved)));
        QVERIFY(d1.px == d2.px && d1.px != d3.px);
        // Motion Tile: half-size tiles repeat the frame twice across.
        Image tiles = ramp();
        applyVideoEffect(fx("motion_tile", {{"tile_width", 50.0}, {"tile_height", 50.0}}), 0, tiles, 1);
        QVERIFY(std::fabs(tiles.at(30, 20)[0] - tiles.at(110, 20)[0]) < 0.02f && std::fabs(tiles.at(30, 20)[0] - tiles.at(30, 65)[0]) < 0.02f);
        // Find Edges: flat goes white, a step a dark line.
        Image step = solid(40, 20, 0.2f, 0.2f, 0.2f);
        for (int y = 0; y < 20; ++y)
            for (int x = 20; x < 40; ++x) std::fill_n(step.at(x, y), 3, 0.9f);
        Image edges = step;
        applyVideoEffect(fx("find_edges", {}), 0, edges, 1);
        QVERIFY(near(edges.at(5, 10)[0], 1) && near(edges.at(35, 10)[0], 1) && edges.at(20, 10)[0] < 0.6f);
        // Emboss: flat is mid-grey; the step stands out light on one side.
        Image emb = step;
        applyVideoEffect(fx("emboss", {}), 0, emb, 1);
        QVERIFY(near(emb.at(5, 10)[0], 0.5f) && std::fabs(emb.at(20, 10)[0] - 0.5f) > 0.3f);
        // Halftone: black dots on white, as much ink as the tone.
        Image ht = solid(160, 90, 0.5f, 0.5f, 0.5f);
        applyVideoEffect(fx("halftone", {{"dot_size", 10.0}}), 0, ht, 1);
        double mean = 0;
        int extreme = 0;
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 160; ++x) mean += ht.at(x, y)[0], extreme += ht.at(x, y)[0] < 0.05f || ht.at(x, y)[0] > 0.95f;
        mean /= 160 * 90;
        QVERIFY2(std::fabs(mean - 0.5) < 0.06 && extreme > 160 * 90 * 0.7, qPrintable(QString("%1 %2").arg(mean).arg(extreme)));
        // Duotone: black to the shadow colour, white to the highlight colour.
        Image dt = step;
        e = fx("duotone", {{"shadows.r", 0.0}, {"shadows.g", 0.0}, {"shadows.b", 1.0}, {"highlights.r", 1.0}, {"highlights.g", 1.0}, {"highlights.b", 0.0}});
        Image bw = solid(2, 1, 0, 0, 0);
        std::fill_n(bw.at(1, 0), 3, 1.0f);
        applyVideoEffect(e, 0, bw, 1);
        QVERIFY(near(bw.at(0, 0)[2], 1) && near(bw.at(0, 0)[0], 0) && near(bw.at(1, 0)[0], 1) && near(bw.at(1, 0)[2], 0));
        // VHS: changes the picture, the same for the same frame; nothing at no amount.
        Image v1 = ramp(), v2 = ramp(), v0 = ramp();
        e = fx("vhs", {});
        applyVideoEffect(e, 7, v1, 1);
        applyVideoEffect(e, 7, v2, 1);
        e.params["amount"] = 0.0;
        applyVideoEffect(e, 7, v0, 1);
        QVERIFY(v1.px != ramp().px && v1.px == v2.px && v0.px == ramp().px);
        // Lens Flare: light at its source, ghosts on the far side of the centre, an anamorphic streak across, nothing at no brightness.
        {
            Image dark = solid(320, 180, 0, 0, 0);
            Image fl = dark;
            applyVideoEffect(fx("lens_flare", {{"pos_x", 0.25}, {"pos_y", 0.25}}), 0, fl, 1);
            QVERIFY(fl.at(80, 45)[0] > 1.0f);                                              // the hot core at the light
            QVERIFY(fl.at(300, 170)[0] < 0.05f && fl.at(300, 10)[0] < 0.05f);              // far corners stay dark
            // The zoom's ghosts: at 1.55 of the way from the light (80, 45) to past the centre (160, 90): (204, 115).
            QVERIFY2(fl.at(204, 115)[2] > 0.02f, qPrintable(QString::number(fl.at(204, 115)[2])));
            Image none = dark;
            applyVideoEffect(fx("lens_flare", {{"pos_x", 0.25}, {"pos_y", 0.25}, {"ghosts", 0.0}}), 0, none, 1);
            QVERIFY(none.at(204, 115)[2] < fl.at(204, 115)[2] * 0.5f);
            Image ana = dark;
            applyVideoEffect(fx("lens_flare", {{"pos_x", 0.5}, {"pos_y", 0.5}, {"lens", 3.0}}), 0, ana, 1);
            QVERIFY(ana.at(30, 90)[2] > 0.1f && ana.at(30, 90)[2] > ana.at(30, 90)[0]);     // the streak, blue, far to the side
            QVERIFY(ana.at(160, 20)[2] < 0.05f);                                           // but not above
            Image off = dark;
            applyVideoEffect(fx("lens_flare", {{"brightness", 0.0}}), 0, off, 1);
            QVERIFY(off.px == dark.px);
        }
        // Tilt-Shift: the band in focus keeps its detail, the top loses it.
        Image checks(160, 90);
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 160; ++x) {
                float* q = checks.at(x, y);
                q[0] = q[1] = q[2] = ((x / 2 + y / 2) & 1) ? 1.0f : 0.0f;
                q[3] = 1;
            }
        Image ts = checks;
        applyVideoEffect(fx("tilt_shift", {{"saturation", 100.0}}), 0, ts, 1);
        auto detail = [](const Image& img, int y) {
            double d = 0;
            for (int x = 1; x < img.width; ++x) d += std::fabs(img.at(x, y)[0] - img.at(x - 1, y)[0]);
            return d;
        };
        QVERIFY2(detail(ts, 45) > detail(checks, 45) * 0.95 && detail(ts, 5) < detail(checks, 5) * 0.2,
                 qPrintable(QString("%1 %2").arg(detail(ts, 45) / detail(checks, 45)).arg(detail(ts, 5) / detail(checks, 5))));
        // Camera Shake: frames move by about the amplitude, the same frame the same, no edges showing.
        Image s1 = spot(80, 45), s2 = s1, s3 = s1;
        e = fx("camera_shake", {{"amplitude", 10.0}, {"rotation", 0.0}});
        applyVideoEffect(e, 4, s1, 1);
        applyVideoEffect(e, 4, s2, 1);
        applyVideoEffect(e, 20, s3, 1);
        double ax, ay, bx, by;
        centroid(s1, ax, ay);
        centroid(s3, bx, by);
        QVERIFY(s1.px == s2.px && std::hypot(ax - bx, ay - by) > 0.5 && std::hypot(ax - 80.5, ay - 45.5) < 20);
        Image frame = solid(160, 90, 0.3f, 0.6f, 0.9f);
        applyVideoEffect(fx("camera_shake", {{"amplitude", 20.0}, {"rotation", 3.0}}), 9, frame, 1);
        rgb(frame, 0, 0, c);
        QVERIFY(near(c[3], 1) && near(c[1], 0.6f));
    }

    void stopMotion() {
        // A colour fading up over 30 frames, held 4 frames at a time.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 32;
        s.height = 18;
        Clip c = colorClip(p, 0, 0, 0, 0, 30);
        c.generator.params["color.r"].addKey(0, 0.0);
        c.generator.params["color.r"].addKey(29, 1.0);
        Effect hold = makeEffect(p, "stop_motion");
        hold.params["hold"] = 4.0;
        c.effects.push_back(hold);
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 0}, c).ok);
        RenderOptions o;
        auto red = [&](FrameTime t) {
            float px[4];
            rgb(renderProgramFrame(p, s, t, o), 5, 5, px);
            return px[0];
        };
        QCOMPARE(red(0), red(3));
        QCOMPARE(red(4), red(7));
        QVERIFY(red(4) > red(3) + 0.05f);
        QCOMPARE(red(9), red(8));
    }

    void cameraLogSpaces() {
        // Code values for scene-linear 0, 0.005, 0.01, 0.18 (grey), 0.9 and 4, from colour-science 0.4.7.
        struct Curve {
            Transfer t;
            double code[6];
        };
        const double lin[6] = {0.0, 0.005, 0.01, 0.18, 0.9, 4.0};
        const Curve curves[] = {
            {Transfer::AppleLog, {0.150476452, 0.178333706, 0.208555319, 0.488272459, 0.681686796, 0.864675980}},
            {Transfer::DLog, {0.092900000, 0.123025000, 0.152283809, 0.398764556, 0.572944426, 0.738174962}},
            {Transfer::FLog2, {0.092864000, 0.130853109, 0.158797484, 0.391007242, 0.557132364, 0.714967701}},
            {Transfer::NLog, {0.124372628, 0.147460056, 0.164961934, 0.363667770, 0.589634333, 0.808352057}},
            {Transfer::Log3G10, {0.091551488, 0.117456111, 0.137898710, 0.333332912, 0.483360528, 0.627292998}},
            {Transfer::BmdFilmGen5, {0.092465753, 0.133883783, 0.167755310, 0.383561644, 0.521383526, 0.650641506}},
            {Transfer::DavinciIntermediate, {0.0, 0.049697572, 0.085275708, 0.336043272, 0.502784181, 0.659830394}},
        };
        for (const Curve& c : curves)
            for (int i = 0; i < 6; ++i) {
                const double e = fromLinear(c.t, lin[i]);
                QVERIFY2(std::fabs(e - c.code[i]) < 2e-6, qPrintable(QString("curve %1 at %2: %3, wanted %4").arg(int(c.t)).arg(lin[i]).arg(e, 0, 'f', 9).arg(c.code[i], 0, 'f', 9)));
                QVERIFY2(std::fabs(toLinear(c.t, e) - lin[i]) < 1e-6 * std::max(1.0, lin[i]), qPrintable(QString("curve %1 back at %2").arg(int(c.t)).arg(lin[i])));
            }
        // Linear (0.5, 0.3, 0.1) in each gamut, in linear Rec.709 (colour-science, Bradford adaptation).
        struct Gamut {
            Primaries p;
            double rgb[3];
        };
        const Gamut gamuts[] = {
            {Primaries::DGamut, {0.653931, 0.327562, 0.035016}},  // from the primaries; DJI's printed matrix is rounded to 4 places
            {Primaries::RedWideGamut, {0.712703, 0.328837, -0.047771}},
            {Primaries::BmdWideGamut, {0.622830, 0.334441, 0.029346}},
            {Primaries::SGamut3, {0.692332, 0.299477, 0.059835}},
            {Primaries::DavinciWideGamut, {0.701011, 0.330216, -0.011751}},
        };
        for (const Gamut& g : gamuts) {
            double m[9];
            primariesMatrix(g.p, Primaries::Bt709, m);
            const double in[3] = {0.5, 0.3, 0.1};
            for (int r = 0; r < 3; ++r) {
                const double v = m[r * 3] * in[0] + m[r * 3 + 1] * in[1] + m[r * 3 + 2] * in[2];
                QVERIFY2(std::fabs(v - g.rgb[r]) < 2e-5, qPrintable(QString("gamut %1 row %2: %3, wanted %4").arg(int(g.p)).arg(r).arg(v, 0, 'f', 6).arg(g.rgb[r], 0, 'f', 6)));
            }
        }
        // Each is offered by name, scene-referred, and an Apple Log grey reads as Rec.709 grey.
        for (const char* id : {"applelog-rec2020", "dlog-dgamut", "flog2-fgamut", "nlog-ngamut", "log3g10-rwg", "bmdfilm5-bmdwg", "di-dwg", "slog3-sgamut3"}) {
            const ColorSpace* cs = findColorSpace(id);
            QVERIFY2(cs && cs->sceneReferred, id);
        }
        Image grey = solid(2, 2, float(fromLinear(Transfer::AppleLog, 0.18)), float(fromLinear(Transfer::AppleLog, 0.18)),
                           float(fromLinear(Transfer::AppleLog, 0.18)));
        convertColor(grey, *findColorSpace("applelog-rec2020"), *findColorSpace("linear-rec709"), 1000);
        float c[4];
        rgb(grey, 0, 0, c);
        QVERIFY2(std::fabs(c[0] - 0.18f) < 1e-3f && std::fabs(c[1] - 0.18f) < 1e-3f && std::fabs(c[2] - 0.18f) < 1e-3f,
                 qPrintable(QString("%1 %2 %3").arg(c[0]).arg(c[1]).arg(c[2])));
    }

    void curvesAndLuts() {
        auto id = buildCurve("0,0 1,1", 256);
        QVERIFY(near(id[128], 128.0f / 256, 0.002f));
        auto inv = buildCurve("0,1 1,0", 256);
        QVERIFY(near(inv[64], 1 - 64.0f / 256, 0.002f));
        auto sCurve = buildCurve("0,0 0.25,0.15 0.75,0.85 1,1", 1000);
        for (size_t i = 1; i < sCurve.size(); ++i) QVERIFY(sCurve[i] >= sCurve[i - 1] - 1e-6f);  // monotone

        QTemporaryDir dir;
        std::string path = (dir.path() + "/invert.cube").toStdString();
        {
            std::ofstream f(path);
            f << "TITLE \"invert\"\nLUT_3D_SIZE 2\n";
            for (int b = 0; b < 2; ++b)
                for (int g = 0; g < 2; ++g)
                    for (int r = 0; r < 2; ++r) f << (1 - r) << " " << (1 - g) << " " << (1 - b) << "\n";
        }
        std::string err;
        auto lut = loadCubeLut(path, &err);
        QVERIFY2(lut, err.c_str());
        float r = 0.25f, g = 0.5f, b = 0.9f;
        lut->apply(r, g, b);
        QVERIFY(near(r, 0.75f, 1e-4f) && near(g, 0.5f, 1e-4f) && near(b, 0.1f, 1e-4f));
        Project p;
        Effect e = makeEffect(p, "lut");
        e.strings["path"] = path;
        e.params["strength"] = 50.0;
        Image img = solid(2, 2, 0.2f, 0.2f, 0.2f);
        applyVideoEffect(e, 0, img, 1);
        float c[4];
        rgb(img, 0, 0, c);
        QVERIFY(near(c[0], 0.5f, 0.001f));
        QVERIFY(!loadCubeLut((dir.path() + "/missing.cube").toStdString()));
    }

    void chromaKeyRemovesGreen() {
        Project p;
        Image img(2, 1);
        img.at(0, 0)[1] = 1;  // pure green
        img.at(0, 0)[3] = 1;
        img.at(1, 0)[0] = 0.9f;  // red stays
        img.at(1, 0)[3] = 1;
        applyVideoEffect(makeEffect(p, "chroma_key"), 0, img, 1);
        QVERIFY(img.at(0, 0)[3] < 0.01f);
        QVERIFY(img.at(1, 0)[3] > 0.99f);
    }

    void blurSmoothsEdges() {
        Image img(64, 8);
        for (int y = 0; y < 8; ++y)
            for (int x = 32; x < 64; ++x) {
                float* p = img.at(x, y);
                p[0] = p[1] = p[2] = p[3] = 1;
            }
        double before = 0;
        for (float v : img.px) before += v;
        gaussianBlur(img, 6);
        double after = 0;
        for (float v : img.px) after += v;
        QVERIFY(std::fabs(before - after) / before < 0.05);  // energy preserved
        QVERIFY(img.at(31, 4)[0] > 0.2f && img.at(31, 4)[0] < 0.8f);
        QVERIFY(img.at(2, 4)[0] < 0.01f);
    }

    void transitions() {
        Image a = solid(4, 4, 1, 0, 0), b = solid(4, 4, 0, 0, 1);
        Effect none;
        Image mid = transitionMix("cross_dissolve", none, a, b, 0.5, 4, 4);
        float c[4];
        rgb(mid, 1, 1, c);
        QVERIFY(near(c[0], 0.5f) && near(c[2], 0.5f));
        Image dip = transitionMix("dip_to_black", none, a, b, 0.5, 4, 4);
        rgb(dip, 1, 1, c);
        QVERIFY(c[0] < 0.05f && c[2] < 0.05f && near(c[3], 1));
        Effect wipe = makeEffect("wipe", 1);
        wipe.params["softness"] = 0.0;
        Image w = transitionMix("wipe", wipe, a, b, 0.5, 4, 4);
        rgb(w, 0, 0, c);
        QVERIFY(c[2] > 0.9f);  // left half already shows B
        rgb(w, 3, 0, c);
        QVERIFY(c[0] > 0.9f);
        Image fadeIn = transitionMix("cross_dissolve", none, Image(), b, 0.25, 4, 4);
        QVERIFY(near(fadeIn.at(0, 0)[3], 0.25f));
        Effect push = makeEffect("push", 2);
        Image pu = transitionMix("push", push, a, b, 1.0, 4, 4);
        rgb(pu, 0, 0, c);
        QVERIFY(c[2] > 0.9f);
    }

    void compositorLayersAndTransforms() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        edit::overwrite(p, s, {TrackKind::Video, 0}, colorClip(p, 0, 0, 1, 0, 30));
        Clip top = colorClip(p, 1, 0, 0, 0, 30);
        top.motion.params["opacity"] = 50.0;
        edit::overwrite(p, s, {TrackKind::Video, 1}, top);
        RenderOptions o;
        Image img = renderProgramFrame(p, s, 10, o);
        QCOMPARE(img.width, 320);
        QCOMPARE(img.height, 180);
        float c[4];
        rgb(img, 160, 90, c);
        QVERIFY(near(c[0], 0.5f) && near(c[2], 0.5f));
        // Past the clips: black.
        Image after = renderProgramFrame(p, s, 40, o);
        rgb(after, 5, 5, c);
        QVERIFY(near(c[0], 0) && near(c[2], 0) && near(c[3], 1));
        // Half scale, moved right: left edge shows blue only, centre-right shows the mix.
        Clip* t = &trackAt(s, {TrackKind::Video, 1})->clips[0];
        t->motion.params["scale"] = 50.0;
        t->motion.params["pos_x"] = 80.0;
        t->motion.params["opacity"] = 100.0;
        img = renderProgramFrame(p, s, 10, o);
        rgb(img, 10, 90, c);
        QVERIFY(near(c[2], 1) && near(c[0], 0));
        rgb(img, 240, 90, c);
        QVERIFY(near(c[0], 1) && near(c[2], 0));
        // Preview resolution scales the output.
        o.scale = 0.5;
        QCOMPARE(renderProgramFrame(p, s, 10, o).width, 160);
        // Hidden (muted) video tracks are skipped.
        trackAt(s, {TrackKind::Video, 1})->muted = true;
        o.scale = 1;
        img = renderProgramFrame(p, s, 10, o);
        rgb(img, 240, 90, c);
        QVERIFY(near(c[2], 1));
    }

    void moreVideoEffects() {
        Project p = makeDefaultProject();
        auto fx = [&](const char* type, std::initializer_list<std::pair<const char*, double>> params) {
            Effect e = makeEffect(p, type);
            for (const auto& [k, v] : params) e.params[k] = v;
            return e;
        };
        auto run = [](const Effect& e, Image img, FrameTime t = 0) {
            applyVideoEffect(e, t, img, 1.0);
            return img;
        };
        float c[4];

        // Levels: input black and white stretch, gamma lifts the mid-tones.
        Image grey = solid(8, 8, 0.5f, 0.2f, 0.8f);
        Image lv = run(fx("levels", {{"in_black", 0.2}, {"in_white", 0.8}}), grey);
        rgb(lv, 2, 2, c);
        QVERIFY(near(c[0], 0.5f) && near(c[1], 0.f) && near(c[2], 1.f));
        lv = run(fx("levels", {{"gamma", 2.0}}), solid(4, 4, 0.25f, 0.25f, 0.25f));
        rgb(lv, 1, 1, c);
        QVERIFY(near(c[0], 0.5f));

        // Posterize to two levels: dark goes to black, light to white.
        Image post = run(fx("posterize", {{"levels", 2}}), solid(4, 4, 0.3f, 0.7f, 0.5f));
        rgb(post, 1, 1, c);
        QVERIFY(near(c[0], 0) && near(c[1], 1));

        // Glow: a bright spot lights the dark around it; a frame below the threshold is unchanged.
        Image spot = solid(64, 64, 0, 0, 0);
        for (int y = 30; y < 34; ++y)
            for (int x = 30; x < 34; ++x) std::fill(spot.at(x, y), spot.at(x, y) + 3, 1.f);
        Image glowed = run(fx("glow", {{"threshold", 0.5}, {"radius", 8}, {"intensity", 2}}), spot);
        rgb(glowed, 40, 32, c);
        QVERIFY2(c[0] > 0.02f, qPrintable(QString::number(c[0])));
        rgb(run(fx("glow", {{"threshold", 0.9}}), solid(16, 16, 0.4f, 0.4f, 0.4f)), 8, 8, c);
        QVERIFY(near(c[0], 0.4f, 1e-4f));

        // Film grain: noisy but the same brightness on average; fixed per frame, new the next.
        const Effect grain = fx("film_grain", {{"amount", 0.2}});
        const Image g0 = run(grain, solid(64, 64, 0.5f, 0.5f, 0.5f), 7), g0b = run(grain, solid(64, 64, 0.5f, 0.5f, 0.5f), 7),
                    g1 = run(grain, solid(64, 64, 0.5f, 0.5f, 0.5f), 8);
        double mean = 0, var = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) mean += g0.at(x, y)[1];
        mean /= 64 * 64;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) var += (g0.at(x, y)[1] - mean) * (g0.at(x, y)[1] - mean);
        QVERIFY2(std::fabs(mean - 0.5) < 0.02, qPrintable(QString::number(mean)));
        QVERIFY(var / (64 * 64) > 1e-4);
        QCOMPARE(g0.px, g0b.px);
        QVERIFY(g0.px != g1.px);

        // Directional blur along x smears a vertical edge sideways but not a horizontal one.
        Image edge = solid(32, 32, 0, 0, 0);
        for (int y = 0; y < 32; ++y)
            for (int x = 16; x < 32; ++x) std::fill(edge.at(x, y), edge.at(x, y) + 3, 1.f);
        Image smeared = run(fx("directional_blur", {{"length", 8}, {"angle", 0}}), edge);
        rgb(smeared, 14, 16, c);
        QVERIFY(c[0] > 0.1f && c[0] < 0.9f);
        smeared = run(fx("directional_blur", {{"length", 8}, {"angle", 90}}), edge);
        rgb(smeared, 14, 16, c);
        QVERIFY(near(c[0], 0));

        // Chromatic aberration: red moves outwards and blue inwards, so a white dot off-centre splits.
        Image dot = solid(101, 101, 0, 0, 0);
        std::fill(dot.at(90, 50), dot.at(90, 50) + 3, 1.f);
        Image split = run(fx("chromatic_aberration", {{"amount", 6}}), dot);
        int redAt = 0, blueAt = 0;
        float bestR = 0, bestB = 0;
        for (int x = 60; x < 101; ++x) {
            if (split.at(x, 50)[0] > bestR) bestR = split.at(x, 50)[0], redAt = x;
            if (split.at(x, 50)[2] > bestB) bestB = split.at(x, 50)[2], blueAt = x;
        }
        QVERIFY2(redAt > blueAt, qPrintable(QString("%1 %2").arg(redAt).arg(blueAt)));

        // Lens distortion: none is identity; barrel pulls the edges in and leaves the centre.
        Image chart = solid(64, 64, 0, 0, 1);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 8; ++x) std::fill(chart.at(x, y), chart.at(x, y) + 3, 1.f);
        QCOMPARE(run(fx("lens_distortion", {{"amount", 0}}), chart).px, chart.px);
        Image barrel = run(fx("lens_distortion", {{"amount", 60}}), chart);
        rgb(barrel, 32, 32, c);
        QVERIFY(near(c[2], 1) && near(c[0], 0));
        rgb(barrel, 9, 32, c);
        QVERIFY2(c[0] > 0.5f, qPrintable(QString::number(c[0])));  // the white strip has moved inwards

        // Corner pin into the right half: the frame squeezes there, the left half is empty.
        Image pin = run(fx("corner_pin", {{"tl_x", 0.5}, {"bl_x", 0.5}}), solid(40, 20, 0, 1, 0));
        QCOMPARE(pin.at(5, 10)[3], 0.f);
        rgb(pin, 30, 10, c);
        QVERIFY(near(c[1], 1) && near(c[3], 1));
        double h[9];
        const double square[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        QVERIFY(vfx::squareToQuad(square, h));
        QVERIFY(std::fabs(h[0] - 1) < 1e-12 && std::fabs(h[4] - 1) < 1e-12 && std::fabs(h[6]) < 1e-12);
        const double flat[4][2] = {{0, 0}, {1, 0}, {2, 0}, {3, 0}};
        QVERIFY(!vfx::squareToQuad(flat, h));

        // Letterbox 2.39:1 on 16:9: black bars top and bottom, the picture between.
        Image boxed = run(fx("letterbox", {{"aspect", 0}}), solid(160, 90, 1, 1, 1));
        rgb(boxed, 80, 2, c);
        QVERIFY(near(c[0], 0) && near(c[3], 1));
        rgb(boxed, 80, 45, c);
        QVERIFY(near(c[0], 1));
        boxed = run(fx("letterbox", {{"aspect", 3}}), solid(160, 90, 1, 1, 1));  // 4:3 pillars
        rgb(boxed, 2, 45, c);
        QVERIFY(near(c[0], 0));
        rgb(boxed, 80, 2, c);
        QVERIFY(near(c[0], 1));
    }

    void gradeVersionsRendered() {
        // Red, inverted in version 1 (cyan), plain in version 2, a blur kept through both.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64;
        s.height = 36;
        Clip red = colorClip(p, 1, 0, 0, 0, 30);
        red.effects = {makeEffect(p, "invert"), makeEffect(p, "gaussian_blur")};
        edit::overwrite(p, s, {TrackKind::Video, 0}, red);
        RenderOptions o;
        float c[4];
        rgb(renderSequenceFrame(p, s, 5, o), 32, 18, c);
        QVERIFY(c[0] < 0.1f && c[1] > 0.9f && c[2] > 0.9f);
        QVERIFY(edit::addGradeVersion(p, s, red.id, "Plain", true).ok);
        rgb(renderSequenceFrame(p, s, 5, o), 32, 18, c);
        QVERIFY(c[0] > 0.9f && c[1] < 0.1f);
        QCOMPARE(edit::clipById(s, red.id)->effects.size(), size_t(1));  // the blur
        QVERIFY(edit::switchGradeVersion(s, red.id, 0).ok);
        rgb(renderSequenceFrame(p, s, 5, o), 32, 18, c);
        QVERIFY(c[0] < 0.1f && c[1] > 0.9f);
    }

    void clipAnimationsRendered() {
        // A red matte that slides in from the right over a second, wiggles, and fades out over its last second.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 160;
        s.height = 90;
        s.fps = Rational{25, 1};
        const Clip red = colorClip(p, 1, 0, 0, 0, 100);
        edit::overwrite(p, s, {TrackKind::Video, 0}, red);
        QVERIFY(edit::setClipAnimation(s, red.id, AnimationSlot::In, "slide_left", 1.0).ok);
        QVERIFY(edit::setClipAnimation(s, red.id, AnimationSlot::Out, "fade", 1.0).ok);
        RenderOptions o;
        float c[4];
        // How much of the frame is red, and where its left edge is.
        auto redShare = [&](FrameTime t, int* leftEdge = nullptr) {
            const Image img = renderSequenceFrame(p, s, t, o);
            int n = 0, left = 160;
            for (int y = 0; y < 90; ++y)
                for (int x = 0; x < 160; ++x)
                    if (img.at(x, y)[0] > 0.5f) ++n, left = std::min(left, x);  // premultiplied: red and opaque
            if (leftEdge) *leftEdge = left;
            return double(n) / (160 * 90);
        };
        int edge = 0;
        QCOMPARE(redShare(0), 0.0);  // still off to the right
        const double partway = redShare(5, &edge);
        QVERIFY2(partway > 0.2 && partway < 0.95 && edge > 5, qPrintable(QString("%1 at %2").arg(partway).arg(edge)));
        QVERIFY(redShare(30) > 0.99);
        // Fading out: half red by the middle of the last second, nothing on the last frame's end.
        rgb(renderSequenceFrame(p, s, 87, o), 80, 45, c);
        QVERIFY2(c[3] > 0.2 && c[3] < 0.8, qPrintable(QString::number(c[3])));  // seen through: its alpha
        rgb(renderSequenceFrame(p, s, 50, o), 80, 45, c);
        QVERIFY(c[3] > 0.99);
        // A combo pulse makes it bigger than the frame some of the time (still full red); a swing turns it.
        QVERIFY(edit::setClipAnimation(s, red.id, AnimationSlot::Combo, "swing", 1.0).ok);
        double least = 1;
        for (FrameTime t = 30; t < 60; t += 3) least = std::min(least, redShare(t));
        QVERIFY2(least < 0.97, qPrintable(QString::number(least)));  // the corners show while it swings
    }

    void videoLayouts() {
        // Red on V1, blue on V2, green on V3, all full frame over 0-30, in a 160 x 90 sequence.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 160;
        s.height = 90;
        while (s.videoTracks.size() < 3) edit::addTrack(p, s, TrackKind::Video);
        const Clip red = colorClip(p, 1, 0, 0, 0, 30), blue = colorClip(p, 0, 0, 1, 0, 30), green = colorClip(p, 0, 1, 0, 0, 30);
        edit::overwrite(p, s, {TrackKind::Video, 0}, red);
        edit::overwrite(p, s, {TrackKind::Video, 1}, blue);
        edit::overwrite(p, s, {TrackKind::Video, 2}, green);
        RenderOptions o;
        float c[4];
        auto at = [&](int x, int y) {
            rgb(renderSequenceFrame(p, s, 5, o), x, y, c);
            return QString("%1 %2 %3 a%4").arg(c[0]).arg(c[1]).arg(c[2]).arg(c[3]);
        };
        // Side by side (red and blue): each fills its half.
        QVERIFY(edit::arrangeLayout(p, s, {blue.id, red.id}, edit::Layout::SideBySide).ok);
        s.videoTracks[2].muted = true;
        at(40, 45);
        QVERIFY2(near(c[0], 1) && near(c[2], 0), qPrintable(at(40, 45)));
        at(120, 45);
        QVERIFY2(near(c[2], 1) && near(c[0], 0), qPrintable(at(120, 45)));
        at(78, 10);
        QVERIFY(near(c[0], 1));  // the halves meet in the middle
        // With a 10 px gap the edges and the middle are empty.
        QVERIFY(edit::arrangeLayout(p, s, {red.id, blue.id}, edit::Layout::SideBySide, {10}).ok);
        at(4, 45);
        QVERIFY(near(c[3], 0));
        at(80, 45);
        QVERIFY(near(c[3], 0));
        at(40, 45);
        QVERIFY(near(c[0], 1));
        // The grid: red, blue, green clockwise from the top left; the fourth cell empty.
        s.videoTracks[2].muted = false;
        QVERIFY(edit::arrangeLayout(p, s, {red.id, blue.id, green.id}, edit::Layout::Grid).ok);
        at(40, 22);
        QVERIFY(near(c[0], 1));
        at(120, 22);
        QVERIFY(near(c[2], 1));
        at(40, 67);
        QVERIFY(near(c[1], 1) && near(c[0], 0));
        at(120, 67);
        QVERIFY(near(c[3], 0));
        // Picture in picture: red full frame, blue bottom right, green bottom left.
        QVERIFY(edit::arrangeLayout(p, s, {red.id, blue.id, green.id}, edit::Layout::PictureInPicture).ok);
        at(80, 30);
        QVERIFY2(near(c[0], 1) && near(c[2], 0), qPrintable(at(80, 30)));
        at(132, 73);
        QVERIFY2(near(c[2], 1), qPrintable(at(132, 73)));
        at(28, 73);
        QVERIFY2(near(c[1], 1), qPrintable(at(28, 73)));
        at(158, 88);
        QVERIFY(near(c[0], 1));  // the margin shows the red
        // Full frame: green on top covers it all again.
        QVERIFY(edit::arrangeLayout(p, s, {red.id, blue.id, green.id}, edit::Layout::FullFrame).ok);
        at(10, 10);
        QVERIFY(near(c[1], 1) && near(c[0], 0));
        at(150, 80);
        QVERIFY(near(c[1], 1));
    }

    void adjustmentLayers() {
        // Red on V1 for 60 frames, an inverting adjustment layer on V2 for frames 10-30, blue in a corner on V3.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 160;
        s.height = 90;
        edit::overwrite(p, s, {TrackKind::Video, 0}, colorClip(p, 0.8f, 0.1f, 0.1f, 0, 60));
        Clip adj = makeGeneratorClip(p, "adjustment", 20);
        adj.start = 10;
        QCOMPARE(adj.name, std::string("Adjustment Layer"));
        adj.effects.push_back(makeEffect(p, "invert"));
        const Id adjId = adj.id;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 1}, adj).ok);
        Clip corner = colorClip(p, 0, 0, 1, 0, 60);
        corner.motion.params["scale"] = 25.0;
        corner.motion.params["pos_x"] = -60.0;
        corner.motion.params["pos_y"] = -34.0;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 2}, corner).ok);
        RenderOptions o;
        float c[4];
        // Outside the layer: the red as it is.
        Image img = renderProgramFrame(p, s, 5, o);
        rgb(img, 80, 45, c);
        QVERIFY(near(c[0], 0.8f) && near(c[1], 0.1f));
        // Under the layer: everything below is inverted; the track above it is not.
        img = renderProgramFrame(p, s, 20, o);
        rgb(img, 80, 45, c);
        QVERIFY2(near(c[0], 0.2f) && near(c[1], 0.9f) && near(c[2], 0.9f), qPrintable(QString("%1 %2 %3").arg(c[0]).arg(c[1]).arg(c[2])));
        rgb(img, 20, 11, c);
        QVERIFY(near(c[2], 1) && near(c[0], 0));
        // Opacity mixes the adjusted picture with the original.
        Clip* layer = edit::clipById(s, adjId);
        layer->motion.params["opacity"] = 50.0;
        img = renderProgramFrame(p, s, 20, o);
        rgb(img, 80, 45, c);
        QVERIFY(near(c[0], 0.5f) && near(c[1], 0.5f));
        layer->motion.params["opacity"] = 100.0;
        // A disabled layer does nothing.
        layer->enabled = false;
        img = renderProgramFrame(p, s, 20, o);
        rgb(img, 80, 45, c);
        QVERIFY(near(c[0], 0.8f));
        layer->enabled = true;
        // With nothing below it, it draws nothing.
        Project empty = makeDefaultProject();
        Sequence& es = *empty.active();
        es.width = 64;
        es.height = 36;
        Clip lone = makeGeneratorClip(empty, "adjustment", 10);
        lone.effects.push_back(makeEffect(empty, "invert"));
        edit::overwrite(empty, es, {TrackKind::Video, 1}, lone);
        img = renderSequenceFrame(empty, es, 5, o);
        QCOMPARE(img.at(32, 18)[3], 0.f);
        // A blend mode applies to the adjusted picture: Multiply by the inverted red darkens it.
        layer->blendMode = "multiply";
        img = renderProgramFrame(p, s, 20, o);
        rgb(img, 80, 45, c);
        QVERIFY2(near(c[0], 0.16f) && near(c[1], 0.09f), qPrintable(QString("%1 %2").arg(c[0]).arg(c[1])));
    }

    void smoothCut() {
        // A jump cut: the same shot either side, the picture 14 px further right after the cut.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        auto bars = [&](FrameTime start, FrameTime len, double x) {
            Clip c = makeGeneratorClip(p, "bars", len);
            c.start = start;
            c.motion.params["pos_x"] = x;
            return c;
        };
        auto ra = edit::overwrite(p, s, {TrackKind::Video, 0}, bars(0, 30, 0));
        edit::overwrite(p, s, {TrackKind::Video, 0}, bars(30, 30, 14));
        QVERIFY(edit::addTransition(p, s, ra.created[0], edit::Edge::Out, "smooth_cut", 6).ok);
        const Transition& tr = trackAt(s, {TrackKind::Video, 0})->transitions.at(0);
        QCOMPARE(tr.type, std::string("smooth_cut"));
        RenderOptions o;
        // Mean difference from the picture shifted by `x`, away from the edges the shift uncovers.
        auto error = [&](const Image& img, double x) {
            Project ip = makeDefaultProject();
            Sequence& is = *ip.active();
            is.width = 320;
            is.height = 180;
            Clip c = makeGeneratorClip(ip, "bars", 10);
            c.motion.params["pos_x"] = x;
            edit::overwrite(ip, is, {TrackKind::Video, 0}, c);
            const Image ideal = renderProgramFrame(ip, is, 0, o);
            double d = 0;
            for (int y = 0; y < 180; ++y)
                for (int xx = 30; xx < 290; ++xx)
                    for (int k = 0; k < 3; ++k) d += std::fabs(img.at(xx, y)[k] - ideal.at(xx, y)[k]);
            return d / (180 * 260 * 3);
        };
        // Frames 27-32: the picture slides across instead of two pictures showing through each other.
        for (FrameTime t = 27; t < 33; ++t) {
            const double u = (double(t - 27) + 0.5) / 6;
            const Image morph = renderProgramFrame(p, s, t, o);
            const double em = error(morph, 14 * u);
            Transition& live = trackAt(s, {TrackKind::Video, 0})->transitions.at(0);
            live.type = "cross_dissolve";
            const double ed = error(renderProgramFrame(p, s, t, o), 14 * u);
            live.type = "smooth_cut";
            QVERIFY2(em < 0.005 && em < ed * 0.25, qPrintable(QString("frame %1: %2 vs dissolve %3").arg(t).arg(em).arg(ed)));
        }
        // Outside the transition the clips play as they are.
        QVERIFY(error(renderProgramFrame(p, s, 20, o), 0) < 1e-4);
        QVERIFY(error(renderProgramFrame(p, s, 40, o), 14) < 1e-4);
    }

    void compositorTransitionAndCrop() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64;
        s.height = 36;
        auto ra = edit::overwrite(p, s, {TrackKind::Video, 0}, colorClip(p, 1, 0, 0, 0, 30));
        edit::overwrite(p, s, {TrackKind::Video, 0}, colorClip(p, 0, 0, 1, 30, 30));
        QVERIFY(edit::addTransition(p, s, ra.created[0], edit::Edge::Out, "cross_dissolve", 10).ok);
        RenderOptions o;
        float c[4];
        rgb(renderProgramFrame(p, s, 29, o), 32, 18, c);  // just before the cut: mostly red, some blue
        QVERIFY(c[0] > 0.4f && c[2] > 0.3f);
        rgb(renderProgramFrame(p, s, 20, o), 32, 18, c);  // before the transition
        QVERIFY(near(c[0], 1));
        // Crop the right half of the second clip.
        Clip& b = trackAt(s, {TrackKind::Video, 0})->clips[1];
        b.motion.params["crop_right"] = 50.0;
        rgb(renderProgramFrame(p, s, 50, o), 60, 18, c);
        QVERIFY(near(c[2], 0));
        rgb(renderProgramFrame(p, s, 50, o), 4, 18, c);
        QVERIFY(near(c[2], 1));
    }

    void autoColorNeutralisesCasts() {
        // A dim, blue-tinted gradient: auto colour should balance channels and stretch levels.
        Image img(64, 16);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 64; ++x) {
                float v = 0.2f + 0.4f * x / 63.0f;
                float* p = img.at(x, y);
                p[0] = v * 0.8f;
                p[1] = v * 0.9f;
                p[2] = std::min(1.0f, v * 1.3f);
                p[3] = 1;
            }
        Effect e = autoColorCorrection(img, 1);
        applyVideoEffect(e, 0, img, 1);
        double mean[3] = {0, 0, 0};
        float lo = 1, hi = 0;
        for (size_t i = 0; i < img.px.size(); i += 4) {
            for (int c = 0; c < 3; ++c) mean[c] += img.px[i + c];
            float l = 0.2126f * img.px[i] + 0.7152f * img.px[i + 1] + 0.0722f * img.px[i + 2];
            lo = std::min(lo, l);
            hi = std::max(hi, l);
        }
        QVERIFY(std::fabs(mean[0] - mean[2]) / mean[1] < 0.05);  // cast removed
        QVERIFY(lo < 0.05f && hi > 0.9f);                         // levels stretched
    }

    void hueCurves() {
        // Flat curves: nothing set is the middle, one point is a level, the ends wrap (hue) or hold (levels).
        for (float v : buildFlatCurve("", 360)) QCOMPARE(v, 0.5f);
        for (float v : buildFlatCurve("0.2,0.8", 360)) QCOMPARE(v, 0.8f);
        const auto wrap = buildFlatCurve("0.1,0.3 0.4,0.8 0.7,0.5", 360, true);
        QVERIFY(std::fabs(wrap.front() - wrap.back()) < 1e-5);
        QVERIFY(std::fabs(wrap[144] - 0.8f) < 1e-4);  // x = 0.4
        const auto held = buildFlatCurve("0.3,0.2 0.7,0.9", 360, false);
        QVERIFY(std::fabs(held.front() - 0.2f) < 1e-4 && std::fabs(held.back() - 0.9f) < 1e-4);
        for (int i = 0; i < 108; ++i) QVERIFY(std::fabs(held[size_t(i)] - 0.2f) < 1e-4);

        // A red, a green, a blue, a grey and a dark red.
        const float colours[5][3] = {{0.8f, 0.1f, 0.1f}, {0.1f, 0.8f, 0.1f}, {0.1f, 0.1f, 0.8f}, {0.5f, 0.5f, 0.5f}, {0.3f, 0.05f, 0.05f}};
        Image src(5, 1);
        for (int i = 0; i < 5; ++i) {
            std::copy(colours[i], colours[i] + 3, src.at(i, 0));
            src.at(i, 0)[3] = 1;
        }
        auto run = [&](const char* curve, const char* points, double mix = 100) {
            Effect e = makeEffect("hue_curves", 1);
            e.strings[curve] = points;
            e.params["mix"] = mix;
            Image img = src;
            applyVideoEffect(e, 0, img, 1);
            return img;
        };
        auto same = [&](const Image& img, int i) {
            for (int k = 0; k < 3; ++k)
                if (std::fabs(img.at(i, 0)[k] - colours[i][k]) > 1e-3) return false;
            return true;
        };
        // A neutral curve changes nothing.
        Image img = run("hue_hue", "0,0.5 0.25,0.5 0.5,0.5 0.75,0.5");
        for (int i = 0; i < 5; ++i) QVERIFY(same(img, i));
        // Hue vs Hue: reds turned a third of the way round become green; blue and grey stay.
        const char* redOnly = "0,0.8333 0.1,0.5 0.3,0.5 0.5,0.5 0.7,0.5 0.9,0.5";
        img = run("hue_hue", redOnly);
        QVERIFY(img.at(0, 0)[1] > 0.7f && img.at(0, 0)[0] < 0.2f);
        QVERIFY(same(img, 2) && same(img, 3));
        // Hue vs Saturation: greens taken to grey, red kept.
        img = run("hue_sat", "0,0.5 0.2,0.5 0.3333,0 0.45,0.5 0.6,0.5 0.7,0.5 0.9,0.5");
        QVERIFY(std::fabs(img.at(1, 0)[0] - img.at(1, 0)[1]) < 0.01 && std::fabs(img.at(1, 0)[1] - img.at(1, 0)[2]) < 0.01);
        QVERIFY(same(img, 0) && same(img, 2));
        // Hue vs Luma: reds darkened (by their saturation), greys untouched.
        img = run("hue_luma", "0,0.25 0.1,0.5 0.3,0.5 0.5,0.5 0.7,0.5 0.9,0.5");
        QVERIFY(img.at(0, 0)[0] < 0.5f);
        QVERIFY(same(img, 3) && same(img, 1));
        // Luma vs Saturation: the shadows drained of colour, bright colours kept.
        img = run("luma_sat", "0,0 0.12,0 0.3,0.5 1,0.5");
        QVERIFY(std::fabs(img.at(4, 0)[0] - img.at(4, 0)[1]) < 0.01);
        QVERIFY(same(img, 3));
        // Saturation vs Saturation: the most saturated pulled back.
        img = run("sat_sat", "0,0.5 0.5,0.5 1,0.3");
        const float s0 = (img.at(0, 0)[0] - img.at(0, 0)[2]) / img.at(0, 0)[0];
        QVERIFY(s0 < 0.8f && s0 > 0.2f);
        // Mix 0 is the picture as it was.
        img = run("hue_hue", redOnly, 0);
        for (int i = 0; i < 5; ++i) QVERIFY(same(img, i));
    }

    void colorMatch() {
        // Random coloured blocks over a gradient, 160 x 90 (another seed: the same kind of scene, framed differently).
        auto texture = [](unsigned seed) {
            Image img(160, 90);
            for (int y = 0; y < 90; ++y)
                for (int x = 0; x < 160; ++x) {
                    float* p = img.at(x, y);
                    p[0] = p[1] = p[2] = 0.15f + 0.6f * float(x) / 159.0f;
                    p[3] = 1;
                }
            unsigned st = seed;
            auto rnd = [&st] {
                st = st * 1664525u + 1013904223u;
                return float(st >> 8) / float(1u << 24);
            };
            for (int i = 0; i < 300; ++i) {
                const int x0 = int(rnd() * 150), y0 = int(rnd() * 82), w = 4 + int(rnd() * 12), h = 4 + int(rnd() * 9);
                const float c[3] = {0.05f + 0.9f * rnd(), 0.05f + 0.9f * rnd(), 0.05f + 0.9f * rnd()};
                for (int y = y0; y < std::min(90, y0 + h); ++y)
                    for (int x = x0; x < std::min(160, x0 + w); ++x) std::copy(c, c + 3, img.at(x, y));
            }
            return img;
        };
        // A warm, lifted, contrasty grade.
        Effect look = makeEffect("color_correct", 1);
        look.params["lift_r"] = 0.06;
        look.params["gain_b"] = 0.8;
        look.params["gamma_g"] = 0.3;
        look.params["gain"] = 1.1;
        auto graded = [&](Image img, const Effect& e) {
            applyVideoEffect(e, 0, img, 1);
            return img;
        };
        auto diff = [](const Image& a, const Image& b) {
            double d = 0;
            for (size_t i = 0; i < a.px.size(); i += 4)
                for (int k = 0; k < 3; ++k) d += std::fabs(a.px[i + k] - b.px[i + k]);
            return d / double(a.px.size() / 4 * 3);
        };
        const Image a = texture(1), b = texture(2);
        // The same shot graded and not: matching brings it onto the grade, and back.
        const Image ga = graded(a, look);
        QVERIFY(diff(a, ga) > 0.05);
        double d = diff(graded(a, colorMatchCorrection(a, ga, 2)), ga);
        QVERIFY2(d < 0.005, qPrintable(QString::number(d)));
        d = diff(graded(ga, colorMatchCorrection(ga, a, 3)), a);
        QVERIFY2(d < 0.005, qPrintable(QString::number(d)));
        // Another shot under the same light: it comes out close to how the grade would have it.
        const Image gb = graded(b, look);
        d = diff(graded(b, colorMatchCorrection(b, ga, 4)), gb);
        QVERIFY2(d < 0.035 && d < diff(b, gb) * 0.45, qPrintable(QString("%1 vs %2").arg(d).arg(diff(b, gb))));
        // Nothing to go on: no change.
        const Effect none = colorMatchCorrection(Image(), ga, 5);
        QCOMPARE(none.p("gain_r", 0, 1), 1.0);
        QCOMPARE(none.p("lift_g", 0, 0), 0.0);
    }

    void titleTemplatesAndAnimation() {
        Project p;
        // Every template is a Title with its own settings, listed as a generator.
        QVERIFY(titleTemplates().size() >= 9);
        for (const TitleTemplate& tpl : titleTemplates()) {
            const EffectInfo* info = findEffectInfo(tpl.id);
            QVERIFY(info && info->category == EffectCategory::Generator);
            QCOMPARE(makeEffect(p, tpl.id).type, std::string("title"));
        }
        Clip lower = makeGeneratorClip(p, "title_lower_third", 90);
        QCOMPARE(lower.generator.type, std::string("title"));
        QCOMPARE(lower.name, std::string("Lower Third"));
        QCOMPARE(lower.generator.s("text"), std::string("Name Surname\nRole or place"));

        // Where the ink is, and how much.
        struct Ink {
            int left = 1 << 30, top = 1 << 30, right = -1, bottom = -1;
            double amount = 0;
        };
        auto ink = [](const Image& img) {
            Ink k;
            for (int y = 0; y < img.height; ++y)
                for (int x = 0; x < img.width; ++x) {
                    const float a = img.at(x, y)[3];
                    k.amount += a;
                    if (a < 0.1f) continue;
                    k.left = std::min(k.left, x);
                    k.right = std::max(k.right, x);
                    k.top = std::min(k.top, y);
                    k.bottom = std::max(k.bottom, y);
                }
            return k;
        };
        // A lower third sits in the lower left inside the title-safe area, at 16:9 and at 9:16.
        for (auto [w, h, sc] : {std::tuple{480, 270, 0.25}, std::tuple{270, 480, 0.25}}) {
            const Ink k = ink(renderGenerator(lower.generator, 45, w, h, sc, 90, 30));
            QVERIFY2(k.left >= int(0.08 * w) - 1 && k.left < w / 3, qPrintable(QString("%1x%2 left %3").arg(w).arg(h).arg(k.left)));
            QVERIFY2(k.bottom <= h - int(0.08 * h) + 1 && k.bottom > h / 2, qPrintable(QString("%1x%2 bottom %3").arg(w).arg(h).arg(k.bottom)));
            QVERIFY(k.right < w);
        }
        // Its accent bar is drawn (amber), and the second line has its own colour.
        Effect styled = lower.generator;
        styled.params["color.r"] = 1.0;
        styled.params["color.g"] = 0.0;
        styled.params["color.b"] = 0.0;
        styled.params["sub_color.r"] = 0.0;
        styled.params["sub_color.g"] = 0.0;
        styled.params["sub_color.b"] = 1.0;
        styled.params["shadow"] = 0.0;
        const Image si = renderGenerator(styled, 45, 480, 270, 0.25, 90, 30);
        int amber = 0, red = 0, blue = 0;
        for (int y = 0; y < si.height; ++y)
            for (int x = 0; x < si.width; ++x) {
                const float* q = si.at(x, y);
                if (q[3] < 0.9f) continue;
                if (q[0] > 0.9f && q[1] > 0.6f && q[2] < 0.2f) ++amber;
                else if (q[0] > 0.9f && q[1] < 0.1f) ++red;
                else if (q[2] > 0.9f && q[0] < 0.1f) ++blue;
            }
        QVERIFY2(amber > 20 && red > 20 && blue > 20, qPrintable(QString("%1 %2 %3").arg(amber).arg(red).arg(blue)));
        // Slides in from the left (moving right), settled after half a second.
        const Ink settled = ink(renderGenerator(lower.generator, 45, 480, 270, 0.25, 90, 30));
        const Ink early = ink(renderGenerator(lower.generator, 4, 480, 270, 0.25, 90, 30));
        QVERIFY2(early.left < settled.left - 5, qPrintable(QString("%1 vs %2").arg(early.left).arg(settled.left)));
        QCOMPARE(ink(renderGenerator(lower.generator, 20, 480, 270, 0.25, 90, 30)).left, settled.left);
        // Fades out over its last 0.4 s, gone on the last frame.
        const double full = settled.amount;
        const double fading = ink(renderGenerator(lower.generator, 87, 480, 270, 0.25, 90, 30)).amount;  // 2 frames from the end
        QVERIFY(fading > 0.05 * full && fading < 0.8 * full);
        QCOMPARE(ink(renderGenerator(lower.generator, 89, 480, 270, 0.25, 90, 30)).amount, 0.0);
        // Pop: small and see-through at first, then full size.
        Effect pop = makeEffect(p, "title_centred");
        const Ink popStart = ink(renderGenerator(pop, 1, 480, 270, 0.25, 90, 30));
        const Ink popDone = ink(renderGenerator(pop, 30, 480, 270, 0.25, 90, 30));
        QVERIFY(popStart.right - popStart.left < popDone.right - popDone.left);
        QVERIFY(popStart.amount < 0.5 * popDone.amount);
        // Centred: free placement puts it in the middle.
        QVERIFY(std::abs((popDone.left + popDone.right) / 2 - 240) <= 3 && std::abs((popDone.top + popDone.bottom) / 2 - 135) <= 4);
        // Typewriter: a third of the way through, about a third of the letters.
        Effect type = makeEffect(p, "title_typewriter");
        const double typed = ink(renderGenerator(type, 15, 480, 270, 0.25, 120, 30)).amount;
        const double all = ink(renderGenerator(type, 60, 480, 270, 0.25, 120, 30)).amount;
        QVERIFY2(typed > 0.15 * all && typed < 0.6 * all, qPrintable(QString("%1 of %2").arg(typed).arg(all)));
        // Wipe: revealed from the left.
        Effect boxed = makeEffect(p, "title_lower_third_box");
        const Ink wiping = ink(renderGenerator(boxed, 5, 480, 270, 0.25, 90, 30));
        const Ink wiped = ink(renderGenerator(boxed, 40, 480, 270, 0.25, 90, 30));
        QVERIFY(wiping.right < wiped.right - 10 && std::abs(wiping.left - wiped.left) <= 1);
        // Without animation or a length, a title is unchanged throughout.
        Effect plain = makeEffect(p, "title");
        QCOMPARE(ink(renderGenerator(plain, 0, 320, 180, 1.0)).amount, ink(renderGenerator(plain, 50, 320, 180, 1.0, 60, 30)).amount);
    }

    void focusRepair() {
        Project proj;
        // Fine detail (a pseudo-random pattern of blocks and lines), blurred by a Gaussian of sigma 2.
        const int W = 160, H = 120;
        Image sharp(W, H);
        uint32_t seed = 12345;
        auto rnd = [&] { return (seed = seed * 1664525u + 1013904223u) >> 8 & 0xffff; };
        for (int by = 0; by < H; by += 4)
            for (int bx = 0; bx < W; bx += 4) {
                const float v = 0.15f + 0.7f * float(rnd() % 2);
                for (int y = by; y < by + 4; ++y)
                    for (int x = bx; x < bx + 4; ++x) {
                        float* p = sharp.at(x, y);
                        p[0] = v, p[1] = v * 0.8f, p[2] = v * 0.5f, p[3] = 1;
                    }
            }
        auto blurred = [&](const Image& src, double sigma) {
            Image out = src;
            for (int c = 0; c < 3; ++c) {
                std::vector<float> plane(size_t(W) * H);
                for (size_t i = 0; i < plane.size(); ++i) plane[i] = src.px[i * 4 + size_t(c)];
                gaussianPlane(plane, W, H, sigma);
                for (size_t i = 0; i < plane.size(); ++i) out.px[i * 4 + size_t(c)] = plane[i];
            }
            return out;
        };
        auto rmse = [&](const Image& a, const Image& b) {
            double e = 0;
            int n = 0;
            for (int y = 8; y < H - 8; ++y)  // away from the edges
                for (int x = 8; x < W - 8; ++x)
                    for (int c = 0; c < 3; ++c) e += std::pow(a.at(x, y)[c] - b.at(x, y)[c], 2), ++n;
            return std::sqrt(e / n);
        };
        const Image soft = blurred(sharp, 2.0);
        Image fixed = soft;
        montage::focusRepair(fixed, FocusRepair{2.0, 30, 1.0, 0.0});
        const double before = rmse(soft, sharp), after = rmse(fixed, sharp);
        QVERIFY2(after < 0.72 * before, qPrintable(QString("%1 -> %2").arg(before).arg(after)));
        // Better than Sharpen at its best for this blur.
        double bestSharpen = 1e9;
        for (double amount : {0.5, 1.0, 1.5, 2.0, 3.0}) {
            Image sh = soft;
            Effect e = makeEffect(proj, "sharpen");
            e.params["amount"] = Param(amount);
            e.params["radius"] = Param(4.0);
            applyVideoEffect(e, 0, sh, 1.0);
            bestSharpen = std::min(bestSharpen, rmse(sh, sharp));
        }
        QVERIFY2(after < bestSharpen, qPrintable(QString("%1 vs sharpen %2").arg(after).arg(bestSharpen)));
        // Colour follows the brightness: the tint is kept.
        for (int y = 20; y < H - 20; y += 13)
            for (int x = 20; x < W - 20; x += 11) {
                const float* p = fixed.at(x, y);
                if (p[0] < 0.05f) continue;
                QVERIFY(std::abs(p[1] / p[0] - 0.8f) < 0.01f && std::abs(p[2] / p[0] - 0.5f) < 0.01f);
            }
        // A flat picture, and no strength, change nothing.
        Image flat(64, 48);
        flat.fill(0.4f, 0.3f, 0.2f, 1.0f);
        const Image flatBefore = flat;
        montage::focusRepair(flat, FocusRepair{});
        QVERIFY(std::abs(flat.at(32, 24)[0] - flatBefore.at(32, 24)[0]) < 1e-4f);
        Image none = soft;
        montage::focusRepair(none, FocusRepair{2.0, 10, 0.0, 0.01});
        QCOMPARE(none.px, soft.px);
        // From the catalogue, scaled with the preview: at half size it undoes half the blur.
        Effect e = makeEffect(proj, "focus_repair");
        QVERIFY(findEffectInfo("focus_repair"));
        e.params["blur"] = Param(2.0);
        e.params["iterations"] = Param(30.0);
        e.params["noise"] = Param(0.0);
        Image viaEffect = soft;
        applyVideoEffect(e, 0, viaEffect, 1.0);
        QVERIFY(std::abs(rmse(viaEffect, sharp) - after) < 1e-6);
        // Noise: a threshold keeps flat grain from being amplified much.
        Image grainy = soft;
        for (size_t i = 0; i < grainy.px.size(); i += 4)
            for (int c = 0; c < 3; ++c) grainy.px[i + size_t(c)] += 0.01f * (float(rnd() % 1000) / 500.0f - 1.0f);
        Image raw = grainy, guarded = grainy;
        montage::focusRepair(raw, FocusRepair{2.0, 30, 1.0, 0.0});
        montage::focusRepair(guarded, FocusRepair{2.0, 30, 1.0, 0.03});
        QVERIFY2(rmse(guarded, grainy) < rmse(raw, grainy), "the threshold changes the picture less");
    }

    void rollingAndCrawlingTitles() {
        Project p;
        QVERIFY(findTitleTemplate("title_credits") && findTitleTemplate("title_crawl"));
        // The extent of the ink (alpha over 0.1) and how much there is.
        struct Box {
            int left = 1 << 30, top = 1 << 30, right = -1, bottom = -1;
            bool empty() const { return right < 0; }
            double cx() const { return (left + right) / 2.0; }
            double cy() const { return (top + bottom) / 2.0; }
        };
        auto ink = [](const Image& img) {
            Box b;
            for (int y = 0; y < img.height; ++y)
                for (int x = 0; x < img.width; ++x)
                    if (img.at(x, y)[3] >= 0.1f) b.left = std::min(b.left, x), b.right = std::max(b.right, x), b.top = std::min(b.top, y), b.bottom = std::max(b.bottom, y);
            return b;
        };
        const int W = 320, H = 180;
        const FrameTime len = 61;
        Effect roll = makeEffect(p, "title");
        roll.strings["text"] = "Credits";
        roll.params["size"] = Param(40.0);
        roll.params["shadow"] = Param(0.0);
        roll.params["motion"] = Param(1.0);
        auto at = [&](const Effect& e, FrameTime t) { return ink(renderGenerator(e, t, W, H, 1.0, len, 30)); };
        // Rolls up from below the frame to above it: nothing at either end, a steady speed between.
        QVERIFY(at(roll, 0).empty());
        QVERIFY(at(roll, len - 1).empty());
        const Box mid = at(roll, 30), q1 = at(roll, 15), q3 = at(roll, 45);
        QVERIFY(!mid.empty() && !q1.empty() && !q3.empty());
        const double half = (mid.bottom - mid.top) / 2.0;
        QVERIFY2(std::abs(mid.cy() - H / 2.0) <= 3, qPrintable(QString::number(mid.cy())));
        QVERIFY(q1.cy() > mid.cy() && q3.cy() < mid.cy());
        QVERIFY2(std::abs((q1.cy() - mid.cy()) - (mid.cy() - q3.cy())) <= 1.5, qPrintable(QString("%1 %2 %3").arg(q1.cy()).arg(mid.cy()).arg(q3.cy())));
        // A quarter of the way: a quarter of the way from just below to just above.
        const double travel = H + 2 * half;
        QVERIFY2(std::abs(q1.cy() - (H / 2.0 + travel / 4)) <= 6, qPrintable(QString("%1 vs %2").arg(q1.cy()).arg(H / 2.0 + travel / 4)));
        QCOMPARE(mid.cx(), at(roll, 20).cx());  // straight up
        // Starting where it is laid out (it fits): in the middle on the first frame, then away.
        Effect onScreen = roll;
        onScreen.params["start_off"] = Param(0.0);
        QVERIFY(std::abs(at(onScreen, 0).cy() - H / 2.0) <= 3);
        QVERIFY(at(onScreen, len - 1).empty());
        // Eased: slower at first than at a steady speed, and the same halfway.
        Effect eased = roll;
        eased.params["motion_ease"] = Param(0.5);
        QVERIFY2(at(eased, 6).empty() || at(eased, 6).cy() > at(roll, 6).cy() + 3, "eases in");
        QVERIFY(std::abs(at(eased, 30).cy() - mid.cy()) <= 1);
        // Credits taller than the frame and not leaving it: the last line ends inside the title-safe area.
        Effect tall = roll;
        tall.strings["text"] = "One\nTwo\nThree\nFour\nFive\nSix\nSeven";
        tall.params["end_off"] = Param(0.0);
        const Box last = at(tall, len - 1);
        QVERIFY2(last.bottom <= H - int(0.08 * H) + 1 && last.bottom > H - int(0.08 * H) - 12, qPrintable(QString::number(last.bottom)));
        QVERIFY(last.bottom - last.top > H / 2);  // several lines still in view

        // Crawls: right to left (the template), and left to right.
        Effect crawl = makeEffect(p, "title_crawl");
        crawl.strings["text"] = "Ticker";
        const Box c1 = at(crawl, 15), c2 = at(crawl, 30);
        QVERIFY(at(crawl, 0).empty() && at(crawl, len - 1).empty());
        QVERIFY2(std::abs(c2.cx() - W / 2.0) <= 3, qPrintable(QString::number(c2.cx())));
        QVERIFY(c1.cx() > c2.cx() + 20);
        QVERIFY(std::abs(c1.cy() - c2.cy()) <= 0.5 && c2.bottom > H / 2);  // along the bottom, level
        crawl.params["motion"] = Param(3.0);
        QVERIFY(at(crawl, 15).cx() < W / 2.0 - 20);
        // Still titles are unaffected by the new settings.
        Effect still = roll;
        still.params["motion"] = Param(0.0);
        QCOMPARE(at(still, 0).cy(), at(still, 40).cy());
    }

    void renderCacheKeysAndFrames() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        RenderCache cache(dir.path() + "/render");
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64;
        s.height = 36;
        edit::overwrite(p, s, {TrackKind::Video, 0}, colorClip(p, 1, 0, 0, 0, 30));
        edit::overwrite(p, s, {TrackKind::Video, 0}, colorClip(p, 0, 0, 1, 30, 30));
        RenderOptions o;
        const QByteArray k10 = frameKey(p, s, 10, o), k40 = frameKey(p, s, 40, o);
        QVERIFY(!k10.isEmpty() && k10 != frameKey(p, s, 11, o) && k10 != k40);
        QCOMPARE(frameKey(p, s, 10, o), k10);
        QVERIFY(frameKey(p, s, 100, o).isEmpty());  // nothing on screen
        RenderOptions half = o;
        half.scale = 0.5;
        QVERIFY(frameKey(p, s, 10, half) != k10);
        // An effect on the blue clip changes its frames' keys, not the red one's.
        Clip& blue = trackAt(s, {TrackKind::Video, 0})->clips.at(1);
        blue.effects.push_back(makeEffect(p, "invert"));
        QCOMPARE(frameKey(p, s, 10, o), k10);
        QVERIFY(frameKey(p, s, 40, o) != k40);
        blue.effects.clear();
        QCOMPARE(frameKey(p, s, 40, o), k40);
        // Moving a clip keeps its frames: the same frame of it has the same key wherever it sits.
        Project lone = makeDefaultProject();
        Sequence& ls = *lone.active();
        ls.width = 64;
        ls.height = 36;
        edit::overwrite(lone, ls, {TrackKind::Video, 0}, colorClip(lone, 0, 1, 0, 0, 20));
        const QByteArray before = frameKey(lone, ls, 5, o);
        trackAt(ls, {TrackKind::Video, 0})->clips[0].start = 10;
        QCOMPARE(frameKey(lone, ls, 15, o), before);
        // Rendering fills the cache once; frames come back as rendered (JPEG, near enough).
        QCOMPARE(renderToCache(p, s, 0, 59, o, cache), 60);
        QCOMPARE(renderToCache(p, s, 0, 59, o, cache), 0);
        QCOMPARE(cache.count(), 60);
        QVERIFY(cache.bytes() > 0);
        const QImage got = cache.load(k10);
        QVERIFY(!got.isNull() && got.width() == 64 && got.height() == 36);
        const QRgb c = got.pixel(32, 18);
        QVERIFY2(qRed(c) > 245 && qGreen(c) < 10 && qBlue(c) < 10, qPrintable(QString("%1 %2 %3").arg(qRed(c)).arg(qGreen(c)).arg(qBlue(c))));
        QCOMPARE(cachedRanges(p, s, 0, 70, o, cache), (std::vector<std::pair<FrameTime, FrameTime>>{{0, 60}}));
        // After an edit, only the frames it does not touch stay rendered.
        blue.effects.push_back(makeEffect(p, "invert"));
        QCOMPARE(cachedRanges(p, s, 0, 70, o, cache), (std::vector<std::pair<FrameTime, FrameTime>>{{0, 30}}));
        // The cache is found again by a new session, and can be cleared.
        RenderCache again(dir.path() + "/render");
        QCOMPARE(again.count(), 60);
        QVERIFY(again.has(k10));
        again.clear();
        QCOMPARE(again.count(), 0);
        QVERIFY(again.load(k10).isNull());
        // Cancelling stops at once.
        std::atomic<bool> stop{true};
        QCOMPARE(renderToCache(p, s, 0, 59, o, again, {}, &stop), -1);
    }

    void captionLooksDrawn() {
        // White text over mid grey, so a shadow (darker) and a coloured outline both show.
        CaptionTrack track;
        track.style = findCaptionLook("broadcast")->style;
        track.style.size = 0.12;
        track.captions = {{0, 10, "shadow test"}};
        auto draw = [&] {
            Image img(320, 180);
            img.fill(0.5f, 0.5f, 0.5f, 1);
            drawCaption(img, track, 5);
            return img;
        };
        auto count = [](const Image& img, auto pred) {
            int n = 0;
            for (int y = 0; y < 180; ++y)
                for (int x = 0; x < 320; ++x)
                    if (pred(img.at(x, y))) ++n;
            return n;
        };
        auto dark = [](const float* p) { return p[0] < 0.25f && p[1] < 0.25f && p[2] < 0.25f; };
        auto magenta = [](const float* p) { return p[0] > 0.6f && p[1] < 0.3f && p[2] > 0.5f; };
        auto white = [](const float* p) { return p[0] > 0.9f && p[1] > 0.9f && p[2] > 0.9f; };
        // The shadow and black outline darken more of the frame than the outline alone.
        const int withShadow = count(draw(), dark);
        track.style.shadow = 0;
        const int outlineOnly = count(draw(), dark);
        QVERIFY2(withShadow > outlineOnly * 1.2, qPrintable(QString("%1 %2").arg(withShadow).arg(outlineOnly)));
        // A magenta outline instead of black.
        track.style.outlineR = 0.9, track.style.outlineG = 0.1, track.style.outlineB = 0.8;
        QVERIFY(count(draw(), magenta) > 100);
        QVERIFY(count(draw(), dark) < outlineOnly / 4);
        // Capitals take more room than lower case.
        track.style.outline = 0;
        const int lower = count(draw(), white);
        track.style.allCaps = true;
        const int upper = count(draw(), white);
        QVERIFY2(upper > lower * 1.15, qPrintable(QString("%1 %2").arg(upper).arg(lower)));
    }

    void captionPlacementDrawn() {
        // Where the ink of a caption falls: the usual bottom centre, the top left, the middle right.
        CaptionTrack track;
        track.style.boxOpacity = 0;
        track.style.size = 0.08;
        track.captions = {{0, 10, "Placed"}};
        struct Ink {
            int left = 1 << 30, right = -1, top = 1 << 30, bottom = -1;
        };
        auto draw = [&](int keypad) {
            setCaptionKeypad(track.captions[0], keypad);
            Image img(320, 180);
            img.fill(0, 0, 0, 1);
            drawCaption(img, track, 5);
            Ink k;
            for (int y = 0; y < 180; ++y)
                for (int x = 0; x < 320; ++x)
                    if (img.at(x, y)[0] > 0.5f) {
                        k.left = std::min(k.left, x);
                        k.right = std::max(k.right, x);
                        k.top = std::min(k.top, y);
                        k.bottom = std::max(k.bottom, y);
                    }
            return k;
        };
        const Ink usual = draw(2), topLeft = draw(7), middleRight = draw(6);
        QVERIFY(usual.right > 0 && topLeft.right > 0 && middleRight.right > 0);
        // Bottom centre: ending above 92 % of the height, centred.
        QVERIFY2(usual.bottom <= 166 && usual.bottom > 140, qPrintable(QString::number(usual.bottom)));
        QVERIFY(std::abs((usual.left + usual.right) / 2 - 160) <= 3);
        // Top left: as far from the top as the usual place is from the bottom, starting at the 5 % margin.
        QVERIFY2(topLeft.top >= 14 && topLeft.top < 30, qPrintable(QString::number(topLeft.top)));
        QVERIFY2(std::abs(topLeft.left - 16) <= 3, qPrintable(QString::number(topLeft.left)));
        QVERIFY(std::abs((topLeft.right - topLeft.left) - (usual.right - usual.left)) <= 2);  // the same text, placed elsewhere
        // Middle right: centred on the height, ending at the right margin.
        QVERIFY2(std::abs((middleRight.top + middleRight.bottom) / 2 - 90) <= 6, qPrintable(QString::number(middleRight.top)));
        QVERIFY2(std::abs(middleRight.right - 304) <= 3, qPrintable(QString::number(middleRight.right)));
    }

    void wordByWordCaptions() {
        CaptionTrack track;
        track.style.textR = track.style.textG = track.style.textB = 1;
        track.style.hiR = 0;
        track.style.hiG = 1;
        track.style.hiB = 0;
        track.style.boxOpacity = 0;
        track.style.size = 0.1;
        track.captions = {{0, 30, "red green blue", {0, 0.33, 0.66}}};
        struct Ink {
            double amount = 0, highlight = 0, hx = 0;
            int left = 1 << 30, right = -1;
        };
        auto draw = [&](int anim, FrameTime t) {
            track.style.animation = anim;
            Image img(320, 180);
            img.fill(0, 0, 0, 1);
            drawCaption(img, track, t);
            Ink k;
            for (int y = 0; y < 180; ++y)
                for (int x = 0; x < 320; ++x) {
                    const float* p = img.at(x, y);
                    const double v = (p[0] + p[1] + p[2]) / 3;
                    if (v < 0.2) continue;
                    k.amount += v;
                    k.left = std::min(k.left, x);
                    k.right = std::max(k.right, x);
                    if (p[1] > 0.6f && p[0] < 0.3f) {  // the highlight colour
                        k.highlight += 1;
                        k.hx += x;
                    }
                }
            if (k.highlight > 0) k.hx /= k.highlight;
            return k;
        };
        // None: no highlight, the whole line.
        const Ink plain = draw(0, 15);
        QVERIFY(plain.amount > 0 && plain.highlight == 0);
        // Highlight: the spoken word is green, moving left to right.
        const Ink first = draw(2, 2), last = draw(2, 25);
        QVERIFY(first.highlight > 20 && last.highlight > 20);
        QVERIFY2(first.hx < 140 && last.hx > 180, qPrintable(QString("%1 %2").arg(first.hx).arg(last.hx)));
        QVERIFY(std::fabs(first.amount - plain.amount) < 0.25 * plain.amount);  // the rest is still there
        // Word by word: more appears as it is said.
        const Ink one = draw(1, 2), two = draw(1, 12), three = draw(1, 25);
        QVERIFY(one.amount < two.amount && two.amount < three.amount);
        QCOMPARE(one.left, plain.left);  // the line keeps its place as it fills in
        // Pop: the spoken word is larger than when only highlighted.
        const Ink popped = draw(3, 14), lit = draw(2, 14);
        QVERIFY2(popped.highlight > lit.highlight * 1.15, qPrintable(QString("%1 vs %2").arg(popped.highlight).arg(lit.highlight)));
        // One word at a time: just the spoken word, large, in the middle.
        const Ink single = draw(4, 14);
        QVERIFY(single.right - single.left < plain.right - plain.left);
        QVERIFY(std::abs((single.left + single.right) / 2 - 160) <= 4);
        QVERIFY(single.highlight > lit.highlight);
        // Outside the caption nothing is drawn.
        QCOMPARE(draw(2, 40).amount, 0.0);
    }

    void burnInsOnFrames() {
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        s.fps = {25, 1};
        Clip c = colorClip(p, 0.5f, 0.5f, 0.5f, 0, 50);
        c.name = "Interview";
        edit::overwrite(p, s, {TrackKind::Video, 0}, c);
        auto frame = [&](const BurnIn& b, FrameTime t, const QImage* logo = nullptr) {
            Image img(320, 180);
            img.fill(0, 0, 0, 1);
            drawBurnIns(img, p, s, t, b, logo);
            return img;
        };
        auto inked = [](const Image& img, int x0, int y0, int x1, int y1) {
            int n = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) n += img.at(x, y)[0] > 0.5f ? 1 : 0;
            return n;
        };
        BurnIn tc;
        tc.timecode = true;
        const Image a = frame(tc, 0), b = frame(tc, 1);
        QVERIFY(inked(a, 0, 0, 160, 60) > 20);         // top left
        QCOMPARE(inked(a, 160, 90, 320, 180), 0);       // nothing elsewhere
        int differ = 0;
        for (size_t i = 0; i < a.px.size(); ++i) differ += std::fabs(a.px[i] - b.px[i]) > 0.1f ? 1 : 0;
        QVERIFY(differ > 0);  // the timecode moves on
        // Clip name and text add lines; another corner puts them there.
        BurnIn more = tc;
        more.clipName = true;
        more.text = "DRAFT";
        more.corner = 5;
        const Image m = frame(more, 10);
        QVERIFY(inked(m, 160, 90, 320, 180) > inked(a, 0, 0, 160, 60));
        QCOMPARE(inked(m, 0, 0, 160, 90), 0);
        // A watermark at 60 % in the bottom right; a quarter of the width wide.
        QImage logo(40, 20, QImage::Format_ARGB32);
        logo.fill(QColor(255, 0, 0));
        BurnIn wm;
        wm.watermark = "logo";
        wm.watermarkWidth = 0.25;
        wm.watermarkOpacity = 0.6;
        const Image w = frame(wm, 0, &logo);
        const float* px = w.at(320 - int(0.03 * 320) - 10, 180 - int(0.03 * 180) - 10);
        QVERIFY2(std::fabs(px[0] - 0.6f) < 0.03f && px[1] < 0.05f, qPrintable(QString::number(px[0])));
        QVERIFY(w.at(100, 100)[0] < 0.01f);
        QVERIFY(!BurnIn{}.any() && tc.any() && wm.any());
    }

    void shapeLayers() {
        const int W = 400, H = 300;
        auto make = [](int shape, double w, double h) {
            Effect e = makeEffect("shape", 1);
            e.params["shape"] = Param(double(shape));
            e.params["width"] = Param(w);
            e.params["height"] = Param(h);
            e.params["fill_color.r"] = Param(1.0);
            e.params["fill_color.g"] = Param(0.0);
            e.params["fill_color.b"] = Param(0.0);
            return e;
        };
        // Coverage in pixels (alpha summed, so anti-aliased edges count by how much they are covered).
        auto area = [](const Image& img) {
            double a = 0;
            for (size_t i = 3; i < img.px.size(); i += 4) a += img.px[i];
            return a;
        };
        auto near = [](double got, double want, double tol) { return std::fabs(got - want) <= tol * want; };
        // Areas against the formulas.
        const Image rect = renderGenerator(make(0, 100, 50), 0, W, H, 1.0);
        QVERIFY2(near(area(rect), 5000, 0.01), qPrintable(QString::number(area(rect))));
        QVERIFY(rect.at(200, 150)[0] > 0.99f && rect.at(200, 150)[3] > 0.99f && rect.at(260, 150)[3] < 0.01f);
        QVERIFY2(near(area(renderGenerator(make(1, 100, 50), 0, W, H, 1.0)), M_PI * 50 * 25, 0.01), "ellipse");
        Effect rounded = make(0, 200, 100);
        rounded.params["roundness"] = Param(20.0);
        QVERIFY2(near(area(renderGenerator(rounded, 0, W, H, 1.0)), 200 * 100 - (4 - M_PI) * 400, 0.01), "rounded rectangle");
        Effect hexagon = make(2, 200, 200);
        hexagon.params["points"] = Param(6.0);
        QVERIFY2(near(area(renderGenerator(hexagon, 0, W, H, 1.0)), 1.5 * std::sqrt(3.0) * 100 * 100, 0.01), "hexagon");
        Effect star = make(3, 200, 200);
        star.params["inner"] = Param(50.0);
        QVERIFY2(near(area(renderGenerator(star, 0, W, H, 1.0)), 5 * 100 * 50 * std::sin(M_PI / 5), 0.015), "star");
        // Drawn at the output's scale: half size is a quarter of the area.
        QVERIFY(near(area(renderGenerator(make(0, 100, 50), 0, W / 2, H / 2, 0.5)), 1250, 0.02));
        // Rotated a quarter turn, moved and see-through.
        Effect turned = make(0, 100, 20);
        turned.params["rotation"] = Param(90.0);
        turned.params["pos_x"] = Param(-100.0);
        turned.params["opacity"] = Param(50.0);
        const Image tu = renderGenerator(turned, 0, W, H, 1.0);
        QVERIFY(std::fabs(tu.at(100, 110)[3] - 0.5f) < 0.01f && tu.at(100, 110)[0] > 0.49f);  // straight up and down now
        QVERIFY(tu.at(140, 150)[3] < 0.01f);
        // A linear gradient left to right, and a radial one from the middle.
        Effect grad = make(0, 300, 100);
        grad.params["gradient"] = Param(1.0);
        grad.params["fill_color2.r"] = Param(0.0);
        grad.params["fill_color2.g"] = Param(0.0);
        grad.params["fill_color2.b"] = Param(1.0);
        const Image gi = renderGenerator(grad, 0, W, H, 1.0);
        QVERIFY(gi.at(52, 150)[0] > 0.97f && gi.at(347, 150)[2] > 0.97f && std::fabs(gi.at(200, 150)[0] - 0.5f) < 0.02f);
        grad.params["gradient"] = Param(2.0);
        const Image ri = renderGenerator(grad, 0, W, H, 1.0);
        QVERIFY(ri.at(200, 150)[0] > 0.98f && ri.at(340, 150)[2] > ri.at(340, 150)[0]);

        // Outlines: a 4 px stroke round a 200 x 100 rectangle, no fill: about 600 px long.
        Effect outline = make(0, 200, 100);
        outline.params["fill"] = Param(0.0);
        outline.params["stroke"] = Param(4.0);
        outline.params["join"] = Param(1.0);  // sharp corners, flat ends
        const double full = area(renderGenerator(outline, 0, W, H, 1.0));
        QVERIFY2(near(full, 600 * 4, 0.02), qPrintable(QString::number(full)));
        // Trimmed to its first half, and to a quarter moved round by an offset.
        outline.params["trim_end"] = Param(50.0);
        QVERIFY2(near(area(renderGenerator(outline, 0, W, H, 1.0)), full / 2, 0.03), "trim to half");
        outline.params["trim_start"] = Param(25.0);
        outline.params["trim_offset"] = Param(270.0);  // three quarters round: 25-50 % becomes 100-125 %, across the join
        const double quarter = area(renderGenerator(outline, 0, W, H, 1.0));
        QVERIFY2(near(quarter, full / 4, 0.04), qPrintable(QString::number(quarter / full)));
        // Dashes: 12 on, 12 off is half the ink.
        outline.params["trim_start"] = Param(0.0);
        outline.params["trim_end"] = Param(100.0);
        outline.params["trim_offset"] = Param(0.0);
        outline.params["dash"] = Param(12.0);
        outline.params["gap"] = Param(12.0);
        QVERIFY2(near(area(renderGenerator(outline, 0, W, H, 1.0)), full / 2, 0.06), "dashes");
        // trimPath on its own: lengths add up, and nothing is left when start meets end.
        QPainterPath line;
        line.moveTo(0, 0);
        line.lineTo(100, 0);
        QCOMPARE(trimPath(line, 0.2, 0.7, 0).length(), 50.0);
        QCOMPARE(trimPath(line, 0.6, 0.9, 0.2).length(), 20.0);  // an open path stops at its end
        QVERIFY(trimPath(line, 0.5, 0.5, 0).isEmpty());

        // Keyframed Trim End draws a line on over the clip; an arrow points right.
        Effect drawOn = make(4, 200, 10);
        drawOn.params["stroke"] = Param(6.0);
        drawOn.params["join"] = Param(1.0);
        Param te;
        te.addKey(0, 0.0);
        te.addKey(30, 100.0);
        drawOn.params["trim_end"] = te;
        QCOMPARE(area(renderGenerator(drawOn, 0, W, H, 1.0)), 0.0);
        QVERIFY(near(area(renderGenerator(drawOn, 15, W, H, 1.0)), 100 * 6, 0.03));
        QVERIFY(near(area(renderGenerator(drawOn, 30, W, H, 1.0)), 200 * 6, 0.03));
        const Image arrow = renderGenerator(make(5, 200, 60), 0, W, H, 1.0);
        QVERIFY(arrow.at(296, 150)[3] > 0.9f && arrow.at(296, 130)[3] < 0.1f);  // the tip, narrow
        QVERIFY(arrow.at(245, 128)[3] > 0.9f && arrow.at(150, 128)[3] < 0.1f);  // the head is wide, the shaft is not
    }

    void videoNoiseReduction() {
        // A picture with flat areas, soft gradients, hard edges and fine texture, moving 2 px right and 1 px down a
        // frame, with fresh noise (sigma 0.04 in each channel) on every frame.
        const int W = 192, H = 108;
        const double sigma = 0.04;
        auto clean = [&](int t) {
            Image img(W, H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const double u = x - 2.0 * t, v = y - 1.0 * t;
                    double r = 0.3 + 0.3 * u / W, g = 0.45, b = 0.35 + 0.25 * v / H;
                    if (std::fmod(std::floor(u / 24) + std::floor(v / 24) + 200, 2.0) < 1) g += 0.25;  // edges
                    if (u > 120 && u < 170) r += 0.08 * std::sin(u * 0.9) * std::sin(v * 0.7);         // texture
                    float* p = img.at(x, y);
                    p[0] = float(r), p[1] = float(g), p[2] = float(b), p[3] = 1;
                }
            return img;
        };
        // Box-Muller on the raw generator, so every platform gets the same noise (std::normal_distribution's
        // algorithm differs between standard libraries, and the edge check below depends on the exact noise).
        std::mt19937 rng(7);
        auto gauss = [&rng, sigma] {
            const double u1 = (double(rng()) + 1.0) / 4294967296.0, u2 = double(rng()) / 4294967296.0;
            return float(sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2));
        };
        std::vector<Image> truth, noisy;
        for (int t = 0; t < 5; ++t) {
            truth.push_back(clean(t));
            Image n = truth.back();
            for (size_t i = 0; i < n.px.size(); ++i)
                if (i % 4 != 3) n.px[i] += gauss();
            noisy.push_back(n);
        }
        // RMS error against the clean frame, away from the borders (where the picture slides in).
        auto rmse = [&](const Image& a, const Image& b) {
            double acc = 0;
            int count = 0;
            for (int y = 8; y < H - 8; ++y)
                for (int x = 8; x < W - 8; ++x)
                    for (int c = 0; c < 3; ++c, ++count) acc += std::pow(double(a.at(x, y)[c]) - b.at(x, y)[c], 2);
            return std::sqrt(acc / count);
        };
        const double before = rmse(noisy[2], truth[2]);
        QVERIFY(std::fabs(before - sigma) < 0.003);
        // The noise is measured from the frame.
        const double measured = estimateNoise(noisy[2]);
        QVERIFY2(std::fabs(measured - sigma) < 0.2 * sigma, qPrintable(QString::number(measured)));
        QVERIFY(estimateNoise(truth[2]) < 0.005);

        const std::vector<const Image*> around = {&noisy[0], &noisy[1], &noisy[3], &noisy[4]};
        // Temporal only, following the motion: close to the sqrt(5) of averaging five frames.
        DenoiseSettings temporal;
        temporal.spatialLuma = temporal.spatialChroma = 0;
        double used = 0;
        const double motion = rmse(denoiseFrame(noisy[2], around, temporal, {}, &used), truth[2]);
        QVERIFY2(motion < 0.45 * before, qPrintable(QString::number(motion / before)));
        QVERIFY(std::fabs(used - measured) < 1e-9);
        // Without motion compensation the moving picture mostly does not match, so less is averaged (but nothing ghosts).
        DenoiseSettings still = temporal;
        still.motion = false;
        const double stillErr = rmse(denoiseFrame(noisy[2], around, still), truth[2]);
        QVERIFY2(stillErr > 1.4 * motion && stillErr < 0.8 * before, qPrintable(QString("%1 %2").arg(stillErr).arg(motion)));
        // Spatial only: less noise, edges kept (the step between checker squares stays within 10 % of its height).
        DenoiseSettings spatial;
        spatial.temporal = 0;
        spatial.spatialLuma = 0.6;
        spatial.spatialChroma = 0.8;
        const Image sp = denoiseFrame(noisy[2], {}, spatial);
        const double spatialErr = rmse(sp, truth[2]);
        QVERIFY2(spatialErr < 0.5 * before, qPrintable(QString::number(spatialErr / before)));
        auto step = [&](const Image& img) {
            // Green across the vertical edge at u = 24 (x = 28 on frame 2), averaged down rows 10-30.
            double left = 0, right = 0;
            for (int y = 10; y < 30; ++y) {
                left += img.at(26, y)[1];
                right += img.at(29, y)[1];
            }
            return std::fabs(right - left) / 20;
        };
        QVERIFY2(std::fabs(step(sp) - step(truth[2])) < 0.1 * step(truth[2]),
                 qPrintable(QString("%1 %2").arg(step(sp)).arg(step(truth[2]))));
        // Both together beat either alone.
        DenoiseSettings both;
        both.spatialLuma = 0.6;
        both.spatialChroma = 0.8;
        const double bothErr = rmse(denoiseFrame(noisy[2], around, both), truth[2]);
        QVERIFY2(bothErr < 0.3 * before && bothErr < motion && bothErr < spatialErr, qPrintable(QString("%1 %2 %3").arg(bothErr).arg(motion).arg(spatialErr)));
        // Motion chained from the flows between consecutive frames: (2, 1) px a frame, and each flow measured once.
        auto frame = [&](int64_t i) -> const Image* { return i >= 0 && i < 5 ? &noisy[size_t(i)] : nullptr; };
        const std::vector<int> offsets = {-2, -1, 1, 2};
        const std::vector<MotionFn> chain = chainedMotion("nr-test", 2, offsets, frame);
        QCOMPARE(chain.size(), size_t(4));
        for (size_t i = 0; i < 4; ++i) {
            QVERIFY(bool(chain[i]));
            // The median over the inside of the picture (one point can sit where an edge leaves the flow ambiguous).
            std::vector<double> dx, dy;
            for (int y = 20; y <= 88; y += 4)
                for (int x = 24; x <= 168; x += 4) {
                    const Point2 d = chain[i](x, y);
                    dx.push_back(d.x), dy.push_back(d.y);
                }
            std::nth_element(dx.begin(), dx.begin() + long(dx.size() / 2), dx.end());
            std::nth_element(dy.begin(), dy.begin() + long(dy.size() / 2), dy.end());
            const double mx = dx[dx.size() / 2], my = dy[dy.size() / 2];
            QVERIFY2(std::fabs(mx - 2 * offsets[i]) < 0.35 && std::fabs(my - offsets[i]) < 0.35,
                     qPrintable(QString("%1: %2 %3").arg(offsets[i]).arg(mx).arg(my)));
        }
        const double chained = rmse(denoiseFrame(noisy[2], around, temporal, chain), truth[2]);
        QVERIFY2(chained < 1.1 * motion, qPrintable(QString("%1 %2").arg(chained).arg(motion)));
        // The next frame reuses them all (no frames given, so nothing could be measured).
        const std::vector<MotionFn> next = chainedMotion("nr-test", 3, {-2, -1, 1}, [](int64_t) { return nullptr; });
        QVERIFY(next[0] && next[1] && next[2]);
        QVERIFY(!chainedMotion("nr-test", 3, {2}, [](int64_t) { return nullptr; })[0]);  // 4 -> 5 was never measured
        // Blend Original at 1 gives the frame back; a clean frame comes back unchanged.
        DenoiseSettings back = both;
        back.blend = 1;
        QVERIFY(rmse(denoiseFrame(noisy[2], around, back), noisy[2]) < 1e-6);
        QVERIFY(rmse(denoiseFrame(truth[2], {&truth[1], &truth[3]}, both), truth[2]) < 0.004);

        // As a clip effect on a still image: the spatial pass, under a mask when it has one.
        QVERIFY(findEffectInfo("video_denoise") && findEffectInfo("video_denoise")->category == EffectCategory::VideoFilter);
    }

    void qualityCheckPicture() {
        // The flash counter on whole frames (linear light): `share` of the rows lit in the "on" state, black otherwise.
        const int W = 32, H = 20;
        auto frame = [&](double r, double g, double b, double share) {
            std::vector<float> f(size_t(W) * H * 3, 0.0f);
            for (int y = 0; y < int(std::lround(H * share)); ++y)
                for (int x = 0; x < W; ++x) {
                    float* px = &f[(size_t(y) * W + x) * 3];
                    px[0] = float(r), px[1] = float(g), px[2] = float(b);
                }
            return f;
        };
        // The most transitions in any second of two seconds alternating every `half` frames at 30 fps.
        auto most = [&](int half, double on, double off, double share, bool red, double g = -1) {
            FlashDetector d(30);
            int best = 0;
            for (int i = 0; i < 60; ++i) {
                const double v = (i / half) % 2 ? on : off;
                const auto f = g < 0 ? frame(v, v, v, share) : frame(v, (i / half) % 2 ? g : off, (i / half) % 2 ? g : off, share);
                d.add(f.data(), W, H);
                best = std::max(best, d.transitionsInLastSecond(red));
            }
            return best;
        };
        QCOMPARE(most(3, 1, 0, 1, false), 10);   // five flashes a second
        QCOMPARE(most(4, 1, 0, 1, false), 8);    // 3.75: fails
        QCOMPARE(most(5, 1, 0, 1, false), 6);    // three a second: allowed
        QCOMPARE(most(15, 1, 0, 1, false), 2);
        QCOMPARE(most(3, 1, 0, 0.2, false), 0);  // a fifth of the screen
        QCOMPARE(most(3, 1, 0, 0.3, false), 10); // over a quarter
        QCOMPARE(most(3, 0.05, 0, 1, false), 0); // 10 cd/m2: too small
        QCOMPARE(most(3, 1, 0.85, 1, false), 0); // both sides brighter than 160 cd/m2
        // Saturated red to black: under the luminance threshold, but a red flash.
        QCOMPARE(most(3, 0.3, 0, 1, false, 0), 0);
        QCOMPARE(most(3, 0.3, 0, 1, true, 0), 10);
        QCOMPARE(most(3, 0.3, 0, 1, true, 0.1), 0);  // not saturated red
        // A slow fade up and down is one transition each way.
        {
            FlashDetector d(30);
            int best = 0;
            for (int i = 0; i < 60; ++i) {
                const double v = i < 30 ? i / 29.0 : (59 - i) / 29.0;
                const auto f = frame(v, v, v, 1);
                d.add(f.data(), W, H);
                best = std::max(best, d.transitionsInLastSecond());
            }
            QVERIFY(best <= 2);
        }

        // Broadcast Safe: hue kept, luma limited, premultiplied pixels handled.
        {
            std::vector<float> px = {1.2f, 0.5f, 0.5f, 1, 0.5f, 0.5f, 0.5f, 1, -0.2f, -0.2f, -0.2f, 1, 1.1f, 1.1f, 1.1f, 0.5f};
            const std::vector<float> orig = px;
            QCOMPARE(broadcastSafe(px.data(), 4, 1, false, 0), 3);
            QCOMPARE(px[0], 1.05f);
            QVERIFY(px[1] == px[2] && px[1] > 0.5f && px[1] < 0.6488f);  // pulled towards luma, keeping hue
            QVERIFY(std::fabs((0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]) - (0.2126 * 1.2 + 0.7152 * 0.5 + 0.0722 * 0.5)) < 1e-4);
            QVERIFY(std::equal(px.begin() + 4, px.begin() + 8, orig.begin() + 4));  // legal: untouched
            QVERIFY(std::fabs(px[8] + 0.01f) < 1e-5);
            QVERIFY(std::fabs(px[12] - 0.515f) < 1e-5 && px[15] == 0.5f);  // 2.2 at half alpha: 1.03, premultiplied
            px = orig;
            QCOMPARE(broadcastSafe(px.data(), 4, 1, true, 0), 3);
            QCOMPARE(px[0], 1.0f);
            QVERIFY(std::fabs(px[8]) < 1e-6);
            // A soft knee eases values near the limit and keeps them under it.
            std::vector<float> knee = {0.97f, 0.97f, 0.97f, 1, 3, 3, 3, 1, 0.5f, 0.5f, 0.5f, 1};
            QCOMPARE(broadcastSafe(knee.data(), 3, 1, true, 0.05), 1);
            QVERIFY(knee[0] < 0.97f && knee[0] > 0.95f);
            QVERIFY(knee[4] <= 1.0f && knee[4] > 0.99f);
            QCOMPARE(knee[8], 0.5f);
            // Highlighting stripes the unsafe pixels only.
            px = orig;
            broadcastSafe(px.data(), 4, 1, false, 0, true);
            QVERIFY(std::equal(px.begin() + 4, px.begin() + 8, orig.begin() + 4));
            QVERIFY(px[1] == 0 && px[0] == px[2]);
        }

        // The whole check on a sequence: flashing, then black, then a long still, then over-range red.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64;
        s.height = 36;
        s.fps = Rational{30, 1};
        auto matte = [&](FrameTime start, FrameTime length, double r, double g, double b) {
            Clip c = makeGeneratorClip(p, "color", length);
            c.start = start;
            c.generator.params["color.r"] = r;
            c.generator.params["color.g"] = g;
            c.generator.params["color.b"] = b;
            return c;
        };
        Clip flash = matte(0, 60, 0, 0, 0);
        for (const char* ch : {"color.r", "color.g", "color.b"}) {
            Param k;
            for (int i = 0; i <= 20; ++i) k.addKey(i * 3, double(i % 2));
            flash.generator.params[ch] = k;
        }
        edit::overwrite(p, s, {TrackKind::Video, 0}, flash);
        edit::overwrite(p, s, {TrackKind::Video, 0}, matte(60, 45, 0, 0, 0));
        edit::overwrite(p, s, {TrackKind::Video, 0}, matte(105, 180, 0.5, 0.5, 0.5));
        edit::overwrite(p, s, {TrackKind::Video, 0}, matte(285, 30, 1.3, 0.2, 0.2));
        QcSettings q;
        q.silenceSeconds = 0;
        q.clipping = false;
        int ticks = 0;
        std::vector<QcIssue> issues = qualityCheck(p, s, 0, -1, q, [&](double) { ++ticks; });
        QVERIFY(ticks >= 315);
        QString got;
        for (const QcIssue& i : issues) got += QStringLiteral("%1 %2-%3; ").arg(qcKindName(i.kind)).arg(i.start).arg(i.end);
        QCOMPARE(issues.size(), size_t(4));
        QCOMPARE(issues[0].kind, QcKind::Flashing);
        QVERIFY2(issues[0].start <= 3 && issues[0].end >= 57 && issues[0].end <= 61, qPrintable(got));
        QVERIFY2(QString::fromStdString(issues[0].text).contains("5 flashes"), issues[0].text.c_str());
        QCOMPARE(issues[1].kind, QcKind::Black);
        QVERIFY2(issues[1].start == 60 && issues[1].end == 105, qPrintable(got));
        QCOMPARE(issues[2].kind, QcKind::Freeze);
        QVERIFY2(issues[2].start == 105 && issues[2].end == 285, qPrintable(got));
        QCOMPARE(issues[3].kind, QcKind::Levels);
        QVERIFY2(issues[3].start == 285 && issues[3].end == 315, qPrintable(got));
        QVERIFY(QString::fromStdString(issues[3].text).contains("100.0 %"));
        // Thresholds: a longer freeze limit, no black check, a range.
        q.freezeSeconds = 7;
        q.blackSeconds = 0;
        issues = qualityCheck(p, s, 0, -1, q);
        QVERIFY(std::none_of(issues.begin(), issues.end(), [](const QcIssue& i) { return i.kind == QcKind::Freeze || i.kind == QcKind::Black; }));
        q.blackSeconds = 1;
        issues = qualityCheck(p, s, 60, 105, q);
        QCOMPARE(issues.size(), size_t(1));
        QCOMPARE(issues[0].kind, QcKind::Black);
        // Broadcast Safe on the red clip makes it legal.
        trackAt(s, {TrackKind::Video, 0})->clips.back().effects.push_back(makeEffect(p, "broadcast_safe"));
        QcSettings levelsOnly;
        levelsOnly.flashing = false, levelsOnly.blackSeconds = 0, levelsOnly.freezeSeconds = 0, levelsOnly.silenceSeconds = 0,
        levelsOnly.clipping = false;
        QVERIFY(qualityCheck(p, s, 0, -1, levelsOnly).empty());
        // Markers: red, spanning each problem, replacing the last check's.
        s.markers.push_back(Marker{10, 0, "Mine", "", 0});
        issues = qualityCheck(p, s, 0, -1, QcSettings{true, true, 1, 5, 0, false});
        QCOMPARE(addQcMarkers(s, issues), int(issues.size()));
        QCOMPARE(addQcMarkers(s, issues), int(issues.size()));
        QCOMPARE(s.markers.size(), issues.size() + 1);
        const auto black = std::find_if(s.markers.begin(), s.markers.end(), [](const Marker& m) { return m.name == "QC: Black"; });
        QVERIFY(black != s.markers.end());
        QVERIFY(black->t == 60 && black->duration == 44 && black->color == 11);
        // Cancelled: nothing.
        std::atomic<bool> cancel{true};
        QVERIFY(qualityCheck(p, s, 0, -1, q, {}, &cancel).empty());
    }

    void lutExport() {
        Project p = makeDefaultProject();
        QTemporaryDir dir;
        // No grade: the identity lattice, red varying fastest.
        const Lut3D id = bakeLut({}, 0, 5);
        QCOMPARE(id.size, 5);
        for (int b = 0; b < 5; ++b)
            for (int g = 0; g < 5; ++g)
                for (int r = 0; r < 5; ++r) {
                    const float* v = &id.data[(size_t(r) + 5 * (size_t(g) + 5 * size_t(b))) * 3];
                    QVERIFY(std::fabs(v[0] - r / 4.0f) < 1e-6f && std::fabs(v[1] - g / 4.0f) < 1e-6f && std::fabs(v[2] - b / 4.0f) < 1e-6f);
                }
        // A grade: Color Correct (warmer, more contrast and saturation), Curves and a hue turn.
        Effect cc = makeEffect(p, "color_correct");
        cc.params["temperature"] = 0.3;
        cc.params["saturation"] = 1.3;
        cc.params["gamma"] = 0.9;
        cc.params["lift"] = 0.03;
        Effect curves = makeEffect(p, "curves");
        curves.strings["master"] = "0,0 0.3,0.25 0.7,0.78 1,1";
        Effect hs = makeEffect(p, "hue_sat");
        hs.params["hue"] = 15.0;
        const std::vector<Effect> grade{cc, curves, hs};
        const std::string path = (dir.path() + "/grade.cube").toStdString();
        std::string err;
        const Lut3D baked = bakeLut(grade, 0, 33);
        QVERIFY2(writeCubeLut(baked, path, "Grade", &err), err.c_str());
        // Read back exactly (to the six decimals written), with its size and title.
        const auto loaded = loadCubeLut(path, &err);
        QVERIFY2(loaded, err.c_str());
        QCOMPARE(loaded->size, 33);
        float worstRead = 0;
        for (size_t i = 0; i < baked.data.size(); ++i) worstRead = std::max(worstRead, std::fabs(loaded->data[i] - baked.data[i]));
        QVERIFY(worstRead < 1e-5f);
        {
            std::ifstream f(path);
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            QVERIFY(text.find("TITLE \"Grade\"") != std::string::npos && text.find("LUT_3D_SIZE 33") != std::string::npos);
        }
        // A picture through the LUT effect looks like the picture through the grade itself.
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> u(0, 1);
        Image direct(64, 32);
        for (size_t i = 0; i < direct.px.size(); i += 4) direct.px[i] = u(rng), direct.px[i + 1] = u(rng), direct.px[i + 2] = u(rng), direct.px[i + 3] = 1;
        Image viaLut = direct;
        for (const Effect& e : grade) applyVideoEffect(e, 0, direct, 1);
        Effect lutFx = makeEffect(p, "lut");
        lutFx.strings["path"] = path;
        applyVideoEffect(lutFx, 0, viaLut, 1);
        float worst = 0;
        double mean = 0;
        for (size_t i = 0; i < direct.px.size(); ++i) {
            worst = std::max(worst, std::fabs(direct.px[i] - viaLut.px[i]));
            mean += std::fabs(direct.px[i] - viaLut.px[i]);
        }
        mean /= double(direct.px.size());
        QVERIFY2(worst < 0.02f && mean < 0.002, qPrintable(QString("%1 %2").arg(worst).arg(mean)));
        // Spatial and masked effects are left out and named; disabled ones are ignored.
        Effect blur = makeEffect(p, "gaussian_blur");
        Effect masked = cc;
        masked.params["mask.shape"] = 1.0;
        Effect off = hs;
        off.enabled = false;
        std::vector<std::string> skipped;
        const Lut3D partial = bakeLut({cc, blur, masked, off}, 0, 9, &skipped);
        QCOMPARE(skipped.size(), size_t(2));
        QCOMPARE(QString::fromStdString(skipped[0]), QString::fromStdString(findEffectInfo("gaussian_blur")->displayName));
        QCOMPARE(partial.data, bakeLut({cc}, 0, 9).data);
        // Rewriting the file is picked up by clips using it.
        QVERIFY(writeCubeLut(id, path));
        Image same(8, 8);
        for (size_t i = 0; i < same.px.size(); i += 4) same.px[i] = 0.25f, same.px[i + 1] = 0.5f, same.px[i + 2] = 0.75f, same.px[i + 3] = 1;
        const Image before = same;
        applyVideoEffect(lutFx, 0, same, 1);
        float moved = 0;
        for (size_t i = 0; i < same.px.size(); ++i) moved = std::max(moved, std::fabs(same.px[i] - before.px[i]));
        QVERIFY2(moved < 1e-4f, qPrintable(QString::number(moved)));
        QVERIFY(!writeCubeLut(Lut3D{}, path));
    }

    void magnifyChannelBlurNoise() {
        Project p = makeDefaultProject();
        // A horizontal ramp on a 200 x 100 frame.
        Image ramp(200, 100);
        for (int y = 0; y < 100; ++y)
            for (int x = 0; x < 200; ++x) {
                float* px = ramp.at(x, y);
                px[0] = px[1] = px[2] = (x + 0.5f) / 200.0f, px[3] = 1;
            }
        // Magnify 2x in a circle 40 % of the height across at the middle, with a 2 px white border.
        Effect mag = makeEffect(p, "magnify");
        Image m = ramp;
        applyVideoEffect(mag, 0, m, 1);
        auto at = [](const Image& im, int x, int y) { return im.at(x, y)[0]; };
        QVERIFY(std::fabs(at(m, 100, 50) - at(ramp, 100, 50)) < 0.01f);               // the centre stays
        QVERIFY(std::fabs(at(m, 112, 50) - (100 + 12.5f / 2) / 200.0f) < 0.01f);       // 12 px out shows 6 px out
        QCOMPARE(at(m, 150, 50), at(ramp, 150, 50));                                  // outside the lens: untouched
        QVERIFY(at(m, 121, 50) > 0.95f && m.at(121, 50)[2] > 0.95f);                   // the border ring at the radius (20 px)
        // A square lens, no border, half opacity.
        mag.params["shape"] = 1.0;
        mag.params["border"] = 0.0;
        mag.params["opacity"] = 50.0;
        m = ramp;
        applyVideoEffect(mag, 0, m, 1);
        const float expect = 0.5f * at(ramp, 118, 32) + 0.5f * (100 + 18.5f / 2) / 200.0f;
        QVERIFY2(std::fabs(at(m, 118, 32) - expect) < 0.01f, qPrintable(QString("%1 %2").arg(at(m, 118, 32)).arg(expect)));  // a square's corner
        // Channel Blur: a vertical stripe; red blurred only across it, green untouched, blue only up and down (no change).
        Image stripe(64, 32);
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 64; ++x) {
                float* px = stripe.at(x, y);
                const float v = x >= 28 && x < 36 ? 1.0f : 0.0f;
                px[0] = px[1] = px[2] = v, px[3] = 1;
            }
        Effect cb = makeEffect(p, "channel_blur");
        cb.params["red"] = 4.0;
        cb.params["blue"] = 4.0;
        Image blurred = stripe;
        applyVideoEffect(cb, 0, blurred, 1);
        QVERIFY(blurred.at(26, 16)[0] > 0.1f && blurred.at(31, 16)[0] < 0.99f);  // red spread across the edge
        QCOMPARE(blurred.at(26, 16)[1], 0.0f);                                   // green untouched
        cb.params["dimensions"] = 2.0;  // vertical only: a vertical stripe does not change
        Image vertical = stripe;
        applyVideoEffect(cb, 0, vertical, 1);
        QVERIFY(std::fabs(vertical.at(26, 16)[0]) < 1e-4f && std::fabs(vertical.at(30, 16)[2] - 1) < 1e-4f);
        // Noise: none at 0; at 40 % the mean holds and the spread is as uniform noise gives; the same frame twice
        // is the same, the next frame differs; monochrome noise moves all three channels together.
        Image grey(64, 64);
        for (size_t i = 0; i < grey.px.size(); i += 4) grey.px[i] = grey.px[i + 1] = grey.px[i + 2] = 0.5f, grey.px[i + 3] = 1;
        Effect nz = makeEffect(p, "noise");
        nz.params["amount"] = 0.0;
        Image same = grey;
        applyVideoEffect(nz, 0, same, 1);
        QVERIFY(same.px == grey.px);
        nz.params["amount"] = 40.0;
        Image a = grey, b = grey, c = grey;
        applyVideoEffect(nz, 3, a, 1);
        applyVideoEffect(nz, 3, b, 1);
        applyVideoEffect(nz, 4, c, 1);
        QVERIFY(a.px == b.px && a.px != c.px);
        double sum = 0, sq = 0;
        for (size_t i = 0; i < a.px.size(); i += 4) sum += a.px[i] - 0.5, sq += (a.px[i] - 0.5) * (a.px[i] - 0.5);
        const double n = double(a.px.size() / 4), mean = sum / n, sd = std::sqrt(sq / n - mean * mean);
        QVERIFY2(std::fabs(mean) < 0.01 && std::fabs(sd - 0.4 / std::sqrt(12.0)) < 0.01, qPrintable(QString("%1 %2").arg(mean).arg(sd)));
        QVERIFY(std::fabs(a.px[0] - a.px[1]) > 1e-4f || std::fabs(a.px[4] - a.px[5]) > 1e-4f);  // colour noise
        nz.params["color"] = 0.0;
        applyVideoEffect(nz, 3, same, 1);
        for (size_t i = 0; i < 400; i += 4) QVERIFY(same.px[i] == same.px[i + 1] && same.px[i + 1] == same.px[i + 2]);
    }

    void shortsFromTranscripts() {
        // Twelve sentences, a word every 0.4 s and 0.7 s between sentences: chat, a hook and its follow-up, a
        // second hook, a stretch thick with fillers, and more chat.
        const std::vector<std::string> said = {
            "Welcome back to the show, it is good to be here today.",
            "So we spent the morning sorting out some boxes in the garage.",
            "And then the weather turned and we went inside for lunch.",
            "It was fine, nothing much happened after that really.",
            "Did you know that most people price their work completely wrong?",
            "They charge for hours when clients actually pay for results.",
            "Here's the one thing that doubled our pricing overnight.",
            "We stopped quoting days and started quoting outcomes.",
            "um so uh we uh um then uh went um back uh to the um garage.",
            "and it was uh um fine I uh guess.",
            "Anyway the boxes were all still there when we got back.",
            "Then we had some tea and called it a day."};
        Project p = makeDefaultProject();
        MediaItem m;
        m.id = p.newId();
        m.kind = MediaKind::Video;
        m.name = "podcast.mov";
        m.path = "/nowhere/podcast.mov";
        m.duration = 120;
        auto t = std::make_shared<Transcript>();
        t->language = "en";
        std::vector<TranscriptWord> words;
        double at = 1;
        for (const std::string& sentence : said) {
            TranscriptSegment seg;
            for (const QString& w : QString::fromStdString(sentence).split(' ')) {
                seg.words.push_back({at, at + 0.3, w.toStdString(), 0.9f});
                words.push_back(seg.words.back());
                at += 0.4;
            }
            at += 0.7;
            t->segments.push_back(seg);
        }
        m.transcript = t;
        p.media.push_back(m);
        // Hooks.
        QVERIFY(hookScore(said[4]) >= 0.9);
        QVERIFY(hookScore(said[6]) > 0.4 && hookScore(said[6]) < 0.8);
        QCOMPARE(hookScore("and it was fine."), 0.0);
        QVERIFY(hookScore("What would you do with a million dollars?") > hookScore("The meeting was moved to Tuesday."));
        // The best windows: sentence-aligned, inside the length range, never overlapping, the hook first.
        ShortsOptions o;
        o.count = 3;
        o.minSeconds = 6;  // two sentences or three
        o.maxSeconds = 12;
        o.liveliness = false;
        std::string err;
        const std::vector<ShortMoment> found = findShorts(p, {m.id}, o, {}, nullptr, &err);
        QCOMPARE(found.size(), size_t(3));
        auto endsSentence = [](const std::string& w) { return w.back() == '.' || w.back() == '?'; };
        for (size_t i = 0; i < found.size(); ++i) {
            const ShortMoment& f = found[i];
            qInfo("short %.2f: %s", f.score, f.text.c_str());
            const double length = words[f.lastWord].end - words[f.firstWord].start;
            QVERIFY2(length >= o.minSeconds && length <= o.maxSeconds, qPrintable(QString::number(length)));
            QVERIFY(f.firstWord == 0 || endsSentence(words[f.firstWord - 1].text));
            QVERIFY(endsSentence(words[f.lastWord].text));
            QVERIFY(f.in < words[f.firstWord].start && f.in >= (f.firstWord ? words[f.firstWord - 1].end : 0.0));
            QVERIFY(f.out > words[f.lastWord].end && f.out <= words[f.lastWord + 1].start);
            QVERIFY(QString::fromStdString(f.text).startsWith(QString::fromStdString(f.hookLine)));
            for (size_t j = 0; j < i; ++j) QVERIFY(f.out <= found[j].in || f.in >= found[j].out);
            if (i) QVERIFY(f.score <= found[i - 1].score);
        }
        QVERIFY(QString::fromStdString(found[0].hookLine).startsWith("Did you know"));
        QVERIFY(QString::fromStdString(found[1].hookLine).startsWith("Here's the one thing"));
        // The rambling stretch thick with fillers is never chosen.
        for (const ShortMoment& f : found) QVERIFY(!QString::fromStdString(f.text).contains("uh we uh"));
        // A topic pulls its moments up.
        o.topic = "the garage and its boxes";
        const std::vector<ShortMoment> garage = findShorts(p, {m.id}, o, {}, nullptr, &err);
        QVERIFY(!garage.empty());
        QVERIFY2(QString::fromStdString(garage[0].text).contains("garage"), garage[0].text.c_str());
        // Nothing fits, or nothing is transcribed.
        o.topic.clear();
        o.minSeconds = 200, o.maxSeconds = 300;
        QVERIFY(findShorts(p, {m.id}, o, {}, nullptr, &err).empty());
        QVERIFY(QString::fromStdString(err).contains("200"));
        p.media[0].transcript.reset();
        QVERIFY(findShorts(p, {m.id}, o, {}, nullptr, &err).empty());
        QVERIFY(QString::fromStdString(err).startsWith("Transcribe"));
    }

    void titlesRender() {
        Project p;
        Effect t = makeEffect(p, "title");
        t.strings["text"] = "MONTAGE";
        t.params["size"] = 40.0;
        Image img = renderGenerator(t, 0, 320, 180, 1.0);
        double alpha = 0;
        for (size_t i = 3; i < img.px.size(); i += 4) alpha += img.px[i];
        QVERIFY(alpha > 100);                 // text was drawn
        QVERIFY(img.at(2, 2)[3] < 0.01f);     // background stays transparent
        Effect bars = makeEffect(p, "bars");
        Image b = renderGenerator(bars, 0, 70, 40, 1);
        QVERIFY(near(b.at(5, 5)[0], 0.75f));  // first bar is 75% grey
    }
};

QTEST_MAIN(TestRender)
#include "test_render.moc"
