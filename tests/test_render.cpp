// Renderer tests: blending, transforms, effects, transitions, generators.
#include <QtTest>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <tuple>

#include "core/Captions.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "render/ColorSpace.h"
#include "render/Compositor.h"
#include "render/Exporter.h"
#include "render/Ocio.h"
#include "render/Processing.h"
#include "render/RenderCache.h"
#include "render/VideoFx.h"

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
        QVERIFY(titleTemplates().size() >= 7);
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
