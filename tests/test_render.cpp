// Renderer tests: blending, transforms, effects, transitions, generators.
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <fstream>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "render/Compositor.h"
#include "render/Processing.h"

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
