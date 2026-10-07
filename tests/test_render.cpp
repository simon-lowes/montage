// Renderer tests: blending, transforms, effects, transitions, generators.
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "render/ColorSpace.h"
#include "render/Compositor.h"
#include "render/Ocio.h"
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
