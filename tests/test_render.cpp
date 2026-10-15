// Renderer tests: blending, transforms, effects, transitions, generators.
#include <QtTest>
#include <map>
#include <QTemporaryDir>
#include <QProcess>

#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <tuple>

#include "core/ClipAnimation.h"
#include "core/ColorGroups.h"
#include "core/GradeVersions.h"
#include "core/GradeGraph.h"
#include "core/Captions.h"
#include "core/EditOps.h"
#include "core/ProjectIO.h"
#include "core/GraphicsTemplate.h"
#include "core/Effects.h"
#include "core/MaskPath.h"
#include "render/ColorChart.h"
#include "core/LensProfile.h"
#include "render/ColorSpace.h"
#include "render/ColorSlice.h"
#include "render/Clf.h"
#include "render/Amf.h"
#include "core/Cdl.h"
#include "render/Gamut.h"
#include "render/HdrView.h"
#include "render/Compositor.h"
#include "render/GpuCompositor.h"
#include "render/Dcp.h"
#include "render/Imf.h"
#include "render/Jpeg2000.h"
#include "render/DcpMxf.h"
#include "media/Decoder.h"
#include "render/Exporter.h"
#include "render/Gallery.h"
#include "render/LinedScript.h"
#include "render/RemoteReview.h"
#include "core/History.h"
#include <QTcpServer>
#include <QUdpSocket>
#include <QTcpSocket>
#include <thread>
#include <QPainter>
#include <QElapsedTimer>
#include "core/ScriptCut.h"
#include "render/TrimPasses.h"
#include "render/RoomTone.h"
#include "render/Ocio.h"
#include "media/Tracking.h"
#include "render/Processing.h"
#include "render/QualityCheck.h"
#include "render/LutExport.h"
#include "render/LightLevel.h"
#include "render/Relight.h"
#include "render/Deconvolve.h"
#include "render/FilmLook.h"
#include "render/FaceRefine.h"
#include "media/FaceTracks.h"
#include "media/DepthMap.h"
#include "render/RenderCache.h"
#include "render/Shapes.h"
#include "render/Shorts.h"
#include "core/Transcript.h"
#include "render/VideoDenoise.h"
#include "render/VideoFx.h"
#include "render/VideoPacking.h"
#include "render/ReferenceOutput.h"
#include "audio/DynLib.h"
#include <random>
#include "render/Restoration.h"
#include "render/LensBlur.h"
#include "render/TransferTables.h"
#include "media/DepthMap.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

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
    void gamutTools() {
        auto near = [](Xy a, double x, double y, double tol) { return std::fabs(a.x - x) < tol && std::fabs(a.y - y) < tol; };
        // Chromaticities: white is D65, the primaries where the standards put them (Rec.709 red, Rec.2020 green, P3 blue).
        const ColorSpace& rec709 = *findColorSpace("rec709");
        const ColorSpace& rec2020 = *findColorSpace("rec2020");
        Xy c;
        const float white[3] = {1, 1, 1}, red[3] = {1, 0, 0}, green[3] = {0, 1, 0}, black[3] = {0, 0, 0};
        QVERIFY(codeToXy(rec709, white, c) && near(c, 0.3127, 0.3290, 1e-4));
        QVERIFY(codeToXy(rec709, red, c) && near(c, 0.64, 0.33, 1e-4));
        QVERIFY(codeToXy(rec2020, green, c) && near(c, 0.170, 0.797, 1e-4));
        QVERIFY(!codeToXy(rec709, black, c));
        const float grey[3] = {0.3f, 0.3f, 0.3f};  // (any brightness: the same chromaticity)
        QVERIFY(codeToXy(*findColorSpace("rec2100pq"), grey, c) && near(c, 0.3127, 0.3290, 1e-4));
        const auto p3 = gamutChromaticities(Primaries::P3D65);
        QVERIFY(near(p3[2], 0.150, 0.060, 1e-4) && near(p3[3], 0.3127, 0.3290, 1e-4));
        // u'v' of D65 is (0.1978, 0.4683).
        QVERIFY(near(xyToUv({0.3127, 0.3290}), 0.1978, 0.4683, 1e-4));
        // The spectral locus: 380 to 700 nm; 520 nm (the greenest light) at (0.0743, 0.8338), 700 nm (0.7347, 0.2653).
        const std::vector<Xy>& locus = spectralLocus();
        QCOMPARE(locus.size(), size_t(65));
        QVERIFY(near(locus[28], 0.0743, 0.8338, 1e-4));
        QVERIFY(near(locus.back(), 0.7347, 0.2653, 1e-4));

        // Outside a gamut: Rec.2020's green is outside Rec.709 and P3; Rec.709's red inside all three; a P3 red inside P3
        // but not Rec.709.
        QVERIFY(GamutCheck(rec2020, Primaries::Bt709).outside(green));
        QVERIFY(GamutCheck(rec2020, Primaries::P3D65).outside(green));
        QVERIFY(!GamutCheck(rec2020, Primaries::Bt2020).outside(green));
        QVERIFY(!GamutCheck(rec709, Primaries::Bt2020).outside(red));
        const ColorSpace& displayP3 = *findColorSpace("p3d65");
        QVERIFY(GamutCheck(displayP3, Primaries::Bt709).outside(red));
        QVERIFY(!GamutCheck(displayP3, Primaries::P3D65).outside(red));
        QVERIFY(!GamutCheck(rec2020, Primaries::Bt709).outside(white) && !GamutCheck(rec2020, Primaries::Bt709).outside(black));
        // (A Rec.709 colour taken to Rec.2020 and its codes rounded to 16 bits is still inside Rec.709.)
        float conv[3] = {0.9f, 0.05f, 0.02f};
        convertPixel(conv, rec709, rec2020);
        for (float& v : conv) v = std::round(v * 65535) / 65535;
        QVERIFY(!GamutCheck(rec2020, Primaries::Bt709).outside(conv));

        // ACES Reference Gamut Compression: colours well inside left alone; one at a limit's distance lands on the
        // edge (its channel 0); further out ones come inside; the inverse undoes it.
        float inside[3] = {0.5f, 0.3f, 0.2f};
        acesGamutCompress(inside);
        QCOMPARE(inside[0], 0.5f);
        QCOMPARE(inside[1], 0.3f);
        QCOMPARE(inside[2], 0.2f);
        for (int ch = 0; ch < 3; ++ch) {
            const double limit = ch == 0 ? 1.147 : ch == 1 ? 1.264 : 1.312;
            float v[3] = {1, 1, 1};
            v[ch] = float(1 - limit);
            acesGamutCompress(v);
            QVERIFY2(std::fabs(v[ch]) < 1e-5f, qPrintable(QString::number(v[ch])));
        }
        float far[3] = {0.8f, -0.6f, 0.1f};
        acesGamutCompress(far);
        QVERIFY(far[1] > -0.6f && far[1] < 0 && far[0] == 0.8f);
        float back[3] = {far[0], far[1], far[2]};
        acesGamutCompress(back, true);
        QVERIFY(std::fabs(back[1] + 0.6f) < 1e-4f && std::fabs(back[2] - 0.1f) < 1e-4f);
        float none[3] = {0, 0, 0};
        acesGamutCompress(none);
        QCOMPARE(none[0], 0.0f);
        // Against OpenColorIO's own (ACES 1.3's LMT, on ACES2065-1): within the spec's 0.002.
        if (ocioAvailable()) {
            double toAp1[9], toAp0[9];
            primariesMatrix(Primaries::Ap0, Primaries::Ap1, toAp1);
            primariesMatrix(Primaries::Ap1, Primaries::Ap0, toAp0);
            std::vector<float> ours, theirs;
            const float samples[][3] = {{0.18f, 0.18f, 0.18f}, {0.9f, -0.05f, 0.02f}, {0.02f, 0.5f, -0.1f}, {-0.02f, 0.1f, 0.6f},
                                        {2.5f, 0.1f, -0.3f}, {0.05f, 0.01f, 0.4f}, {1.0f, 0.4f, 0.0f}, {0.3f, -0.2f, 0.9f}};
            for (const auto& s : samples) {
                theirs.insert(theirs.end(), s, s + 3);
                float a[3];
                for (int r = 0; r < 3; ++r) a[r] = float(toAp1[r * 3] * s[0] + toAp1[r * 3 + 1] * s[1] + toAp1[r * 3 + 2] * s[2]);
                acesGamutCompress(a);
                for (int r = 0; r < 3; ++r) ours.push_back(float(toAp0[r * 3] * a[0] + toAp0[r * 3 + 1] * a[1] + toAp0[r * 3 + 2] * a[2]));
            }
            std::string err;
            QVERIFY2(applyOcioBuiltin("ACES-LMT - ACES 1.3 Reference Gamut Compression", theirs.data(), theirs.size() / 3, false, &err), err.c_str());
            for (size_t i = 0; i < ours.size(); ++i)
                QVERIFY2(std::fabs(ours[i] - theirs[i]) < 0.002f, qPrintable(QString("%1: %2 vs %3").arg(i).arg(ours[i]).arg(theirs[i])));
        }

        // The effect, in the working space: an ACEScct sequence's out-of-gamut colour brought in, an in-gamut one alone;
        // Inverse takes it back; LUT export holds it.
        Project p = makeDefaultProject();
        Effect e = makeEffect(p, "aces_gamut_compress");
        const ColorSpace& cct = *findColorSpace("acescct");
        const WorkingSpaceScope working(&cct);
        float outCode[3] = {float(fromLinear(Transfer::AcesCct, 0.6)), float(fromLinear(Transfer::AcesCct, -0.05)),
                            float(fromLinear(Transfer::AcesCct, 0.1))};
        Image img = solid(1, 1, outCode[0], outCode[1], outCode[2]);
        applyVideoEffect(e, 0, img, 1.0);
        float lin[3] = {0.6f, -0.05f, 0.1f};
        acesGamutCompress(lin);
        QVERIFY(std::fabs(toLinear(Transfer::AcesCct, img.at(0, 0)[1]) - lin[1]) < 1e-4);
        QVERIFY(toLinear(Transfer::AcesCct, img.at(0, 0)[1]) > -0.05);
        e.params["direction"] = Param(1.0);
        applyVideoEffect(e, 0, img, 1.0);
        QVERIFY(std::fabs(img.at(0, 0)[1] - outCode[1]) < 1e-4f);
        Image calm = solid(1, 1, 0.4f, 0.4f, 0.35f);
        e.params["direction"] = Param(0.0);
        applyVideoEffect(e, 0, calm, 1.0);
        QVERIFY(std::fabs(calm.at(0, 0)[2] - 0.35f) < 1e-5f);
        QVERIFY(isColorOnlyEffect(e, 0));

        // On a clip of media it works on the media's colours, before the input transform clips them to the working
        // gamut: an ACES2065-1 blue outside AP1 (and Rec.709) comes out compressed, not clipped.
        QTemporaryDir dir;
        const QString path = dir.filePath("ap0.png");
        QImage q(8, 8, QImage::Format_RGBA64);
        q.fill(QColor::fromRgbF(0.05f, 0.05f, 0.4f));
        QVERIFY(q.save(path));
        Project pm = makeDefaultProject();
        MediaItem m;
        QVERIFY(probeMedia(path.toStdString(), m));
        m.id = pm.newId();
        m.colorOverride = "aces2065-1";
        pm.media.push_back(m);
        Sequence& sm = *pm.active();
        sm.width = sm.height = 8;
        QVERIFY(edit::placeMedia(pm, sm, m.id, 0, 0, 10, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false).ok);
        RenderOptions o;
        float plain[4], compressed[4];
        rgb(renderProgramFrame(pm, sm, 0, o), 4, 4, plain);
        sm.videoTracks[0].clips[0].effects.push_back(makeEffect(pm, "aces_gamut_compress"));
        rgb(renderProgramFrame(pm, sm, 0, o), 4, 4, compressed);
        float want[3] = {std::round(0.05f * 65535) / 65535, std::round(0.05f * 65535) / 65535, std::round(0.4f * 65535) / 65535};
        AcesGamutCompressor(*findColorSpace("aces2065-1"))(want);
        convertPixel(want, *findColorSpace("aces2065-1"), rec709);
        for (int i = 0; i < 3; ++i)
            QVERIFY2(std::fabs(compressed[i] - want[i]) < 0.005f, qPrintable(QString("%1: %2 vs %3").arg(i).arg(compressed[i]).arg(want[i])));
        QVERIFY2(std::fabs(compressed[1] - plain[1]) > 0.005f, qPrintable(QString("%1 / %2").arg(plain[1]).arg(compressed[1])));
    }

    void acesMetadataFiles() {
        // Transform ids, ACES 1 and 2, and Montage's colour spaces both ways.
        bool exact = false;
        QCOMPARE(acesInputSpace("urn:ampas:aces:transformId:v1.5:IDT.ARRI.Alexa-v3-logC-EI800.a1.v2", &exact), std::string("logc3-awg3"));
        QVERIFY(exact);
        QCOMPARE(acesInputSpace("urn:ampas:aces:transformId:v1.5:IDT.ARRI.Alexa-v3-logC-EI1600.a1.v2", &exact), std::string("logc3-awg3"));
        QVERIFY(!exact);  // (another exposure index's curve)
        QCOMPARE(acesInputSpace("urn:ampas:aces:transformId:v2.0:CSC.Sony.Venice_SLog3_SGamut3Cine_to_ACES.a2.v1", &exact), std::string("slog3-sgamut3cine"));
        QVERIFY(!exact);
        QCOMPARE(acesInputSpace("urn:ampas:aces:transformId:v1.5:ACEScsc.Academy.ACEScct_to_ACES.a1.0.3"), std::string("acescct"));
        QCOMPARE(acesInputSpace("urn:ampas:aces:transformId:v2.0:CSC.Canon.CLog2_CGamut_to_ACES.a1.v1"), std::string());
        double peak = 0;
        QCOMPARE(acesOutputSpace("urn:ampas:aces:transformId:v1.5:RRTODT.Academy.Rec2020_2000nits_15nits_ST2084.a1.1.0", &peak), std::string("rec2100pq"));
        QCOMPARE(peak, 2000.0);
        QCOMPARE(acesOutputSpace("urn:ampas:aces:transformId:v1.5:RRTODT.Academy.P3D65_108nits_7point2nits_ST2084.a1.1.0", &peak), std::string("p3d65pq"));
        QCOMPARE(peak, 108.0);
        QCOMPARE(acesOutputSpace("urn:ampas:aces:transformId:v2.0:Output.Academy.P3-D65_1000nit_in_Rec2100-D65_HLG.a2.v1"), std::string("rec2100hlg"));
        QCOMPARE(acesOutputSpace("urn:ampas:aces:transformId:v1.5:ODT.Academy.P3D60_48nits.a1.0.3"), std::string());  // (cinema)
        QCOMPARE(acesOutputSpace("urn:ampas:aces:transformId:v2.0:Output.Academy.P3-D65_48nit_in_P3-D65_Gamma2pt6.a2.v1"), std::string());
        for (const auto& cs : colorSpaces()) {
            const std::string in = acesInputTransformId(cs.id);
            if (!in.empty()) QCOMPARE(acesInputSpace(in, &exact), cs.id);
            const std::string out = acesOutputTransformId(cs.id, 1000);
            if (!out.empty()) QCOMPARE(acesOutputSpace(out), cs.id);
            if (cs.id == "rec709" || cs.id == "rec2100pq" || cs.id == "p3d65pq" || cs.id == "slog3-sgamut3cine") QVERIFY(!(in + out).empty());
        }
        QVERIFY(acesOutputTransformId("rec2100pq", 1200).find("_1000nit_") != std::string::npos);  // (the nearest ACES has)
        QVERIFY(acesOutputTransformId("p3d65pq", 100).find("_108nit_") != std::string::npos);

        // Reading: an ACES 1 pipeline for a camera clip, and an ACES 2 one with its CDL in a collection.
        const std::string data = std::string(MONTAGE_TEST_DATA_DIR) + "/amf";
        AmfDocument a;
        std::string err;
        QVERIFY2(readAmf(data + "/A001C012.amf", a, &err), err.c_str());
        QCOMPARE(a.clipName, std::string("A001C012"));
        QCOMPARE(a.clipSequence, std::string("A001_C012_AE0306_####.ari"));
        QCOMPARE(a.sequenceMax, 240);
        QVERIFY(a.hasInput && !a.inputApplied);
        QCOMPARE(a.inputSpace, std::string("logc3-awg3"));
        QCOMPARE(a.looks.size(), size_t(5));
        QVERIFY(a.looks[0].applied && a.looks[0].cdl.slope[0] == 2.0 && a.looks[0].cdl.saturation == 0.5);  // (ASC_SOP, as ACES 1 wrote it)
        QVERIFY(!a.looks[1].applied && a.looks[1].kind == AmfLook::Kind::Cdl);
        QCOMPARE(a.looks[1].cdlSpace, std::string("acescct"));
        QVERIFY(a.looks[1].cdl.slope[2] == 0.9 && a.looks[1].cdl.power[1] == 1.05 && a.looks[1].cdl.saturation == 0.9);
        QCOMPARE(a.looks[2].file, data + "/show_look.clf");
        QVERIFY(a.looks[3].kind == AmfLook::Kind::TransformId);
        QCOMPARE(a.outputSpace, std::string("rec709"));
        AmfDocument b;
        QVERIFY2(readAmf(data + "/B002C003.amf", b, &err), err.c_str());
        QCOMPARE(b.inputSpace, std::string("rec709"));  // an inverse output transform: video made for that display
        QCOMPARE(b.looks.size(), size_t(1));
        QVERIFY(b.looks[0].cdl.slope[2] == 1.08 && b.looks[0].cdl.saturation == 1.15);  // grade7 from grades.ccc
        QCOMPARE(b.outputSpace, std::string("rec2100pq"));
        QCOMPARE(b.outputPeakNits, 1000.0);
        QVERIFY(!parseAmf("<aces:acesMetadataFile xmlns:aces=\"urn:ampas:aces:amf:v2.0\"/>", {}, b, &err));
        QVERIFY(err.find("pipeline") != std::string::npos);
        QVERIFY(!parseAmf("<ProcessList/>", {}, b, &err));

        // Onto clips: the one its clip name matches (the camera file's), not the other.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        auto media = [&](const char* name, const char* space) {
            MediaItem m;
            m.id = p.newId();
            m.kind = MediaKind::Video;
            m.name = name;
            m.path = std::string("/cards/") + name;
            m.duration = 10;
            m.width = 1920;
            m.height = 1080;
            m.fps = s.fps;
            m.hasVideo = true;
            m.colorSpace = space;
            p.media.push_back(m);
            QVERIFY(edit::placeMedia(p, s, m.id, s.duration(), 0, 24, {TrackKind::Video, 0}, {}, false).ok);
        };
        media("A001C012.mov", "");
        media("B002C003.mxf", "");
        const Id camera = s.videoTracks[0].clips[0].id, interview = s.videoTracks[0].clips[1].id;
        AmfImportResult r = applyAmf(p, s, {}, a);
        QCOMPARE(r.clips, 1);
        QVERIFY(std::find_if(r.notes.begin(), r.notes.end(), [](const std::string& n) { return n.find("Bleach") != std::string::npos; }) != r.notes.end());
        const Clip* cam = edit::clipById(s, camera);
        QCOMPARE(p.findMedia(cam->mediaId)->colorOverride, std::string("logc3-awg3"));
        QCOMPARE(p.findMedia(cam->mediaId)->metadata.at("amf_name"), std::string("A001C012.amf"));  // (as an ALE names it)
        QCOMPARE(p.findMedia(cam->mediaId)->metadata.at("amf_uuid"), std::string("urn:uuid:0d6f5a8e-3b6c-4c55-9a51-7f0d2b1c9e01"));
        QCOMPARE(cam->effects.size(), size_t(3));  // the CDL, the look and the gamut compression (not the applied one)
        QCOMPARE(cam->effects[0].type, std::string("cdl"));
        QCOMPARE(cam->effects[0].s("space"), std::string("ACEScct"));
        Cdl g;
        QVERIFY(clipCdl(*cam, 0, g) && g.slope[0] == 1.1 && g.saturation == 0.9);
        QCOMPARE(cam->effects[1].type, std::string("lut"));
        QCOMPARE(cam->effects[1].s("path"), data + "/show_look.clf");
        QCOMPARE(cam->effects[1].s("space"), findColorSpace("aces2065-1")->label);
        QCOMPARE(cam->effects[2].type, std::string("aces_gamut_compress"));
        QVERIFY(edit::clipById(s, interview)->effects.empty());
        QCOMPARE(s.colorSpace, std::string("rec709"));
        // Again: the earlier AMF's effects give way; a hand-added effect stays. The other file onto a chosen clip, with
        // its output transform's space for the sequence.
        edit::clipById(s, camera)->effects.push_back(makeEffect(p, "hue_sat"));
        QCOMPARE(applyAmf(p, s, {}, a).clips, 1);
        QCOMPARE(edit::clipById(s, camera)->effects.size(), size_t(4));
        QCOMPARE(edit::clipById(s, camera)->effects[3].type, std::string("hue_sat"));
        AmfDocument named;
        QVERIFY(readAmf(data + "/B002C003.amf", named));
        named.clipName = "nothing here";
        named.clipFile.clear();
        QCOMPARE(applyAmf(p, s, {}, named).clips, 0);
        // ... unless an ALE named the AMF for the media.
        p.findMedia(edit::clipById(s, interview)->mediaId)->metadata["amf_uuid"] = named.uuid;
        QVERIFY(amfMatchesClip(named, p, *edit::clipById(s, interview)));
        p.findMedia(edit::clipById(s, interview)->mediaId)->metadata.erase("amf_uuid");
        AmfImportOptions o;
        o.setSequenceSpace = true;
        QCOMPARE(applyAmf(p, s, {interview}, named, o).clips, 1);
        QCOMPARE(s.colorSpace, std::string("rec2100pq"));
        QCOMPARE(s.hdrPeakNits, 1000.0);
        QCOMPARE(edit::clipById(s, interview)->effects.size(), size_t(1));

        // Out again: the camera clip's AMF has its IDT, its looks and the sequence's output transform; the look file
        // beside it. A clip of video gets an inverse output transform; a CDL in a space ACES has no id for, a CLF of it.
        QTemporaryDir dir;
        std::vector<std::string> notes;
        const std::string camPath = dir.path().toStdString() + "/cam.amf";
        QVERIFY2(writeClipAmf(p, s, *edit::clipById(s, camera), camPath, &notes, {}, &err), err.c_str());
        QVERIFY(std::find_if(notes.begin(), notes.end(), [](const std::string& n) { return n.find("Hue / Saturation") != std::string::npos; }) != notes.end());
        AmfDocument back;
        QVERIFY2(readAmf(camPath, back, &err), err.c_str());
        QCOMPARE(back.clipName, std::string("A001C012"));
        QCOMPARE(back.clipFile, std::string("A001C012.mov"));
        QCOMPARE(back.inputTransformId, acesInputTransformId("logc3-awg3"));
        QCOMPARE(back.looks.size(), size_t(3));
        QVERIFY(back.looks[0].cdl.sameGrade(g));
        QCOMPARE(back.looks[0].cdlSpace, std::string("acescct"));
        QCOMPARE(back.looks[1].file, dir.path().toStdString() + "/show_look.clf");
        QVERIFY(QFileInfo::exists(QString::fromStdString(back.looks[1].file)));
        QCOMPARE(back.looks[2].transformId, std::string("urn:ampas:aces:transformId:v2.0:Look.Academy.ReferenceGamutCompress.a2.v1"));
        // Again into the same folder, its look found there already, not copied over itself; another LUT of the same
        // name numbered beside it; the inverse gamut compression (no AMF id) left out.
        QVERIFY(writeClipAmf(p, s, *edit::clipById(s, camera), camPath, &notes, {}, &err));
        QVERIFY(readAmf(camPath, back) && back.looks[1].file == dir.path().toStdString() + "/show_look.clf");
        QTemporaryDir other;
        QFile odd(other.path() + "/show_look.clf");
        QVERIFY(odd.open(QIODevice::WriteOnly) && odd.write(QByteArray::fromStdString(clfToAces("acescct"))) > 0);
        odd.close();
        Clip second = *edit::clipById(s, camera);
        second.effects[1].strings["path"] = odd.fileName().toStdString();
        second.effects[2].params["direction"] = Param(1.0);
        QVERIFY(writeClipAmf(p, s, second, dir.path().toStdString() + "/second.amf", &notes, {}, &err));
        QVERIFY(readAmf(dir.path().toStdString() + "/second.amf", back));
        QCOMPARE(back.looks.size(), size_t(2));
        QCOMPARE(back.looks[1].file, dir.path().toStdString() + "/show_look_2.clf");
        QVERIFY(std::find_if(notes.begin(), notes.end(), [](const std::string& n) { return n.find("inverse gamut") != std::string::npos; }) != notes.end());
        QVERIFY(QFileInfo::exists(QString::fromStdString(data + "/show_look.clf")));
        QCOMPARE(back.outputSpace, std::string("rec2100pq"));
        QCOMPARE(back.outputPeakNits, 1000.0);
        Clip& iv = *edit::clipById(s, interview);
        for (Effect& e : iv.effects) e.strings["space"] = "Working space";
        const std::string ivPath = dir.path().toStdString() + "/interview.amf";
        QVERIFY2(writeClipAmf(p, s, iv, ivPath, &notes, {}, &err), err.c_str());
        QVERIFY(readAmf(ivPath, back, &err));
        QCOMPARE(back.inputSpace, std::string("rec709"));
        QVERIFY(back.inputTransformId.find(":InvOutput.Academy.Rec709-D65_100nit_in_Rec709-D65_BT1886") != std::string::npos);
        QCOMPARE(back.looks.size(), size_t(1));
        QCOMPARE(back.looks[0].cdlSpace, std::string("rec2100pq"));  // the working space, by the CLF Montage wrote for it
        // That CLF is the space's code values in ACES2065-1.
        auto clf = loadClf(dir.path().toStdString() + "/rec2100pq_to_ACES2065-1.clf", &err);
        QVERIFY2(clf, err.c_str());
        float v[3] = {0.5f, 0.3f, 0.7f};
        clf->apply(v);
        double m[9], l[3];
        primariesMatrix(Primaries::Bt2020, Primaries::Ap0, m);
        for (int i = 0; i < 3; ++i) l[i] = toLinear(Transfer::Pq, i == 0 ? 0.5 : i == 1 ? 0.3 : 0.7);
        for (int i = 0; i < 3; ++i) QVERIFY(std::fabs(v[i] - (m[i * 3] * l[0] + m[i * 3 + 1] * l[1] + m[i * 3 + 2] * l[2])) < 2e-3);
        if (ocioAvailable()) {
            float o[3] = {0.5f, 0.3f, 0.7f};
            QVERIFY2(applyOcioFile(dir.path().toStdString() + "/rec2100pq_to_ACES2065-1.clf", o, 1, &err), err.c_str());
            for (int i = 0; i < 3; ++i) QVERIFY(std::fabs(o[i] - v[i]) < 1e-4);
        }
        // A LUT in another space cannot be an AMF look.
        Effect lut = makeEffect(p, "lut");
        lut.strings["path"] = data + "/show_look.clf";
        lut.strings["space"] = "ACEScct";
        iv.effects.push_back(lut);
        QVERIFY(writeClipAmf(p, s, iv, ivPath, &notes, {}, &err));
        QVERIFY(std::find_if(notes.begin(), notes.end(), [](const std::string& n) { return n.find("left out") != std::string::npos; }) != notes.end());
        // What Montage writes is a valid AMF 2.0 (checked against the Academy's schema when it is at hand).
        if (const char* xsd = getenv("MONTAGE_AMF_SCHEMA")) {
            for (const std::string& f : {camPath, ivPath}) {
                QProcess lint;
                lint.start("xmllint", {"--noout", "--schema", xsd, QString::fromStdString(f)});
                QVERIFY(lint.waitForFinished());
                QVERIFY2(lint.exitCode() == 0, lint.readAllStandardError().constData());
            }
        }
    }

    void clfMatchesOcio() {
        // Montage's CLF reader gives what OpenColorIO gives for the same files: every operator, integer bit depths,
        // both 3D LUT interpolations, values outside 0..1.
        if (!ocioAvailable()) QSKIP("Built without OpenColorIO");
        std::vector<float> colours;
        for (float r : {-0.1f, 0.0f, 0.18f, 0.37f, 0.5f, 0.73f, 1.0f, 1.2f})
            for (float g : {0.0f, 0.05f, 0.42f, 0.9f, 1.0f})
                for (float b : {-0.02f, 0.11f, 0.6f, 0.95f}) colours.insert(colours.end(), {r, g, b});
        for (const char* name : {"operators.clf", "more_operators.clf", "lut3d_tetrahedral.clf", "lut3d_trilinear.clf"}) {
            const std::string path = std::string(MONTAGE_TEST_DATA_DIR) + "/clf/" + name;
            std::string error;
            auto clf = loadClf(path, &error);
            QVERIFY2(clf, error.c_str());
            QVERIFY(!clf->description.empty());
            std::vector<float> want = colours;
            QVERIFY2(applyOcioFile(path, want.data(), want.size() / 3, &error), error.c_str());
            double worst = 0;
            for (size_t i = 0; i < colours.size(); i += 3) {
                float v[3] = {colours[i], colours[i + 1], colours[i + 2]};
                clf->apply(v);
                for (int c = 0; c < 3; ++c) worst = std::max(worst, double(std::fabs(v[c] - want[i + size_t(c)])));
            }
            QVERIFY2(worst < 1e-4, (std::string(name) + " differs by " + std::to_string(worst)).c_str());
            QCOMPARE(loadClf(path).get(), clf.get());  // (kept while the file is unchanged)
        }
        // The LUT effect applies a CLF as it is in the working space, or in the space it names (here ACEScct, by way of
        // linear light), mixed by its strength.
        const std::string ops = std::string(MONTAGE_TEST_DATA_DIR) + "/clf/operators.clf";
        auto clf = loadClf(ops);
        Project p;
        Effect e = makeEffect(p, "lut");
        e.strings["path"] = ops;
        const float in[3] = {0.3f, 0.55f, 0.8f};
        Image img = solid(2, 2, in[0], in[1], in[2]);
        applyVideoEffect(e, 0, img, 1);
        float c[4], want[3] = {in[0], in[1], in[2]};
        clf->apply(want);
        rgb(img, 0, 0, c);
        for (int i = 0; i < 3; ++i) QVERIFY(std::fabs(c[i] - want[i]) < 1e-5);
        e.strings["space"] = "ACEScct";
        e.params["strength"] = 50.0;
        img = solid(2, 2, in[0], in[1], in[2]);
        applyVideoEffect(e, 0, img, 1);
        const ColorSpace& rec = rec709Space();
        double to[9], from[9], l[3], o[3];
        primariesMatrix(rec.primaries, Primaries::Ap1, to);
        primariesMatrix(Primaries::Ap1, rec.primaries, from);
        for (int i = 0; i < 3; ++i) l[i] = toLinear(rec.transfer, in[i]);
        for (int i = 0; i < 3; ++i) want[i] = float(fromLinear(Transfer::AcesCct, to[i * 3] * l[0] + to[i * 3 + 1] * l[1] + to[i * 3 + 2] * l[2]));
        clf->apply(want);
        for (int i = 0; i < 3; ++i) o[i] = toLinear(Transfer::AcesCct, want[i]);
        for (int i = 0; i < 3; ++i) want[i] = float(fromLinear(rec.transfer, from[i * 3] * o[0] + from[i * 3 + 1] * o[1] + from[i * 3 + 2] * o[2]));
        rgb(img, 0, 0, c);
        for (int i = 0; i < 3; ++i) QVERIFY2(std::fabs(c[i] - (in[i] + want[i]) / 2) < 3e-3, std::to_string(c[i] - (in[i] + want[i]) / 2).c_str());

        // Files that are not CLF, or use what is not read, say why.
        std::string why;
        QVERIFY(!parseClf("<ProcessList><LUT1D halfDomain=\"true\"><Array dim=\"2 1\">0 1</Array></LUT1D></ProcessList>", &why));
        QVERIFY(why.find("Half") != std::string::npos);
        QVERIFY(!parseClf("<ProcessList><LUT3D><Array dim=\"2 2 2 3\">0 1</Array></LUT3D></ProcessList>", &why));
        QVERIFY(why.find("size") != std::string::npos);
        QVERIFY(!parseClf("<ProcessList><LUT1D rawHalfs=\"true\"><Array dim=\"2 1\">0 1</Array></LUT1D></ProcessList>", &why));
        QVERIFY(!parseClf("<ProcessList><LUT3D><Array dim=\"4194304 4194304 4194304 3\"></Array></LUT3D></ProcessList>", &why));
        QVERIFY(why.find("size") != std::string::npos);  // (not an allocation that overflows)
        QVERIFY(!parseClf("<ProcessList><Gamut /></ProcessList>", &why));
        QVERIFY(why.find("Gamut") != std::string::npos);
        QVERIFY(!parseClf("<Something />", &why));
        QVERIFY(!parseClf("not xml", &why));
        // Not-a-number in (from an EXR, or an earlier operator) comes out as a number, not a read outside a table.
        for (const char* name : {"lut3d_tetrahedral.clf", "operators.clf"}) {
            float nan[3] = {std::nanf(""), 0.5f, std::nanf("")};
            loadClf(std::string(MONTAGE_TEST_DATA_DIR) + "/clf/" + name)->apply(nan);
            QVERIFY(std::isfinite(nan[1]));
        }
        // A .ctf (Autodesk's CLF) as a LUT effect's file.
        QTemporaryDir dir;
        const QString ctf = dir.path() + "/look.ctf";
        QVERIFY(QFile::copy(QString::fromStdString(ops), ctf));
        e = makeEffect(p, "lut");
        e.strings["path"] = ctf.toStdString();
        img = solid(2, 2, in[0], in[1], in[2]);
        applyVideoEffect(e, 0, img, 1);
        float again[3] = {in[0], in[1], in[2]};
        clf->apply(again);
        rgb(img, 0, 0, c);
        for (int i = 0; i < 3; ++i) QVERIFY(std::fabs(c[i] - again[i]) < 1e-5);
    }

    void colorSliceAndTexture() {
        // Colour Slice in a Rec.709 sequence: each vector changes its own colours, density darkens richer colours more,
        // subtractive saturation keeps the brightest channel, additive keeps the brightness, highlights can be kept.
        Project p = makeDefaultProject();
        const ColorSpace& rec = rec709Space();
        auto code = [&](float r, float g, float b) {
            return std::array<float, 3>{float(fromLinear(rec.transfer, r)), float(fromLinear(rec.transfer, g)), float(fromLinear(rec.transfer, b))};
        };
        auto run = [&](const Effect& e, std::array<float, 3> c) {
            Image img = solid(1, 1, c[0], c[1], c[2]);
            applyVideoEffect(e, 0, img, 1.0);
            return std::array<float, 3>{float(toLinear(rec.transfer, img.at(0, 0)[0])), float(toLinear(rec.transfer, img.at(0, 0)[1])),
                                        float(toLinear(rec.transfer, img.at(0, 0)[2]))};
        };
        auto lumaOf = [](std::array<float, 3> l) { return 0.2126f * l[0] + 0.7152f * l[1] + 0.0722f * l[2]; };
        const auto red = code(0.4f, 0.03f, 0.03f), paleRed = code(0.4f, 0.25f, 0.25f), grey = code(0.3f, 0.3f, 0.3f),
                   blue = code(0.03f, 0.03f, 0.4f), skinTone = code(0.4f, 0.22f, 0.15f), green = code(0.03f, 0.4f, 0.03f);
        Effect e = makeEffect(p, "color_slice");
        for (auto c : {red, grey, skinTone}) {
            const auto out = run(e, c);
            for (int k = 0; k < 3; ++k) QVERIFY(std::fabs(out[k] - toLinear(rec.transfer, c[size_t(k)])) < 1e-5);
        }
        e.params["protect_highlights"] = Param(0.0);
        e.params["red_density"] = Param(1.0);
        {
            const auto r = run(e, red), pr = run(e, paleRed), g = run(e, grey), b = run(e, blue);
            const float redStops = std::log2(0.4f / r[0]), paleStops = std::log2(0.4f / pr[0]);
            QVERIFY2(redStops > 0.8f && redStops < 1.05f, qPrintable(QString::number(redStops)));
            QVERIFY2(paleStops > 0.05f && paleStops < 0.5f * redStops, qPrintable(QString::number(paleStops)));
            QVERIFY(std::fabs(r[1] / r[0] - 0.03f / 0.4f) < 1e-3);  // (darker, the same colour)
            QVERIFY(std::fabs(g[0] - 0.3f) < 1e-4 && std::fabs(b[2] - 0.4f) < 1e-4 && std::fabs(b[0] - 0.03f) < 1e-4);
        }
        {
            const float full = std::log2(0.4f / run(e, red)[0]);
            e.params["depth"] = Param(50.0);
            QVERIFY(std::fabs(std::log2(0.4f / run(e, red)[0]) - 0.5f * full) < 1e-3);
        }
        e = makeEffect(p, "color_slice");
        e.params["protect_highlights"] = Param(0.0);
        e.params["saturation"] = Param(1.5);
        const auto warm = code(0.5f, 0.2f, 0.1f);
        {
            const auto sub = run(e, warm);
            QVERIFY(std::fabs(sub[0] - 0.5f) < 1e-3 && sub[1] < 0.2f && sub[2] < 0.1f);
            QVERIFY(lumaOf(sub) < lumaOf({0.5f, 0.2f, 0.1f}) - 0.02f);
            e.params["subtractive"] = Param(0.0);
            const auto add = run(e, warm);
            QVERIFY(add[0] > 0.5f && add[1] < 0.2f);
            QVERIFY(std::fabs(lumaOf(add) - lumaOf({0.5f, 0.2f, 0.1f})) < 2e-3);
        }
        // A hue shift turns red towards yellow, and skin's own vector leaves green alone.
        e = makeEffect(p, "color_slice");
        e.params["protect_highlights"] = Param(0.0);
        e.params["red_hue"] = Param(30.0);
        {
            const auto r = run(e, red);
            QVERIFY2(r[1] > 3 * r[2] && std::fabs(r[0] - 0.4f) < 1e-3, qPrintable(QString("%1 %2 %3").arg(r[0]).arg(r[1]).arg(r[2])));
            QVERIFY(std::fabs(run(e, blue)[2] - 0.4f) < 1e-4);
        }
        e = makeEffect(p, "color_slice");
        e.params["protect_highlights"] = Param(0.0);
        e.params["skin_density"] = Param(1.0);
        QVERIFY(run(e, skinTone)[0] < 0.37f);
        QVERIFY(std::fabs(run(e, green)[1] - 0.4f) < 1e-4);
        // Protect Highlights keeps colours at white; Mix 0 changes nothing.
        e = makeEffect(p, "color_slice");
        e.params["saturation"] = Param(1.5);
        const auto bright = code(1.0f, 0.4f, 0.2f);
        e.params["protect_highlights"] = Param(100.0);
        QVERIFY(std::fabs(run(e, bright)[1] - 0.4f) < 1e-4);
        e.params["protect_highlights"] = Param(0.0);
        QVERIFY(run(e, bright)[1] < 0.35f);
        e.params["mix"] = Param(0.0);
        QVERIFY(std::fabs(run(e, bright)[1] - 0.4f) < 1e-4);
        // Much more saturation never takes a channel below no light; a wide-gamut working space slices the same hues.
        e = makeEffect(p, "color_slice");
        e.params["protect_highlights"] = Param(0.0);
        e.params["saturation"] = Param(2.0);
        e.params["red_saturation"] = Param(2.0);
        {
            const auto r = run(e, red);
            QVERIFY2(r[1] >= 0 && r[2] >= 0 && r[1] < 0.03f, qPrintable(QString("%1 %2").arg(r[1]).arg(r[2])));
        }
        {
            e = makeEffect(p, "color_slice");
            e.params["protect_highlights"] = Param(0.0);
            e.params["red_density"] = Param(1.0);
            const float in709 = run(e, red)[0];
            const ColorSpace& wide = *findColorSpace("rec2020");
            const WorkingSpaceScope working(&wide);
            auto inWide = [&](std::array<float, 3> lin) {
                float v[3] = {lin[0], lin[1], lin[2]};
                double m[9];
                primariesMatrix(Primaries::Bt709, Primaries::Bt2020, m);
                float w[3];
                for (int r = 0; r < 3; ++r) w[r] = float(m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2]);
                Image img = solid(1, 1, float(fromLinear(wide.transfer, w[0])), float(fromLinear(wide.transfer, w[1])),
                                  float(fromLinear(wide.transfer, w[2])));
                applyVideoEffect(e, 0, img, 1.0);
                float back[3];
                for (int k = 0; k < 3; ++k) back[k] = float(toLinear(wide.transfer, img.at(0, 0)[k]));
                primariesMatrix(Primaries::Bt2020, Primaries::Bt709, m);
                return std::array<float, 3>{float(m[0] * back[0] + m[1] * back[1] + m[2] * back[2]),
                                            float(m[3] * back[0] + m[4] * back[1] + m[5] * back[2]),
                                            float(m[6] * back[0] + m[7] * back[1] + m[8] * back[2])};
            };
            const auto r = inWide({0.4f, 0.03f, 0.03f});
            QVERIFY2(std::fabs(r[0] - in709) < 2e-3, qPrintable(QString("%1 %2").arg(r[0]).arg(in709)));
            QVERIFY(std::fabs(inWide({0.03f, 0.4f, 0.03f})[1] - 0.4f) < 2e-3);
        }
        QVERIFY(isColorOnlyEffect(makeEffect(p, "color_slice"), 0));
        QVERIFY(!isColorOnlyEffect(makeEffect(p, "texture_pop"), 0));

        // Texture Pop and Mid-tone Detail: a flat picture stays as it is; texture grows above the noise floor and not below
        // it; colours keep their balance; negative mid-tone detail softens; a strong edge gets little halo; sizes follow
        // the preview's scale.
        auto pattern = [&](int w, int h, double period, float amp, float r = 1, float g = 1, float b = 1) {
            Image img(w, h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const float v = 0.18f * std::exp2(amp * float(std::sin(2 * M_PI * x / period) * std::sin(2 * M_PI * y / period)));
                    float* px = img.at(x, y);
                    px[0] = float(fromLinear(rec.transfer, v * r)), px[1] = float(fromLinear(rec.transfer, v * g));
                    px[2] = float(fromLinear(rec.transfer, v * b)), px[3] = 1;
                }
            return img;
        };
        auto spread = [](const Image& img) {
            double s = 0, s2 = 0;
            const int n = img.width * img.height;
            for (int y = 0; y < img.height; ++y)
                for (int x = 0; x < img.width; ++x) s += img.at(x, y)[1], s2 += double(img.at(x, y)[1]) * img.at(x, y)[1];
            return std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n)));
        };
        Effect tx = makeEffect(p, "texture_pop");
        tx.params["texture_pop"] = Param(100.0);
        tx.params["midtone_detail"] = Param(100.0);
        {
            Image flat = solid(32, 32, 0.4f, 0.4f, 0.4f);
            applyVideoEffect(tx, 0, flat, 1.0);
            QVERIFY(std::fabs(flat.at(16, 16)[0] - 0.4f) < 1e-5 && std::fabs(flat.at(0, 0)[2] - 0.4f) < 1e-5);
        }
        tx.params["midtone_detail"] = Param(0.0);
        {
            const Image fine = pattern(64, 64, 4, 0.5f);
            Image popped = fine;
            applyVideoEffect(tx, 0, popped, 1.0);
            QVERIFY2(spread(popped) > 1.3 * spread(fine), qPrintable(QString("%1 %2").arg(spread(popped)).arg(spread(fine))));
            const Image faint = pattern(64, 64, 4, 0.02f);
            Image kept = faint;
            tx.params["noise_floor"] = Param(100.0);
            applyVideoEffect(tx, 0, kept, 1.0);
            QVERIFY2(spread(kept) < 1.05 * spread(faint), qPrintable(QString("%1 %2").arg(spread(kept)).arg(spread(faint))));
            tx.params["noise_floor"] = Param(0.0);
            kept = faint;
            applyVideoEffect(tx, 0, kept, 1.0);
            QVERIFY(spread(kept) > 1.3 * spread(faint));
            // (each pixel's channels in the same ratio as before)
            Image tinted = pattern(32, 32, 4, 0.5f, 1.0f, 0.6f, 0.3f);
            applyVideoEffect(tx, 0, tinted, 1.0);
            for (int x = 0; x < 8; ++x) {
                const float* px = tinted.at(x, 5);
                const double rg = toLinear(rec.transfer, px[1]) / toLinear(rec.transfer, px[0]);
                QVERIFY2(std::fabs(rg - 0.6) < 2e-3, qPrintable(QString::number(rg)));
            }
        }
        {
            // Texture Pop's size follows the preview too; and a matte's transparent surround is not taken for black.
            tx.params["noise_floor"] = Param(0.0);
            const Image full = pattern(96, 96, 6, 0.3f), half = pattern(48, 48, 3, 0.3f);
            Image fullOut = full, halfOut = half;
            applyVideoEffect(tx, 0, fullOut, 1.0);
            applyVideoEffect(tx, 0, halfOut, 0.5);
            const double fullRatio = spread(fullOut) / spread(full), halfRatio = spread(halfOut) / spread(half);
            QVERIFY2(std::fabs(fullRatio - halfRatio) < 0.1 * fullRatio, qPrintable(QString("%1 %2").arg(fullRatio).arg(halfRatio)));
            tx.params["midtone_detail"] = Param(100.0);
            Image matte(48, 48);
            const float grey = float(fromLinear(rec.transfer, 0.18));
            for (int y = 12; y < 36; ++y)
                for (int x = 12; x < 36; ++x) {
                    float* px = matte.at(x, y);
                    px[0] = px[1] = px[2] = grey, px[3] = 1;
                }
            applyVideoEffect(tx, 0, matte, 1.0);
            for (int x : {12, 13, 24, 35})
                QVERIFY2(std::fabs(matte.at(x, 12)[1] - grey) < 1e-4, qPrintable(QString("%1: %2").arg(x).arg(matte.at(x, 12)[1])));
            QCOMPARE(matte.at(5, 5)[3], 0.0f);
        }
        tx = makeEffect(p, "texture_pop");
        tx.params["midtone_detail"] = Param(-100.0);
        {
            const Image wide = pattern(128, 128, 48, 0.4f);
            Image soft = wide;
            applyVideoEffect(tx, 0, soft, 1.0);
            QVERIFY2(spread(soft) < 0.85 * spread(wide), qPrintable(QString("%1 %2").arg(spread(soft)).arg(spread(wide))));
            // Sizes follow the preview: the same picture at half size, half the scale, changes as much.
            Image half = pattern(64, 64, 24, 0.4f), halfOut = half;
            applyVideoEffect(tx, 0, halfOut, 0.5);
            const double fullRatio = spread(soft) / spread(wide), halfRatio = spread(halfOut) / spread(half);
            QVERIFY2(std::fabs(fullRatio - halfRatio) < 0.08, qPrintable(QString("%1 %2").arg(fullRatio).arg(halfRatio)));
        }
        tx.params["midtone_detail"] = Param(100.0);
        {
            // A step of four stops: next to it the dark side darkens and the bright side brightens by little.
            Image step(96, 8);
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 96; ++x) {
                    const float v = float(fromLinear(rec.transfer, x < 48 ? 0.05 : 0.8));
                    float* px = step.at(x, y);
                    px[0] = px[1] = px[2] = v, px[3] = 1;
                }
            applyVideoEffect(tx, 0, step, 1.0);
            const double darkStops = std::log2(0.05 / toLinear(rec.transfer, step.at(46, 4)[1])),
                         brightStops = std::log2(toLinear(rec.transfer, step.at(49, 4)[1]) / 0.8);
            QVERIFY2(std::fabs(darkStops) < 0.35 && std::fabs(brightStops) < 0.35, qPrintable(QString("%1 %2").arg(darkStops).arg(brightStops)));
        }
    }

    void ascCdlEffect() {
        // The cdl effect grades like the ASC's maths, in the working space or in the space the CDL was made in.
        Project p = makeDefaultProject();
        Effect e = makeEffect(p, "cdl");
        e.params["slope.r"] = Param(2.0);
        e.params["offset.g"] = Param(0.1);
        e.params["power.b"] = Param(2.0);
        float c[4];
        Image img = solid(2, 2, 0.3f, 0.3f, 0.6f);
        applyVideoEffect(e, 0, img, 1.0);
        rgb(img, 0, 0, c);
        QVERIFY(std::fabs(c[0] - 0.6f) < 1e-5f && std::fabs(c[1] - 0.4f) < 1e-5f && std::fabs(c[2] - 0.36f) < 1e-5f);
        // Half-transparent pixels grade as their colour.
        Image half = solid(1, 1, 0.3f, 0.3f, 0.6f, 0.5f);
        applyVideoEffect(e, 0, half, 1.0);
        rgb(half, 0, 0, c);
        QVERIFY(std::fabs(c[0] - 0.6f) < 1e-5f && std::fabs(c[3] - 0.5f) < 1e-6f);
        // In ACEScct: into it and back exactly, so an identity CDL leaves the picture alone and an offset there lifts
        // the picture by what it is in log, not the same as in Rec.709.
        Effect plain = makeEffect(p, "cdl");
        plain.strings["space"] = findColorSpace("acescct")->label;
        plain.params["offset.g"] = Param(1e-6);  // (not quite an identity, so the conversions run)
        Image same = solid(1, 1, 0.2f, 0.5f, 0.8f);
        applyVideoEffect(plain, 0, same, 1.0);
        rgb(same, 0, 0, c);
        QVERIFY(std::fabs(c[0] - 0.2f) < 1e-3f && std::fabs(c[1] - 0.5f) < 1e-3f && std::fabs(c[2] - 0.8f) < 1e-3f);
        Effect lifted = plain;
        lifted.params.erase("offset.g");
        lifted.params["offset.r"] = lifted.params["offset.g"] = lifted.params["offset.b"] = Param(0.05);
        Image inLog = solid(1, 1, 0.2f, 0.2f, 0.2f), inVideo = inLog;
        applyVideoEffect(lifted, 0, inLog, 1.0);
        lifted.strings["space"] = "Working space";
        applyVideoEffect(lifted, 0, inVideo, 1.0);
        QVERIFY(std::fabs(inVideo.at(0, 0)[1] - 0.25f) < 1e-5f);
        // Rec.709 0.2 is 0.0213 in linear light, 0.2367 in ACEScct; 0.2867 there is 2^(0.2867 x 17.52 - 9.72) = 0.0385,
        // which is 0.2570 in Rec.709 (BT.1886).
        const double lin = std::pow(2.0, ((std::log2(std::pow(0.2, 2.4)) + 9.72) / 17.52 + 0.05) * 17.52 - 9.72);
        QVERIFY2(std::fabs(inLog.at(0, 0)[1] - std::pow(lin, 1 / 2.4)) < 2e-3, qPrintable(QString::number(inLog.at(0, 0)[1])));
        QVERIFY(std::fabs(inLog.at(0, 0)[1] - inVideo.at(0, 0)[1]) > 0.005f);
        // Off: nothing.
        e.enabled = false;
        Image off = solid(1, 1, 0.3f, 0.3f, 0.3f);
        applyVideoEffect(e, 0, off, 1.0);
        QCOMPARE(off.at(0, 0)[0], 0.3f);
    }

    void redactFacesCoversTheFaces() {
        // A busy picture (fine checks) and a face's box in the middle (40 x 36 px at 80, 42).
        const int W = 200, H = 120;
        Image img(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float* p = img.at(x, y);
                const float v = ((x / 2 + y / 2) % 2) ? 0.9f : 0.1f;
                p[0] = v, p[1] = 0.5f * v, p[2] = 1 - v, p[3] = 1;
            }
        FaceBox f;
        f.x = 0.4f, f.y = 0.35f, f.w = 0.2f, f.h = 0.3f;
        auto detail = [](const Image& im, int x0, int y0, int x1, int y1) {
            double sum = 0;
            int n = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1 - 1; ++x, ++n) sum += std::fabs(im.at(x + 1, y)[0] - im.at(x, y)[0]);
            return sum / n;
        };
        auto same = [&](const Image& a, int x, int y) {
            for (int c = 0; c < 4; ++c)
                if (a.at(x, y)[c] != img.at(x, y)[c]) return false;
            return true;
        };
        const double before = detail(img, 90, 50, 110, 70);
        // Blurred: the face's detail gone, the rest of the picture as it was.
        RedactSettings rs;
        Image blurred = img;
        redactFaces(blurred, {f}, rs);
        QVERIFY2(detail(blurred, 90, 50, 110, 70) < before * 0.05, qPrintable(QString::number(detail(blurred, 90, 50, 110, 70))));
        for (int x : {0, 20, W - 1})
            for (int y : {0, H - 1}) QVERIFY(same(blurred, x, y));
        // Pixelated: flat blocks (4 px across this face at 70%), each the mean of what it covers (here all grey).
        rs.style = 1;
        Image blocks = img;
        redactFaces(blocks, {f}, rs);
        double cx, cy, rx, ry;
        redactionShape(f, W, H, rs.expand, cx, cy, rx, ry);
        std::set<float> values;
        for (int y = int(cy) - 6; y < int(cy) + 6; ++y)
            for (int x = int(cx) - 6; x < int(cx) + 6; ++x) values.insert(blocks.at(x, y)[0]);
        QVERIFY2(values.size() <= 16, qPrintable(QString::number(values.size())));
        QVERIFY(detail(blocks, 90, 50, 110, 70) < before * 0.05);
        QVERIFY(std::fabs(blocks.at(int(cx), int(cy))[0] - 0.5f) < 0.15f);  // checks averaged to grey
        // A solid colour, keeping the picture's alpha; the ellipse leaves its box's corners, a rectangle covers them.
        rs.style = 2;
        rs.color[0] = 1, rs.color[1] = 0, rs.color[2] = 0;
        Image solid = img;
        redactFaces(solid, {f}, rs);
        QCOMPARE(solid.at(int(cx), int(cy))[0], 1.0f);
        QCOMPARE(solid.at(int(cx), int(cy))[1], 0.0f);
        QCOMPARE(solid.at(int(cx), int(cy))[3], 1.0f);
        const int kx = int(cx - rx * 0.9), ky = int(cy - ry * 0.9);
        QVERIFY(same(solid, kx, ky));
        rs.ellipse = false;
        solid = img;
        redactFaces(solid, {f}, rs);
        QCOMPARE(solid.at(kx, ky)[0], 1.0f);
        // The face's box grown upward for the forehead and hair, and by Expand all round.
        QVERIFY(cy < (f.y + f.h / 2) * H && ry > f.h * H / 2 * 1.3 && rx > f.w * W / 2 * 1.29);

        // Through the effect: two tracked faces, the second left showing, covered in green.
        FaceTracks tr;
        tr.fps = 25, tr.start = 0, tr.end = 1, tr.step = 0.04;
        FaceTrack a, b;
        a.id = 1, b.id = 2;
        for (int i = 0; i <= 25; ++i) {
            a.boxes.push_back({i * 0.04, 0.1f, 0.35f, 0.2f, 0.3f});
            b.boxes.push_back({i * 0.04, 0.6f, 0.35f, 0.2f, 0.3f});
        }
        tr.tracks = {a, b};
        Effect e = makeEffect("redact_faces", 1);
        QCOMPARE(e.p("hold", 0), 12.0);
        e.strings["tracks"] = faceTracksToString(tr);
        e.strings["keep"] = "2";
        e.params["style"] = Param(2.0);
        e.params["color.g"] = Param(1.0);
        QVERIFY(needsFaces(e));
        QVERIFY(!redactNeedsLiveFaces(e, 0.5));
        QVERIFY(redactNeedsLiveFaces(e, 5.0));
        Image out = img;
        applyVideoEffect(e, 0, out, 1.0, 0.5);
        QCOMPARE(out.at(40, 60)[1], 1.0f);
        QCOMPARE(out.at(40, 60)[0], 0.0f);
        QVERIFY(same(out, 140, 60));
        // Beyond the seconds analysed only the frame's own faces are covered (none known here)...
        out = img;
        applyVideoEffect(e, 0, out, 1.0, 5.0);
        QVERIFY(same(out, 40, 60));
        // ...and when the frame's faces are known, all of them.
        {
            FaceScope scope(std::make_shared<const std::vector<FaceBox>>(std::vector<FaceBox>{f}));
            out = img;
            applyVideoEffect(e, 0, out, 1.0, 5.0);
            QCOMPARE(out.at(100, 57)[1], 1.0f);
            // (inside the analysis the tracks decide, not the frame's faces)
            out = img;
            applyVideoEffect(e, 0, out, 1.0, 0.5);
            QVERIFY(same(out, 100, 57));
        }
        // Even a frame past the seconds analysed finds the frame's faces too, while the tracked ones are still held.
        {
            FaceScope scope(std::make_shared<const std::vector<FaceBox>>(std::vector<FaceBox>{f}));
            QVERIFY(redactNeedsLiveFaces(e, 1.0 + 2 / 25.0));
            out = img;
            applyVideoEffect(e, 0, out, 1.0, 1.0 + 2 / 25.0);
            QCOMPARE(out.at(100, 57)[1], 1.0f);
            QCOMPARE(out.at(40, 60)[1], 1.0f);
            QVERIFY(same(out, 140, 60));
        }
        // An analysis of other footage (the effect pasted onto another clip, or its media replaced), or of a 360° frame
        // before it was reframed, is not used: the frame's own faces are.
        e.strings["media"] = "7";
        {
            TrackedSourceScope source(7, false);
            QVERIFY(!redactNeedsLiveFaces(e, 0.5));
        }
        for (auto [media, reframed] : {std::pair<uint64_t, bool>{8, false}, {7, true}}) {
            TrackedSourceScope source(media, reframed);
            FaceScope scope(std::make_shared<const std::vector<FaceBox>>(std::vector<FaceBox>{f}));
            QVERIFY(redactNeedsLiveFaces(e, 0.5));
            out = img;
            applyVideoEffect(e, 0, out, 1.0, 0.5);
            QCOMPARE(out.at(100, 57)[1], 1.0f);
            QVERIFY(same(out, 40, 60));
        }
        e.strings.erase("media");
        // An analysis that found nobody leaves it to the frame's faces.
        {
            FaceTracks none = tr;
            none.tracks.clear();
            Effect empty = e;
            empty.strings["tracks"] = faceTracksToString(none);
            QVERIFY(redactNeedsLiveFaces(empty, 0.5));
        }
        // With no analysis at all: the frame's faces.
        Effect live = makeEffect("redact_faces", 2);
        QVERIFY(redactNeedsLiveFaces(live, 0.5));
        live.enabled = false;
        QVERIFY(!redactNeedsLiveFaces(live, 0.5));
        // Show Tracked Faces: red round the covered face, green round the one left showing.
        e.params["show"] = Param(1.0);
        out = img;
        applyVideoEffect(e, 0, out, 1.0, 0.5);
        FaceBox kept;
        kept.x = 0.6f, kept.y = 0.35f, kept.w = 0.2f, kept.h = 0.3f;
        redactionShape(kept, W, H, 0.3, cx, cy, rx, ry);
        const float* edge = out.at(int(cx - rx), int(cy));
        QVERIFY(edge[0] < 0.3f && edge[1] == 1.0f);
        FaceBox gone;
        gone.x = 0.1f, gone.y = 0.35f, gone.w = 0.2f, gone.h = 0.3f;
        redactionShape(gone, W, H, 0.3, cx, cy, rx, ry);
        edge = out.at(std::max(0, int(cx - rx)), int(cy));
        QVERIFY(edge[0] == 1.0f && edge[1] < 0.3f);
    }

    void roomTone() {
        constexpr int sr = 48000;
        std::mt19937 rng(7);
        std::normal_distribution<double> g(0, 1);
        // A room: low-passed noise around -40 dBFS, the same in both ears; a voice (440 Hz) over half the time.
        const int64_t n = sr * 4;
        std::vector<float> clip(size_t(n) * 2);
        double lp = 0;
        double floorEnergy = 0;
        for (int64_t i = 0; i < n; ++i) {
            lp += 0.1 * (g(rng) - lp);
            const double room = 0.03 * lp;
            floorEnergy += room * room;
            const bool talking = (i / (sr / 4)) % 2 == 0;
            const double v = room + (talking ? 0.25 * std::sin(2 * M_PI * 440 * double(i) / sr) : 0);
            clip[size_t(i) * 2] = clip[size_t(i) * 2 + 1] = float(v);
        }
        const double floorRms = std::sqrt(floorEnergy / double(n));
        RoomToneProfile prof;
        std::string err;
        QVERIFY2(learnRoomTone(clip, sr, prof, &err), err.c_str());
        auto db = [](double v) { return 20 * std::log10(v); };
        QVERIFY2(std::fabs(db(prof.rms) - db(floorRms)) < 2, qPrintable(QString("%1 vs %2 dB").arg(db(prof.rms)).arg(db(floorRms))));
        // Two seconds of it: the room's level, its dark tone, none of the voice, the same in both ears.
        const std::vector<float> tone = synthesizeRoomTone(prof, sr * 2, 3);
        QCOMPARE(tone.size(), size_t(sr * 2 * 2));
        double e = 0, lr = 0, ll = 0, rr = 0;
        for (size_t i = size_t(sr / 10); i < size_t(sr * 19 / 10); ++i) {
            const double l = tone[i * 2], r = tone[i * 2 + 1];
            e += 0.5 * (l * l + r * r);
            lr += l * r, ll += l * l, rr += r * r;
        }
        const double toneRms = std::sqrt(e / double(sr * 18 / 10));
        QVERIFY2(std::fabs(db(toneRms) - db(floorRms)) < 2, qPrintable(QString::number(db(toneRms))));
        QVERIFY(lr / std::sqrt(ll * rr) > 0.95);
        // Far more energy below 1 kHz than above 4 kHz, as in the room; the voice's 440 Hz no stronger than its neighbours.
        auto bandRatio = [&](const std::vector<float>& x, int64_t from, int64_t count) {
            // A crude DFT over a few bins in each band.
            auto power = [&](double hz) {
                std::complex<double> acc = 0;
                for (int64_t i = 0; i < count; ++i)
                    acc += double(x[size_t(from + i) * 2]) * std::polar(1.0, -2 * M_PI * hz * double(i) / sr);
                return std::norm(acc);
            };
            double lo = 0, hi = 0;
            for (double hz = 100; hz < 1000; hz += 150) lo += power(hz);
            for (double hz = 4000; hz < 12000; hz += 1300) hi += power(hz);
            return std::make_pair(lo, hi);
        };
        const auto [tl, th] = bandRatio(tone, sr / 2, sr / 4);
        QVERIFY2(tl > 20 * th, qPrintable(QString("%1 %2").arg(tl).arg(th)));
        auto toneAt = [&](double hz) {
            std::complex<double> acc = 0;
            for (int64_t i = 0; i < sr / 2; ++i) acc += double(tone[size_t(sr / 4 + i) * 2]) * std::polar(1.0, -2 * M_PI * hz * double(i) / sr);
            return std::abs(acc);
        };
        QVERIFY(toneAt(440) < 4 * (toneAt(380) + toneAt(500)) / 2);
        // Repeatable by seed; a wide room stays wide.
        QVERIFY(synthesizeRoomTone(prof, 4800, 3) == synthesizeRoomTone(prof, 4800, 3));
        QVERIFY(synthesizeRoomTone(prof, 4800, 3) != synthesizeRoomTone(prof, 4800, 4));
        std::vector<float> wide(size_t(n) * 2);
        for (int64_t i = 0; i < n; ++i) wide[size_t(i) * 2] = float(0.02 * g(rng)), wide[size_t(i) * 2 + 1] = float(0.02 * g(rng));
        RoomToneProfile wp;
        QVERIFY(learnRoomTone(wide, sr, wp, &err));
        const std::vector<float> wt = synthesizeRoomTone(wp, sr, 5);
        lr = ll = rr = 0;
        for (int64_t i = 2000; i < sr - 2000; ++i) {
            const double l = wt[size_t(i) * 2], r = wt[size_t(i) * 2 + 1];
            lr += l * r, ll += l * l, rr += r * r;
        }
        QVERIFY(std::fabs(lr / std::sqrt(ll * rr)) < 0.2);
        // Fades at both ends; nothing to learn from silence or a scrap.
        QCOMPARE(tone[0], 0.0f);
        QVERIFY(!learnRoomTone(std::vector<float>(size_t(sr) * 2, 0.0f), sr, wp, &err));
        QVERIFY(!learnRoomTone(std::vector<float>(200, 0.1f), sr, wp, &err));
        // A 24-bit stereo WAV.
        const QString wav = QDir::temp().filePath("montage-roomtone-test.wav");
        QVERIFY(writeStereoWav(wav.toStdString(), tone, sr, &err));
        QCOMPARE(QFileInfo(wav).size(), qint64(44 + tone.size() * 3));
        QFile::remove(wav);
    }

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

        // Several objects in one mask: added ones joined, cut-out ones taken away, each on the frames it was segmented.
        {
            auto field = [](double cx, double cy, double r) {
                std::vector<float> l(size_t(kObjectGrid) * kObjectGrid);
                for (int y = 0; y < kObjectGrid; ++y)
                    for (int x = 0; x < kObjectGrid; ++x) l[size_t(y) * kObjectGrid + size_t(x)] = float(r - std::hypot(x - cx, y - cy)) * 0.5f;
                return l;
            };
            ObjectMask mask;
            mask.fps = 25;
            mask.frames[10] = packObjectLogits(field(64, 128, 30).data());  // left disc
            ObjectMask right;
            right.fps = 25;
            right.frames[10] = packObjectLogits(field(192, 128, 30).data());
            right.frames[11] = right.frames[10];  // (only this object on frame 11)
            ObjectMask hole;
            hole.fps = 25;
            hole.subtract = true;
            hole.frames[10] = packObjectLogits(field(64, 128, 12).data());  // cut out of the left disc
            mask.more = {right, hole};
            std::vector<float> l;
            QVERIFY(mask.maskLogits(10, l));
            auto in = [&](int x, int y) { return l[size_t(y) * kObjectGrid + size_t(x)] > 0; };
            QVERIFY(in(64 + 20, 128) && in(192, 128) && !in(64, 128) && !in(128, 128));
            QVERIFY(mask.maskLogits(11, l) && in(192, 128) && !in(64 + 20, 128));
            // A frame with only a cut-out object is no mask; none at all, none.
            ObjectMask onlyCut = mask;
            onlyCut.frames.clear();
            onlyCut.more[0].frames.clear();
            QVERIFY(!onlyCut.maskLogits(10, l));
            QVERIFY(!mask.maskLogits(12, l));
            // Rendered through an effect.
            Effect two = makeEffect(p, "invert");
            two.params["mask.shape"] = Param(3.0);
            two.params["mask.feather"] = Param(1.0);
            two.object = std::make_shared<ObjectMask>(mask);
            Image flat(256, 256);
            flat.fill(0.5f, 0.5f, 0.5f, 1);
            const std::vector<float> m2 = effectMatte(two, 0, flat, 1.0, 10.2 / 25);
            QVERIFY(m2[size_t(128) * 256 + 64 + 20] > 0.9f && m2[size_t(128) * 256 + 192] > 0.9f && m2[size_t(128) * 256 + 64] < 0.1f);
        }
    }

    void maskEdgeRefinement() {
        // A white disc (radius 50) on a textured dark ground; the coarse mask a model gives, blocky on an 8 px grid.
        const int W = 240, H = 200;
        Image img(W, H);
        std::vector<float> truth(size_t(W) * H), coarse(size_t(W) * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                // Coverage of the disc at each pixel (4 x 4 samples), and the ground's texture.
                double cov = 0;
                for (int j = 0; j < 4; ++j)
                    for (int i = 0; i < 4; ++i) cov += std::hypot(x + (i + 0.5) / 4 - 120, y + (j + 0.5) / 4 - 100) < 50;
                cov /= 16;
                truth[size_t(y) * W + size_t(x)] = float(cov);
                const float g = 0.15f + 0.05f * float(std::sin(x * 0.7) * std::cos(y * 0.9));
                float* p = img.at(x, y);
                for (int k = 0; k < 3; ++k) p[k] = float(cov * 0.9 + (1 - cov) * g);
                p[3] = 1;
                const int bx = x / 8 * 8 + 4, by = y / 8 * 8 + 4;
                coarse[size_t(y) * W + size_t(x)] = std::hypot(bx - 120.0, by - 100.0) < 50 ? 1.f : 0.f;
            }
        auto error = [&](const std::vector<float>& m) {
            double e = 0;
            for (size_t i = 0; i < m.size(); ++i) e += std::fabs(m[i] - truth[i]);
            return e;
        };
        // Refine Edges: the blocky edge settled on the disc's.
        std::vector<float> refined = coarse;
        refineMatteEdges(refined, img, 8, 1.0);
        QVERIFY2(error(refined) < 0.15 * error(coarse), qPrintable(QString("%1 -> %2").arg(error(coarse)).arg(error(refined))));
        for (float v : refined) QVERIFY(v >= 0 && v <= 1);
        // Half of it: in between.
        std::vector<float> half = coarse;
        refineMatteEdges(half, img, 8, 0.5);
        QVERIFY(error(half) < error(coarse) && error(half) > error(refined));
        // A flat picture gives the filter nothing to follow: the mask stays close to itself, softened.
        Image grey(W, H);
        grey.fill(0.4f, 0.4f, 0.4f, 1);
        std::vector<float> same = truth;
        refineMatteEdges(same, grey, 8, 1.0);
        QVERIFY(same[size_t(100) * W + 120] > 0.95f && same[size_t(10) * W + 10] < 0.05f);

        // Consolidate: a hole and a narrow gap closed, the outline otherwise kept.
        std::vector<float> holey = truth;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (std::hypot(x - 120.0, y - 100.0) < 6) holey[size_t(y) * W + size_t(x)] = 0;  // a hole
                if (x >= 150 && x < 154 && y > 80 && y < 120) holey[size_t(y) * W + size_t(x)] = 0;  // a slit
            }
        std::vector<float> closed = holey;
        consolidateMatte(closed, W, H, 8);
        QVERIFY(closed[size_t(100) * W + 120] > 0.99f && closed[size_t(100) * W + 152] > 0.99f);
        QVERIFY(closed[size_t(100) * W + 200] < 0.01f && closed[size_t(20) * W + 120] < 0.01f);  // still nothing outside
        QVERIFY(std::fabs(closed[size_t(100) * W + 170] - truth[size_t(100) * W + 170]) < 0.05f);  // the edge where it was
        std::vector<float> none = holey;
        consolidateMatte(none, W, H, 0);
        QVERIFY(none == holey);

        // A disc feathered by 20 px keeps its whole ramp; only the slit cut into it fills, feathered the same way.
        auto smooth = [](double d, double fe) {
            const double k = std::clamp(0.5 - d / fe, 0.0, 1.0);
            return float(k * k * (3 - 2 * k));
        };
        std::vector<float> soft(size_t(W) * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const bool slit = x >= 150 && x < 154 && y > 80 && y < 120;
                soft[size_t(y) * W + size_t(x)] = slit ? 0.f : smooth(std::hypot(x + 0.5 - 120, y + 0.5 - 100) - 50, 20);
            }
        std::vector<float> softClosed = soft;
        consolidateMatte(softClosed, W, H, 8, 20);
        QVERIFY(softClosed[size_t(100) * W + 152] > 0.99f);
        for (int x : {66, 72, 76, 80})  // the left edge's ramp, far from the slit
            QCOMPARE(softClosed[size_t(100) * W + size_t(x)], soft[size_t(100) * W + size_t(x)]);
        QVERIFY(soft[size_t(100) * W + 72] > 0.3f && soft[size_t(100) * W + 72] < 0.7f && soft[size_t(100) * W + 76] < 0.95f);
        // Two feathered discs 4 px apart joined by a small Consolidate: the gap fills, and no haze spreads round them.
        std::vector<float> pair(size_t(W) * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double d = std::min(std::hypot(x + 0.5 - 68, y + 0.5 - 100), std::hypot(x + 0.5 - 172, y + 0.5 - 100)) - 50;
                pair[size_t(y) * W + size_t(x)] = smooth(d, 20);
            }
        for (float& v : pair) v = v < 0.02f ? 0.f : v;  // (the feather's faint tail cut, so the shapes stand 4 px apart)
        std::vector<float> joined = pair;
        consolidateMatte(joined, W, H, 3, 20);
        QVERIFY(joined[size_t(100) * W + 120] > pair[size_t(100) * W + 120] + 0.2f);
        QVERIFY2(joined[size_t(70) * W + 120] < 0.01f && pair[size_t(70) * W + 120] == 0, qPrintable(QString::number(joined[size_t(70) * W + 120])));
        // Nothing joins a shape to the frame's edge: beyond the frame counts as outside.
        std::vector<float> nearEdge(size_t(W) * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) nearEdge[size_t(y) * W + size_t(x)] = std::hypot(x + 0.5 - 53, y + 0.5 - 100) < 50 ? 1.f : 0.f;
        consolidateMatte(nearEdge, W, H, 8);
        QVERIFY(nearEdge[size_t(100) * W + 1] < 0.01f && nearEdge[size_t(100) * W + 53] > 0.99f);

        // Refine Edges across a wide feather: the whole ramp settles on the disc, with no step where the band would end.
        std::vector<float> wide(size_t(W) * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) wide[size_t(y) * W + size_t(x)] = smooth(std::hypot(x + 0.5 - 120, y + 0.5 - 100) - 50, 30);
        std::vector<float> wideRefined = wide;
        refineMatteEdges(wideRefined, img, 4, 1.0);
        QVERIFY2(error(wideRefined) < 0.3 * error(wide), qPrintable(QString("%1 -> %2").arg(error(wide)).arg(error(wideRefined))));
        // A stray faint pixel far from the edge changes nothing, and a mask that is sure but never whole (a people
        // mask at 0.96 inside) is refined all the same.
        std::vector<float> stray = coarse, strayRefined;
        stray[size_t(5) * W + 5] = 0.05f;
        strayRefined = stray;
        refineMatteEdges(strayRefined, img, 8, 1.0);
        QVERIFY2(error(strayRefined) < 0.15 * error(coarse), qPrintable(QString::number(error(strayRefined))));
        std::vector<float> unsure = coarse;
        for (float& v : unsure) v *= 0.96f;
        const double unsureBefore = error(unsure);
        refineMatteEdges(unsure, img, 8, 1.0);
        QVERIFY2(error(unsure) < 0.5 * unsureBefore, qPrintable(QString("%1 -> %2").arg(unsureBefore).arg(error(unsure))));
        // Colours above white (HDR) count as they are, and transparent pixels are left alone.
        Image bright = img;
        for (size_t i = 0; i < size_t(W) * H; ++i)
            for (int k = 0; k < 3; ++k) bright.px[i * 4 + k] += 1.5f;
        std::vector<float> brightRefined = coarse;
        refineMatteEdges(brightRefined, bright, 8, 1.0);
        QVERIFY2(error(brightRefined) < 0.15 * error(coarse), qPrintable(QString::number(error(brightRefined))));
        Image clear = img;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W / 2; ++x) std::fill_n(clear.at(x, y), 4, 0.f);
        std::vector<float> clearRefined = coarse;
        refineMatteEdges(clearRefined, clear, 8, 1.0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W / 2; ++x) QCOMPARE(clearRefined[size_t(y) * W + size_t(x)], coarse[size_t(y) * W + size_t(x)]);

        // Through an effect's mask: an ellipse drawn loosely round the disc, refined onto it.
        Project p = makeDefaultProject();
        Effect e = makeEffect(p, "invert");
        e.params["mask.shape"] = Param(1.0);
        e.params["mask.x"] = Param(0.5);
        e.params["mask.y"] = Param(0.5);
        e.params["mask.w"] = Param(0.45);  // radii of 54 px, the disc's 50
        e.params["mask.h"] = Param(0.54);
        e.params["mask.feather"] = Param(1.0);
        const std::vector<float> loose = effectMatte(e, 0, img, 1.0);
        e.params["mask.refine"] = Param(100.0);
        e.params["mask.refine_radius"] = Param(12.0);
        const std::vector<float> snapped = effectMatte(e, 0, img, 1.0);
        QVERIFY2(error(snapped) < 0.15 * error(loose), qPrintable(QString("%1 -> %2").arg(error(loose)).arg(error(snapped))));
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

    void syntheticFocus() {
        // A checked backdrop far away (depth 0.2) and a pale square near (0.9).
        const int W = 160, H = 100;
        Image scene(W, H);
        std::vector<float> depth(size_t(W) * H, 0.2f);
        const float pale[3] = {1.0f, 0.9f, 0.8f};
        auto inSquare = [](int x, int y) { return x >= 60 && x < 100 && y >= 30 && y < 70; };
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float* p = scene.at(x, y);
                const float g = ((x / 8 + y / 8) % 2) ? 0.5f : 0.3f;
                for (int c = 0; c < 3; ++c) p[c] = inSquare(x, y) ? pale[c] : g;
                p[3] = 1;
                if (inSquare(x, y)) depth[size_t(y) * W + x] = 0.9f;
            }
        const TransferTables video(Transfer::Bt1886);
        auto lum = [](const Image& im, int x, int y) {
            const float* q = im.at(x, y);
            return 0.2126f * q[0] + 0.7152f * q[1] + 0.0722f * q[2];
        };
        const float paleLum = 0.2126f * pale[0] + 0.7152f * pale[1] + 0.0722f * pale[2];
        LensBlurSettings s;
        s.half = 0.02;
        s.falloff = 0.3;
        s.radius = 8;

        // Focused far: the backdrop away from the square is as it was; the square's edge blurs both ways, over the
        // backdrop outside it and with the backdrop showing through inside it (no hard inner edge).
        s.focus = 0.2;
        Image far = scene;
        lensBlur(far, depth, s, &video);
        float worst = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (x < 45 || x >= 115 || y < 15 || y >= 85)
                    for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(far.at(x, y)[c] - scene.at(x, y)[c]));
        QVERIFY2(worst < 2e-3f, std::to_string(worst).c_str());
        QVERIFY(std::fabs(lum(far, 80, 50) - paleLum) < 0.01f);  // the middle of the square
        QVERIFY2(lum(far, 56, 50) > lum(scene, 56, 50) + 0.1f, std::to_string(lum(far, 56, 50)).c_str());
        QVERIFY2(lum(far, 61, 50) < paleLum - 0.08f, std::to_string(lum(far, 61, 50)).c_str());
        // And nothing darker than both: the edge goes from one to the other.
        for (int x = 50; x < 70; ++x) QVERIFY(lum(far, x, 50) > 0.29f);

        // Focused near: the square, edges and all, stays exactly sharp; the backdrop blurs without taking the
        // square's colour.
        s.focus = 0.9;
        Image nearFocus = scene;
        lensBlur(nearFocus, depth, s, &video);
        worst = 0;
        for (int y = 30; y < 70; ++y)
            for (int x = 60; x < 100; ++x)
                for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(nearFocus.at(x, y)[c] - scene.at(x, y)[c]));
        QVERIFY2(worst < 2e-3f, std::to_string(worst).c_str());
        auto spread = [&](const Image& im) {
            double sum = 0, sum2 = 0;
            int n = 0;
            for (int y = 10; y < 20; ++y)
                for (int x = 10; x < 40; ++x, ++n) sum += lum(im, x, y), sum2 += double(lum(im, x, y)) * lum(im, x, y);
            return std::sqrt(std::max(0.0, sum2 / n - (sum / n) * (sum / n)));
        };
        QVERIFY2(spread(nearFocus) < 0.4 * spread(scene), qPrintable(QString("%1 %2").arg(spread(nearFocus)).arg(spread(scene))));
        QVERIFY2(nearFocus.at(57, 50)[2] < 0.55f, std::to_string(nearFocus.at(57, 50)[2]).c_str());  // not the square's

        // Aperture shapes: a hexagon's flat sides are cos 30 degrees of its corners from the centre; anamorphic
        // squeeze makes the disc taller; the cat's eye cuts it toward the frame's edge.
        Aperture hex;
        hex.blades = 6;
        QVERIFY(apertureWeight(hex, 19, 0, 20) > 0.99f && apertureWeight(hex, 21, 0, 20) < 0.01f);
        const double c30 = std::cos(M_PI / 6), s30 = std::sin(M_PI / 6);
        QVERIFY(apertureWeight(hex, 16.5 * c30, 16.5 * s30, 20) > 0.99f && apertureWeight(hex, 18.3 * c30, 18.3 * s30, 20) < 0.01f);
        hex.rotation = 30;  // a corner where the side was
        QVERIFY(apertureWeight(hex, 19 * c30, 19 * s30, 20) > 0.99f && apertureWeight(hex, 19, 0, 20) < 0.01f);
        Aperture tall;
        tall.squeeze = 2;
        QVERIFY(apertureWeight(tall, 0, 38, 20) > 0.99f && apertureWeight(tall, 0, 42, 20) < 0.01f && apertureWeight(tall, 21, 0, 20) < 0.01f);
        Aperture round;
        QVERIFY(apertureWeight(round, 15, 0, 20, 10, 0) > 0.99f && apertureWeight(round, -15, 0, 20, 10, 0) < 0.01f);

        // A point of light out of focus opens into a disc: taller with squeeze; brighter with Bokeh Highlights; and
        // an in-focus highlight keeps its value.
        const int PW = 120, PH = 120;
        auto point = [&](int px, int py, const Aperture& a, double highlights, float focusDepth) {
            Image im(PW, PH);
            for (float& v : im.px) v = 0;
            for (int y = 0; y < PH; ++y)
                for (int x = 0; x < PW; ++x) im.at(x, y)[3] = 1;
            for (int c = 0; c < 3; ++c) im.at(px, py)[c] = 1;
            LensBlurSettings ps;
            ps.focus = focusDepth;
            ps.half = 0.01;
            ps.falloff = 0.1;
            ps.radius = 8;
            ps.aperture = a;
            ps.highlights = highlights;
            lensBlur(im, std::vector<float>(size_t(PW) * PH, 0.2f), ps, &video);
            return im;
        };
        auto extent = [&](const Image& im, int px, int py, bool vertical) {
            const float peak = im.at(px, py)[1];
            int n = 0;
            for (int k = -40; k <= 40; ++k) {
                const int x = vertical ? px : px + k, y = vertical ? py + k : py;
                if (x >= 0 && y >= 0 && x < PW && y < PH && im.at(x, y)[1] > 0.3f * peak) ++n;
            }
            return n;
        };
        const Image disc = point(60, 60, round, 0, 0.9f), oval = point(60, 60, tall, 0, 0.9f);
        const double ratio = double(extent(oval, 60, 60, true)) / extent(oval, 60, 60, false);
        QVERIFY2(ratio > 1.6 && ratio < 2.4, std::to_string(ratio).c_str());
        QVERIFY(std::abs(extent(disc, 60, 60, true) - extent(disc, 60, 60, false)) <= 1);
        QVERIFY2(extent(disc, 60, 60, false) >= 15 && extent(disc, 60, 60, false) <= 21, std::to_string(extent(disc, 60, 60, false)).c_str());
        const Image bright = point(60, 60, round, 1, 0.9f);
        QVERIFY2(video.decode(bright.at(60, 60)[1]) > 2.5f * video.decode(disc.at(60, 60)[1]),  // (in light)
                 qPrintable(QString("%1 %2").arg(bright.at(60, 60)[1]).arg(disc.at(60, 60)[1])));
        const Image sharp = point(60, 60, round, 1, 0.2f);
        QVERIFY(std::fabs(sharp.at(60, 60)[1] - 1) < 2e-3f && sharp.at(61, 60)[1] < 1e-4f);
        Aperture eye;
        eye.catEye = 1;
        auto area = [&](const Image& im) {
            int n = 0;
            for (int y = 0; y < PH; ++y)
                for (int x = 0; x < PW; ++x) n += im.at(x, y)[1] > 1e-3f;
            return n;
        };
        const Image middle = point(60, 60, eye, 0, 0.9f), corner = point(100, 100, eye, 0, 0.9f);
        QVERIFY2(area(corner) < 0.8 * area(middle), qPrintable(QString("%1 %2").arg(area(corner)).arg(area(middle))));
        // (cut across the line to the centre: narrower that way than across it)
        auto across = [&](const Image& im, int px, int py, int dx, int dy) {
            float peak = 0;
            for (size_t i = 1; i < im.px.size(); i += 4) peak = std::max(peak, im.px[i]);
            int n = 0;
            for (int k = -20; k <= 20; ++k) n += im.at(px + k * dx, py + k * dy)[1] > 0.5f * peak;
            return n;
        };
        // (through the shape's middle, which the cut moves toward the centre)
        double mx = 0, my = 0, mw = 0;
        for (int y = 0; y < PH; ++y)
            for (int x = 0; x < PW; ++x) {
                const double v = corner.at(x, y)[1];
                mx += v * x, my += v * y, mw += v;
            }
        const int gx = int(std::lround(mx / mw)), gy = int(std::lround(my / mw));
        QVERIFY2(across(corner, gx, gy, 1, 1) + 2 <= across(corner, gx, gy, 1, -1),
                 qPrintable(QString("%1 %2 at %3 %4").arg(across(corner, gx, gy, 1, 1)).arg(across(corner, gx, gy, 1, -1)).arg(gx).arg(gy)));

        // Where it focuses: its Focus Depth, the depth at its Focus Point, the nearest face's; keys on the point rack
        // in depth (half way through, half way between the two depths, though the point is then on the backdrop).
        Effect lens = makeEffect("depth_blur", 1);
        lens.params["focus"] = Param(40.0);
        QVERIFY(std::fabs(depthBlurFocus(lens, 0, depth, W, H) - 0.4) < 1e-9);
        lens.params["follow"] = Param(1.0);
        lens.params["point_x"] = Param(0.5);
        lens.params["point_y"] = Param(0.5);
        QVERIFY(std::fabs(depthBlurFocus(lens, 0, depth, W, H) - 0.9) < 1e-6);
        lens.params["point_x"] = Param();
        lens.params["point_x"].addKey(0, 0.1, Interp::Smooth);
        lens.params["point_x"].addKey(10, 0.5, Interp::Smooth);
        QVERIFY(std::fabs(depthBlurFocus(lens, 0, depth, W, H) - 0.2) < 1e-6);
        QVERIFY(std::fabs(depthBlurFocus(lens, 10, depth, W, H) - 0.9) < 1e-6);
        const double mid = depthBlurFocus(lens, 5, depth, W, H), early = depthBlurFocus(lens, 2, depth, W, H);
        QVERIFY2(std::fabs(mid - 0.55) < 0.02, std::to_string(mid).c_str());
        QVERIFY(early > 0.2 && early < mid);
        lens.params["follow"] = Param(2.0);
        QVERIFY(std::fabs(depthBlurFocus(lens, 0, depth, W, H) - 0.4) < 1e-9);  // no faces: the Focus Depth
        {
            FaceBox face;
            face.x = 0.4f, face.y = 0.35f, face.w = 0.2f, face.h = 0.3f;
            FaceBox small;
            small.x = 0.05f, small.y = 0.05f, small.w = 0.05f, small.h = 0.05f;
            FaceScope scope(std::make_shared<const std::vector<FaceBox>>(std::vector<FaceBox>{small, face}));
            QVERIFY(std::fabs(depthBlurFocus(lens, 0, depth, W, H) - 0.9) < 1e-6);
        }

        // Through the effect: Focus Point on the square keeps it sharp; Focus Breathing makes the picture a little
        // larger focused near.
        auto map = std::make_shared<DepthMap>();
        map->width = W, map->height = H, map->values = depth;
        DepthScope depthScope(map);
        Effect fx = makeEffect("depth_blur", 2);
        fx.params["follow"] = Param(1.0);
        fx.params["radius"] = Param(8.0);
        fx.params["range"] = Param(4.0);
        Image viaEffect = scene;
        applyVideoEffect(fx, 0, viaEffect, 1.0);
        QVERIFY(std::fabs(viaEffect.at(61, 31)[0] - scene.at(61, 31)[0]) < 2e-3f);
        QVERIFY(spread(viaEffect) < 0.4 * spread(scene));
        fx.params["breathing"] = Param(100.0);
        Image breathed = scene;
        applyVideoEffect(fx, 0, breathed, 1.0);
        // (1.016 times: the square's right edge, 20 pixels from the centre, moves out by about a third of a pixel)
        QVERIFY(lum(breathed, 100, 50) > lum(viaEffect, 100, 50) + 0.05f);

        // Depth Fog thinning with height: at the top of the frame the fog is far thinner than at the bottom.
        Effect fog = makeEffect("depth_fog", 3);
        fog.params["amount"] = Param(100.0);
        fog.params["start"] = Param(100.0);
        fog.params["height"] = Param(100.0);
        fog.params["horizon"] = Param(100.0);
        auto flatMap = std::make_shared<DepthMap>();
        flatMap->width = W, flatMap->height = H, flatMap->values.assign(size_t(W) * H, 0.0f);
        DepthScope flat(flatMap);
        Image black(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) black.at(x, y)[3] = 1;
        applyVideoEffect(fog, 0, black, 1.0);
        QVERIFY2(lum(black, 80, 0) < 0.01f && lum(black, 80, H - 1) > 0.7f, qPrintable(QString("%1 %2").arg(lum(black, 80, 0)).arg(lum(black, 80, H - 1))));
    }

    void restorationKernels() {
        // A picture with fine detail panning 3 px across and 1 down a frame, a little noise in each frame.
        auto tex = [](double x, double y) {
            return 0.45 + 0.15 * std::sin(x * 0.21) * std::cos(y * 0.17) + 0.1 * std::sin((x + 2 * y) * 0.07);
        };
        const int W = 160, H = 96;
        auto frame = [&](int k) {
            Image f(W, H);
            std::mt19937 rng(unsigned(k + 7));
            std::normal_distribution<float> noise(0, 0.004f);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const float v = float(tex(x + 3 * k, y + k));
                    float* q = f.at(x, y);
                    q[0] = v + noise(rng), q[1] = v * 0.9f + noise(rng), q[2] = v * 0.8f + noise(rng), q[3] = 1;
                }
            return f;
        };
        const Image prev = frame(-1), clean = frame(0), next = frame(1);
        const MotionFn toPrev = [](double, double) { return Point2{3, 1}; };
        const MotionFn toNext = [](double, double) { return Point2{-3, -1}; };
        // Specks of dirt, dark and light, one to three pixels across, and a blot too big to be dirt.
        struct Speck {
            int x, y;
            double r;
            float d;
        };
        const std::vector<Speck> specks = {{20, 20, 2, -0.3f}, {60, 30, 1.5, 0.35f}, {100, 70, 3, -0.25f},
                                           {130, 15, 2, 0.3f}, {40, 80, 2.5, -0.4f}, {90, 45, 1, 0.3f}};
        Image dirty = clean;
        auto blot = [&](int cx, int cy, double r, float d) {
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    if (std::hypot(x - cx, y - cy) <= r)
                        for (int k = 0; k < 3; ++k) dirty.at(x, y)[k] = std::clamp(dirty.at(x, y)[k] + d, 0.0f, 1.0f);
        };
        for (const Speck& sp : specks) blot(sp.x, sp.y, sp.r, sp.d);
        blot(140, 70, 9, -0.3f);
        DirtSettings ds;
        int count = -1;
        QVERIFY(detectDirt(clean, prev, next, toPrev, toNext, ds, &count).size() == size_t(W) * H);
        QCOMPARE(count, 0);  // nothing found in the clean frame
        const std::vector<float> matte = detectDirt(dirty, prev, next, toPrev, toNext, ds, &count);
        QCOMPARE(count, int(specks.size()));
        Image fixed = dirty;
        fillFromNeighbours(fixed, matte, prev, next, toPrev, toNext);
        auto nearSpeck = [&](int x, int y) {
            for (const Speck& sp : specks)
                if (std::hypot(x - sp.x, y - sp.y) <= sp.r + 5) return true;
            return false;
        };
        float worst = 0, elsewhere = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int k = 0; k < 3; ++k) {
                    const float e = std::fabs(fixed.at(x, y)[k] - clean.at(x, y)[k]);
                    if (nearSpeck(x, y)) worst = std::max(worst, e);
                    else elsewhere = std::max(elsewhere, std::fabs(fixed.at(x, y)[k] - dirty.at(x, y)[k]));
                }
        QVERIFY2(worst < 0.03f, std::to_string(worst).c_str());  // the specks are gone
        QVERIFY2(elsewhere == 0, std::to_string(elsewhere).c_str());  // and nothing else changed, the blot included
        QVERIFY(fixed.at(140, 70)[0] < clean.at(140, 70)[0] - 0.2f);

        // Scratches: a dark one a pixel wide all the way down and a light one three wide in the top half; a 12-pixel bar
        // and an edge in the picture are not scratches.
        const int SW = 200, SH = 128;
        Image plain(SW, SH);
        for (int y = 0; y < SH; ++y)
            for (int x = 0; x < SW; ++x) {
                const float v = float(0.4 + 0.06 * std::sin(x * 0.21) * std::cos(y * 0.17) + (x >= 180 ? 0.2 : 0) - (x >= 160 && x < 172 ? 0.3 : 0));
                float* q = plain.at(x, y);
                q[0] = v, q[1] = v, q[2] = v, q[3] = 1;
            }
        Image scratched = plain;
        for (int y = 0; y < SH; ++y) {
            for (int k = 0; k < 3; ++k) scratched.at(50, y)[k] -= 0.25f;
            if (y < 64)
                for (int x = 120; x <= 122; ++x)
                    for (int k = 0; k < 3; ++k) scratched.at(x, y)[k] += 0.2f;
        }
        ScratchSettings ss;
        const std::vector<float> sm = detectScratches(scratched, ss, &count);
        QVERIFY2(count >= 8 + 4, std::to_string(count).c_str());
        int dark = 0, lightTop = 0, lightBottom = 0, wrong = 0;
        for (int y = 0; y < SH; ++y) {
            dark += sm[size_t(y) * SW + 50] >= 1;
            if (y < 64) lightTop += sm[size_t(y) * SW + 121] >= 1;
            else lightBottom += sm[size_t(y) * SW + 121] > 0;
            for (int x : {30, 100, 158, 160, 165, 171, 179, 180, 185}) wrong += sm[size_t(y) * SW + x] > 0;
        }
        QCOMPARE(dark, SH);
        QCOMPARE(lightTop, 64);
        QCOMPARE(lightBottom, 0);
        QCOMPARE(wrong, 0);
        Image unscratched = scratched;
        fillAcross(unscratched, sm);
        float scratchError = 0;
        for (int y = 0; y < SH; ++y)
            for (int x : {50, 120, 121, 122}) scratchError = std::max(scratchError, std::fabs(unscratched.at(x, y)[0] - plain.at(x, y)[0]));
        QVERIFY2(scratchError < 0.03f, std::to_string(scratchError).c_str());
        // A setting for dark scratches leaves the light one.
        ss.polarity = 1;
        const std::vector<float> darkOnly = detectScratches(scratched, ss);
        QVERIFY(darkOnly[size_t(10) * SW + 50] >= 1 && darkOnly[size_t(10) * SW + 121] == 0);

        // Patch Fill: a logo on a wall lit brighter to the right, covered from the same wall 80 pixels left (two
        // periods of its pattern), seamlessly: the light matches and no edge shows. Copied plainly, it is too dark.
        auto wall = [](int x, int y) { return float(0.3 + 0.4 * x / 200.0 + 0.05 * std::sin(x * 2 * M_PI / 40) * std::sin(y * 2 * M_PI / 40)); };
        Image withLogo(SW, SH);
        for (int y = 0; y < SH; ++y)
            for (int x = 0; x < SW; ++x) {
                const float v = x >= 120 && x < 150 && y >= 40 && y < 70 ? 0.95f : wall(x, y);
                float* q = withLogo.at(x, y);
                q[0] = v, q[1] = v, q[2] = v, q[3] = 1;
            }
        Effect patch = makeEffect("patch_fill", 1);
        patch.params["mask.shape"] = 2.0;
        patch.params["mask.x"] = 135.0 / SW;
        patch.params["mask.y"] = 55.0 / SH;
        patch.params["mask.w"] = 40.0 / SW;
        patch.params["mask.h"] = 40.0 / SH;
        patch.params["mask.feather"] = 0.0;
        patch.params["source_x"] = -80.0 / SW;
        auto patchError = [&](const Image& img, bool inside) {
            float e = 0;
            for (int y = 0; y < SH; ++y)
                for (int x = 0; x < SW; ++x) {
                    const bool in = x >= 114 && x < 156 && y >= 34 && y < 76;
                    if (in == inside) e = std::max(e, std::fabs(img.at(x, y)[0] - (inside ? wall(x, y) : withLogo.at(x, y)[0])));
                }
            return e;
        };
        Image seamless = withLogo;
        applyVideoEffect(patch, 0, seamless, 1.0);
        QVERIFY2(patchError(seamless, true) < 0.01f, std::to_string(patchError(seamless, true)).c_str());
        QCOMPARE(patchError(seamless, false), 0.0f);
        patch.params["blend"] = 1.0;
        Image copied = withLogo;
        applyVideoEffect(patch, 0, copied, 1.0);
        QVERIFY(patchError(copied, true) > 0.1f);

        // Paint: a heal stroke over the logo on frame 3 only; strokes kept as text.
        Effect paint = makeEffect("paint", 2);
        PaintStroke heal;
        heal.heal = true;
        heal.frame = 3;
        heal.dx = -80.0 / SW;
        heal.radius = 25.0 / SW;
        heal.softness = 0.2;
        heal.points = {{134.5 / SW, 54.5 / SH}, {135.5 / SW, 55.5 / SH}};
        PaintStroke clone = heal;
        clone.heal = false;
        clone.hold = true;
        clone.track = true;
        clone.frame = 0;
        clone.radius = 4.0 / SW;
        clone.points = {{40.5 / SW, 100.5 / SH}};
        paint.strings["strokes"] = paintStrokesToString({heal, clone});
        const std::vector<PaintStroke> back = paintStrokesFromString(paint.strings["strokes"]);
        QCOMPARE(int(back.size()), 2);
        QVERIFY(back[0].heal && !back[0].hold && back[0].frame == 3 && back[0].points.size() == 2 && !back[1].heal && back[1].hold && back[1].track);
        QVERIFY(std::fabs(back[0].dx - heal.dx) < 1e-6 && std::fabs(back[1].points[0][1] - clone.points[0][1]) < 1e-6);
        Image healed = withLogo;
        applyVideoEffect(paint, 3, healed, 1.0);
        float healError = 0;
        for (int y = 40; y < 70; ++y)
            for (int x = 120; x < 150; ++x) healError = std::max(healError, std::fabs(healed.at(x, y)[0] - wall(x, y)));
        QVERIFY2(healError < 0.015f, std::to_string(healError).c_str());
        // The clone stroke copies exactly at its centre (from 80 pixels left, off the frame's edge held).
        QVERIFY(std::fabs(healed.at(40, 100)[0] - withLogo.at(0, 100)[0]) < 1e-4f);
        Image later = withLogo;
        applyVideoEffect(paint, 4, later, 1.0);  // the heal was on frame 3 alone
        QVERIFY(later.at(135, 55)[0] > 0.9f);
        // A tracked stroke follows the mask's box: moved 20 pixels right by frame 5.
        paint.params["mask.x"].addKey(0, 0.5);
        paint.params["mask.x"].addKey(5, 0.6);
        const std::vector<PaintStrokeAt> at5 = paintStrokesAt(paint, 5, SW, SH);
        QCOMPARE(int(at5.size()), 1);
        QVERIFY(std::fabs(at5[0].points[0][0] - 60.0) < 1e-6 && std::fabs(at5[0].dx + 80) < 1e-6);
        // The mask does not limit the strokes: one outside the box still paints.
        Effect outside = makeEffect("paint", 1);
        outside.params["mask.shape"] = Param(2.0);
        outside.params["mask.w"] = Param(0.1);
        outside.params["mask.h"] = Param(0.1);
        PaintStroke corner;
        corner.frame = 0;
        corner.dx = 0.25;
        corner.radius = 0.03;
        corner.points = {{0.1, 0.5}};
        outside.strings["strokes"] = paintStrokesToString({corner});
        Image painted = withLogo;
        applyVideoEffect(outside, 0, painted, 1.0);
        QVERIFY(std::fabs(painted.at(int(0.1 * SW), SH / 2)[0] - withLogo.at(int(0.35 * SW), SH / 2)[0]) < 1e-3f);

        // A razor cut and a head trim keep spots and strokes on their pictures' frames.
        Clip clip;
        clip.start = 0;
        clip.duration = 100;
        Effect spots = makeEffect("dust_spots", 2), strokes = makeEffect("paint", 3);
        spots.strings["spots"] = dustSpotsToString({{60, 0.5, 0.5, 0.02}, {20, 0.4, 0.4, 0.02}});
        PaintStroke at70;
        at70.frame = 70;
        at70.points = {{0.5, 0.5}};
        strokes.strings["strokes"] = paintStrokesToString({at70});
        clip.effects = {spots, strokes};
        const Clip right = edit::subClip(clip, 50, 100);
        const std::vector<DustSpot> moved = dustSpotsFromString(right.effects[0].s("spots"));
        QCOMPARE(int(moved.size()), 2);
        QCOMPARE(moved[0].frame, int64_t(10));
        QCOMPARE(moved[1].frame, int64_t(-30));  // (before the part: never shown, kept for an undo of the trim)
        QVERIFY(std::fabs(moved[0].x - 0.5) < 1e-9 && std::fabs(moved[0].r - 0.02) < 1e-9);
        QCOMPARE(paintStrokesFromString(right.effects[1].s("strokes"))[0].frame, int64_t(20));
        QVERIFY(hasDustSpotAt(right.effects[0].s("spots"), 10) && !hasDustSpotAt(right.effects[0].s("spots"), 60));

        // Scratch Removal on a picture narrower than two scratch widths finds nothing (and reads nothing outside it).
        ScratchSettings wide;
        wide.maxWidth = 32;
        QVERIFY(detectScratches(Image(64, 64), wide) == std::vector<float>(64 * 64, 0.0f));
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

    void hdrPaletteZones() {
        Project p;
        float c[4];
        auto graded = [&](Image img, const std::map<std::string, double>& set) {
            Effect e = makeEffect(p, "hdr_palette");
            for (const auto& [k, v] : set) e.params[k] = v;
            applyVideoEffect(e, 0, img, 1);
            return img;
        };
        // Untouched it changes nothing; a stop up in the shadows doubles the light of a dark grey (Rec.709, gamma 2.4)
        // and leaves a bright one alone.
        Image same = graded(solid(2, 2, 0.3f, 0.5f, 0.7f), {});
        rgb(same, 0, 0, c);
        QVERIFY(near(c[0], 0.3f, 1e-3f) && near(c[1], 0.5f, 1e-3f) && near(c[2], 0.7f, 1e-3f));
        const float up = std::pow(2.0f, 1 / 2.4f);
        rgb(graded(solid(2, 2, 0.2f, 0.2f, 0.2f), {{"shadow_exposure", 1.0}}), 0, 0, c);
        QVERIFY2(near(c[0], 0.2f * up, 0.006f), qPrintable(QString::number(c[0])));
        rgb(graded(solid(2, 2, 0.9f, 0.9f, 0.9f), {{"shadow_exposure", 1.0}}), 0, 0, c);
        QVERIFY(near(c[0], 0.9f, 1e-3f));
        // The global exposure moves everything.
        rgb(graded(solid(2, 2, 0.5f, 0.5f, 0.5f), {{"exposure", 1.0}}), 0, 0, c);
        QVERIFY(near(c[0], 0.5f * up, 0.003f));
        // Warmer highlights: a bright grey turns warm, a dark one stays grey.
        const std::map<std::string, double> warm{{"highlight_r", 0.3}, {"highlight_g", -0.1}, {"highlight_b", -0.2}};
        rgb(graded(solid(2, 2, 0.98f, 0.98f, 0.98f), warm), 0, 0, c);
        QVERIFY2(c[0] > c[1] + 0.01f && c[1] > c[2] + 0.01f, qPrintable(QString("%1 %2 %3").arg(c[0]).arg(c[1]).arg(c[2])));
        rgb(graded(solid(2, 2, 0.15f, 0.15f, 0.15f), warm), 0, 0, c);
        QVERIFY(near(c[0], c[1], 1e-3f) && near(c[1], c[2], 1e-3f));
        // No colour left in the shadows; the highlights keep theirs.
        rgb(graded(solid(2, 2, 0.25f, 0.1f, 0.05f), {{"shadow_saturation", 0.0}}), 0, 0, c);
        QVERIFY(near(c[0], c[1], 0.003f) && near(c[1], c[2], 0.003f));
        rgb(graded(solid(2, 2, 1.0f, 0.8f, 0.7f), {{"shadow_saturation", 0.0}}), 0, 0, c);
        QVERIFY(c[0] - c[2] > 0.25f);
        // Zones pulled hard against each other never turn tones over; a Mix of 0 is the original.
        Image ramp(101, 1);
        for (int x = 0; x <= 100; ++x) {
            float* q = ramp.at(x, 0);
            q[0] = q[1] = q[2] = x / 100.0f;
            q[3] = 1;
        }
        const Image fight = graded(ramp, {{"shadow_exposure", 4.0}, {"light_exposure", -4.0}, {"contrast", 2.0}});
        for (int x = 1; x <= 100; ++x) QVERIFY2(fight.at(x, 0)[1] >= fight.at(x - 1, 0)[1] - 1e-5f, qPrintable(QString::number(x)));
        const Image none = graded(ramp, {{"exposure", 3.0}, {"mix", 0.0}});
        for (int x = 0; x <= 100; ++x) QVERIFY(std::fabs(none.at(x, 0)[1] - ramp.at(x, 0)[1]) < 1e-5f);
        // In an HDR sequence it works in nits: a stop up takes reference white (203 nits) to 406, and a highlight zone
        // set from 2 stops above grey brings a 1000-nit specular down a stop while leaving reference white alone.
        const ColorSpace& pq = *findColorSpace("rec2100pq");
        {
            const WorkingSpaceScope working(&pq);
            const float white = float(nitsToCode(pq, 203));
            rgb(graded(solid(2, 2, white, white, white), {{"exposure", 1.0}}), 0, 0, c);
            QVERIFY2(std::fabs(codeToNits(pq, c[1]) / 406 - 1) < 0.02, qPrintable(QString::number(codeToNits(pq, c[1]))));
            const float spec = float(nitsToCode(pq, 1000));
            const std::map<std::string, double> tame{{"specular_exposure", -1.0}, {"specular_range", 3.5}, {"specular_falloff", 0.5}};
            rgb(graded(solid(2, 2, spec, spec, spec), tame), 0, 0, c);
            QVERIFY2(std::fabs(codeToNits(pq, c[1]) / 500 - 1) < 0.03, qPrintable(QString::number(codeToNits(pq, c[1]))));
            rgb(graded(solid(2, 2, white, white, white), tame), 0, 0, c);
            QVERIFY(std::fabs(codeToNits(pq, c[1]) / 203 - 1) < 0.01);
        }
        // Half-transparent pixels (feathered edges, keys, titles) grade like opaque ones: untouched they stay as they are
        // (in PQ too, where a doubled code would clip), graded they get the opaque colour at their alpha.
        auto halfAlpha = [](float v) {
            Image img = solid(2, 2, v * 0.5f, v * 0.5f, v * 0.5f);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x) img.at(x, y)[3] = 0.5f;
            return img;
        };
        {
            const WorkingSpaceScope working(&pq);
            const Image edge = graded(halfAlpha(0.6f), {});
            QVERIFY2(near(edge.at(0, 0)[1], 0.3f, 1e-3f), qPrintable(QString::number(edge.at(0, 0)[1])));
        }
        const Image half = graded(halfAlpha(0.2f), {{"shadow_exposure", 1.0}});
        rgb(graded(solid(2, 2, 0.2f, 0.2f, 0.2f), {{"shadow_exposure", 1.0}}), 0, 0, c);
        QVERIFY2(near(half.at(0, 0)[1], c[1] * 0.5f, 1e-3f), qPrintable(QString("%1 %2").arg(half.at(0, 0)[1]).arg(c[1])));
        // HLG's zones sit where SDR's and PQ's do: its mid grey (18% of reference white, 75% code) is in no shadow zone,
        // and a stop up doubles its light.
        {
            const ColorSpace& hlg = *findColorSpace("rec2100hlg");
            const WorkingSpaceScope working(&hlg);
            const double white = toLinear(Transfer::Hlg, 0.75);
            const float grey = float(fromLinear(Transfer::Hlg, 0.18 * white));
            rgb(graded(solid(2, 2, grey, grey, grey), {{"shadow_exposure", 1.0}}), 0, 0, c);
            QVERIFY2(near(c[1], grey, 2e-3f), qPrintable(QString("%1 %2").arg(c[1]).arg(grey)));
            rgb(graded(solid(2, 2, grey, grey, grey), {{"exposure", 1.0}}), 0, 0, c);
            QVERIFY2(std::fabs(toLinear(Transfer::Hlg, c[1]) / (0.18 * white) - 2) < 0.03, qPrintable(QString::number(c[1])));
        }
        // A channel balance that changes quickly with brightness never turns that channel's tones over.
        const Image blue = graded(ramp, {{"highlight_b", -1.0}, {"highlight_falloff", 0.25}});
        for (int x = 1; x <= 100; ++x) QVERIFY2(blue.at(x, 0)[2] >= blue.at(x - 1, 0)[2] - 1e-5f, qPrintable(QString::number(x)));
        // The compositor says which space the sequence is in: a grey matte in a PQ sequence, a stop up, has twice the light.
        Sequence seq = makeSequence(p, "HDR", 64, 36, Rational{25, 1});
        seq.colorSpace = "rec2100pq";
        Clip matte = makeGeneratorClip(p, "color", 10);
        matte.generator.params["color.r"] = matte.generator.params["color.g"] = matte.generator.params["color.b"] = 0.5;
        QVERIFY(edit::overwrite(p, seq, {TrackKind::Video, 0}, matte).ok);
        RenderOptions o;
        const Image before = renderSequenceFrame(p, seq, 2, o);
        Effect stop = makeEffect(p, "hdr_palette");
        stop.params["exposure"] = 1.0;
        seq.videoTracks[0].clips[0].effects.push_back(stop);
        const Image after = renderSequenceFrame(p, seq, 2, o);
        const double ratio = codeToNits(pq, after.at(30, 18)[1]) / codeToNits(pq, before.at(30, 18)[1]);
        QVERIFY2(std::fabs(ratio - 2) < 0.05, qPrintable(QString::number(ratio)));
        // A grade a LUT can hold.
        std::vector<std::string> skipped;
        bakeLut({stop}, 0, 9, &skipped);
        QVERIFY(skipped.empty());
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

    void hdrViewForHdrDisplays() {
        const ColorSpace& pq = *findColorSpace("rec2100pq");
        const ColorSpace& hlg = *findColorSpace("rec2100hlg");
        auto view = [](const ColorSpace& s, float r, float g, float b, double headroom, double peak = 1000) {
            float c[3] = {r, g, b};
            hdrViewPixel(c, s, peak, headroom);
            return std::array<float, 3>{c[0], c[1], c[2]};
        };
        auto show = [](std::array<float, 3> c) { return QString("%1 %2 %3").arg(c[0]).arg(c[1]).arg(c[2]); };
        // PQ's 203 nits is SDR white; 1000 nits five times it, shown as such where the display has the headroom.
        const float w203 = float(nitsToCode(pq, 203)), w1000 = float(nitsToCode(pq, 1000)), w4000 = float(nitsToCode(pq, 4000));
        auto white = view(pq, w203, w203, w203, 8);
        QVERIFY2(std::fabs(white[0] - 1) < 0.01 && std::fabs(white[1] - 1) < 0.01 && std::fabs(white[2] - 1) < 0.01, qPrintable(show(white)));
        auto spec = view(pq, w1000, w1000, w1000, 8);
        QVERIFY2(std::fabs(spec[1] - 1000.0 / 203) < 0.05, qPrintable(show(spec)));
        // A display with less headroom than the master: highlights roll off below its peak, SDR white untouched,
        // hue kept; one with enough shows the master as it is.
        auto rolled = view(pq, w1000, w1000, w1000, 2);
        QVERIFY2(rolled[1] > 1.6 && rolled[1] < 2.0, qPrintable(show(rolled)));
        QVERIFY2(std::fabs(view(pq, w203, w203, w203, 2)[1] - 1) < 0.01, qPrintable(show(view(pq, w203, w203, w203, 2))));
        const float o1 = float(nitsToCode(pq, 1000)), o2 = float(nitsToCode(pq, 250));
        auto orange = view(pq, o1, o2, 0, 2), orangeFull = view(pq, o1, o2, 0, 8);
        QVERIFY2(orange[0] < 2.0 && std::fabs(orange[1] / orange[0] - orangeFull[1] / orangeFull[0]) < 0.01, qPrintable(show(orange) + " / " + show(orangeFull)));
        QVERIFY2(view(pq, w4000, w4000, w4000, 8, 4000)[1] < 8.0 && view(pq, w4000, w4000, w4000, 8, 4000)[1] > 6.0,
                 qPrintable(show(view(pq, w4000, w4000, w4000, 8, 4000))));
        QCOMPARE(hdrContentPeak(pq, 1000), 1000.0 / 203);
        QCOMPARE(hdrContentPeak(rec709Space(), 1000), 1.0);
        // On an SDR display (headroom 1) the HDR picture stays within white.
        QVERIFY(view(pq, w1000, w1000, w1000, 1)[1] <= 1.0f);
        // Wide gamut kept: Rec. 2020's green is outside Rec. 709, so red and blue go negative.
        const float g100 = float(nitsToCode(pq, 100));
        auto green = view(pq, 0, g100, 0, 8);
        QVERIFY2(green[0] < -0.05f && green[2] < -0.01f && green[1] > 0.5f, qPrintable(show(green)));
        // HLG's 75 % is SDR white on its 1000-nit reference display (BT.2408).
        auto hw = view(hlg, 0.75f, 0.75f, 0.75f, 8);
        QVERIFY2(std::fabs(hw[1] - 1) < 0.04, qPrintable(show(hw)));
        // SDR is its own display light, within white.
        auto sdr = view(rec709Space(), 1, 0.5f, 0, 8);
        QVERIFY2(std::fabs(sdr[0] - 1) < 1e-4 && std::fabs(sdr[1] - std::pow(0.5f, 2.4f)) < 1e-4 && std::fabs(sdr[2]) < 1e-6, qPrintable(show(sdr)));
        // A frame as half floats, the table matching the pixel function, alpha 1, scaled for scRGB.
        Image img(3, 2);
        const float codes[3] = {w203, w1000, g100};
        for (int x = 0; x < 3; ++x)
            for (int y = 0; y < 2; ++y) {
                float* p = img.at(x, y);
                p[0] = p[1] = p[2] = codes[x];
                p[3] = 1;
            }
        std::vector<uint16_t> half;
        hdrViewImage(img, pq, 1000, 8, half, 2.5f);
        QCOMPARE(half.size(), size_t(3 * 2 * 4));
        std::vector<float> back(half.size());
        qFloatFromFloat16(back.data(), reinterpret_cast<const qfloat16*>(half.data()), qsizetype(half.size()));
        for (int x = 0; x < 3; ++x) {
            auto expect = view(pq, codes[x], codes[x], codes[x], 8);
            QVERIFY2(std::fabs(back[size_t(x) * 4 + 1] / 2.5f - expect[1]) < 0.01f * std::max(1.0f, expect[1]),
                     qPrintable(QString("%1: %2 vs %3").arg(x).arg(back[size_t(x) * 4 + 1] / 2.5f).arg(expect[1])));
            QCOMPARE(back[size_t(x) * 4 + 3], 1.0f);
        }
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
            {Transfer::LLog, {0.090000000, 0.130000000, 0.165074843, 0.435313904, 0.619557106, 0.793579940}},  // also colour-science
            // GoPro's formula from its Labs docs (base 400) and GP-Log2 white paper (base 600), grey at L = 0.0517.
            {Transfer::GoProLog, {0.0, 0.075595792, 0.127435427, 0.513037243, 0.775367400, 1.023083798}},
            {Transfer::GoProLog2, {0.0, 0.097019004, 0.156433877, 0.541601097, 0.789234046, 1.021637515}},
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
            {Primaries::AppleWideGamut, {0.679108, 0.312928, 0.044307}},
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
        for (const char* id : {"applelog-rec2020", "applelog2-applewg", "dlog-dgamut", "flog2-fgamut", "nlog-ngamut", "log3g10-rwg", "bmdfilm5-bmdwg",
                               "di-dwg", "slog3-sgamut3", "gplog-rec709", "gplog2-rec2020", "llog-rec2020"}) {
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
        // Apple Log 2 is Apple Log's curve over Apple Gamut (ACES' CSC.Apple.AppleLog2_to_ACES matrix, through Bradford).
        double toAp0[9];
        primariesMatrix(Primaries::AppleWideGamut, Primaries::Ap0, toAp0);
        const double aces[9] = {0.694961049318, 0.241405268785, 0.063633681897, 0.047362746415, 1.004295925054,
                                -0.051658671469, -0.021989789360, -0.028989104971, 1.050978894331};
        for (int i = 0; i < 9; ++i) QVERIFY2(std::fabs(toAp0[i] - aces[i]) < 2e-4, qPrintable(QString("%1: %2").arg(i).arg(toAp0[i], 0, 'f', 6)));
        // A GoPro or Leica grey reads as grey.
        for (const char* id : {"gplog2-rec2020", "llog-rec2020"}) {
            const ColorSpace& cs = *findColorSpace(id);
            const float code = float(fromLinear(cs.transfer, 0.18));
            Image g = solid(1, 1, code, code, code);
            convertColor(g, cs, *findColorSpace("linear-rec709"), 1000);
            rgb(g, 0, 0, c);
            QVERIFY2(std::fabs(c[1] - 0.18f) < 2e-3f, qPrintable(QString("%1: %2").arg(id).arg(c[1])));
        }
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

    void screenKeyer() {
        Project p = makeDefaultProject();
        // A green screen with a little noise, a skin-toned subject, a soft half-and-half edge and green spill on the
        // subject's rim.
        const int w = 200, h = 120;
        Image img(w, h);
        std::mt19937 rng(9);
        std::normal_distribution<float> n(0, 0.015f);
        const float screen[3] = {0.12f, 0.72f, 0.2f}, skin[3] = {0.8f, 0.58f, 0.47f};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double d = std::hypot(x - 100.0, y - 60.0);
                float c[3];
                for (int k = 0; k < 3; ++k) {
                    const float mix = d < 30 ? 1.0f : d < 34 ? 0.5f : 0.0f;  // subject, its soft edge, screen
                    c[k] = mix * skin[k] + (1 - mix) * screen[k] + n(rng);
                }
                if (d >= 26 && d < 30) c[1] += 0.12f;  // spill on the rim
                float* px = img.at(x, y);
                px[0] = c[0], px[1] = c[1], px[2] = c[2], px[3] = 1;
            }
        // The screen colour read from the picture.
        double picked[3];
        QVERIFY(estimateScreenColor(img, picked));
        for (int k = 0; k < 3; ++k) QVERIFY2(std::fabs(picked[k] - screen[k]) < 0.04, qPrintable(QString("%1: %2").arg(k).arg(picked[k])));
        QVERIFY(!estimateScreenColor(solid(64, 64, 0.5f, 0.45f, 0.4f), picked));  // no screen
        Effect key = makeEffect(p, "screen_key");
        key.params["key.r"] = picked[0], key.params["key.g"] = picked[1], key.params["key.b"] = picked[2];
        auto run = [&](const Effect& e) {
            Image out = img;
            applyVideoEffect(e, 0, out, 1.0);
            return out;
        };
        auto alphaAt = [](const Image& im, int x, int y) { return im.at(x, y)[3]; };
        const Image keyed = run(key);
        QVERIFY2(alphaAt(keyed, 5, 5) < 0.03f && alphaAt(keyed, 190, 110) < 0.03f, qPrintable(QString::number(alphaAt(keyed, 5, 5))));
        QVERIFY(alphaAt(keyed, 100, 60) > 0.97f);
        QVERIFY2(std::fabs(alphaAt(keyed, 132, 60) - 0.5f) < 0.15f, qPrintable(QString::number(alphaAt(keyed, 132, 60))));
        // The rim's green spill is gone (green no stronger than red or blue) and its brightness partly kept.
        float c[4];
        rgb(keyed, 128, 60, c);
        QVERIFY2(c[1] <= std::max(c[0], c[2]) + 0.01f, qPrintable(QString("%1 %2 %3").arg(c[0]).arg(c[1]).arg(c[2])));
        rgb(keyed, 100, 60, c);
        QVERIFY(std::fabs(c[0] - skin[0]) < 0.05f && std::fabs(c[2] - skin[2]) < 0.05f);  // the subject's colour kept
        // Views: the matte as grey, and the status (black, white, grey between).
        Effect matte = key;
        matte.params["view"] = 1;
        const Image mv = run(matte);
        QVERIFY(std::fabs(mv.at(132, 60)[0] - alphaAt(keyed, 132, 60)) < 1e-4f && mv.at(5, 5)[3] == 1.0f);
        Effect status = key;
        status.params["view"] = 2;
        const Image sv = run(status);
        QCOMPARE(sv.at(5, 5)[0], 0.0f);
        QCOMPARE(sv.at(100, 60)[0], 1.0f);
        QCOMPARE(sv.at(132, 60)[0], 0.5f);
        // Shrinking takes pixels off the subject's edge; growing adds them; softening widens the edge.
        auto opaque = [](const Image& im) {
            int nOpaque = 0;
            for (int y = 0; y < im.height; ++y)
                for (int x = 0; x < im.width; ++x) nOpaque += im.at(x, y)[3] > 0.97f;
            return nOpaque;
        };
        auto partial = [](const Image& im) {
            int nPartial = 0;
            for (int y = 0; y < im.height; ++y)
                for (int x = 0; x < im.width; ++x) nPartial += im.at(x, y)[3] > 0.03f && im.at(x, y)[3] < 0.97f;
            return nPartial;
        };
        Effect shrink = key, grow = key, soft = key;
        shrink.params["shrink"] = 3;
        grow.params["shrink"] = -3;
        soft.params["soften"] = 8;
        QVERIFY(opaque(run(shrink)) < opaque(keyed) - 200);
        QVERIFY(opaque(run(grow)) > opaque(keyed) + 200);
        QVERIFY(partial(run(soft)) > partial(keyed) * 2);
        // Clip black makes the half-and-half edge clearer; more gain more transparent.
        Effect clip = key;
        clip.params["clip_black"] = 0.6;
        QVERIFY(alphaAt(run(clip), 132, 60) < 0.05f);
        Effect strong = key;
        strong.params["gain"] = 1.6;
        QVERIFY(alphaAt(run(strong), 132, 60) < alphaAt(keyed, 132, 60));
        // A blue screen is found and keyed as well.
        Image blue = solid(64, 64, 0.1f, 0.2f, 0.8f);
        for (int y = 24; y < 40; ++y)
            for (int x = 24; x < 40; ++x) {
                float* px = blue.at(x, y);
                px[0] = 0.7f, px[1] = 0.6f, px[2] = 0.5f;
            }
        QVERIFY(estimateScreenColor(blue, picked) && picked[2] > 0.7);
        Effect bk = makeEffect(p, "screen_key");
        bk.params["key.r"] = picked[0], bk.params["key.g"] = picked[1], bk.params["key.b"] = picked[2];
        applyVideoEffect(bk, 0, blue, 1.0);
        QVERIFY(blue.at(2, 2)[3] < 0.03f && blue.at(32, 32)[3] > 0.97f);
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

    void galleryStillsAndGrades() {
        // Red inverted (cyan) with a blur after the grade; blue with only a blur; green with nothing.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::string folder = dir.path().toStdString() + "/Gallery/film";
        // (beside the project; on Windows a rooted path gains the current drive)
        QCOMPARE(galleryFolder("/x/y/film.montage"), (QFileInfo("/x/y/film.montage").absolutePath() + "/Gallery/film").toStdString());
        QVERIFY(QString::fromStdString(galleryFolder("/x/y/film.montage")).endsWith("/x/y/Gallery/film"));
        QVERIFY(!galleryFolder("").empty());
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64;
        s.height = 36;
        Clip red = colorClip(p, 1, 0, 0, 0, 30);
        red.name = "Red";
        red.effects = {makeEffect(p, "invert"), makeEffect(p, "gaussian_blur")};
        Clip blue = colorClip(p, 0, 0, 1, 30, 30);
        blue.effects = {makeEffect(p, "gaussian_blur")};
        Clip green = colorClip(p, 0, 1, 0, 60, 30);
        edit::overwrite(p, s, {TrackKind::Video, 0}, red);
        edit::overwrite(p, s, {TrackKind::Video, 0}, blue);
        edit::overwrite(p, s, {TrackKind::Video, 0}, green);

        // Grabbed: the picture as the viewer shows it, the grade (the invert, first among the clip's effects).
        GalleryStill one;
        std::string err;
        QVERIFY2(grabStill(p, s, 5, folder, kDefaultAlbum, one, &err), err.c_str());
        QCOMPARE(one.label, std::string("1"));
        QCOMPARE(one.clip, std::string("Red"));
        QCOMPARE(one.effects.size(), size_t(1));
        QCOMPARE(one.effects[0].type, std::string("invert"));
        QVERIFY(one.anchors == std::vector<int>{0});
        const QImage picture(QString::fromStdString(one.picture));
        QCOMPARE(picture.size(), QSize(64, 36));
        const QColor cyan = picture.pixelColor(32, 18);
        QVERIFY(cyan.red() < 30 && cyan.green() > 220 && cyan.blue() > 220);
        GalleryStill two;
        QVERIFY(grabStill(p, s, 40, folder, kDefaultAlbum, two, &err));
        QCOMPARE(two.label, std::string("2"));
        QVERIFY(two.effects.empty());
        QVERIFY(!grabStill(p, s, 200, folder, kDefaultAlbum, two, &err) && !err.empty());  // nothing there
        std::vector<GalleryStill> stills = galleryStills(folder, kDefaultAlbum);
        QCOMPARE(stills.size(), size_t(2));
        QCOMPARE(stills[0].id, one.id);
        GalleryStill found;
        QVERIFY(findStill(folder, kDefaultAlbum, "1", found) && found.id == one.id && found.effects.size() == 1);

        // Applied to blue: the invert in place of its (empty) grade, before its blur; blue inverted is yellow.
        QVERIFY(edit::applyStillGrade(p, s, found, {blue.id}).ok);
        const Clip* b = edit::clipById(s, blue.id);
        QCOMPARE(b->effects.size(), size_t(2));
        QCOMPARE(b->effects[0].type, std::string("invert"));
        QVERIFY(b->effects[0].id != red.effects[0].id);
        RenderOptions o;
        float c[4];
        rgb(renderSequenceFrame(p, s, 40, o), 32, 18, c);
        QVERIFY(c[0] > 0.9f && c[1] > 0.9f && c[2] < 0.1f);
        // The second still (no grade) takes it off again, the blur kept.
        QVERIFY(findStill(folder, kDefaultAlbum, "2", found));
        QVERIFY(edit::applyStillGrade(p, s, found, {blue.id}).ok);
        QCOMPARE(edit::clipById(s, blue.id)->effects.size(), size_t(1));
        QCOMPARE(edit::clipById(s, blue.id)->effects[0].type, std::string("gaussian_blur"));

        // A node grade goes with its graph.
        QVERIFY(edit::convertToNodes(s, red.id).ok);
        QVERIFY(edit::addGradeNode(p, s, red.id, edit::NodeHow::Serial).ok);
        GalleryStill nodes;
        QVERIFY(grabStill(p, s, 5, folder, kDefaultAlbum, nodes, &err));
        QCOMPARE(nodes.graph.nodes.size(), edit::clipById(s, red.id)->grade.nodes.size());
        QVERIFY(findStill(folder, kDefaultAlbum, nodes.id, found));
        QVERIFY(found.graph == nodes.graph);
        QVERIFY(edit::applyStillGrade(p, s, found, {green.id}).ok);
        const Clip* g = edit::clipById(s, green.id);
        QVERIFY(g->grade == edit::clipById(s, red.id)->grade);
        QVERIFY(std::any_of(g->effects.begin(), g->effects.end(), [](const Effect& e) { return e.type == "invert" && e.node != 0; }));
        rgb(renderSequenceFrame(p, s, 70, o), 32, 18, c);
        QVERIFY(c[0] > 0.9f && c[1] < 0.1f && c[2] > 0.9f);  // green inverted: magenta
        QVERIFY(!edit::applyStillGrade(p, s, found, {}).ok);

        // A shared node goes as the still has it: not one of the same id here that does something else (another
        // project's ids could be anything's here), and one copy for all the clips it goes on, reused when applied again.
        GalleryStill shared = found;
        shared.sharedNodes = {SharedNode{9000, "Look", {makeEffect(p, "invert")}}};
        shared.graph.nodes.front().shared = 9000;
        s.sharedNodes.push_back(SharedNode{9000, "Skin", {makeEffect(p, "gaussian_blur")}});
        QVERIFY(edit::applyStillGrade(p, s, shared, {blue.id, green.id}).ok);
        const Id lookNode = edit::clipById(s, blue.id)->grade.nodes.front().shared;
        QVERIFY(lookNode && lookNode != 9000);
        QCOMPARE(edit::clipById(s, green.id)->grade.nodes.front().shared, lookNode);
        QCOMPARE(findSharedNode(s, lookNode)->name, std::string("Look"));
        QCOMPARE(findSharedNode(s, 9000)->name, std::string("Skin"));
        const size_t nodeCount = s.sharedNodes.size();
        QVERIFY(edit::applyStillGrade(p, s, shared, {red.id}).ok);
        QCOMPARE(s.sharedNodes.size(), nodeCount);
        QCOMPARE(edit::clipById(s, red.id)->grade.nodes.front().shared, lookNode);

        // Albums, renaming, removing.
        QVERIFY(addAlbum(folder, "Looks"));
        QVERIFY(!addAlbum(folder, "a/b"));
        QVERIFY(!addAlbum(folder, ".."));
        const std::vector<std::string> albums = galleryAlbums(folder);
        QCOMPARE(albums.front(), std::string(kDefaultAlbum));
        QVERIFY(std::find(albums.begin(), albums.end(), "Looks") != albums.end());
        GalleryStill look;
        QVERIFY(grabStill(p, s, 5, folder, "Looks", look, &err));
        QCOMPARE(look.label, std::string("1"));
        QVERIFY(galleryStills(folder, "../Stills").empty());
        GalleryStill other;
        QVERIFY(grabStill(p, s, 40, folder, "Looks", other, &err));
        QVERIFY(renameStill(folder, look, "Night"));
        QVERIFY(findStill(folder, "Looks", "Night", found) && found.id == look.id);
        QVERIFY(!renameStill(folder, other, "Night", &err) && !err.empty());  // a label names one still
        QVERIFY(findStill(folder, "Looks", other.id, found) && found.label == "2");
        // A picture is the one beside its JSON, whatever the JSON says.
        {
            const QString json = QString::fromStdString(folder) + "/Looks/" + QString::fromStdString(other.id) + ".json";
            QFile f(json);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            o["picture"] = "../../outside.png";
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(QJsonDocument(o).toJson());
            f.close();
            QVERIFY(findStill(folder, "Looks", other.id, found));
            QCOMPARE(QFileInfo(QString::fromStdString(found.picture)).absolutePath(), QFileInfo(json).absolutePath());
            QVERIFY(removeStill(folder, found));
        }
        QVERIFY(removeStill(folder, look));
        QVERIFY(galleryStills(folder, "Looks").empty());
        QVERIFY(!QFileInfo::exists(QString::fromStdString(look.picture)));
        // Under a track matte the still is of the shot the matte cuts out, not of the hidden matte above it.
        {
            Clip fill = colorClip(p, 1, 0, 0, 0, 30);
            fill.name = "Fill";
            fill.effects = {makeEffect(p, "track_matte")};
            QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 1}, fill).ok);
            Clip matte = colorClip(p, 1, 1, 1, 0, 30);
            matte.name = "Matte";
            QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 2}, matte).ok);
            GalleryStill shot;
            QVERIFY2(grabStill(p, s, 5, folder, kDefaultAlbum, shot, &err), err.c_str());
            QCOMPARE(shot.clip, std::string("Fill"));
        }
    }

    void trimPasses() {
        // A PQ master at 4000 nits: two shots, a title over the first.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 32;
        s.height = 18;
        s.colorSpace = "rec2100pq";
        s.hdrPeakNits = 4000;
        Clip a = colorClip(p, 0.5f, 0.5f, 0.5f, 0, 30);
        a.name = "A";
        Clip b = colorClip(p, 0.5f, 0.5f, 0.5f, 30, 30);
        b.name = "B";
        edit::overwrite(p, s, {TrackKind::Video, 0}, a);
        edit::overwrite(p, s, {TrackKind::Video, 0}, b);
        Clip title = makeGeneratorClip(p, "title", 30);
        edit::overwrite(p, s, {TrackKind::Video, 1}, title);
        QCOMPARE(trimShot(s, 10)->id, a.id);  // (the title is not a shot)
        QCOMPARE(trimShot(s, 40)->id, b.id);
        QVERIFY(!trimShot(s, 90));

        // Targets: presets, one of its own, ids kept unique; a camera log space is not a display.
        for (const char* id : {"sdr", "hdr10-600", "sdr"}) {
            const auto& all = trimTargetPresets();
            QVERIFY(edit::addTrimTarget(s, *std::find_if(all.begin(), all.end(), [&](const TrimTarget& t) { return t.id == id; })).ok);
        }
        QVERIFY(edit::addTrimTarget(s, TrimTarget{"", "", "rec2100pq", 400}).ok);
        QVERIFY(!edit::addTrimTarget(s, TrimTarget{"", "", "slog3-sgamut3cine", 100}).ok);
        QCOMPARE(s.trimTargets.size(), size_t(4));
        QCOMPARE(s.trimTargets[2].id, std::string("sdr-2"));
        QCOMPARE(s.trimTargets[3].id, std::string("rec2100pq-400"));
        QCOMPARE(trimTargetPeak(s, "hdr10-600"), 600.0);
        QCOMPARE(trimTargetSpace(s, "sdr").id, std::string("rec709"));
        QCOMPARE(trimTargetSpace(s, "").id, std::string("rec2100pq"));

        // 600 nits: 2000-nit light rolled off under the peak, 100 nits left as it is.
        const ColorSpace& pq = *findColorSpace("rec2100pq");
        auto frame = [&](double nits) {
            Image img(4, 2);
            for (int i = 0; i < 8; ++i) {
                float* px = img.px.data() + i * 4;
                px[0] = px[1] = px[2] = float(nitsToCode(pq, i < 4 ? nits : 100));
                px[3] = 1;
            }
            return img;
        };
        Image hdr = frame(2000);
        applyTrimPass(hdr, s, 10, *findTrimTarget(s, "hdr10-600"));
        const double bright = codeToNits(pq, hdr.at(0, 0)[0]), mid = codeToNits(pq, hdr.at(0, 1)[0]);
        QVERIFY2(bright > 400 && bright <= 601, qPrintable(QString::number(bright)));
        QVERIFY2(std::fabs(mid - 100) < 1, qPrintable(QString::number(mid)));
        Image peakImg = frame(4000);
        rollOffPq(peakImg, 4000, 1000);
        QVERIFY(std::fabs(codeToNits(pq, peakImg.at(0, 0)[0]) - 1000) < 15);

        // SDR: the master tone mapped into Rec.709; then shot A's trim (half the gain), not shot B's.
        auto sdrAt = [&](FrameTime t) {
            Image img = frame(100);
            applyTrimPass(img, s, t, *findTrimTarget(s, "sdr"));
            return img.at(0, 1)[0];
        };
        const float plain = sdrAt(10);
        QVERIFY(plain > 0.6f && plain < 0.85f);  // 100 nits under BT.2408's 203-nit white
        QVERIFY(edit::setTrim(s, a.id, "sdr", {{"gain", 0.5}}, 10).ok);
        QVERIFY(!edit::setTrim(s, a.id, "sdr", {{"exposure", 1}}, 10).ok);
        QVERIFY(!edit::setTrim(s, a.id, "nope", {{"gain", 1}}, 10).ok);
        QVERIFY(std::fabs(sdrAt(10) - plain * 0.5f) < 0.01f);
        QVERIFY(std::fabs(sdrAt(40) - plain) < 1e-4f);
        // Saturation and lift do what they say on a coloured pixel.
        {
            Image img(1, 1);
            float* px = img.at(0, 0);
            px[0] = float(nitsToCode(pq, 150)), px[1] = px[2] = float(nitsToCode(pq, 20)), px[3] = 1;
            Image grey = img;
            QVERIFY(edit::setTrim(s, b.id, "sdr", {{"saturation", 0.0}}, 40).ok);
            applyTrimPass(grey, s, 40, *findTrimTarget(s, "sdr"));
            QVERIFY(std::fabs(grey.at(0, 0)[0] - grey.at(0, 0)[2]) < 1e-4f);
        }
        // Keyframed: set at the playhead once the control has keys.
        Clip* ca = edit::clipById(s, a.id);
        ca->trims["sdr"].params["gain"].addKey(0, 1.0);
        ca->trims["sdr"].params["gain"].addKey(20, 0.5);
        QVERIFY(std::fabs(sdrAt(0) - plain) < 0.01f);
        QVERIFY(edit::setTrim(s, a.id, "sdr", {{"gain", 0.25}}, 10).ok);
        QCOMPARE(edit::clipById(s, a.id)->trims.at("sdr").params.at("gain").keys.size(), size_t(3));
        QVERIFY(std::fabs(sdrAt(10) - plain * 0.25f) < 0.01f);
        // A split keeps each half's keys on the same frames of the cut.
        QVERIFY(edit::razor(p, s, {TrackKind::Video, 0}, 15).ok);
        QVERIFY(std::fabs(sdrAt(18) - plain * float(0.25 + 0.25 * 8 / 10)) < 0.01f);

        // The program as a target shows it: the viewer's frame and the frame key.
        RenderOptions o;
        o.trimTarget = "sdr";
        const Image shown = renderProgramFrame(p, s, 45, o);
        QVERIFY(shown.at(16, 9)[0] > 0 && shown.at(16, 9)[0] < 1);
        RenderOptions master;
        QVERIFY(frameKey(p, s, 45, o) != frameKey(p, s, 45, master));

        // Paste and Remove Attributes carry the trims; Reset and Copy too; a removed target takes its trims.
        QVERIFY(edit::pasteAttributes(p, s, *edit::clipById(s, b.id), TrackKind::Video, {a.id}, edit::AttrTrims).ok);
        QVERIFY(edit::clipById(s, a.id)->trims == edit::clipById(s, b.id)->trims);
        QVERIFY(edit::removeAttributes(p, s, {a.id}, edit::AttrTrims).ok);
        QVERIFY(edit::clipById(s, a.id)->trims.empty());
        QVERIFY(edit::copyTrim(s, b.id, {a.id}, "sdr").ok);
        QVERIFY(edit::clipById(s, a.id)->trims.count("sdr"));
        QVERIFY(edit::resetTrims(s, {a.id}, "sdr").ok);
        QVERIFY(!edit::clipById(s, a.id)->trims.count("sdr"));
        QVERIFY(edit::setTrim(s, a.id, "hdr10-600", {{"highlights", 0.5}}, 5).ok);

        // Saved and read back.
        QTemporaryDir dir;
        const std::string file = dir.path().toStdString() + "/trims.montage";
        QVERIFY(saveProject(p, file));
        Project back;
        QVERIFY(loadProject(file, back));
        QVERIFY(back.active()->trimTargets == s.trimTargets);
        QVERIFY(edit::clipById(*back.active(), b.id)->trims == edit::clipById(s, b.id)->trims);

        // Delivered: the SDR pass as Rec.709 PNG frames, darker where its trim says.
        ExportSettings st;
        st.path = dir.path().toStdString() + "/pass.png";
        st.videoCodec = "png";
        st.audioCodec = "none";
        st.in = 40;
        st.out = 41;
        st.trimTarget = "sdr";
        std::string err;
        QVERIFY2(exportSequence(p, s, st, nullptr, nullptr, &err), err.c_str());
        const QStringList written = QDir(dir.path()).entryList({"pass_*.png"});
        QCOMPARE(written.size(), 1);
        const QImage pass(dir.path() + "/" + written.first());
        QVERIFY(!pass.isNull());
        const QColor c = pass.pixelColor(16, 9);
        QVERIFY2(std::abs(c.red() - c.blue()) <= 2, "saturation 0: grey");
        st.trimTarget = "nope";
        QVERIFY(!exportSequence(p, s, st, nullptr, nullptr, &err) && err.find("trim target") != std::string::npos);
        st.trimTarget = "sdr";
        st.videoCodec = "gif";
        st.path = dir.path().toStdString() + "/pass.gif";
        QVERIFY(!exportSequence(p, s, st, nullptr, nullptr, &err));

        // Pasting from a clip with no trims leaves the clips' own.
        const Id ta = edit::clipById(s, a.id)->id;
        const size_t had = edit::clipById(s, ta)->trims.size();
        Clip plainClip = colorClip(p, 0, 0, 0, 0, 5);
        QVERIFY(had > 0);
        edit::pasteAttributes(p, s, plainClip, TrackKind::Video, {ta}, edit::AttrTrims);
        QCOMPARE(edit::clipById(s, ta)->trims.size(), had);
        // Keyframes made by setting with `keyframe`: the first keeps the value at the clip's start.
        QVERIFY(edit::setTrim(s, b.id, "sdr", {{"lift", 0.2}}, 50, true).ok);
        const Param& lift = edit::clipById(s, b.id)->trims.at("sdr").params.at("lift");
        QCOMPARE(lift.keys.size(), size_t(2));
        QCOMPARE(lift.at(0), 0.0);
        QCOMPARE(lift.at(20), 0.2);

        // A track matte's hidden source is not the shot: the clip it keys is.
        {
            Sequence m = s;
            Clip under = colorClip(p, 1, 0, 0, 100, 10);
            under.effects.push_back(makeEffect(p, "track_matte"));
            Clip matte = colorClip(p, 1, 1, 1, 100, 10);
            edit::overwrite(p, m, {TrackKind::Video, 0}, under);
            edit::overwrite(p, m, {TrackKind::Video, 1}, matte);  // (the track above: its matte by default)
            QVERIFY(hiddenMatteTracks(m, 105).at(1));
            QCOMPARE(trimShot(m, 105)->id, under.id);
        }

        // An HLG master on a 300-nit PQ display: its highlights rolled off, not clipped (two levels stay two).
        {
            Sequence h = s;
            h.colorSpace = "rec2100hlg";
            const ColorSpace& hlg = *findColorSpace("rec2100hlg");
            Image img(2, 1);
            for (int i = 0; i < 2; ++i) {
                float* px = img.at(i, 0);
                px[0] = px[1] = px[2] = float(nitsToCode(hlg, i ? 1000 : 500));
                px[3] = 1;
            }
            QVERIFY(edit::addTrimTarget(h, *std::find_if(trimTargetPresets().begin(), trimTargetPresets().end(),
                                                         [](const TrimTarget& x) { return x.id == "hdr10-300"; })).ok);
            applyTrimPass(img, h, 90, *findTrimTarget(h, "hdr10-300"));
            const double lo = codeToNits(pq, img.at(0, 0)[0]), hi = codeToNits(pq, img.at(1, 0)[0]);
            QVERIFY2(hi <= 301 && hi > lo + 10 && lo > 150, qPrintable(QStringLiteral("%1 %2").arg(lo).arg(hi)));
        }

        // Removing a target takes its trims away.
        QVERIFY(edit::removeTrimTarget(s, "hdr10-600").ok);
        QVERIFY(!edit::clipById(s, a.id)->trims.count("hdr10-600"));
        QVERIFY(!edit::removeTrimTarget(s, "hdr10-600").ok);
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

    void graphicsTemplateValues() {
        // A graphic (a white title in a sequence of its own) drawn in another sequence with its own text and colour.
        Project p = makeDefaultProject();
        Sequence& outer = *p.active();
        outer.width = 160, outer.height = 90;
        Sequence g;
        g.id = p.newId();
        g.width = 160, g.height = 90, g.fps = outer.fps;
        g.videoTracks.resize(1);
        g.videoTracks[0].id = p.newId();
        Clip title;
        title.id = p.newId();
        title.duration = 30;
        title.generator = makeEffect(p, "title");
        title.generator.strings["text"] = "IIII";
        title.generator.params["size"] = Param(60);
        title.generator.params["shadow"] = Param(0);
        g.videoTracks[0].clips.push_back(title);
        QVERIFY(publishField(g, *fieldFor(g, title.id, 0, "text", "Name")).ok);
        QVERIFY(publishField(g, *fieldFor(g, title.id, 0, "color", "Colour")).ok);
        MediaItem gm;
        gm.id = p.newId();
        gm.kind = MediaKind::Sequence;
        gm.sequenceId = g.id;
        gm.hasVideo = true;
        gm.width = 160, gm.height = 90;
        p.media.push_back(gm);
        p.sequences.push_back(g);
        Sequence& s = *p.active();
        const edit::Result placed = edit::placeMedia(p, s, gm.id, 0, 0, -1, {TrackKind::Video, 0}, {}, false);
        QVERIFY(placed.ok);
        RenderOptions o;
        auto inkOf = [&](double& redness) {
            const Image img = renderSequenceFrame(p, *p.active(), 5, o);
            double ink = 0;
            redness = 0;
            for (int y = 0; y < img.height; ++y)
                for (int x = 0; x < img.width; ++x) {
                    const float* px = img.at(x, y);
                    if (px[3] < 0.5f) continue;
                    ink += 1;
                    redness = std::max(redness, double(px[0] - px[1]));
                }
            return ink;
        };
        double red = 0;
        const double white = inkOf(red);
        QVERIFY2(white > 100 && red < 0.1, qPrintable(QString("%1 %2").arg(white).arg(red)));
        edit::clipById(*p.active(), placed.created[0])->fields = {{"colour", "#ff0000"}};
        const double coloured = inkOf(red);
        QVERIFY2(red > 0.8 && std::abs(coloured - white) < white * 0.05, qPrintable(QString("%1 %2").arg(coloured).arg(red)));
        edit::clipById(*p.active(), placed.created[0])->fields = {{"colour", "#ff0000"}, {"name", "I"}};
        const double shorter = inkOf(red);
        QVERIFY2(shorter < white * 0.4 && shorter > 0, qPrintable(QString("%1 of %2").arg(shorter).arg(white)));
        // The template itself is unchanged.
        QCOMPARE(p.findSequence(g.id)->videoTracks[0].clips[0].generator.s("text"), std::string("IIII"));
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
        // An object mask's names don't show in its frames: renaming an object keeps their keys; a cut-out changes them.
        Effect masked = makeEffect(p, "invert");
        masked.params["mask.shape"] = Param(3.0);
        auto objects = std::make_shared<ObjectMask>();
        objects->fps = 25;
        objects->more.emplace_back();
        masked.object = objects;
        blue.effects.push_back(masked);
        const QByteArray withMask = frameKey(p, s, 40, o);
        auto renamed = std::make_shared<ObjectMask>(*objects);
        renamed->name = "Car";
        renamed->more[0].name = "Driver";
        blue.effects.back().object = renamed;
        QCOMPARE(frameKey(p, s, 40, o), withMask);
        auto cut = std::make_shared<ObjectMask>(*objects);
        cut->more[0].subtract = true;
        blue.effects.back().object = cut;
        QVERIFY(frameKey(p, s, 40, o) != withMask);
        blue.effects.clear();
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

    void exportsImfMaster() {
        if (!openJpegAvailable()) QSKIP("Built without OpenJPEG 2.5");
        QTemporaryDir dir;
        // A second of a colour matte at 23.976 with a 1 kHz tone, as an IMF Application #2E master.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320, s.height = 180, s.fps = Rational{24000, 1001};
        s.videoTracks[0].clips.push_back(colorClip(p, 0.5f, 0.25f, 0.75f, 0, 24));
        {
            const QString wav = dir.filePath("tone.wav");
            QFile f(wav);
            QVERIFY(f.open(QIODevice::WriteOnly));
            const int n = 48000;
            QByteArray d;
            auto u32 = [&](uint32_t v) { d.append(reinterpret_cast<const char*>(&v), 4); };
            auto u16 = [&](uint16_t v) { d.append(reinterpret_cast<const char*>(&v), 2); };
            d.append("RIFF");
            u32(36 + n * 2);
            d.append("WAVEfmt ");
            u32(16), u16(1), u16(1), u32(48000), u32(96000), u16(2), u16(16);
            d.append("data");
            u32(n * 2);
            for (int i = 0; i < n; ++i) u16(uint16_t(int16_t(std::lround(0.5 * 32767 * std::sin(2 * M_PI * 1000 * i / 48000.0)))));
            f.write(d);
            f.close();
            MediaItem m;
            m.id = p.newId();
            QVERIFY(probeMedia(wav.toStdString(), m));
            p.media.push_back(m);
            Clip a = makeClip(p, m, TrackKind::Audio, s);
            a.duration = 24;
            s.audioTracks[0].clips.push_back(a);
        }
        ImfSettings st;
        st.title = "Test master: one";
        st.threads = 2;
        ImfResult r;
        std::string err;
        QVERIFY2(exportImf(p, s, st, dir.path().toStdString(), &r, {}, &err), err.c_str());
        QVERIFY(r.frames == 24 && r.rateNum == 24000 && r.rateDen == 1001 && r.width == 320 && r.height == 180);
        QVERIFY(r.bits == 10 && r.channels == 2 && r.colour == "rec709");
        QCOMPARE(int(r.rsiz), 0x0701);  // the reversible 2K IMF profile, main level 1
        const QDir out(QString::fromStdString(r.folder));
        QVERIFY2(out.dirName().startsWith("Test_master_one_IMF_"), qPrintable(out.dirName()));
        const QStringList videos = out.entryList({"VIDEO_*.mxf"}), audios = out.entryList({"AUDIO_*.mxf"}), cpls = out.entryList({"CPL_*.xml"});
        QVERIFY(videos.size() == 1 && audios.size() == 1 && cpls.size() == 1 && out.entryList({"PKL_*.xml"}).size() == 1 && out.exists("ASSETMAP.xml"));
        std::vector<std::string> issues = verifyImf(r.folder);
        QVERIFY2(issues.empty(), issues.empty() ? "" : issues.front().c_str());
        // The composition: Application #2E, the rate, both resources with their essence descriptors (as the files'), the
        // picture's profile, colour and component layout, the sound's stereo labels.
        QFile cplFile(out.filePath(cpls[0]));
        QVERIFY(cplFile.open(QIODevice::ReadOnly));
        const QString cpl = QString::fromUtf8(cplFile.readAll());
        for (const char* want : {"http://www.smpte-ra.org/ns/2067-21/2021</cc:ApplicationIdentification>", "<EditRate>24000 1001</EditRate>",
                                 "<IntrinsicDuration>24</IntrinsicDuration>", "<IntrinsicDuration>48048</IntrinsicDuration>",
                                 "<EditRate>48000 1</EditRate>", "<r1:Rsiz>1793</r1:Rsiz>", "<r2:Code>CompRed</r2:Code>",
                                 "<r1:ComponentMaxRef>1023</r1:ComponentMaxRef>", "060e2b34.04010106.04010101.03030000",
                                 "<r1:PictureCompression>urn:smpte:ul:060e2b34.0401010d.04010202.03010502</r1:PictureCompression>",
                                 "<r1:MCATagSymbol>sgST</r1:MCATagSymbol>", "<r1:MCATagSymbol>chR</r1:MCATagSymbol>",
                                 ">feature</ContentKind>", "<ContentTitle language=\"en\">Test master: one</ContentTitle>"})
            QVERIFY2(cpl.contains(want), want);
        QVERIFY(!cpl.contains("MasteringDisplay"));
        // The picture, lossless: the matte's own codes in the middle of frame 5.
        std::vector<uint16_t> rgb;
        int w = 0, h = 0, bits = 0;
        QVERIFY2(readImfFrame(out.filePath(videos[0]).toStdString(), 5, rgb, w, h, bits, &err), err.c_str());
        QVERIFY(w == 320 && h == 180 && bits == 10);
        const uint16_t* mid = &rgb[(size_t(90) * 320 + 160) * 3];
        QVERIFY2(std::abs(mid[0] - 512) <= 1 && std::abs(mid[1] - 256) <= 1 && std::abs(mid[2] - 767) <= 1,
                 qPrintable(QString("%1 %2 %3").arg(mid[0]).arg(mid[1]).arg(mid[2])));
        // The sound: as many samples as the frames last at 23.976 (48048), the tone on both sides.
        AudioBufferPtr tone = decodeAudio(out.filePath(audios[0]).toStdString(), 48000, &err);
        QVERIFY2(tone, err.c_str());
        QVERIFY2(std::abs(tone->frames() - 48048) < 4, qPrintable(QString::number(tone->frames())));
        int crossings = 0;
        for (int64_t i = 1; i < 24000; ++i) crossings += (tone->samples[size_t(i - 1) * 2] < 0) != (tone->samples[size_t(i) * 2] < 0);
        QVERIFY2(std::abs(crossings - 1000) < 10, qPrintable(QString::number(crossings)));
        // A damaged file is caught; the same package again is refused; stopping leaves nothing.
        {
            QFile f(out.filePath(audios[0]));
            QVERIFY(f.open(QIODevice::Append));
            f.write("x");
        }
        issues = verifyImf(r.folder);
        QVERIFY(std::any_of(issues.begin(), issues.end(), [](const std::string& i) { return i.find("hash") != std::string::npos; }));
        QVERIFY(!exportImf(p, s, st, dir.path().toStdString(), nullptr, {}, &err) && err.find("already") != std::string::npos);
        st.title = "Stopped";
        int calls = 0;
        QVERIFY(!exportImf(p, s, st, dir.path().toStdString(), nullptr, [&](double) { return ++calls < 3; }, &err));
        QVERIFY(QDir(dir.path()).entryList({"Stopped_*"}, QDir::Dirs).isEmpty());

        // HDR: P3-D65 PQ at 12 bits with the mastering display, no coding equations; lossy within its cap at main level
        // 1, sub level 1; a 5.1 mix with its labels.
        st.title = "HDR";
        st.colour = "p3d65-pq";
        st.lossless = false;
        st.megabitsPerSecond = 50;
        st.masteringPeak = 4000;
        Sequence surround = s;
        surround.audioLayout = "5.1";
        ImfResult hdr;
        QVERIFY2(exportImf(p, surround, st, dir.path().toStdString(), &hdr, {}, &err), err.c_str());
        QVERIFY(hdr.bits == 12 && hdr.channels == 6 && hdr.colour == "p3d65-pq");
        QCOMPARE(int(hdr.rsiz), 0x0411);
        issues = verifyImf(hdr.folder);
        QVERIFY2(issues.empty(), issues.empty() ? "" : issues.front().c_str());
        const QDir hout(QString::fromStdString(hdr.folder));
        QFile hcpl(hout.filePath(hout.entryList({"CPL_*.xml"}).value(0)));
        QVERIFY(hcpl.open(QIODevice::ReadOnly));
        const QString htext = QString::fromUtf8(hcpl.readAll());
        for (const char* want : {"<r1:MasteringDisplayMaximumLuminance>40000000</r1:MasteringDisplayMaximumLuminance>", "<r2:X>34000</r2:X>",
                                 "060e2b34.0401010d.04010101.010a0000", "060e2b34.0401010d.04010101.03060000", "<r1:MCATagSymbol>sg51</r1:MCATagSymbol>",
                                 "<r1:MCATagSymbol>chLFE</r1:MCATagSymbol>", "<r1:ComponentMaxRef>4095</r1:ComponentMaxRef>"})
            QVERIFY2(htext.contains(want), want);
        QVERIFY(!htext.contains("CodingEquations"));
        QVERIFY(readImfFrame(hout.filePath(hout.entryList({"VIDEO_*.mxf"}).value(0)).toStdString(), 0, rgb, w, h, bits, &err) && bits == 12);
        mid = &rgb[(size_t(90) * 320 + 160) * 3];
        QVERIFY(mid[0] > mid[1] && mid[2] > mid[0] && mid[2] < 4095);  // the matte in PQ, its order kept
        // Refused: an unknown colour, a frame rate Application #2E does not take, a picture larger than 4096 x 3112.
        st.colour = "sepia";
        QVERIFY(!exportImf(p, s, st, dir.path().toStdString(), nullptr, {}, &err) && err.find("colour") != std::string::npos);
        st.colour.clear();
        Sequence odd = s;
        odd.fps = Rational{48, 1};
        QVERIFY(!imfFrameRateAllowed(odd) && !exportImf(p, odd, st, dir.path().toStdString(), nullptr, {}, &err));
        odd.fps = Rational{60000, 1001};
        QVERIFY(imfFrameRateAllowed(odd));
        int iw = 0, ih = 0;
        Sequence big = s;
        big.width = 7680, big.height = 4320;
        QVERIFY(!imfPictureSize(big, "sequence", iw, ih, &err) && imfPictureSize(big, "uhd", iw, ih) && iw == 3840 && ih == 2160);
        QCOMPARE(defaultImfColour(s), std::string("rec709"));
        big.colorSpace = "rec2100pq";
        QCOMPARE(defaultImfColour(big), std::string("rec2020-pq"));
        // The IMF profiles and levels: 4K above 2048 x 1556, main levels by samples per second, sub levels by bit rate.
        QCOMPARE(int(imfRsiz(3840, 2160, 3, 24000.0 / 1001, true)), 0x0806);
        QCOMPARE(int(imfRsiz(1920, 1080, 3, 24, true)), 0x0703);
        QCOMPARE(int(imfRsiz(3840, 2160, 3, 24, false, 400)), 0x0526);
        QCOMPARE(int(imfRsiz(16384, 8640, 3, 24, true)), 0);
        uint8_t ul[16];
        imfPictureCoding(0x0806, ul);
        QVERIFY(ul[14] == 0x06 && ul[15] == 0x0f);
        imfPictureCoding(0x0526, ul);
        QVERIFY(ul[14] == 0x03 && ul[15] == 0x11);
        // Above 6400 Mbit/s the levels go past main level 8, and the label with them.
        QCOMPARE(int(imfRsiz(3840, 2160, 3, 24, false, 12800)), 0x0579);
        imfPictureCoding(0x0579, ul);
        QVERIFY(ul[14] == 0x03 && ul[15] == 0x28);
        // 120 fps only above HD, judged on the picture written; a rate as the sequence happens to store it is written
        // reduced.
        QVERIFY(!imfFrameRateAllowed(Rational{120, 1}, 1920, 1080) && imfFrameRateAllowed(Rational{120, 1}, 3840, 2160));
        Rational canonical;
        QVERIFY(imfFrameRateAllowed(Rational{48000, 2002}, 320, 180, &canonical) && canonical == (Rational{24000, 1001}));
        {
            Sequence oddRate = s;
            oddRate.fps = Rational{48000, 2002};
            st = ImfSettings{};
            st.title = "Reduced";
            st.threads = 1;
            ImfResult rr;
            QVERIFY2(exportImf(p, oddRate, st, dir.path().toStdString(), &rr, {}, &err), err.c_str());
            QVERIFY(rr.rateNum == 24000 && rr.rateDen == 1001);
        }
        // At 29.97 the sound fills whole frames only every five: seven frames are padded to ten, with 16016 samples.
        {
            Sequence ntsc = s;
            ntsc.fps = Rational{30000, 1001};
            ntsc.videoTracks[0].clips = {colorClip(p, 0.5f, 0.25f, 0.75f, 0, 7)};
            ntsc.audioTracks[0].clips.clear();
            st = ImfSettings{};
            st.title = "NTSC";
            st.threads = 1;
            ImfResult nr;
            QVERIFY2(exportImf(p, ntsc, st, dir.path().toStdString(), &nr, {}, &err), err.c_str());
            QCOMPARE(nr.frames, int64_t(10));
            issues = verifyImf(nr.folder);
            QVERIFY2(issues.empty(), issues.empty() ? "" : issues.front().c_str());
            const QDir nout(QString::fromStdString(nr.folder));
            QFile ncpl(nout.filePath(nout.entryList({"CPL_*.xml"}).value(0)));
            QVERIFY(ncpl.open(QIODevice::ReadOnly));
            const QString ntext = QString::fromUtf8(ncpl.readAll());
            QVERIFY(ntext.contains("<IntrinsicDuration>10</IntrinsicDuration>") && ntext.contains("<IntrinsicDuration>16016</IntrinsicDuration>"));
            // The padding is black.
            QVERIFY(readImfFrame(nout.filePath(nout.entryList({"VIDEO_*.mxf"}).value(0)).toStdString(), 8, rgb, w, h, bits, &err));
            QVERIFY(rgb[(size_t(90) * 320 + 160) * 3 + 2] == 0);
        }
        // A frame of 16 MiB or more (lossless UHD can be) gets the 8-byte length, and the file still walks as KLV to its end.
        {
            std::vector<uint16_t> grey(16 * 16 * 3, 512);
            std::vector<uint8_t> cs;
            QVERIFY(encodeJpeg2000(grey.data(), 16, 16, 10, 0x0701, 0, cs, &err));
            std::vector<uint8_t> huge = cs;
            huge.resize(cs.size() + (size_t(17) << 20), 0);
            const QString path = dir.filePath("huge.mxf");
            dcp::ImfPictureWriter wr;
            QVERIFY(wr.open(path.toStdString(), dcp::newUuid(), dcp::EditRate{24, 1}, 10, dcp::ImfColour{}, 16, 9, &err));
            QVERIFY(wr.write(cs.data(), cs.size(), &err) && wr.write(huge.data(), huge.size(), &err));
            QVERIFY2(wr.close(&err), err.c_str());
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray all = f.readAll();
            const auto* d = reinterpret_cast<const uint8_t*>(all.constData());
            const uint8_t element[12] = {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x02, 0x01, 0x01, 0x0d, 0x01, 0x03, 0x01};
            std::vector<uint64_t> frames;
            size_t at = 0;
            while (at + 17 <= size_t(all.size())) {
                const uint8_t* key = d + at;
                size_t pos = at + 16;
                uint64_t len = d[pos++];
                if (len & 0x80) {
                    const int n = int(len & 0x7f);
                    len = 0;
                    for (int i = 0; i < n; ++i) len = (len << 8) | d[pos++];
                }
                if (std::memcmp(key, element, 12) == 0 && key[12] == 0x15) frames.push_back(len);
                at = pos + size_t(len);
            }
            QCOMPARE(at, size_t(all.size()));
            QCOMPARE(frames.size(), size_t(2));
            QCOMPARE(frames[1], uint64_t(huge.size()));
        }
    }

    void exportsDigitalCinemaPackage() {
        QTemporaryDir dir;
        // A second of a mid-grey matte (Rec.709, 16:9) with a 1 kHz tone, as a 2K Scope DCP.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64, s.height = 36, s.fps = Rational{24, 1};
        s.videoTracks[0].clips.push_back(colorClip(p, 0.5f, 0.5f, 0.5f, 0, 24));
        {
            const QString wav = dir.filePath("tone.wav");
            QFile f(wav);
            QVERIFY(f.open(QIODevice::WriteOnly));
            const int n = 48000;
            QByteArray d;
            auto u32 = [&](uint32_t v) { d.append(reinterpret_cast<const char*>(&v), 4); };
            auto u16 = [&](uint16_t v) { d.append(reinterpret_cast<const char*>(&v), 2); };
            d.append("RIFF");
            u32(36 + n * 2);
            d.append("WAVEfmt ");
            u32(16), u16(1), u16(1), u32(48000), u32(96000), u16(2), u16(16);
            d.append("data");
            u32(n * 2);
            for (int i = 0; i < n; ++i) u16(uint16_t(int16_t(std::lround(0.5 * 32767 * std::sin(2 * M_PI * 1000 * i / 48000.0)))));
            f.write(d);
            f.close();
            MediaItem m;
            m.id = p.newId();
            QVERIFY(probeMedia(wav.toStdString(), m));
            p.media.push_back(m);
            Clip a = makeClip(p, m, TrackKind::Audio, s);
            a.duration = 24;
            s.audioTracks[0].clips.push_back(a);
        }
        DcpSettings st;
        st.title = "Test film: a short";
        st.kind = "short";
        st.container = "scope";
        st.threads = 2;
        DcpResult r;
        std::string err;
        QVERIFY2(exportDcp(p, s, st, dir.path().toStdString(), &r, {}, &err), err.c_str());
        QCOMPARE(r.frames, int64_t(24));
        QVERIFY(r.fps == 24 && r.width == 2048 && r.height == 858 && r.channels == 6);
        const QString name = QString::fromStdString(r.name);
        QVERIFY2(name.startsWith("TestFilmAShort_SHR_S_EN-XX_XX_51_2K_") && name.endsWith("_SMPTE_OV"), qPrintable(name));
        QVERIFY2(r.cinemaProfile, "encoded in the DCI 2K profile");
        const QDir out(QString::fromStdString(r.folder));
        QVERIFY(out.exists("ASSETMAP.xml") && out.exists("VOLINDEX.xml"));
        const QStringList pictures = out.entryList({"j2c_*.mxf"}), sounds = out.entryList({"pcm_*.mxf"}), cpls = out.entryList({"CPL_*.xml"});
        QVERIFY(pictures.size() == 1 && sounds.size() == 1 && cpls.size() == 1 && out.entryList({"PKL_*.xml"}).size() == 1);
        // It checks out as a server would read it.
        std::vector<std::string> issues = verifyDcp(r.folder);
        QVERIFY2(issues.empty(), issues.empty() ? "" : issues.front().c_str());
        // The composition: 24 fps, 24 frames, Scope, 5.1, and the track files by the ids their names carry.
        QFile cpl(out.filePath(cpls[0]));
        QVERIFY(cpl.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(cpl.readAll());
        const QString pictureId = pictures[0].mid(4, 36), soundId = sounds[0].mid(4, 36);
        QVERIFY(text.contains("<Id>urn:uuid:" + pictureId + "</Id>") && text.contains("<Id>urn:uuid:" + soundId + "</Id>"));
        QVERIFY(text.contains("<EditRate>24 1</EditRate>") && text.contains("<IntrinsicDuration>24</IntrinsicDuration>"));
        QVERIFY(text.contains("<AnnotationText>" + name + "</AnnotationText>"));
        QVERIFY(text.contains("<ScreenAspectRatio>2048 858</ScreenAspectRatio>") && text.contains("51/L,R,C,LFE,Ls,Rs"));
        QVERIFY(text.contains("SMPTE-RDD-52:2020-Bv2.1") && text.contains("<ContentKind>short</ContentKind>"));
        // The picture: the grey as DCI X'Y'Z' in the middle (Rec.709 at gamma 2.4, D65 white kept, 48/52.37, 1/2.6),
        // black on either side where the 16:9 frame is pillarboxed in Scope.
        std::vector<uint16_t> xyz;
        int w = 0, h = 0;
        QVERIFY2(readDcpFrame(out.filePath(pictures[0]).toStdString(), 3, xyz, w, h, &err), err.c_str());
        QVERIFY(w == 2048 && h == 858);
        const double lin = std::pow(0.5, 2.4);
        auto code = [](double v) { return 4095 * std::pow(v * 48 / 52.37, 1 / 2.6); };
        const double want[3] = {code(0.9505 * lin), code(lin), code(1.0891 * lin)};
        const uint16_t* mid = &xyz[(size_t(429) * 2048 + 1024) * 3];
        for (int c = 0; c < 3; ++c) QVERIFY2(std::fabs(mid[c] - want[c]) < 12, qPrintable(QString("%1: %2, not %3").arg(c).arg(mid[c]).arg(want[c])));
        const uint16_t* side = &xyz[(size_t(429) * 2048 + 100) * 3];
        QVERIFY2(side[0] < 12 && side[1] < 12 && side[2] < 12, qPrintable(QString("%1 %2 %3").arg(side[0]).arg(side[1]).arg(side[2])));
        // The sound: 24-bit 48 kHz, six channels, a second; the tone on the left (at the mix's level), the centre silent.
        AudioBufferPtr lc = decodeAudio(out.filePath(sounds[0]).toStdString(), 48000, &err, nullptr, {0, 2});
        QVERIFY2(lc, err.c_str());
        QVERIFY2(std::abs(lc->frames() - 48000) < 10, qPrintable(QString::number(lc->frames())));
        double left = 0, centre = 0;
        for (int64_t i = 2000; i < 10000; ++i) left += double(lc->samples[size_t(i) * 2]) * lc->samples[size_t(i) * 2], centre += double(lc->samples[size_t(i) * 2 + 1]) * lc->samples[size_t(i) * 2 + 1];
        left = std::sqrt(left / 8000), centre = std::sqrt(centre / 8000);
        QVERIFY2(left > 0.2 && centre < 1e-3, qPrintable(QString("%1 %2").arg(left).arg(centre)));
        // A damaged file is caught.
        {
            QFile f(out.filePath(sounds[0]));
            QVERIFY(f.open(QIODevice::Append));
            f.write("x");
        }
        issues = verifyDcp(r.folder);
        QVERIFY(std::any_of(issues.begin(), issues.end(), [](const std::string& i) { return i.find("hash") != std::string::npos; }));
        // The same package again is refused (the folder is there); stopping part way leaves nothing behind.
        QVERIFY(!exportDcp(p, s, st, dir.path().toStdString(), nullptr, {}, &err) && err.find("already") != std::string::npos);
        st.title = "Stopped";
        int calls = 0;
        QVERIFY(!exportDcp(p, s, st, dir.path().toStdString(), nullptr, [&](double) { return ++calls < 3; }, &err));
        QVERIFY(QDir(dir.path()).entryList({"Stopped_*"}, QDir::Dirs).isEmpty());
        // Under a second is refused.
        Sequence brief = s;
        brief.inPoint = 0, brief.outPoint = 12;
        st.inOut = true;
        QVERIFY(!exportDcp(p, brief, st, dir.path().toStdString(), nullptr, {}, &err) && err.find("second") != std::string::npos);
        // A 60 fps sequence plays at 30 from every other frame, at its own speed; In to Out takes both ends; one encoder
        // falling behind the renderer still finishes (frames taken back in order before more are pushed).
        {
            Sequence sixty = s;
            sixty.fps = Rational{60, 1};
            sixty.videoTracks[0].clips[0].duration = 120;
            sixty.audioTracks[0].clips[0].duration = 60;
            sixty.inPoint = 0, sixty.outPoint = 59;
            QCOMPARE(dcpFrameRate(sixty, 0), 30);
            QCOMPARE(dcpFrameStep(sixty, 30), 2);
            DcpSettings fast = st;
            fast.title = "Sixty";
            fast.threads = 1;
            DcpResult r60;
            QVERIFY2(exportDcp(p, sixty, fast, dir.path().toStdString(), &r60, {}, &err), err.c_str());
            QVERIFY(r60.fps == 30 && r60.frames == 30);
            const QDir out60(QString::fromStdString(r60.folder));
            AudioBufferPtr tone = decodeAudio(out60.filePath(out60.entryList({"pcm_*.mxf"}).value(0)).toStdString(), 48000, &err, nullptr, {0});
            QVERIFY2(tone, err.c_str());
            QVERIFY2(std::abs(tone->frames() - 48000) < 10, qPrintable(QString::number(tone->frames())));
            int crossings = 0;
            for (int64_t i = 1; i < 40000; ++i) crossings += (tone->samples[size_t(i - 1) * 2] < 0) != (tone->samples[size_t(i) * 2] < 0);  // (stereo)
            QVERIFY2(std::abs(crossings - 1667) < 20, qPrintable(QString::number(crossings)));  // still 1 kHz (not slowed to 800 Hz)
        }
        Sequence fifty = s;
        fifty.fps = Rational{50, 1};
        QVERIFY(dcpFrameRate(fifty, 0) == 25 && dcpFrameStep(fifty, 25) == 2);
        fifty.fps = Rational{60000, 1001};
        QVERIFY(dcpFrameRate(fifty, 0) == 30 && dcpFrameStep(fifty, 30) == 2);
        fifty.fps = Rational{48, 1};
        QVERIFY(dcpFrameRate(fifty, 0) == 48 && dcpFrameStep(fifty, 48) == 1);
        // Checking reports its progress and stops when asked.
        double seen = 0;
        issues = verifyDcp(r.folder, [&](double f) {
            seen = std::max(seen, f);
            return f < 0.3;
        });
        QVERIFY(!issues.empty() && issues.back() == "Stopped" && seen >= 0.3 && seen < 1);
        // 23.976 plays at 24; Flat and Full containers.
        Sequence ntsc = s;
        ntsc.fps = Rational{24000, 1001};
        QCOMPARE(dcpFrameRate(ntsc, 0), 24);
        ntsc.fps = Rational{30000, 1001};
        QCOMPARE(dcpFrameRate(ntsc, 0), 30);
        QCOMPARE(dcpFrameRate(ntsc, 25), 25);
        int cw = 0, ch = 0;
        QVERIFY(dcpContainer("flat", cw, ch) && cw == 1998 && ch == 1080);
        Sequence wide = s;
        wide.width = 2048, wide.height = 858;
        QVERIFY(defaultDcpContainer(wide) == "scope" && defaultDcpContainer(s) == "flat");
        // The full naming convention with a studio and a facility.
        DcpSettings named;
        named.title = "Midnight Run";
        named.studio = "di";
        named.facility = "Mtg";
        named.language = "fr-CA";
        named.territory = "ca";
        QCOMPARE(dcpName(named, 8, "20261009"), std::string("MidnightRun_FTR_F_FR-XX_CA_71_2K_DI_20261009_MTG_SMPTE_OV"));
        QVERIFY(dcpContainer("full", cw, ch) && cw == 2048 && ch == 1080 && !dcpContainer("imax", cw, ch));
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

    void titleTextAnimations() {
        // "HELLO" coming on a letter at a time across one second of a three-second title at 30 fps.
        Project p;
        auto title = [&](int anim, int by = 0, bool out = false) {
            Effect t = makeEffect(p, "title");
            t.strings["text"] = "HELLO WORLD";
            t.params["size"] = 40.0;
            t.params["shadow"] = 0.0;
            t.params["text_anim"] = double(anim);
            t.params["text_anim_by"] = double(by);
            t.params["text_anim_dur"] = 1.0;
            t.params["text_anim_out"] = out ? 1.0 : 0.0;
            return t;
        };
        auto render = [&](const Effect& t, FrameTime f) { return renderGenerator(t, f, 320, 180, 1.0, 90, 30); };
        auto ink = [](const Image& img, int x0, int x1) {
            double a = 0;
            for (int y = 0; y < img.height; ++y)
                for (int x = x0; x < x1; ++x) a += img.at(x, y)[3];
            return a;
        };
        auto diff = [](const Image& a, const Image& b) {
            double d = 0;
            for (size_t i = 0; i < a.px.size(); ++i) d += std::fabs(a.px[i] - b.px[i]);
            return d / double(a.px.size());
        };
        const Image still = render(title(0), 45);
        const double full = ink(still, 0, 320);
        QVERIFY(full > 100);
        // Rise: nothing at the start, the first letters before the last half way, all in place once done.
        const Effect rise = title(1);
        QVERIFY(ink(render(rise, 0), 0, 320) < 1);
        const Image half = render(rise, 15);
        // Half way: the first letters (left) all but in, the last (right) still well short of theirs.
        QVERIFY2(ink(half, 0, 160) > 0.9 * ink(still, 0, 160) && ink(half, 160, 320) < 0.75 * ink(still, 160, 320),
                 qPrintable(QString("%1 %2").arg(ink(half, 0, 160) / ink(still, 0, 160)).arg(ink(half, 160, 320) / ink(still, 160, 320))));
        QVERIFY(diff(render(rise, 45), still) < 1e-4);
        // Each kind, by letter, word and line, ends as the plain title does; animating out empties it by the end.
        for (int anim = 1; anim <= 6; ++anim)
            for (int by = 0; by <= 2; ++by) {
                if (anim == 5) continue;  // a wave never settles
                QVERIFY2(diff(render(title(anim, by), 45), still) < 1e-4, qPrintable(QString("kind %1 by %2").arg(anim).arg(by)));
            }
        const Effect both = title(1, 0, true);
        QVERIFY(diff(render(both, 45), still) < 1e-4);
        QVERIFY(ink(render(both, 89), 0, 320) < 1);
        QVERIFY(ink(render(both, 75), 0, 320) < full * 0.9);  // on its way out
        // Pop and drop are part way at the start of their turn; scramble shows other letters until it settles.
        QVERIFY(diff(render(title(3, 1), 8), still) > 1e-3);
        QVERIFY(diff(render(title(4), 10), still) > 1e-3);
        const Image scrambled = render(title(6), 5);
        QVERIFY(ink(scrambled, 0, 320) > full * 0.5 && diff(scrambled, still) > 1e-3);
        // Wave keeps moving.
        QVERIFY(diff(render(title(5), 40), render(title(5), 50)) > 1e-4);
        // Word by word: the first word in before the second.
        const Image words = render(title(1, 1), 12);
        QVERIFY(ink(words, 0, 160) / ink(still, 0, 160) > ink(words, 160, 320) / ink(still, 160, 320) + 0.2);
        // The templates using them render.
        for (const char* id : {"title_cascade", "title_pop_words", "title_drop", "title_wave", "title_decode"}) {
            QVERIFY(findTitleTemplate(id));
            Effect t = makeEffect(p, id);
            QVERIFY2(ink(renderGenerator(t, 60, 640, 360, 1.0, 90, 30), 0, 640) > 50, id);
        }
    }

    void gradeNodeGraphs() {
        // A grey shot (0.25) graded through nodes; each result checked against the same effect run alone as a stack.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64, s.height = 36;
        Clip grey = makeGeneratorClip(p, "color", 10);
        for (const char* k : {"color.r", "color.g", "color.b"}) grey.generator.params[k] = 0.25;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 0}, grey).ok);
        const Id id = s.videoTracks[0].clips[0].id;
        auto clip = [&]() -> Clip& { return *edit::clipById(s, id); };
        Effect gain = makeEffect(p, "color_correct");
        gain.params["gain"] = 2.0;
        Effect invert = makeEffect(p, "invert");
        RenderOptions o;
        auto at = [&](const Sequence& seq, int x, int y) {
            const Image img = renderProgramFrame(p, seq, 2, o);
            const float* v = img.at(x, y);
            return v[0];
        };
        auto stackOf = [&](std::vector<Effect> fx) {
            Sequence alone = s;
            Clip& c = alone.videoTracks[0].clips[0];
            c.effects = std::move(fx);
            c.grade = {};
            return alone;
        };
        const float plain = at(stackOf({}), 32, 18), gained = at(stackOf({gain}), 32, 18), inverted = at(stackOf({invert}), 32, 18);
        QVERIFY(gained > plain + 0.1f && inverted > plain + 0.1f && std::fabs(gained - inverted) > 0.01f);
        auto near = [](float a, float b) { return std::fabs(a - b) < 2e-4f; };
        // A rectangle over the left (or right) half, as a node's window.
        auto half = [&](int node, bool left) {
            Effect st = makeEffect(p, "grade_node");
            st.node = node;
            st.params["mask.shape"] = 2.0;
            st.params["mask.x"] = left ? 0.25 : 0.75;
            st.params["mask.w"] = 0.5;
            st.params["mask.h"] = 2.0;
            st.params["mask.feather"] = 0.0;
            return st;
        };

        // In series, converted from the stack: the same picture.
        Effect curves = makeEffect(p, "hue_sat");
        curves.params["saturation"] = 50.0;
        clip().effects = {gain, curves};
        const float stacked = at(s, 32, 18);
        QVERIFY(edit::convertToNodes(s, id).ok);
        QCOMPARE(at(s, 32, 18), stacked);
        // Show Mask on a node with no window or mix of its own: its key (everywhere) as grey.
        Effect shown = makeEffect(p, "grade_node");
        shown.node = 1;
        shown.params["mask.show"] = 1.0;
        clip().effects.push_back(shown);
        QVERIFY2(near(at(s, 32, 18), 1.0f), qPrintable(QString::number(at(s, 32, 18))));
        clip().effects.pop_back();

        // Parallel: gain on the left, invert on the right; unkeyed, their average.
        QVERIFY(edit::flattenGrade(p, s, id).ok);
        clip().effects = {gain};
        QVERIFY(edit::convertToNodes(s, id).ok);
        int right = 0;
        QVERIFY(edit::addGradeNode(p, s, id, edit::NodeHow::Parallel, 1, &right).ok);
        Effect inv = invert;
        inv.node = right;
        clip().effects.push_back(inv);
        QVERIFY2(near(at(s, 32, 18), (gained + inverted) / 2), qPrintable(QString::number(at(s, 32, 18))));
        clip().effects.push_back(half(1, true));
        clip().effects.push_back(half(right, false));
        QVERIFY2(near(at(s, 8, 18), gained), qPrintable(QString::number(at(s, 8, 18))));
        QVERIFY2(near(at(s, 56, 18), inverted), qPrintable(QString::number(at(s, 56, 18))));
        // Its LUT: a window is about where a colour is, so those nodes are named as left out (and the LUT is the input).
        std::vector<std::string> skipped;
        const Lut3D lut = bakeClipLut(s, clip(), 2, 9, &skipped);
        QCOMPARE(skipped.size(), size_t(2));
        QVERIFY(std::fabs(lut.data[(1 + 9 * (1 + 9 * 1)) * 3] - 0.125f) < 1e-4f);
        // Unkeyed, the LUT holds the average.
        std::erase_if(clip().effects, [](const Effect& e) { return e.type == "grade_node"; });
        skipped.clear();
        const Lut3D both = bakeClipLut(s, clip(), 2, 9, &skipped);
        QVERIFY(skipped.empty());
        Image one(1, 1);
        one.px = {0.25f, 0.25f, 0.25f, 1.0f};
        Image viaGain = one, viaInvert = one;
        applyVideoEffect(gain, 2, viaGain, 1.0);
        applyVideoEffect(invert, 2, viaInvert, 1.0);
        const float expect = (viaGain.px[0] + viaInvert.px[0]) / 2;
        QVERIFY2(std::fabs(lut.data.size() ? both.data[(2 + 9 * (2 + 9 * 2)) * 3] - expect : 1) < 1e-4f,
                 qPrintable(QString::number(both.data[(2 + 9 * (2 + 9 * 2)) * 3])));

        // Layer: invert over the picture where its window is.
        QVERIFY(edit::flattenGrade(p, s, id).ok);
        clip().effects = {};
        QVERIFY(edit::convertToNodes(s, id).ok);
        int top = 0;
        QVERIFY(edit::addGradeNode(p, s, id, edit::NodeHow::Layer, 1, &top).ok);
        inv.node = top;
        clip().effects = {inv, half(top, true)};
        QVERIFY(near(at(s, 8, 18), inverted) && near(at(s, 56, 18), plain));
        // A multiply blend: the picture times the inverted one, on the left.
        for (GradeNode& n : clip().grade.nodes)
            if (n.kind == "layer") n.blend = {"multiply"};
        QVERIFY2(near(at(s, 8, 18), plain * inverted) || std::fabs(at(s, 8, 18) - plain * inverted) < 0.02f, qPrintable(QString::number(at(s, 8, 18))));
        // The mixer's key is where its inputs are (here the base node chooses the left too, changing nothing): a node
        // outside it grades the rest (the right), not nothing.
        clip().effects.push_back(half(1, true));
        int mixer = 0;
        for (GradeNode& n : clip().grade.nodes)
            if (n.kind == "layer") n.blend = {"normal"}, mixer = n.key;
        int rest = 0;
        QVERIFY(edit::addGradeNode(p, s, id, edit::NodeHow::Outside, mixer, &rest).ok);
        Effect g2 = gain;
        g2.node = rest;
        clip().effects.push_back(g2);
        QVERIFY2(near(at(s, 8, 18), inverted), qPrintable(QString::number(at(s, 8, 18))));
        QVERIFY2(near(at(s, 56, 18), gained), qPrintable(QString::number(at(s, 56, 18))));

        // Outside: gain inside node 1's left half, invert outside it.
        QVERIFY(edit::flattenGrade(p, s, id).ok);
        clip().effects = {};
        Effect g1 = gain;
        QVERIFY(edit::convertToNodes(s, id).ok);
        g1.node = 1;
        clip().effects = {g1, half(1, true)};
        int outside = 0;
        QVERIFY(edit::addGradeNode(p, s, id, edit::NodeHow::Outside, 1, &outside).ok);
        inv.node = outside;
        clip().effects.push_back(inv);
        QVERIFY2(near(at(s, 8, 18), gained), qPrintable(QString::number(at(s, 8, 18))));
        QVERIFY2(near(at(s, 56, 18), inverted), qPrintable(QString::number(at(s, 56, 18))));

        // A key input: node 1 only chooses the left half (no effects), its key limits node 2's invert.
        QVERIFY(edit::flattenGrade(p, s, id).ok);
        clip().effects = {};
        QVERIFY(edit::convertToNodes(s, id).ok);
        int second = 0;
        QVERIFY(edit::addGradeNode(p, s, id, edit::NodeHow::Serial, 1, &second).ok);
        QVERIFY(edit::connectGradeNodes(s, id, {1, second, 1, true}).ok);
        inv.node = second;
        clip().effects = {half(1, true), inv};
        QVERIFY(near(at(s, 8, 18), inverted) && near(at(s, 56, 18), plain));
        // Switched off, a node passes its picture on; a mix of 0 too.
        GradeNode off = *findGradeNode(clip().grade, second);
        off.enabled = false;
        QVERIFY(edit::setGradeNode(s, id, off).ok);
        QVERIFY(near(at(s, 8, 18), plain));

        // A shared node: one change seen on both clips using it.
        QVERIFY(edit::flattenGrade(p, s, id).ok);
        clip().effects = {gain};
        QVERIFY(edit::convertToNodes(s, id).ok);
        Id shared = 0;
        QVERIFY(edit::shareGradeNode(p, s, id, 1, "Look", &shared).ok);
        Clip other = grey;
        other.id = p.newId();
        other.start = 10;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 0}, other).ok);
        const Id otherId = s.videoTracks[0].clips[1].id;
        QVERIFY(edit::useSharedNode(p, s, otherId, shared).ok);
        auto atTime = [&](FrameTime t) { return renderProgramFrame(p, s, t, o).at(32, 18)[0]; };
        QVERIFY(near(atTime(2), gained) && near(atTime(12), gained));
        edit::effectChain(s, shared)->front() = invert;
        QVERIFY(near(atTime(2), inverted) && near(atTime(12), inverted));
    }

    void colorGroupGrades() {
        // Two grey shots in a group: its pre-clip grade (gain x2) before each one's own (invert on the second), its
        // post-clip grade (tint) after.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64, s.height = 36;
        for (int i = 0; i < 2; ++i) {
            Clip c = makeGeneratorClip(p, "color", 10);
            for (const char* k : {"color.r", "color.g", "color.b"}) c.generator.params[k] = 0.25;
            c.start = i * 10;
            QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 0}, c).ok);
        }
        const Id a = s.videoTracks[0].clips[0].id, b = s.videoTracks[0].clips[1].id;
        edit::clipById(s, b)->effects.push_back(makeEffect(p, "invert"));
        Id group = 0;
        QVERIFY(edit::makeColorGroup(p, s, {a, b}, "Interview", &group).ok);
        ColorGroup* g = findColorGroup(s, group);
        Effect gain = makeEffect(p, "color_correct");
        gain.params["gain"] = 2.0;
        g->pre.push_back(gain);
        Effect warm = makeEffect(p, "color_correct");
        warm.params["temperature"] = 40.0;
        g->post.push_back(warm);
        RenderOptions o;
        auto px = [&](const Sequence& seq, FrameTime t) {
            const Image img = renderProgramFrame(p, seq, t, o);
            const float* v = img.at(32, 18);
            return std::array<float, 3>{v[0], v[1], v[2]};
        };
        // The same as each clip with the chain written out on it.
        Sequence flat = s;
        flat.colorGroups.clear();
        for (Clip& c : flat.videoTracks[0].clips) {
            std::vector<Effect> chain = {gain};
            chain.insert(chain.end(), c.effects.begin(), c.effects.end());
            chain.push_back(warm);
            c.effects = chain;
            c.colorGroup = 0;
        }
        for (FrameTime t : {FrameTime(2), FrameTime(12)}) {
            const auto grouped = px(s, t), written = px(flat, t);
            for (int k = 0; k < 3; ++k) QVERIFY2(std::fabs(grouped[k] - written[k]) < 1e-5, qPrintable(QString::number(t)));
        }
        // The order matters: gain then invert is not invert then gain.
        Sequence wrong = flat;
        std::swap(wrong.videoTracks[0].clips[1].effects[0], wrong.videoTracks[0].clips[1].effects[1]);
        QVERIFY(std::fabs(px(wrong, 12)[1] - px(s, 12)[1]) > 0.05);
        // Warmer than neutral: the post-clip grade ran.
        QVERIFY(px(s, 2)[0] > px(s, 2)[2] + 0.02);
        // A change to the group's grade changes the rendered frames' cache key; ungrouped clips render as before.
        const QByteArray before = frameKey(p, s, 2, o);
        g->post[0].params["temperature"] = 10.0;
        QVERIFY(frameKey(p, s, 2, o) != before);
        Sequence none = s;
        none.colorGroups.clear();
        const float plain = px(none, 2)[1];
        QVERIFY2(std::fabs(plain - 0.25f) < 0.02f, qPrintable(QString::number(plain)));
    }

    void qualityCheckSpelling() {
        // Only text: no picture or sound checks.
        QcSettings q;
        q.flashing = q.levels = q.clipping = false;
        q.blackSeconds = q.freezeSeconds = q.silenceSeconds = 0;
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.fps = {25, 1};
        Clip bg = makeGeneratorClip(p, "color", 250);
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 0}, bg).ok);
        Clip title = makeGeneratorClip(p, "title", 50);
        title.generator.strings["text"] = "Welcom to Montaj";
        title.name = "Opening";
        title.start = 100;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 1}, title).ok);
        CaptionTrack en;
        en.id = p.newId();
        en.captions = {{0, 50, "Teh show begins."}, {50, 100, "All good here."}, {150, 200, "Its recieved well."}};
        CaptionTrack fr;
        fr.id = p.newId();
        fr.name = "French";
        fr.language = "fr";
        fr.captions = {{0, 50, "Bonjour tout le monde."}};
        s.captionTracks = {en, fr};
        std::vector<QcIssue> issues = qualityCheck(p, s, 0, -1, q);
        QCOMPARE(issues.size(), size_t(3));  // two captions and the title; French has no dictionary
        for (const QcIssue& i : issues) QCOMPARE(i.kind, QcKind::Spelling);
        QCOMPARE(issues[0].start, FrameTime(0));
        QCOMPARE(issues[0].end, FrameTime(50));
        QVERIFY2(QString::fromStdString(issues[0].text).contains("'Teh' (The?)"), issues[0].text.c_str());
        QVERIFY2(QString::fromStdString(issues[1].text).contains("Opening") && QString::fromStdString(issues[1].text).contains("'Welcom'") &&
                     QString::fromStdString(issues[1].text).contains("'Montaj'"),
                 issues[1].text.c_str());
        QVERIFY2(QString::fromStdString(issues[2].text).contains("caption 3") && QString::fromStdString(issues[2].text).contains("(received?)"),
                 issues[2].text.c_str());
        QCOMPARE(QString(qcKindName(QcKind::Spelling)), QString("Spelling"));
        // The project's words are right; a range checks only what is in it; off checks nothing.
        p.vocabulary = {"Montaj"};
        issues = qualityCheck(p, s, 90, 160, q);
        QCOMPARE(issues.size(), size_t(2));
        QVERIFY(!QString::fromStdString(issues[0].text).contains("Montaj"));
        QCOMPARE(issues[0].start, FrameTime(100));
        q.spelling = false;
        QVERIFY(qualityCheck(p, s, 0, -1, q).empty());
        // Titles in British English: "colour" is right there, not in American.
        q.spelling = true;
        s.captionTracks.clear();
        Clip* t = edit::clipById(s, s.videoTracks[1].clips[0].id);
        t->generator.strings["text"] = "Colour grading";
        QCOMPARE(qualityCheck(p, s, 0, -1, q).size(), size_t(1));
        q.titleLanguage = "en-GB";
        QVERIFY(qualityCheck(p, s, 0, -1, q).empty());
    }

    void hdrLightLevels() {
        const ColorSpace& sdr = *findColorSpace("rec709");
        const ColorSpace& pq = *findColorSpace("rec2100pq");
        const ColorSpace& hlg = *findColorSpace("rec2100hlg");
        // Code values and nits: PQ reference white 203 and the 1000-nit level; HLG 75 % is 203 on a 1000-nit display;
        // SDR white is 100; camera log and linear light have none.
        QVERIFY(std::fabs(codeToNits(pq, 0.5807) - 203) < 1);
        QVERIFY(std::fabs(nitsToCode(pq, 1000) - 0.7518) < 0.001);
        QVERIFY(std::fabs(codeToNits(hlg, 0.75) - 203) < 2);
        QVERIFY(std::fabs(codeToNits(hlg, 1.0) - 1000) < 0.5);
        QVERIFY(std::fabs(codeToNits(sdr, 1.0) - 100) < 1e-6);
        QVERIFY(codeToNits(*findColorSpace("slog3-sgamut3cine"), 0.5) < 0);
        for (const ColorSpace* cs : {&sdr, &pq, &hlg})
            for (double n : {1.0, 50.0, 100.0})
                QVERIFY2(std::fabs(codeToNits(*cs, nitsToCode(*cs, n)) - n) < 0.01 * n, cs->id.c_str());
        // A pixel's brightest channel: HLG's OOTF dims a saturated red against a neutral of the same code.
        const float pqHot[3] = {float(nitsToCode(pq, 1000)), 0.1f, 0.1f};
        QVERIFY(std::fabs(pixelMaxNits(pq, pqHot) - 1000) < 1);
        const float hlgRed[3] = {1, 0, 0}, hlgWhite[3] = {1, 1, 1};
        QVERIFY(std::fabs(pixelMaxNits(hlg, hlgWhite) - 1000) < 0.5);
        QVERIFY(std::fabs(pixelMaxNits(hlg, hlgRed) - 1000 * std::pow(0.2627, 0.2)) < 1);

        // A frame: a quarter at 1000 nits, the rest at 100.
        Image img(20, 20);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x) {
                const float v = float(nitsToCode(pq, x < 10 && y < 10 ? 1000 : 100));
                float* px = img.at(x, y);
                px[0] = px[1] = px[2] = v, px[3] = 1;
            }
        const LightMeter meter(pq);
        QVERIFY(meter.valid());
        QVERIFY(!LightMeter(*findColorSpace("linear-rec709")).valid());
        double peak = 0, average = 0;
        meter.measure(img, peak, average);
        QVERIFY2(std::fabs(peak - 1000) < 2 && std::fabs(average - 325) < 1, qPrintable(QString("%1 %2").arg(peak).arg(average)));
        // The same light measured in HLG (the codes that give it there).
        Image h(20, 20);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 20; ++x) {
                const float v = float(nitsToCode(hlg, x < 10 && y < 10 ? 1000 : 100));
                float* px = h.at(x, y);
                px[0] = px[1] = px[2] = v, px[3] = 1;
            }
        LightMeter(hlg).measure(h, peak, average);
        QVERIFY2(std::fabs(peak - 1000) < 2 && std::fabs(average - 325) < 1.5, qPrintable(QString("%1 %2").arg(peak).arg(average)));

        // A sequence: mid grey for five frames, then a white title on black (brighter, but less light on average).
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 160, s.height = 90, s.fps = {25, 1};
        s.colorSpace = "rec2100pq";
        Clip grey = makeGeneratorClip(p, "color", 5);
        for (const char* c : {"color.r", "color.g", "color.b"}) grey.generator.params[c] = 0.5;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 0}, grey).ok);
        Clip black = makeGeneratorClip(p, "color", 5);
        for (const char* c : {"color.r", "color.g", "color.b"}) black.generator.params[c] = 0.0;
        black.start = 5;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 0}, black).ok);
        Clip title = makeGeneratorClip(p, "title", 5);
        title.generator.strings["text"] = "HDR";
        title.generator.params["size"] = 40.0;
        title.generator.params["shadow"] = 0.0;
        title.start = 5;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 1}, title).ok);
        LightLevels l;
        std::string err;
        QVERIFY2(measureLightLevels(p, s, 0, 0, l, &err), err.c_str());
        QCOMPARE(l.frames, int64_t(10));
        const double greyNits = 203 * std::pow(0.5, 2.4);  // Rec.709 mid grey placed in HDR
        QVERIFY2(std::fabs(l.maxCll - 203) < 3, qPrintable(QString::number(l.maxCll)));
        QCOMPARE(l.maxCllFrame, FrameTime(5));
        QVERIFY2(std::fabs(l.maxFall - greyNits) < 1, qPrintable(QString::number(l.maxFall)));
        QCOMPARE(l.maxFallFrame, FrameTime(0));
        // Part of it, and in HDR10's terms.
        QVERIFY(measureLightLevels(p, s, 5, 10, l, &err, 0.5));
        QCOMPARE(l.frames, int64_t(5));
        QVERIFY(l.maxFall < greyNits);
        unsigned cll = 0, fall = 0;
        hdr10LightLevels(LightLevels{203.4, 250, 0, 0, 1}, cll, fall);
        QVERIFY(cll == 203u && fall == 203u);
        hdr10LightLevels(LightLevels{}, cll, fall);
        QVERIFY(cll == 1u && fall == 1u);
        // An empty sequence has none.
        Project empty = makeDefaultProject();
        empty.active()->colorSpace = "rec2100pq";
        QVERIFY(!measureLightLevels(empty, *empty.active(), 0, 0, l, &err) && err.find("empty") != std::string::npos);
    }

    void videoPacking() {
        using namespace montage::video;
        std::mt19937 rng(9);
        // v210 as FFmpeg's encoder writes it, byte for byte (its last pixels in a row of 1280 and its padding, codes
        // outside 4..1019 kept in), from random 10-bit pictures.
        for (int w : {720, 1280, 1920, 2048, 3840, 1924}) {
            const int h = 3;
            Ycbcr422 pic(w, h);
            std::uniform_int_distribution<int> any(0, 1023);
            AVFrame* f = av_frame_alloc();
            f->format = AV_PIX_FMT_YUV422P10LE;
            f->width = w, f->height = h;
            QVERIFY(av_frame_get_buffer(f, 0) == 0);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const int v = any(rng);
                    pic.y[size_t(y * w + x)] = uint16_t(v << 6);
                    reinterpret_cast<uint16_t*>(f->data[0] + y * f->linesize[0])[x] = uint16_t(v);
                }
                for (int x = 0; x < w / 2; ++x) {
                    const int u = any(rng), v = any(rng);
                    pic.cb[size_t(y * (w / 2) + x)] = uint16_t(u << 6);
                    pic.cr[size_t(y * (w / 2) + x)] = uint16_t(v << 6);
                    reinterpret_cast<uint16_t*>(f->data[1] + y * f->linesize[1])[x] = uint16_t(u);
                    reinterpret_cast<uint16_t*>(f->data[2] + y * f->linesize[2])[x] = uint16_t(v);
                }
            }
            const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_V210);
            QVERIFY(codec);
            AVCodecContext* c = avcodec_alloc_context3(codec);
            c->width = w, c->height = h, c->pix_fmt = AV_PIX_FMT_YUV422P10LE, c->time_base = {1, 25};
            QVERIFY(avcodec_open2(c, codec, nullptr) == 0);
            AVPacket* pkt = av_packet_alloc();
            QVERIFY(avcodec_send_frame(c, f) == 0 && avcodec_receive_packet(c, pkt) == 0);
            const size_t stride = v210RowBytes(w);
            QCOMPARE(size_t(pkt->size), stride * h);
            std::vector<uint8_t> ours(stride * h, 0xAB);
            packV210(pic, ours.data(), stride);
            QVERIFY2(std::memcmp(ours.data(), pkt->data, ours.size()) == 0, qPrintable(QString::number(w)));
            av_packet_free(&pkt);
            avcodec_free_context(&c);
            av_frame_free(&f);
        }
        // UYVY as swscale packs 8-bit 4:2:2, and clipped to 1..254.
        {
            const int w = 64, h = 2;
            Ycbcr422 pic(w, h);
            std::vector<uint8_t> y8(w * h), u8(w / 2 * h), v8(w / 2 * h);
            std::uniform_int_distribution<int> legal(1, 254);
            for (size_t i = 0; i < y8.size(); ++i) pic.y[i] = uint16_t((y8[i] = uint8_t(legal(rng))) << 8);
            for (size_t i = 0; i < u8.size(); ++i) {
                pic.cb[i] = uint16_t((u8[i] = uint8_t(legal(rng))) << 8);
                pic.cr[i] = uint16_t((v8[i] = uint8_t(legal(rng))) << 8);
            }
            SwsContext* sws = sws_getContext(w, h, AV_PIX_FMT_YUV422P, w, h, AV_PIX_FMT_UYVY422, SWS_POINT, nullptr, nullptr, nullptr);
            QVERIFY(sws);
            std::vector<uint8_t> want(w * 2 * h), ours(w * 2 * h);
            const uint8_t* src[3] = {y8.data(), u8.data(), v8.data()};
            const int srcStride[3] = {w, w / 2, w / 2};
            uint8_t* dst[1] = {want.data()};
            const int dstStride[1] = {w * 2};
            sws_scale(sws, src, srcStride, 0, h, dst, dstStride);
            sws_freeContext(sws);
            packUyvy(pic, ours.data(), w * 2);
            QVERIFY(ours == want);
            pic.y[0] = 0, pic.cb[0] = 65535;
            packUyvy(pic, ours.data(), w * 2);
            QCOMPARE(int(ours[0]), 254);
            QCOMPARE(int(ours[1]), 1);
        }
        // P216: 16-bit Y, then the Cb Cr pairs below it, little-endian, rows `stride` apart.
        {
            Ycbcr422 pic(4, 2);
            for (size_t i = 0; i < pic.y.size(); ++i) pic.y[i] = uint16_t(0x1000 + i);
            for (size_t i = 0; i < pic.cb.size(); ++i) pic.cb[i] = uint16_t(0x2000 + i), pic.cr[i] = uint16_t(0x3000 + i);
            const size_t stride = 16;
            std::vector<uint8_t> out(stride * 4, 0);
            packP216(pic, out.data(), stride);
            auto at = [&](size_t byte) { return int(out[byte] | out[byte + 1] << 8); };
            QCOMPARE(at(0), 0x1000);
            QCOMPARE(at(stride + 6), 0x1007);
            QCOMPARE(at(stride * 2), 0x2000);      // Cb of row 0
            QCOMPARE(at(stride * 2 + 2), 0x3000);  // Cr
            QCOMPARE(at(stride * 3 + 4), 0x2003);  // row 1's second pair
            QCOMPARE(at(stride * 3 + 6), 0x3003);
        }
        // An odd width: the last pixel's chroma packed too (no green column), and in UYVY the pixel itself.
        {
            Ycbcr422 pic(3, 1);
            QCOMPARE(pic.chromaWidth(), 2);
            pic.y = {0x1000, 0x1100, 0x1200};
            pic.cb = {0x2000, 0x2100};
            pic.cr = {0x3000, 0x3100};
            const size_t stride = output::rowBytes(output::PixelFormat::P216, 3);
            QCOMPARE(stride, size_t(8));
            std::vector<uint8_t> out(stride * 2, 0);
            packP216(pic, out.data(), stride);
            auto at = [&](size_t byte) { return int(out[byte] | out[byte + 1] << 8); };
            QCOMPARE(at(stride + 4), 0x2100);
            QCOMPARE(at(stride + 6), 0x3100);
            QCOMPARE(output::rowBytes(output::PixelFormat::Uyvy8, 3), size_t(8));
            std::vector<uint8_t> uyvy(8, 0);
            packUyvy(pic, uyvy.data(), 8);
            QCOMPARE(int(uyvy[4]), 0x21);
            QCOMPARE(int(uyvy[5]), 0x12);
            QCOMPARE(int(uyvy[6]), 0x31);
        }
        // 75 % colour bars in Y'CbCr: SMPTE RP 219's codes for BT.709 (white 721 512 512, yellow 674 176 543, cyan 581
        // 589 176, green 534 253 207, magenta 251 771 817, red 204 435 848, blue 111 848 481), black 64 512 512.
        {
            const float bars[8][3] = {{0.75f, 0.75f, 0.75f}, {0.75f, 0.75f, 0}, {0, 0.75f, 0.75f}, {0, 0.75f, 0},
                                      {0.75f, 0, 0.75f}, {0.75f, 0, 0}, {0, 0, 0.75f}, {0, 0, 0}};
            const int want[8][3] = {{721, 512, 512}, {674, 176, 543}, {581, 589, 176}, {534, 253, 207},
                                    {251, 771, 817}, {204, 435, 848}, {111, 848, 481}, {64, 512, 512}};
            Image img(8 * 4, 2);
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 32; ++x) {
                    float* p = img.at(x, y);
                    p[0] = bars[x / 4][0], p[1] = bars[x / 4][1], p[2] = bars[x / 4][2], p[3] = 1;
                }
            const Ycbcr422 yc = toYcbcr422(img, YcbcrMatrix::Bt709);
            for (int b = 0; b < 8; ++b) {
                // The middle of each bar (away from the chroma filter's edges).
                const int x = b * 4 + 2;
                const int got[3] = {(yc.y[size_t(x)] + 32) >> 6, (yc.cb[size_t(x / 2)] + 32) >> 6, (yc.cr[size_t(x / 2)] + 32) >> 6};
                for (int k = 0; k < 3; ++k) QVERIFY2(std::abs(got[k] - want[b][k]) <= 1, qPrintable(QStringLiteral("bar %1 [%2]: %3").arg(b).arg(k).arg(got[k])));
            }
            // BT.2020's matrix: its white is the same, its red has more luma than BT.709's.
            const Ycbcr422 wide = toYcbcr422(img, YcbcrMatrix::Bt2020);
            QCOMPARE(wide.y[2], yc.y[2]);
            QVERIFY(wide.y[22] > yc.y[22]);
            // Packed as v210, a bar's codes as they are.
            std::vector<uint8_t> packed(v210RowBytes(32) * 2);
            packV210(yc, packed.data(), v210RowBytes(32));
            const uint32_t w0 = packed[0] | packed[1] << 8 | packed[2] << 16 | uint32_t(packed[3]) << 24;
            QCOMPARE(int(w0 >> 10 & 1023), (yc.y[0] + 32) >> 6);
        }
    }

    void referenceOutput() {
        using namespace montage::output;
        // Modes: the sequence's own, progressive first; else its rate at the nearest larger size; none at a rate no
        // mode has.
        const std::vector<Mode> modes = standardModes();
        QCOMPARE(QString::fromStdString(matchMode(modes, 1920, 1080, {25, 1})->name), QStringLiteral("1080p25"));
        QCOMPARE(QString::fromStdString(matchMode(modes, 1920, 1080, {30000, 1001})->name), QStringLiteral("1080p29.97"));
        QCOMPARE(QString::fromStdString(matchMode(modes, 1280, 720, {25, 1})->name), QStringLiteral("1080p25"));
        QCOMPARE(QString::fromStdString(matchMode(modes, 1280, 720, {60000, 1001})->name), QStringLiteral("720p59.94"));
        QCOMPARE(QString::fromStdString(matchMode(modes, 3840, 2160, {24000, 1001})->name), QStringLiteral("2160p23.98"));
        QCOMPARE(QString::fromStdString(matchMode(modes, 8192, 4320, {25, 1})->name), QStringLiteral("4096x2160p25"));
        QVERIFY(!matchMode(modes, 1920, 1080, {48, 1}));
        QVERIFY(findMode(modes, "1080i50") && findMode(modes, "1080i50")->interlaced && findMode(modes, "1080i50")->fps == (Rational{25, 1}));
        QVERIFY(findMode(modes, "2048x1080p24"));
        // Timecode, drop-frame at 29.97.
        const Timecode tc = timecodeOf(1800, {30000, 1001});
        QVERIFY(tc.dropFrame && tc.minutes == 1 && tc.seconds == 0 && tc.frames == 2);
        QCOMPARE(timecodeOf(90061, {25, 1}).hours, 1);

        // The signal: the sequence's own (HDR needing 10 bits; P3 PQ as BT.2020 PQ; sRGB as BT.709), or BT.709.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64, s.height = 36, s.fps = {25, 1}, s.sampleRate = 48000;
        Config cfg;
        Signal sig;
        s.colorSpace = "rec2100pq";
        s.hdrPeakNits = 1000;
        QVERIFY(signalFor(s, cfg, PixelFormat::V210, sig));
        QVERIFY(sig.eotf == Eotf::Pq && sig.bt2020 && sig.matrix == video::YcbcrMatrix::Bt2020 && sig.hdrMetadata);
        QCOMPARE(sig.maxCll, 1000);
        QCOMPARE(sig.maxFall, 400);
        QCOMPARE(QString::fromStdString(ndiColorInfo(sig)), QStringLiteral("<ndi_color_info transfer=\"bt_2100_pq\" matrix=\"bt_2020\" primaries=\"bt_2020\"/>"));
        std::string why;
        QVERIFY(!signalFor(s, cfg, PixelFormat::Uyvy8, sig, &why) && QString::fromStdString(why).contains("10-bit"));
        s.colorSpace = "p3d65pq";
        QVERIFY(signalFor(s, cfg, PixelFormat::P216, sig) && sig.colorSpace == "rec2100pq" && sig.masteringP3);
        s.colorSpace = "rec2100hlg";
        QVERIFY(signalFor(s, cfg, PixelFormat::V210, sig) && sig.eotf == Eotf::Hlg && !sig.hdrMetadata);
        cfg.rec709 = true;
        QVERIFY(signalFor(s, cfg, PixelFormat::Uyvy8, sig) && sig.colorSpace == "rec709" && sig.eotf == Eotf::Sdr);
        cfg.rec709 = false;
        s.colorSpace = "srgb";
        QVERIFY(signalFor(s, cfg, PixelFormat::V210, sig) && sig.colorSpace == "rec709" && !sig.bt2020);
        s.colorSpace = "rec709";

        // Framing: letterboxed, pillarboxed, cropped to fill.
        Rect r = fitRect(1920, 800, 1920, 1080, Config::Scaling::Fit);
        QVERIFY(r.x == 0 && r.y == 140 && r.width == 1920 && r.height == 800);
        r = fitRect(1440, 1080, 1920, 1080, Config::Scaling::Fit);
        QVERIFY(r.x == 240 && r.width == 1440);
        r = fitRect(1280, 720, 1920, 1080, Config::Scaling::Fit);
        QVERIFY(r.x == 0 && r.y == 0 && r.width == 1920 && r.height == 1080);
        r = fitRect(1920, 800, 1920, 1080, Config::Scaling::Fill);
        QVERIFY(r.height == 1080 && r.x < 0);
        Image small(4, 2);
        small.fill(1, 1, 1, 1);
        Image boxed = fitInto(small, 8, 8, Config::Scaling::Fit);
        QCOMPARE(boxed.at(4, 0)[0], 0.0f);  // black above
        QCOMPARE(boxed.at(4, 4)[0], 1.0f);

        // Scheduled on a virtual device (manual clock, a tiny mode).
        auto dev = std::make_shared<VirtualBackend>();
        dev->setManual(true);
        Device d = dev->devices()[0];
        d.modes = {Mode{"tiny", 64, 36, {25, 1}, false, 0}};
        Scheduler sched(dev);
        QVERIFY2(sched.start(d, d.modes[0], s, cfg, &why), why.c_str());
        QCOMPARE(dev->openChannels(), 2);
        QCOMPARE(sched.renderScale(), 1.0);
        auto grey = [&](float v) {
            auto img = std::make_shared<Image>(64, 36);
            img->fill(v, v, v, 1);
            return std::shared_ptr<const Image>(img);
        };
        auto lumaOf = [&](const VirtualBackend::Sent& x) {
            const uint32_t w0 = x.data[0] | x.data[1] << 8 | x.data[2] << 16 | uint32_t(x.data[3]) << 24;
            return int(w0 >> 10 & 1023);
        };
        // Sends one frame once `frame` is ready; it must be the one sent.
        auto sendNext = [&](FrameTime frame) {
            for (int i = 0; i < 500 && !sched.ready(frame); ++i) QThread::msleep(2);
            const size_t n = dev->sent().size();
            dev->step(1);
            return dev->waitSent(n + 1) && dev->sent().back().frame == frame;
        };
        // Parked: the frame at the playhead (mid grey: 64 + 876 / 2), repeated.
        sched.setPosition(5, 0);
        sched.submit(5, grey(0.5f));
        QVERIFY(sendNext(5));
        QCOMPARE(lumaOf(dev->sent().back()), 502);
        QVERIFY(sendNext(5));
        QCOMPARE(dev->sent().back().timecode.frames, 5);
        // Playing: each frame as the playhead reaches it, the sound with it and running on unbroken.
        std::vector<std::pair<int64_t, int>> asked;
        sched.setAudioSource([&](int64_t start, int frames, int channels, float* out) {
            asked.push_back({start, frames});
            for (int i = 0; i < frames; ++i) out[size_t(i) * size_t(channels)] = float(start + i);
        });
        for (FrameTime t = 10; t < 16; ++t) sched.submit(t, grey(0.25f + 0.1f * float(t - 10)));
        const size_t before = dev->sent().size();
        for (FrameTime t = 10; t < 16; ++t) {
            sched.setPosition(t, 1);
            QVERIFY(sendNext(t));
            QCOMPARE(lumaOf(dev->sent().back()), int(std::lround(64 + 876 * (0.25 + 0.1 * double(t - 10)))));
        }
        const auto sent = dev->sent();
        QCOMPARE(sent.size(), before + 6);
        QCOMPARE(asked.size(), size_t(6));
        QCOMPARE(asked.front().first, int64_t(10 * 1920));  // with its picture
        for (size_t i = 1; i < asked.size(); ++i) {
            QCOMPARE(asked[i].first, asked[i - 1].first + asked[i - 1].second);  // unbroken
            QCOMPARE(asked[i].second, 1920);
        }
        const std::vector<float> heard = dev->audio();
        QCOMPARE(heard[heard.size() - 2 * 1920], float(15 * 1920));  // embedded, channel 1 of 2
        // Frames not rendered in time: the last one again, counted late, while the sound runs on (not back over what
        // it played).
        const int64_t lateBefore = sched.stats().late;
        for (FrameTime t = 16; t < 19; ++t) {
            sched.setPosition(t, 1);
            const size_t n = dev->sent().size();
            dev->step(1);
            QVERIFY(dev->waitSent(n + 1));
            QCOMPARE(dev->sent().back().frame, FrameTime(15));
        }
        QCOMPARE(sched.stats().late, lateBefore + 3);
        QCOMPARE(asked.size(), size_t(9));
        for (size_t i = 6; i < asked.size(); ++i) QCOMPARE(asked[i].first, asked[i - 1].first + asked[i - 1].second);
        // What it no longer wants: frames packed or behind the one due.
        QVERIFY(!sched.wants(15) && !sched.wants(10) && sched.wants(19) && sched.wants(30));
        // Stopping while the device waits.
        sched.stop();
        QVERIFY(!sched.running());

        // The picture ahead of the playhead by the video offset; HDR as the device was opened with.
        cfg.videoOffset = 2;
        s.colorSpace = "rec2100pq";
        QVERIFY2(sched.start(d, d.modes[0], s, cfg, &why), why.c_str());
        QCOMPARE(dev->openSignal().eotf, Eotf::Pq);
        QVERIFY(dev->openSignal().hdrMetadata);
        sched.setPosition(20, 1);
        sched.submit(22, grey(0.5f));
        QVERIFY(sendNext(22));
        sched.stop();
        // A mode at another rate than the sequence's: timecode counts the output's frames (a 25 fps sequence's frame 5
        // is 0.2 s, frame 10 at 50p), a frame a tick while playing.
        cfg.videoOffset = 0;
        s.colorSpace = "rec709";
        s.fps = {25, 1};
        const Mode fifty{"tiny50", 64, 36, {50, 1}, false, 0};
        QVERIFY2(sched.start(d, fifty, s, cfg, &why), why.c_str());
        sched.setPosition(5, 0);
        sched.submit(5, grey(0.5f));
        QVERIFY(sendNext(5));
        QCOMPARE(dev->sent().back().timecode.frames, 10);
        sched.setPosition(6, 1);
        sched.submit(6, grey(0.5f));
        QVERIFY(sendNext(6));
        QCOMPARE(dev->sent().back().timecode.frames, 12);
        QVERIFY(sendNext(6));
        QCOMPARE(dev->sent().back().timecode.frames, 13);
        // Played backwards it counts down.
        sched.setPosition(6, -1);
        QVERIFY(sendNext(6));
        QCOMPARE(dev->sent().back().timecode.frames, 12);
        QVERIFY(sendNext(6));
        QCOMPARE(dev->sent().back().timecode.frames, 11);
        sched.stop();
        s.colorSpace = "rec2100pq";  // (HDR is not sent in 8 bits)
        cfg.depth = 8;
        QVERIFY(!sched.start(d, d.modes[0], s, cfg, &why));
    }

    void deckLinkWithoutDriver() {
        using namespace montage::output;
        // No Desktop Video here: listed, not available, with the reason, and no devices or opening.
        const auto dl = findBackend("DeckLink");
        QVERIFY(dl);
        std::string why;
        QVERIFY(!dl->available(&why));
        QVERIFY(!why.empty());
        QVERIFY(dl->devices().empty());
        Device d;
        d.id = "0";
        QVERIFY(!dl->open(d, standardModes()[0], Signal{}, 2, 48000, &why));
        QVERIFY(!why.empty());
        QCOMPARE(dl->receivers(), -1);
    }

    void remoteReviewStream() {
        using namespace montage::output;
        // Settings: SRT needs a passphrase of 10 to 79 characters, and so do markers; addresses need a port.
        StreamSettings st;
        QVERIFY(!checkStreamSettings(st).empty());
        st.passphrase = "too short";
        QVERIFY(QString::fromStdString(checkStreamSettings(st)).contains("passphrase"));
        st.passphrase = generatePassphrase();
        QCOMPARE(st.passphrase.size(), size_t(20));
        QVERIFY(st.passphrase != generatePassphrase());
        QVERIFY2(checkStreamSettings(st).empty(), checkStreamSettings(st).c_str());
        QCOMPARE(markersPortFor(st), 9001);
        st.address = "rtmp://example.com:1935";
        QVERIFY(!checkStreamSettings(st).empty());
        st.address = "srt://example.com";
        QVERIFY(!checkStreamSettings(st).empty());
        std::string scheme, host;
        int port = 0;
        QVERIFY(parseStreamAddress("srt://[::1]:9000?streamid=publish:review", scheme, host, port));
        QVERIFY(scheme == "srt" && host == "::1" && port == 9000);
        QVERIFY(parseStreamAddress("udp://239.1.1.1:5000", scheme, host, port) && scheme == "udp" && port == 5000);
        StreamSettings lan;
        lan.address = "udp://239.1.1.1:5000";
        lan.markersPort = -1;
        QVERIFY(checkStreamSettings(lan).empty());  // (a local network: neither encrypted nor taking markers)
        QCOMPARE(markersPortFor(lan), -1);

        // The tag each picture carries, as written and as x265 writes it (after a UUID of its own).
        StreamTag tag{1234, "00:00:49:09", {25, 1}, 7};
        std::vector<uint8_t> sei = streamTagPayload(tag);
        StreamTag back;
        QVERIFY(parseStreamTag(sei.data(), sei.size(), back));
        QVERIFY(back.frame == 1234 && back.timecode == "00:00:49:09" && back.fps == (Rational{25, 1}) && back.sequence == 7);
        std::vector<uint8_t> x265(16, 0x2c);
        x265.insert(x265.end(), sei.begin(), sei.end());
        QVERIFY(parseStreamTag(x265.data(), x265.size(), back) && back.frame == 1234);
        sei[3] ^= 1;
        QVERIFY(!parseStreamTag(sei.data(), sei.size(), back));

        // Burnt in: white figures on a black box at the bottom, the rest of the picture as it was.
        Planes420 burn(320, 180);
        std::fill(burn.y.begin(), burn.y.end(), uint16_t(500));
        burnTimecode(burn, "01:23:45:12");
        const Rect box = burnInBox(320, 180, 11);
        QVERIFY(box.x % 2 == 0 && box.y % 2 == 0 && box.y + box.height <= 180 && box.width < 320);
        int white = 0, black = 0, elsewhere = 0;
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x) {
                const uint16_t v = burn.y[size_t(y) * 320 + size_t(x)];
                const bool in = x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
                if (!in) elsewhere += v != 500;
                else (v == 940 ? white : black) += v == 940 || v == 64;
            }
        QCOMPARE(elsewhere, 0);
        QVERIFY(white > 100 && black > white);

        StreamBackend probe;
        std::string why;
        if (!probe.available(&why)) QSKIP(why.c_str());
        auto freeUdp = [] {
            QUdpSocket s;
            s.bind(QHostAddress::LocalHost, 0);
            return int(s.localPort());
        };
        auto freeTcp = [] {
            QTcpServer s;
            s.listen(QHostAddress::Any, 0);
            return int(s.serverPort());
        };

        // A grey ramp across the picture, the frame in a bar's brightness.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320, s.height = 180, s.fps = {25, 1}, s.sampleRate = 48000, s.colorSpace = "rec709";
        auto ramp = [&](FrameTime) {
            auto img = std::make_shared<Image>(320, 180);
            for (int y = 0; y < 180; ++y)
                for (int x = 0; x < 320; ++x) {
                    float* px = img->at(x, y);
                    px[0] = px[1] = px[2] = float(x) / 319.0f;
                    px[3] = 1;
                }
            return std::shared_ptr<const Image>(img);
        };
        // Plays from frame 100 in real time, as playback does, until `done`.
        FrameTime playhead = 100;
        auto play = [&](Scheduler& sched, const std::function<bool()>& done, int seconds) {
            QElapsedTimer clock;
            clock.start();
            FrameTime submitted = 99;
            while (!done() && clock.elapsed() < seconds * 1000) {
                const FrameTime t = 100 + clock.elapsed() * 25 / 1000;
                sched.setPosition(t, 1);
                playhead = t;
                (void)sched.stats();  // (read while it streams, as the panel does)
                while (submitted < t + 3) sched.submit(++submitted, ramp(submitted));
                QThread::msleep(5);
            }
            return done();
        };

        auto stream = std::make_shared<StreamBackend>();
        StreamSettings srt;
        srt.address = "srt://127.0.0.1:" + std::to_string(freeUdp());
        srt.passphrase = generatePassphrase();
        srt.hardware = false;
        srt.bitrateKbps = 6000;
        srt.markersPort = freeTcp();
        stream->setSettings(srt);
        stream->setSequence(s.id);
        Device dev = stream->devices()[0];
        const Mode mode{"stream", 320, 180, {25, 1}, false, 0};
        Config cfg;
        cfg.depth = 10;
        cfg.audioChannels = 2;
        Scheduler sched(stream);
        QVERIFY2(sched.start(dev, mode, s, cfg, &why), why.c_str());
        QCOMPARE(sched.signal().format, PixelFormat::P216);
        sched.setAudioSource([](int64_t start, int frames, int channels, float* out) {
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < channels; ++c) out[size_t(i) * size_t(channels) + size_t(c)] = 0.25f * float(std::sin(2 * M_PI * 1000 * double(start + i) / 48000));
        });
        QCOMPARE(stream->stats().encoder, std::string("libx265"));
        QCOMPARE(stream->receivers(), 0);

        // A viewer with the wrong passphrase is refused.
        {
            ReviewReceiver wrong;
            QVERIFY(wrong.start(srt.address, "not the passphrase", 120, false, &why));
            play(sched, [&] { return !wrong.status().error.empty(); }, 10);
            QVERIFY(!wrong.status().error.empty());
            QCOMPARE(wrong.status().pictures, int64_t(0));
        }
        // The right one sees 10-bit HEVC, its frames tagged in order with their timecodes burnt in, and hears the tone.
        ReviewReceiver rx;
        QVERIFY(rx.start(srt.address, srt.passphrase, 120, false, &why));
        QVERIFY2(play(sched, [&] { return rx.status().pictures >= 60; }, 30), rx.status().error.c_str());
        QCOMPARE(stream->receivers(), 1);
        QVERIFY2(QString::fromStdString(rx.status().video).startsWith("hevc 320x180 10-bit"), rx.status().video.c_str());
        QVERIFY(rx.status().audio);
        ReviewReceiver::Picture pic;
        QVERIFY(rx.latest(pic));
        QVERIFY(pic.tagged && pic.tag.fps == (Rational{25, 1}) && pic.tag.frame >= 100 && pic.tag.sequence == s.id);
        QCOMPARE(QString::fromStdString(pic.tag.timecode), QString::fromStdString(formatTimecode(pic.tag.frame, {25, 1}, true)));
        int off = 0, fine = 0;
        for (int x = 0; x < 320; ++x) {
            const int got = pic.y[20 * 320 + size_t(x)], want = int(std::lround(64 + 876 * x / 319.0));
            off = std::max(off, std::abs(got - want));
            fine += got % 4 != 0;
        }
        QVERIFY2(off <= 6, std::to_string(off).c_str());
        QVERIFY2(fine > 100, std::to_string(fine).c_str());  // (codes an 8-bit picture could not have)
        // The figures burnt in are this picture's timecode's, not the next one's.
        auto boxDiff = [&](const std::string& text) {
            Planes420 want(320, 180);
            burnTimecode(want, text);
            const Rect b = burnInBox(320, 180, text.size());
            double sum = 0;
            for (int y = b.y; y < b.y + b.height; ++y)
                for (int x = b.x; x < b.x + b.width; ++x) sum += std::abs(int(pic.y[size_t(y) * 320 + size_t(x)]) - int(want.y[size_t(y) * 320 + size_t(x)]));
            return sum / (b.width * b.height);
        };
        const double same = boxDiff(pic.tag.timecode), next = boxDiff(formatTimecode(pic.tag.frame + 1, {25, 1}, true));
        QVERIFY2(same < 25 && next > same * 2, (std::to_string(same) + " " + std::to_string(next)).c_str());
        const std::vector<StreamTag> tags = rx.tags();
        QVERIFY(tags.size() >= 50);
        // In order, and keeping up with the playhead. Not every picture a new frame: the output's clock and the
        // playhead's are not the same, so where their ticks fall close together a frame shows twice and the next is
        // passed over, and a busy machine skips pictures it could not encode in time.
        int backwards = 0, repeated = 0;
        for (size_t i = 1; i < tags.size(); ++i) {
            backwards += tags[i].frame < tags[i - 1].frame;
            repeated += tags[i].frame == tags[i - 1].frame;
        }
        const double step = double(tags.back().frame - tags.front().frame) / double(tags.size() - 1);
        qInfo("Pictures: %d, %d repeated, %.2f frames apart", int(tags.size()), repeated, step);
        QVERIFY2(backwards <= 2 && step > 0.6 && step < 4, (std::to_string(backwards) + " " + std::to_string(step)).c_str());
        // Latency here: the frame a picture shows against the playhead as it arrives.
        std::vector<int> lags;
        int64_t seen = rx.status().pictures;
        play(sched, [&] {
            ReviewReceiver::Picture q;
            if (rx.latest(q, seen) && q.tagged) {
                seen = q.serial;
                lags.push_back(int(playhead - q.tag.frame));
            }
            return lags.size() >= 25;
        }, 10);
        QVERIFY(lags.size() >= 25);
        std::sort(lags.begin(), lags.end());
        qInfo("Latency: %d frames", lags[lags.size() / 2]);
        QVERIFY2(lags[lags.size() / 2] <= 10, std::to_string(lags[lags.size() / 2]).c_str());  // (0.4 s)
        std::vector<float> heard;
        play(sched, [&] {
            const std::vector<float> a = rx.takeAudio();
            heard.insert(heard.end(), a.begin(), a.end());
            return heard.size() >= 48000;
        }, 10);
        QVERIFY(heard.size() >= 48000);
        double sum = 0, re = 0, im = 0;
        const size_t n = 24000;
        for (size_t i = 0; i < n; ++i) {
            const double v = heard[heard.size() - 2 * n + 2 * i];
            sum += v * v;
            re += v * std::cos(2 * M_PI * 1000 * double(i) / 48000);
            im += v * std::sin(2 * M_PI * 1000 * double(i) / 48000);
        }
        const double rms = std::sqrt(sum / n), tone = 2 * std::hypot(re, im) / n;
        QVERIFY2(std::fabs(rms - 0.25 / std::sqrt(2.0)) < 0.03 && std::fabs(tone - 0.25) < 0.04, (std::to_string(rms) + " " + std::to_string(tone)).c_str());

        // Markers come back over the side channel with the passphrase, at the frame the viewer was shown.
        RemoteMarker note;
        note.frame = pic.tag.frame;
        note.timecode = pic.tag.timecode;
        note.author = "Ana";
        note.text = "Lift the shadows here";
        QVERIFY(!sendRemoteMarkers("127.0.0.1", srt.markersPort, "not the passphrase", {note}, &why));
        QVERIFY2(QString::fromStdString(why).contains("passphrase"), why.c_str());
        QVERIFY(stream->takeMarkers().empty());
        QVERIFY2(sendRemoteMarkers("127.0.0.1", srt.markersPort, srt.passphrase, {note, RemoteMarker{note.frame + 25, 10, 0, "", "Bob", "Too long"}}, &why), why.c_str());
        const std::vector<RemoteMarker> got = stream->takeMarkers();
        QCOMPARE(got.size(), size_t(2));
        QVERIFY(got[0].frame == note.frame && got[0].author == "Ana" && got[0].text == note.text && got[0].timecode == note.timecode);
        QVERIFY(got[1].frame == note.frame + 25 && got[1].duration == 10);
        QCOMPARE(stream->stats().markers, 2);
        // A channel that cannot show it has the passphrase is not sent to.
        {
            QTcpServer fake;
            QVERIFY(fake.listen(QHostAddress::LocalHost, 0));
            std::string fakeWhy;
            bool sent = true;
            std::thread client([&] { sent = sendRemoteMarkers("127.0.0.1", fake.serverPort(), srt.passphrase, {note}, &fakeWhy, 5000); });
            if (!fake.waitForNewConnection(5000)) {
                client.join();
                QFAIL("The client did not call");
            }
            QTcpSocket* c = fake.nextPendingConnection();
            c->write(QJsonDocument(QJsonObject{{"montage", "review"}, {"version", 2}, {"salt", "00112233445566778899aabbccddeeff"}, {"iterations", 10000},
                                               {"nonce", "0123456789abcdef0123456789abcdef"}}).toJson(QJsonDocument::Compact) + '\n');
            c->flush();
            while (!c->canReadLine() && c->waitForReadyRead(5000)) {
            }
            c->readLine();
            c->write(QJsonDocument(QJsonObject{{"ok", true}, {"proof", "00"}}).toJson(QJsonDocument::Compact) + '\n');
            c->flush();
            client.join();
            QVERIFY(!sent);
            QVERIFY2(QString::fromStdString(fakeWhy).contains("could not show"), fakeWhy.c_str());
        }

        // The viewer leaves; another can call in.
        rx.stop();
        QVERIFY(play(sched, [&] { return stream->receivers() == 0; }, 15));
        ReviewReceiver again;
        QVERIFY(again.start(srt.address, srt.passphrase, 120, false, &why));
        QVERIFY2(play(sched, [&] { return again.status().pictures >= 10; }, 30), again.status().error.c_str());
        again.stop();
        sched.stop();

        // H.264 (8-bit) over UDP to a viewer on this computer, still tagged; HDR refused on it.
        auto udp = std::make_shared<StreamBackend>();
        StreamSettings plain;
        plain.address = "udp://127.0.0.1:" + std::to_string(freeUdp());
        plain.codec = "h264";
        plain.hardware = false;
        plain.markersPort = -1;
        udp->setSettings(plain);
        ReviewReceiver lanViewer;
        QVERIFY(lanViewer.start(plain.address, "", 120, false, &why));
        QThread::msleep(300);
        Scheduler sched8(udp);
        QVERIFY2(sched8.start(dev, mode, s, cfg, &why), why.c_str());
        QVERIFY2(play(sched8, [&] { return lanViewer.status().pictures >= 20; }, 30), lanViewer.status().error.c_str());
        QVERIFY2(QString::fromStdString(lanViewer.status().video).startsWith("h264 320x180 8-bit"), lanViewer.status().video.c_str());
        QVERIFY(lanViewer.latest(pic) && pic.tagged);
        sched8.stop();
        lanViewer.stop();
        Signal pq;
        pq.eotf = Eotf::Pq;
        QVERIFY(!udp->open(dev, mode, pq, 2, 48000, &why) && QString::fromStdString(why).contains("HDR"));
    }

    void ndiOutput() {
        using namespace montage::output;
        // A stand-in NDI Runtime where the SDK says to look.
        qputenv("NDI_RUNTIME_DIR_V6", MONTAGE_TEST_NDI_DIR);
        const auto ndi = findBackend("NDI");
        QVERIFY(ndi);
        std::string why;
        QVERIFY2(ndi->available(&why), why.c_str());
        const std::vector<Device> devices = ndi->devices();
        QCOMPARE(devices.size(), size_t(1));
        QVERIFY(devices[0].modes.empty());  // any size and rate
        montage::plugins::DynLib fake;
        QVERIFY2(fake.open(MONTAGE_TEST_NDI_LIBRARY, &why), why.c_str());
        auto info = [&] {
            auto fn = reinterpret_cast<int (*)(char*, int)>(fake.symbol("fake_ndi_info"));
            char buf[1024] = {};
            if (fn) fn(buf, sizeof buf);
            return QString::fromUtf8(buf);
        };
        // An HDR (PQ) sequence: 16-bit P216 with its colour metadata, stereo sound as planar float, timecode in
        // 100 ns units.
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 64, s.height = 36, s.fps = {25, 1}, s.sampleRate = 48000, s.colorSpace = "rec2100pq";
        Scheduler sched(ndi);
        const Mode mode{modeName(64, 36, {25, 1}, false), 64, 36, {25, 1}, false, 0};
        Config cfg;
        QVERIFY2(sched.start(devices[0], mode, s, cfg, &why), why.c_str());
        sched.setAudioSource([](int64_t, int frames, int channels, float* out) {
            for (int i = 0; i < frames; ++i) out[size_t(i) * size_t(channels)] = 0.5f, out[size_t(i) * size_t(channels) + 1] = -0.25f;
        });
        auto img = std::make_shared<Image>(64, 36);
        img->fill(0, 0, 0, 1);
        sched.submit(3, img);
        sched.setPosition(3, 1);
        QTRY_VERIFY_WITH_TIMEOUT(sched.stats().sent >= 6 && info().contains("audios=") && !info().contains("audios=0 "), 10000);
        QCOMPARE(sched.stats().receivers, 1);
        sched.stop();
        const QString got = info();
        for (const char* want : {"name=Montage clock=1", "fourcc=P216", "x=64 y=36 rate=25/1 stride=128",
                                 "meta=<ndi_color_info transfer=\"bt_2100_pq\" matrix=\"bt_2020\" primaries=\"bt_2020\"/>",
                                 "arate=48000 ch=2 n=1920 cstride=7680 a0=0.5 a1=-0.25", "tc=1200000 "})
            QVERIFY2(got.contains(QString::fromUtf8(want)), qPrintable(got + " lacks " + want));
        // Black, limited range: Y 16 << 8 (4096, little-endian), then Y again.
        QVERIFY2(got.contains("bytes=0,16,0,16"), qPrintable(got));
        // 8-bit SDR: UYVY.
        s.colorSpace = "rec709";
        cfg.depth = 8;
        QVERIFY2(sched.start(devices[0], mode, s, cfg, &why), why.c_str());
        sched.submit(0, img);
        QTRY_VERIFY_WITH_TIMEOUT(info().contains("fourcc=UYVY") && info().contains("bytes=128,16,128,16"), 10000);
        QVERIFY(info().contains("transfer=\"bt_709\""));
        sched.stop();
    }

    void lensCorrectionEffect() {
        QFile file(QStringLiteral(MONTAGE_TEST_DATA_DIR "/lensfun/test-lenses.xml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        lens::Database db;
        QVERIFY(db.addXml(file.readAll().toStdString()));
        const lens::Lens* zoom = db.lens("FE 24-38mm f/2.8 Test");
        QVERIFY(zoom);
        Effect e = makeEffect("lens_correction", 1);
        QVERIFY(findEffectInfo("lens_correction"));
        const int W = 192, H = 108;
        // A horizontal ramp (each pixel's x in linear light / W), opaque.
        Image ramp(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float* p = ramp.at(x, y);
                p[0] = p[1] = p[2] = float((x + 0.5) / W);
                p[3] = 1;
            }
        // No profile chosen yet: nothing happens.
        Image same = ramp;
        applyVideoEffect(e, 0, same, 1.0);
        QVERIFY(same.px == ramp.px);

        e.strings["profile"] = lens::lensXml(*zoom);
        e.params["focal"] = Param(27);
        e.params["camera_crop"] = Param(1);
        e.params["vignetting"] = Param(0);
        // Each pixel comes from where the correction says (green), red and blue from their own places; filled to the
        // frame's edges.
        Image out = ramp;
        applyVideoEffect(e, 0, out, 1.0);
        lens::Settings s;
        s.focal = 27;
        s.cameraCrop = 1;
        s.vignetting = false;
        const lens::Correction c(*zoom, s, W, H);
        QVERIFY(c.scale() > 1);  // edges pulled in by the correction, then the frame filled
        for (const auto& [x, y] : {std::pair{0, 0}, {W - 1, H - 1}, {40, 70}, {W / 2, H / 2}}) {
            double at[6];
            c.source(x + 0.5, y + 0.5, at);
            const float* p = out.at(x, y);
            QVERIFY2(std::fabs(p[1] - std::clamp(at[2], 0.5, W - 0.5) / W) < 1e-4, qPrintable(QString("%1,%2: %3 vs %4").arg(x).arg(y).arg(p[1]).arg(at[2] / W)));
            QVERIFY(std::fabs(p[0] - std::clamp(at[0], 0.5, W - 0.5) / W) < 1e-4);
            QVERIFY(std::fabs(p[2] - std::clamp(at[4], 0.5, W - 0.5) / W) < 1e-4);
            QVERIFY(std::fabs(p[3] - 1) < 1e-4);
        }
        // Made smaller without filling the frame, there is nothing beyond the picture.
        Effect none = e;
        none.params["fit"] = Param(2);
        none.params["scale"] = Param(50);
        Image bare = ramp;
        applyVideoEffect(none, 0, bare, 1.0);
        QVERIFY(bare.at(0, 0)[3] < 0.5);
        QVERIFY(bare.at(W / 2, H / 2)[3] > 0.99);

        // Vignetting: a flat grey brightened towards the corners in linear light (Rec.709 coded here), the middle kept.
        Effect vig = makeEffect("lens_correction", 2);
        vig.strings["profile"] = e.strings["profile"];
        vig.params["focal"] = Param(30);
        vig.params["aperture"] = Param(3.3);
        vig.params["camera_crop"] = Param(1);
        for (const char* off : {"distortion", "tca", "rectilinear"}) vig.params[off] = Param(0);
        Image grey(W, H);
        grey.fill(0.4f, 0.4f, 0.4f, 1);
        applyVideoEffect(vig, 0, grey, 1.0);
        lens::Settings vs;
        vs.focal = 30;
        vs.aperture = 3.3;
        vs.cameraCrop = 1;
        const lens::Correction vc(*zoom, vs, W, H);
        const double corner = fromLinear(Transfer::Bt1886, toLinear(Transfer::Bt1886, 0.4) / vc.falloff(0.5, 0.5));
        QVERIFY2(std::fabs(grey.at(0, 0)[1] - corner) < 2e-3, qPrintable(QString::number(grey.at(0, 0)[1])));
        QVERIFY(grey.at(0, 0)[1] > 0.5f);
        QVERIFY(std::fabs(grey.at(W / 2, H / 2)[1] - 0.4f) < 2e-3);
        // Half-covered pixels keep their coverage; the colour is brightened as it is straight.
        Image half(W, H);
        half.fill(0.4f, 0.4f, 0.4f, 0.5f);  // (premultiplied by fill)
        applyVideoEffect(vig, 0, half, 1.0);
        QVERIFY(std::fabs(half.at(0, 0)[3] - 0.5f) < 1e-6);
        QVERIFY(std::fabs(half.at(0, 0)[1] / 0.5f - corner) < 2e-3);
    }

    void colorChartCalibration() {
        // A ColorChecker shot under a warm cast: its colours through a matrix in linear light, on a grey wall.
        const ChartColors& ref = colorChecker24();
        auto lin = [](double v) { return std::pow(std::max(v, 0.0), 2.4); };
        auto code = [](double v) { return std::pow(std::max(v, 0.0), 1 / 2.4); };
        const double cast[3][3] = {{1.15, 0.08, 0.0}, {0.02, 0.95, 0.03}, {0.0, 0.05, 0.7}};
        ChartColors shot;
        for (int i = 0; i < 24; ++i)
            for (int c = 0; c < 3; ++c) {
                const double l = cast[c][0] * lin(ref[size_t(i)][0]) + cast[c][1] * lin(ref[size_t(i)][1]) + cast[c][2] * lin(ref[size_t(i)][2]);
                shot[size_t(i)][size_t(c)] = code(l * 0.9);
            }
        // Draws the chart over (x, y, w, h) in pixels of a W x H picture, turned: 0 upright, 1 upside down, 2 with its top
        // to the right, 3 with its top to the left.
        auto picture = [&](int turn, int x0, int y0, int w, int h, int W = 640, int H = 360) {
            Image img(W, H);
            img.fill(0.33f, 0.33f, 0.33f, 1);
            for (int y = y0 - 8; y < y0 + h + 8; ++y)
                for (int x = x0 - 8; x < x0 + w + 8; ++x) {
                    float* p = img.at(x, y);
                    p[0] = p[1] = p[2] = 0.05f;  // the chart's black frame and gaps
                }
            const int cols = turn >= 2 ? 4 : 6, rows = turn >= 2 ? 6 : 4;
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c) {
                    // Which patch sits in this cell.
                    int pr = r, pc = c;
                    if (turn == 1) pr = 3 - r, pc = 5 - c;
                    if (turn == 2) pr = 3 - c, pc = r;
                    if (turn == 3) pr = c, pc = 5 - r;
                    const ChartColor& col = shot[size_t(pr * 6 + pc)];
                    const double cw = double(w) / cols, ch = double(h) / rows;
                    for (int y = int(y0 + r * ch + ch * 0.08); y < int(y0 + (r + 1) * ch - ch * 0.08); ++y)
                        for (int x = int(x0 + c * cw + cw * 0.08); x < int(x0 + (c + 1) * cw - cw * 0.08); ++x) {
                            float* p = img.at(x, y);
                            for (int k = 0; k < 3; ++k) p[k] = float(col[size_t(k)]);
                        }
                }
            return img;
        };
        auto near = [](const ChartColors& a, const ChartColors& b, double tol) {
            for (size_t i = 0; i < 24; ++i)
                for (size_t k = 0; k < 3; ++k)
                    if (std::fabs(a[i][k] - b[i][k]) > tol) return false;
            return true;
        };
        // Upright: found, its outline the patch area, its patches as shot.
        Image img = picture(0, 120, 80, 300, 200);
        TrackQuad q;
        double residual = 1;
        QVERIFY(detectChart(img, q, &residual));
        QVERIFY2(residual < 0.03, qPrintable(QString::number(residual)));
        QVERIFY2(std::fabs(q.p[0].x - 120.0 / 640) < 0.02 && std::fabs(q.p[0].y - 80.0 / 360) < 0.03, qPrintable(QString("%1 %2").arg(q.p[0].x).arg(q.p[0].y)));
        QVERIFY(std::fabs(q.p[2].x - 420.0 / 640) < 0.02 && std::fabs(q.p[2].y - 280.0 / 360) < 0.03);
        const ChartColors measured = sampleChart(img, q);
        QVERIFY(near(measured, shot, 0.01));
        // The fit takes the cast away: far off before, close after.
        const ChartFit fit = fitChart(measured);
        QVERIFY(fit.ok);
        QVERIFY2(fit.deltaEBefore > 5 && fit.deltaEAfter < 1 && fit.deltaEMax < 2,
                 qPrintable(QString("%1 %2 %3").arg(fit.deltaEBefore).arg(fit.deltaEAfter).arg(fit.deltaEMax)));
        // The Chart Match effect does the same to the picture.
        Effect e = chartMatchEffect(fit, 1);
        QCOMPARE(e.type, std::string("chart_match"));
        Image graded = img;
        applyChartMatch(e, 0, graded);
        QVERIFY(near(sampleChart(graded, q), ref, 0.02));
        e.params["amount"] = 0.0;  // none of it
        Image same = img;
        applyChartMatch(e, 0, same);
        QVERIFY(near(sampleChart(same, q), shot, 1e-4));
        // Upside down and on either side: found, the outline starting at the chart's own top left; and in a tall frame.
        for (int turn : {1, 2, 3, 4}) {
            const Image turned = turn == 1   ? picture(1, 200, 100, 330, 220)
                                 : turn == 4 ? picture(0, 40, 240, 270, 180, 360, 640)
                                             : picture(turn, 250, 40, 180, 270);
            TrackQuad tq;
            QVERIFY2(detectChart(turned, tq), qPrintable(QString::number(turn)));
            QVERIFY2(near(sampleChart(turned, tq), shot, 0.015), qPrintable(QString::number(turn)));
        }
        // An outline that is none (corners in a line) or off the picture is no measurement.
        {
            bool complete = false;
            sampleChart(img, q, rec709Space(), &complete);
            QVERIFY(complete);
            TrackQuad line;
            line.p[0] = {0.1, 0.5}, line.p[1] = {0.5, 0.5}, line.p[2] = {0.9, 0.5}, line.p[3] = {0.3, 0.5};
            sampleChart(img, line, rec709Space(), &complete);
            QVERIFY(!complete);
            TrackQuad off = q;
            for (auto& pt : off.p) pt.x += 0.6;
            sampleChart(img, off, rec709Space(), &complete);
            QVERIFY(!complete);
        }
        // At no change it changes nothing: deep shadows, highlights past white and colour outside the gamut kept.
        {
            const float vals[6] = {0.002f, 0.01f, 0.02f, 0.03f, 1.3f, -0.02f};
            Image px(6, 1);
            for (int i = 0; i < 6; ++i) {
                float* p = px.at(i, 0);
                p[0] = p[1] = p[2] = vals[i], p[3] = 1;
            }
            applyChartMatch(chartMatchEffect(ChartFit{}, 3), 0, px);
            for (int i = 0; i < 6; ++i) QVERIFY2(std::fabs(px.at(i, 0)[0] - vals[i]) < 2e-4, qPrintable(QString("%1 %2").arg(vals[i]).arg(px.at(i, 0)[0])));
        }
        // Premultiplied pixels: an empty one stays empty, a half-covered one gets the same colour at half strength; a pixel
        // that is not a number does no harm.
        {
            ChartFit lift;
            lift.m[0][0] = 1.2;
            lift.offset[0] = lift.offset[1] = lift.offset[2] = 0.002;
            Image a(4, 1);
            a.fill(0, 0, 0, 0);
            float* half = a.at(1, 0);
            half[0] = half[1] = half[2] = 0.25f, half[3] = 0.5f;
            float* whole = a.at(2, 0);
            whole[0] = whole[1] = whole[2] = 0.5f, whole[3] = 1;
            float* bad = a.at(3, 0);
            bad[0] = std::numeric_limits<float>::quiet_NaN(), bad[1] = bad[2] = 0.5f, bad[3] = 1;
            applyChartMatch(chartMatchEffect(lift, 4), 0, a);
            for (int k = 0; k < 4; ++k) QCOMPARE(a.at(0, 0)[k], 0.0f);
            for (int k = 0; k < 3; ++k) QVERIFY(std::fabs(a.at(1, 0)[k] - a.at(2, 0)[k] * 0.5f) < 1e-5);
            QVERIFY(a.at(2, 0)[0] > 0.5f);
            QVERIFY(std::isfinite(a.at(3, 0)[0]));
        }
        // In HDR and wide-gamut sequences: the patches read back as the chart's own values, and the effect, told the
        // space, grades the picture there to the chart.
        QCOMPARE(e.s("space"), std::string("rec709"));
        for (const char* id : {"rec2100pq", "rec2100hlg", "p3d65", "rec2020"}) {
            const ColorSpace& cs = *findColorSpace(id);
            Image wide = img;
            convertColor(wide, rec709Space(), cs);
            const ChartColors read = sampleChart(wide, q, cs);
            QVERIFY2(near(read, shot, 0.01), id);
            applyChartMatch(chartMatchEffect(fitChart(read), 5, id), 0, wide);
            QVERIFY2(near(sampleChart(wide, q, cs), ref, 0.02), id);
        }
        // The camera's calibration stays under every grade version.
        QVERIFY(!isGradeEffect(e));
        QVERIFY(isGradeEffect(makeEffect("color_correct", 9)));
        // A picture without a chart: none.
        Image wall(640, 360);
        for (int y = 0; y < 360; ++y)
            for (int x = 0; x < 640; ++x) {
                float* p = wall.at(x, y);
                p[0] = float(x) / 640, p[1] = float(y) / 360, p[2] = 0.4f, p[3] = 1;
            }
        QVERIFY(!detectChart(wall, q));
        QVERIFY(std::fabs(deltaE76({1, 1, 1}, {0, 0, 0}) - 100) < 0.5);
        QCOMPARE(std::string(colorCheckerName(18)), std::string("White"));
    }

    void stereoViews() {
        // Two eyes, a red left and a blue right, 8 x 4, shown each way the viewer and exports offer.
        Image left(8, 4), right(8, 4);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 8; ++x) {
                float* l = left.at(x, y);
                float* r = right.at(x, y);
                l[0] = 1, l[1] = 0, l[2] = 0, l[3] = 1;
                r[0] = 0, r[1] = 0.5f, r[2] = 1, r[3] = 1;
            }
        auto same = [](const float* a, float r, float g, float b) {
            return std::fabs(a[0] - r) < 1e-4f && std::fabs(a[1] - g) < 1e-4f && std::fabs(a[2] - b) < 1e-4f;
        };
        QVERIFY(same(combineStereo(left, right, StereoView::Left).at(3, 2), 1, 0, 0));
        QVERIFY(same(combineStereo(left, right, StereoView::Right).at(3, 2), 0, 0.5f, 1));
        // Anaglyph: red from the left eye's brightness, green and blue from the right eye.
        QVERIFY(same(combineStereo(left, right, StereoView::Anaglyph).at(3, 2), 0.299f, 0.5f, 1));
        // Difference: twice the gap, clipped.
        QVERIFY(same(combineStereo(left, right, StereoView::Difference).at(0, 0), 1, 1, 1));
        QVERIFY(same(combineStereo(left, left, StereoView::Difference).at(0, 0), 0, 0, 0));
        // Packed: full size doubles the frame, squeezed keeps it.
        Image p = combineStereo(left, right, StereoView::SideBySide);
        QVERIFY(p.width == 16 && p.height == 4 && same(p.at(7, 1), 1, 0, 0) && same(p.at(8, 1), 0, 0.5f, 1));
        p = combineStereo(left, right, StereoView::TopBottom);
        QVERIFY(p.width == 8 && p.height == 8 && same(p.at(2, 3), 1, 0, 0) && same(p.at(2, 4), 0, 0.5f, 1));
        p = combineStereo(left, right, StereoView::SideBySideHalf);
        QVERIFY(p.width == 8 && p.height == 4 && same(p.at(1, 1), 1, 0, 0) && same(p.at(6, 1), 0, 0.5f, 1));
        p = combineStereo(left, right, StereoView::TopBottomHalf);
        QVERIFY(p.width == 8 && p.height == 4 && same(p.at(4, 0), 1, 0, 0) && same(p.at(4, 3), 0, 0.5f, 1));
        // Eyes of different sizes cannot be combined: the left is shown.
        QCOMPARE(combineStereo(left, Image(4, 4), StereoView::SideBySide).width, 8);
        // Names, as files and MCP use them, and how many eyes go across and down.
        for (const std::string& n : stereoViewNames()) {
            StereoView v;
            QVERIFY(stereoViewFromName(n, v));
            QCOMPARE(stereoViewName(v), n);
        }
        StereoView v;
        QVERIFY(!stereoViewFromName("checkerboard", v));
        int across = 0, down = 0;
        stereoPacking(StereoView::SideBySide, across, down);
        QVERIFY(across == 2 && down == 1);
        stereoPacking(StereoView::TopBottom, across, down);
        QVERIFY(across == 1 && down == 2);
        stereoPacking(StereoView::SideBySideHalf, across, down);
        QVERIFY(across == 1 && down == 1);
    }

    void titles3D() {
        // Extruded text, lit flat (all ambient, no shine) so faces and sides show their own colours.
        auto title = [](const std::string& text) {
            Effect g = makeEffect("title3d", 1);
            g.strings["text"] = text;
            g.params["size"] = Param(200.0);
            g.params["rot_x"] = Param(0.0);
            g.params["rot_y"] = Param(0.0);
            g.params["ambient"] = Param(100.0);
            g.params["specular"] = Param(0.0);
            g.params["color.r"] = Param(1.0), g.params["color.g"] = Param(0.8), g.params["color.b"] = Param(0.2);
            g.params["side_color.r"] = Param(0.2), g.params["side_color.g"] = Param(0.3), g.params["side_color.b"] = Param(0.9);
            return g;
        };
        const int w = 480, h = 270;
        auto dump = [&](const Image& img, const char* name) {
            if (const QByteArray dir = qgetenv("MONTAGE_TEST_DUMP"); !dir.isEmpty()) {
                QImage q(img.width, img.height, QImage::Format_RGBA8888);
                toRgba8(img, q.bits(), size_t(q.bytesPerLine()));
                q.save(QString::fromLocal8Bit(dir) + "/" + name + ".png");
            }
        };
        struct Ink {
            int x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1, faces = 0, sides = 0, covered = 0;
        };
        auto ink = [](const Image& img) {
            Ink k;
            for (int y = 0; y < img.height; ++y)
                for (int x = 0; x < img.width; ++x) {
                    const float* p = img.at(x, y);
                    if (p[3] < 0.5f) continue;
                    ++k.covered;
                    k.x0 = std::min(k.x0, x), k.x1 = std::max(k.x1, x), k.y0 = std::min(k.y0, y), k.y1 = std::max(k.y1, y);
                    if (p[3] > 0.99f && std::fabs(p[0] - 1) < 0.02f && std::fabs(p[1] - 0.8f) < 0.02f && std::fabs(p[2] - 0.2f) < 0.02f) ++k.faces;
                    if (p[3] > 0.99f && std::fabs(p[0] - 0.2f) < 0.02f && std::fabs(p[1] - 0.3f) < 0.02f && std::fabs(p[2] - 0.9f) < 0.02f) ++k.sides;
                }
            return k;
        };
        // Face on: the face's colour where the letters are, nothing round them; the sides hidden behind (only a sliver
        // shows in perspective away from the middle).
        const Image flat = renderGenerator(title("H"), 0, w, h, 1.0, 90, 30);
        dump(flat, "t3d-flat");
        const Ink f = ink(flat);
        QVERIFY2(f.covered > 2000 && f.faces > 0.85 * f.covered, qPrintable(QString("%1 %2").arg(f.covered).arg(f.faces)));
        QVERIFY(flat.at(2, 2)[3] == 0 && flat.at(w - 3, h - 3)[3] == 0);
        // A letter's hole stays open (nonzero winding): the middle of an O is empty, its ring is not.
        const Image o = renderGenerator(title("O"), 0, w, h, 1.0, 90, 30);
        dump(o, "t3d-o");
        const Ink oi = ink(o);
        QVERIFY(oi.covered > 1000);
        QCOMPARE(o.at((oi.x0 + oi.x1) / 2, (oi.y0 + oi.y1) / 2)[3], 0.0f);
        // Turned 60° about Y, a thin title is about half as wide (in perspective); a deep one shows its side.
        Effect thin = title("H");
        thin.params["rot_y"] = Param(60.0);
        thin.params["depth"] = Param(2.0);
        const Ink th = ink(renderGenerator(thin, 0, w, h, 1.0, 90, 30));
        const double ratio = double(th.x1 - th.x0) / double(f.x1 - f.x0);
        QVERIFY2(ratio > 0.4 && ratio < 0.65, qPrintable(QString::number(ratio)));
        Effect turned = title("H");
        turned.params["rot_y"] = Param(60.0);
        turned.params["depth"] = Param(80.0);
        const Image tu = renderGenerator(turned, 0, w, h, 1.0, 90, 30);
        dump(tu, "t3d-turned");
        const Ink ti = ink(tu);
        QVERIFY2(ti.sides > 300 && ti.faces > 300, qPrintable(QString("%1 %2").arg(ti.sides).arg(ti.faces)));
        // Turned the right side back, the left side shows: the sides' pixels lie left of the face's.
        double faceX = 0, sideX = 0;
        int nf = 0, ns = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float* p = tu.at(x, y);
                if (p[3] < 0.99f) continue;
                if (std::fabs(p[2] - 0.2f) < 0.02f) faceX += x, ++nf;
                else if (std::fabs(p[2] - 0.9f) < 0.02f) sideX += x, ++ns;
            }
        QVERIFY(nf && ns && sideX / ns < faceX / nf);
        // Seen from behind (180°): the back face, mirrored, the same size.
        Effect back = title("H");
        back.params["rot_y"] = Param(180.0);
        const Ink bi = ink(renderGenerator(back, 0, w, h, 1.0, 90, 30));
        QVERIFY(std::abs(bi.covered - f.covered) < f.covered / 10 && bi.faces > 0.8 * bi.covered);
        // Depth goes back from the face, which stays at its set size: as wide deep as flat (perspective draws the
        // extrusion inside its outline).
        Effect deep = title("H");
        deep.params["depth"] = Param(600.0);
        const Ink di = ink(renderGenerator(deep, 0, w, h, 1.0, 90, 30));
        QVERIFY2(std::abs((di.x1 - di.x0) - (f.x1 - f.x0)) <= 3, qPrintable(QString("%1 %2").arg(di.x1 - di.x0).arg(f.x1 - f.x0)));
        // A wide lens and a deep title (which used to put the face behind the camera) still draws it.
        deep.params["fov"] = Param(120.0);
        deep.params["depth"] = Param(700.0);
        const Ink wide = ink(renderGenerator(deep, 0, w, h, 1.0, 90, 30));
        QVERIFY2(wide.faces > 0.5 * wide.covered && wide.covered > 300, qPrintable(QString("%1 %2").arg(wide.covered).arg(wide.faces)));
        // Turned so its near end comes past the camera, the rest of the face is still drawn (cut at the near plane).
        Effect past = title("WIDE TITLE");
        past.params["size"] = Param(160.0);
        past.params["rot_y"] = Param(-80.0);
        past.params["fov"] = Param(100.0);
        past.params["pos_x"] = Param(-120.0);
        const Image pi = renderGenerator(past, 0, w, h, 1.0, 90, 30);
        dump(pi, "t3d-near");
        const Ink pk = ink(pi);
        QVERIFY2(pk.faces > 2000, qPrintable(QString("%1 %2").arg(pk.covered).arg(pk.faces)));
        // Letters run into each other (tight tracking pulls the dash across the I): merged first, so their sides still
        // face out and show. (Before, the dash's sides were judged from a point that fell inside the I, and turned
        // inside out: about half the sides went missing.)
        Effect apart = title("I\u2014");
        apart.params["rot_y"] = Param(50.0);
        apart.params["depth"] = Param(120.0);
        Effect together = apart;
        together.params["tracking"] = Param(-140.0);
        const Image ti2 = renderGenerator(together, 0, w, h, 1.0, 90, 30);
        dump(ti2, "t3d-overlap");
        const Ink ap = ink(renderGenerator(apart, 0, w, h, 1.0, 90, 30)), tg = ink(ti2);
        QVERIFY2(tg.sides > 0.65 * ap.sides && tg.covered < ap.covered, qPrintable(QString("%1 %2 %3 %4").arg(tg.sides).arg(ap.sides).arg(tg.covered).arg(ap.covered)));
        // Flat (no depth) and edge on: next to nothing.
        Effect edge = title("H");
        edge.params["depth"] = Param(0.0);
        edge.params["rot_y"] = Param(90.0);
        QVERIFY(ink(renderGenerator(edge, 0, w, h, 1.0, 90, 30)).covered < f.covered / 20);
        // Lit from where the camera is, the face is fully lit; from straight above, only the ambient light reaches it.
        Effect lit = title("H");
        lit.params["ambient"] = Param(20.0);
        lit.params["light_height"] = Param(90.0);
        const Image li = renderGenerator(lit, 0, w, h, 1.0, 90, 30);
        const float* centre = li.at(f.x0 + 12, (f.y0 + f.y1) / 2 + 30);  // on the left stem
        lit.params["light_height"] = Param(0.0);
        lit.params["light_angle"] = Param(0.0);
        const Image dark = renderGenerator(lit, 0, w, h, 1.0, 90, 30);
        const float* dc = dark.at(f.x0 + 12, (f.y0 + f.y1) / 2 + 30);
        QVERIFY2(centre[3] > 0.99f && std::fabs(centre[0] - 1.0f) < 0.02f, qPrintable(QString::number(centre[0])));
        QVERIFY2(dc[3] > 0.99f && std::fabs(dc[0] - 0.2f) < 0.03f, qPrintable(QString::number(dc[0])));
        // Animation in: flipped up edge on at the start, settled by the end of it; faded out at the clip's end.
        Effect anim = title("H");
        anim.params["depth"] = Param(4.0);  // thin, so edge on is next to nothing
        anim.params["anim_in"] = Param(3.0);
        anim.params["anim_out"] = Param(1.0);
        Effect still = anim;
        still.params["anim_in"] = Param(0.0);
        const int settledCover = ink(renderGenerator(still, 30, w, h, 1.0, 90, 30)).covered;
        const Ink a0 = ink(renderGenerator(anim, 0, w, h, 1.0, 90, 30)), a1 = ink(renderGenerator(anim, 30, w, h, 1.0, 90, 30));
        QVERIFY2(a0.covered < a1.covered / 5 && a1.covered == settledCover, qPrintable(QString("%1 %2").arg(a0.covered).arg(a1.covered)));
        QCOMPARE(ink(renderGenerator(anim, 89, w, h, 1.0, 90, 30)).covered, 0);
        // The same at a quarter size (preview), and the same picture each time.
        const Image small = renderGenerator(title("H"), 0, w / 4, h / 4, 0.25, 90, 30);
        const Ink si = ink(small);
        QVERIFY(std::fabs(double(si.x1 - si.x0) * 4 - double(f.x1 - f.x0)) < 12);
        const Image again = renderGenerator(turned, 0, w, h, 1.0, 90, 30);
        QVERIFY(std::equal(again.px.begin(), again.px.end(), tu.px.begin()));
        // The default look (shaded, turned a little), for looking at.
        Effect plain = makeEffect("title3d", 2);
        plain.strings["text"] = "Montage\n3D";
        dump(renderGenerator(plain, 0, w, h, 0.5, 90, 30), "t3d-default");
        // In the catalog, with its own text.
        const EffectInfo* info = findEffectInfo("title3d");
        QVERIFY(info && info->category == EffectCategory::Generator && info->displayName == "3D Title");
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

    // The GPU compositor draws a frame the same as the CPU renderer: media through its input transform, the colour
    // effects it has (a LUT, Color Correct, Curves, Hue Curves), scaled, rotated and cropped layers in every blend mode,
    // a cross dissolve, a layer made on the CPU, and the display conversion. Skipped where there is no GPU.
    void gpuCompositorMatchesCpu() {
        std::string why;
        if (!gpu::initialize(&why)) QSKIP(why.c_str());
        QTemporaryDir dir;
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 320;
        s.height = 180;
        auto addPicture = [&](const char* name, int w, int h, bool alpha) {
            QImage q(w, h, QImage::Format_RGBA64);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    q.setPixelColor(x, y, QColor::fromRgbF(float(x) / w, float(y) / h, 0.5f + 0.4f * std::sin(x * 0.11f + y * 0.07f),
                                                           alpha ? std::min(1.0f, 0.15f + 0.85f * float((x + 2 * y) % 97) / 96) : 1.0f));
            const QString path = dir.filePath(name);
            if (!q.save(path)) return Id{};
            MediaItem m;
            if (!probeMedia(path.toStdString(), m)) return Id{};
            m.id = p.newId();
            p.media.push_back(m);
            return m.id;
        };
        const Id wide = addPicture("wide.png", 320, 180, false), small = addPicture("small.png", 211, 147, true),
                 other = addPicture("other.png", 320, 180, false);
        QVERIFY(wide && small && other);
        // V1: the full-frame picture through a tinted LUT, then another picture dissolving in.
        const std::string cube = (dir.path() + "/tint.cube").toStdString();
        {
            std::ofstream f(cube);
            f << "LUT_3D_SIZE 3\n";
            for (int b = 0; b < 3; ++b)
                for (int g = 0; g < 3; ++g)
                    for (int r = 0; r < 3; ++r) f << std::pow(r / 2.0, 1.3) << " " << g / 2.0 * 0.9 << " " << std::sqrt(b / 2.0) << "\n";
        }
        auto r1 = edit::placeMedia(p, s, wide, 0, 0, 40, {TrackKind::Video, 0}, {}, false);
        QVERIFY(r1.ok);
        QVERIFY(edit::placeMedia(p, s, other, 40, 0, 40, {TrackKind::Video, 0}, {}, false).ok);
        Effect lut = makeEffect(p, "lut");
        lut.strings["path"] = cube;
        lut.params["strength"] = 80.0;
        s.videoTracks[0].clips[0].effects.push_back(lut);
        QVERIFY(edit::addTransition(p, s, r1.created[0], edit::Edge::Out, "cross_dissolve", 10).ok);
        // V2: the small picture with transparency, scaled, rotated, cropped and faded, graded three ways.
        auto r2 = edit::placeMedia(p, s, small, 0, 0, 80, {TrackKind::Video, 1}, {}, false);
        QVERIFY(r2.ok);
        Clip* top = edit::clipById(s, r2.created[0]);
        QVERIFY(top);
        top->motion.params["scale"] = 60.0;
        top->motion.params["rotation"] = 17.0;
        top->motion.params["pos_x"] = 23.0;
        top->motion.params["pos_y"] = -11.0;
        top->motion.params["crop_left"] = 7.0;
        top->motion.params["crop_bottom"] = 12.0;
        top->motion.params["opacity"] = 70.0;
        Effect cc = makeEffect(p, "color_correct");
        cc.params["exposure"] = 0.4;
        cc.params["saturation"] = 1.3;
        cc.params["temperature"] = 20.0;
        cc.params["lift_b"] = 0.05;
        cc.params["gamma"] = -0.1;
        cc.params["gain_r"] = 1.1;
        cc.params["highlights"] = -30.0;
        cc.params["shadows"] = 25.0;
        cc.params["vibrance"] = 40.0;
        top->effects.push_back(cc);
        Effect curves = makeEffect(p, "curves");
        curves.strings["master"] = "0,0 0.3,0.22 0.7,0.8 1,1";
        curves.strings["blue"] = "0,0.05 1,0.9";
        curves.params["mix"] = 85.0;
        top->effects.push_back(curves);
        Effect hue = makeEffect(p, "hue_curves");
        hue.strings["hue_sat"] = "0,0.5 0.3,0.8 0.6,0.3 0.9,0.5";
        hue.strings["luma_sat"] = "0,0.3 0.5,0.5 1,0.6";
        top->effects.push_back(hue);
        // V3: a colour generator, made on the CPU and composited on the GPU.
        Clip gen = colorClip(p, 0.2f, 0.9f, 0.4f, 0, 80);
        gen.motion.params["scale"] = 30.0;
        gen.motion.params["pos_x"] = 100.0;
        gen.motion.params["pos_y"] = 50.0;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 2}, gen).ok);

        auto compare = [&](FrameTime t, RenderOptions o, const QString& what) {
            o.gpu = false;
            const Image cpu = renderProgramFrame(p, s, t, o);
            const long before = gpu::stats().gpuFrames;
            o.gpu = true;
            const Image onGpu = renderProgramFrame(p, s, t, o);
            QVERIFY2(gpu::stats().gpuFrames == before + 1, qPrintable(what + ": fell back: " + QString::fromStdString(gpu::stats().lastFallback)));
            QCOMPARE(onGpu.width, cpu.width);
            QCOMPARE(onGpu.height, cpu.height);
            double maxDiff = 0, sum = 0;
            int mx = 0, my = 0;
            for (int y = 0; y < cpu.height; ++y)
                for (int x = 0; x < cpu.width; ++x)
                    for (int c = 0; c < 4; ++c) {
                        const double d = std::fabs(double(cpu.at(x, y)[c]) - onGpu.at(x, y)[c]);
                        sum += d;
                        if (d > maxDiff) maxDiff = d, mx = x, my = y;
                    }
            const double mean = sum / (double(cpu.width) * cpu.height * 4);
            QVERIFY2(maxDiff <= 2e-3 && mean <= 2e-4,
                     qPrintable(QString("%1: max %2 at %3,%4 (cpu %5 %6 %7 %8, gpu %9 %10 %11 %12), mean %13")
                                    .arg(what).arg(maxDiff).arg(mx).arg(my)
                                    .arg(cpu.at(mx, my)[0]).arg(cpu.at(mx, my)[1]).arg(cpu.at(mx, my)[2]).arg(cpu.at(mx, my)[3])
                                    .arg(onGpu.at(mx, my)[0]).arg(onGpu.at(mx, my)[1]).arg(onGpu.at(mx, my)[2]).arg(onGpu.at(mx, my)[3])
                                    .arg(mean)));
            // The 8-bit picture for the viewer is the float one as toRgba8 gives it, to within a step.
            o.gpu = true;
            const QImage eight = renderProgramFrameRgba8(p, s, t, o);
            const std::vector<uint8_t> want = toRgba8(cpu);
            QCOMPARE(eight.width(), cpu.width);
            QCOMPARE(eight.height(), cpu.height);
            int worst = 0;
            for (int y = 0; y < cpu.height; ++y) {
                const uchar* a = eight.constScanLine(y);
                const uint8_t* b = want.data() + size_t(y) * cpu.width * 4;
                for (int i = 0; i < cpu.width * 4; ++i) worst = std::max(worst, std::abs(int(a[i]) - int(b[i])));
            }
            QVERIFY2(worst <= 1, qPrintable(what + QString(": 8-bit off by %1").arg(worst)));
        };
        RenderOptions o;
        compare(10, o, "layers");
        compare(37, o, "dissolve");
        const char* modes[] = {"normal", "add", "multiply", "screen", "overlay", "darken", "lighten",
                               "difference", "soft_light", "hard_light", "color_dodge", "color_burn", "subtract"};
        for (const char* mode : modes) {
            edit::clipById(s, r2.created[0])->blendMode = mode;
            compare(20, o, mode);
        }
        edit::clipById(s, r2.created[0])->blendMode = "normal";
        // At half size for playback, flipped.
        RenderOptions half;
        half.scale = 0.5;
        compare(20, half, "half size");
        edit::clipById(s, r2.created[0])->motion.params["flip_h"] = 1.0;
        compare(20, o, "flipped");
        // Nothing on screen: transparent, flattened to black.
        compare(90, o, "empty");
        // Camera log footage with a look applied in ACEScct, as an AMF has it: made on the camera's picture once.
        p.findMedia(other)->colorOverride = "acescct";
        Effect look = makeEffect(p, "lut");
        look.strings["path"] = cube;
        look.strings["space"] = "ACEScct";
        s.videoTracks[0].clips[1].effects.push_back(look);
        compare(60, o, "source look");
        // Masks: an ellipse, feathered and rotated, with a qualifier on reds and greens; a gradient, inverted and faded;
        // a qualifier alone. And the other per-pixel effects the GPU has.
        Clip* graded = edit::clipById(s, r2.created[0]);
        graded->effects[0].params["mask.shape"] = 1.0;
        graded->effects[0].params["mask.x"] = 0.4;
        graded->effects[0].params["mask.y"] = 0.55;
        graded->effects[0].params["mask.w"] = 0.5;
        graded->effects[0].params["mask.h"] = 0.35;
        graded->effects[0].params["mask.rotation"] = 25.0;
        graded->effects[0].params["mask.feather"] = 12.0;
        graded->effects[0].params["mask.qualify"] = 1.0;
        graded->effects[0].params["mask.hue"] = 40.0;
        graded->effects[0].params["mask.hue_width"] = 140.0;
        graded->effects[1].params["mask.shape"] = 6.0;
        graded->effects[1].params["mask.rotation"] = -30.0;
        graded->effects[1].params["mask.invert"] = 1.0;
        graded->effects[1].params["mask.opacity"] = 70.0;
        graded->effects[2].params["mask.qualify"] = 1.0;
        graded->effects[2].params["mask.lum_low"] = 30.0;
        compare(20, o, "masks");
        Effect cdl = makeEffect(p, "cdl");
        cdl.params["slope.r"] = 1.2;
        cdl.params["offset.b"] = 0.05;
        cdl.params["power.g"] = 0.8;
        cdl.params["saturation"] = 1.3;
        Effect cst = makeEffect(p, "color_space_transform");
        cst.strings["from"] = "Rec.709 (SDR video)";
        cst.strings["to"] = "Display P3";
        Effect inv = makeEffect(p, "invert");
        inv.params["amount"] = 30.0;
        inv.params["mask.shape"] = 2.0;
        Effect bw = makeEffect(p, "black_white");
        bw.params["amount"] = 40.0;
        bw.params["tint.b"] = 0.8;
        Effect hs = makeEffect(p, "hue_sat");
        hs.params["hue"] = 25.0;
        hs.params["vibrance"] = 0.4;
        hs.params["lightness"] = -0.1;
        Effect lv = makeEffect(p, "levels");
        lv.params["in_black"] = 0.05;
        lv.params["gamma"] = 1.3;
        lv.params["out_white"] = 0.9;
        for (const Effect& e : {cdl, cst, inv, bw, hs, lv}) graded->effects.push_back(e);
        gpu::resetStats();
        compare(20, o, "more effects");
        QCOMPARE(gpu::stats().cpuLayers, 0L);
        // As grade nodes in series, one with a window and a qualifier at 60 %, one faded to 50 %; then beside another
        // through a parallel mixer, which the GPU leaves to the CPU.
        const Id topId = r2.created[0];
        QVERIFY(edit::convertToNodes(s, topId).ok);
        // A plain chain with one node showing its key.
        {
            Effect shown = makeEffect(p, "grade_node");
            shown.node = 1;
            shown.params["mask.show"] = 1.0;
            edit::clipById(s, topId)->effects.push_back(shown);
            gpu::resetStats();
            compare(20, o, "plain chain shows a key");
            QCOMPARE(gpu::stats().cpuLayers, 0L);
            edit::clipById(s, topId)->effects.pop_back();
        }
        Effect window = makeEffect(p, "grade_node");
        window.node = 2;
        window.params["mask.shape"] = 1.0;
        window.params["mask.x"] = 0.6;
        window.params["mask.w"] = 0.6;
        window.params["mask.h"] = 0.7;
        window.params["mask.qualify"] = 1.0;
        window.params["mask.sat_low"] = 20.0;
        window.params["mix"] = 60.0;
        Effect faded = makeEffect(p, "grade_node");
        faded.node = 4;
        faded.params["mix"] = 50.0;
        edit::clipById(s, topId)->effects.push_back(window);
        edit::clipById(s, topId)->effects.push_back(faded);
        gpu::resetStats();
        compare(20, o, "serial nodes");
        QCOMPARE(gpu::stats().cpuLayers, 0L);
        // The faded node showing its matte as grey.
        edit::clipById(s, topId)->effects.back().params["mask.show"] = 1.0;
        gpu::resetStats();
        compare(20, o, "node shows its matte");
        QCOMPARE(gpu::stats().cpuLayers, 0L);
        edit::clipById(s, topId)->effects.back().params["mask.show"] = 0.0;
        // Mixers and keys: a node with a window beside node 3 through a parallel mixer; one in Screen beside node 2
        // through a layer mixer, keyed by a key mixer of nodes 1 and 2 (node 1 has none: everywhere), inverted, at 80 %,
        // added to its own qualifier; and an outside node (node 2's key inverted).
        auto nodeEffect = [&](int node, const char* type, std::initializer_list<std::pair<const char*, double>> params) {
            Effect e = makeEffect(p, type);
            e.node = node;
            for (const auto& [k, v] : params) e.params[k] = v;
            edit::clipById(s, topId)->effects.push_back(e);
        };
        int par = 0, lay = 0, outside = 0;
        QVERIFY(edit::addGradeNode(p, s, topId, edit::NodeHow::Parallel, 3, &par).ok);
        nodeEffect(par, "black_white", {});
        nodeEffect(par, "grade_node", {{"mask.shape", 2.0}, {"mask.x", 0.3}, {"mask.w", 0.5}, {"mask.h", 0.6}});
        gpu::resetStats();
        compare(20, o, "parallel nodes");
        QCOMPARE(gpu::stats().cpuLayers, 0L);
        QVERIFY(edit::addGradeNode(p, s, topId, edit::NodeHow::Layer, 2, &lay).ok);
        for (GradeNode& n : edit::clipById(s, topId)->grade.nodes)
            if (n.kind == "layer") n.blend = {"screen"};
        nodeEffect(lay, "hue_sat", {{"hue", 90.0}});
        gpu::resetStats();
        compare(20, o, "layer nodes");
        QCOMPARE(gpu::stats().cpuLayers, 0L);
        QVERIFY(edit::addKeyMixer(s, topId, {1, 2}, lay).ok);
        nodeEffect(lay, "grade_node", {{"mask.qualify", 1.0}, {"mask.sat_low", 30.0}, {"key_invert", 1.0}, {"key_gain", 80.0}, {"key_mode", 1.0}});
        gpu::resetStats();
        compare(20, o, "keys");
        QCOMPARE(gpu::stats().cpuLayers, 0L);
        QVERIFY(edit::addGradeNode(p, s, topId, edit::NodeHow::Outside, 2, &outside).ok);
        nodeEffect(outside, "invert", {});
        gpu::resetStats();
        compare(20, o, "outside node");
        QCOMPARE(gpu::stats().cpuLayers, 0L);
        // The keyers, which change the alpha too, on V1 (V2 over it, the gaps black): a chroma key (its matte shown, then
        // the result), a luma key through a drawn mask, and a screen key (the result, its matte and its status view, then
        // its matte shrunk, grown and softened, each changing the picture).
        Clip& under = s.videoTracks[0].clips[0];
        auto keyedAway = [&](const QString& what) {  // (the keyer took some of the picture away: the test means something)
            RenderOptions c = o;
            c.gpu = false;
            const Image with = renderProgramFrame(p, s, 20, c);
            const Effect last = under.effects.back();
            under.effects.pop_back();
            const Image without = renderProgramFrame(p, s, 20, c);
            under.effects.push_back(last);
            double diff = 0;
            for (size_t i = 0; i < with.px.size(); ++i) diff += std::fabs(with.px[i] - without.px[i]);
            QVERIFY2(diff / double(with.px.size()) > 0.01, qPrintable(what));
        };
        Effect ck = makeEffect(p, "chroma_key");
        ck.params["key.r"] = 0.5, ck.params["key.g"] = 0.3, ck.params["key.b"] = 0.6;
        ck.params["tolerance"] = 0.2, ck.params["softness"] = 0.15, ck.params["spill"] = 0.5, ck.params["choke"] = 0.3;
        ck.params["show_matte"] = 1.0;
        under.effects.push_back(ck);
        gpu::resetStats();
        compare(20, o, "chroma key matte");
        QCOMPARE(gpu::stats().cpuLayers, 0L);
        under.effects.back().params["show_matte"] = 0.0;
        compare(20, o, "chroma key");
        keyedAway("chroma key");
        Effect lk = makeEffect(p, "luma_key");
        lk.params["threshold"] = 0.45, lk.params["softness"] = 0.1, lk.params["invert"] = 1.0, lk.params["mask.shape"] = 2.0;
        under.effects.push_back(lk);
        compare(20, o, "luma key");
        keyedAway("luma key");
        under.effects.erase(under.effects.end() - 2, under.effects.end());
        Effect sk = makeEffect(p, "screen_key");
        sk.params["key.r"] = 0.3, sk.params["key.g"] = 0.35, sk.params["key.b"] = 0.85;
        sk.params["clip_black"] = 0.1, sk.params["clip_white"] = 0.9, sk.params["edge_desat"] = 40.0;
        under.effects.push_back(sk);
        for (int view : {0, 1, 2}) {
            under.effects.back().params["view"] = double(view);
            gpu::resetStats();
            compare(20, o, QString("screen key view %1").arg(view));
            QCOMPARE(gpu::stats().cpuLayers, 0L);
        }
        under.effects.back().params["view"] = 0.0;
        keyedAway("screen key");
        auto changes = [&](const QString& what, const Effect& before) {  // (the matte step did something)
            RenderOptions c = o;
            c.gpu = false;
            const Image with = renderProgramFrame(p, s, 20, c);
            const Effect now = under.effects.back();
            under.effects.back() = before;
            const Image without = renderProgramFrame(p, s, 20, c);
            under.effects.back() = now;
            double diff = 0;
            for (size_t i = 0; i < with.px.size(); ++i) diff += std::fabs(with.px[i] - without.px[i]);
            QVERIFY2(diff / double(with.px.size()) > 1e-3, qPrintable(what));
        };
        const Effect plain = under.effects.back();
        const std::vector<std::pair<double, double>> morphs = {{3, 0}, {-2, 0}, {0, 6}, {-2, 9}};
        for (const auto& [shrink, soften] : morphs)
            for (int view : {0, 1, 2}) {
                under.effects.back() = plain;
                under.effects.back().params["shrink"] = shrink, under.effects.back().params["soften"] = soften;
                under.effects.back().params["view"] = double(view);
                const QString what = QString("screen key shrink %1 soften %2 view %3").arg(shrink).arg(soften).arg(view);
                gpu::resetStats();
                compare(20, o, what);
                QCOMPARE(gpu::stats().cpuLayers, 0L);
                Effect same = under.effects.back();
                same.params["shrink"] = 0.0, same.params["soften"] = 0.0;
                changes(what, same);
            }
        under.effects.back() = plain;
        under.effects.back().params["soften"] = 6.0, under.effects.back().params["mask.shape"] = 2.0;
        gpu::resetStats();
        compare(20, o, "screen key softened through a mask");
        QCOMPARE(gpu::stats().cpuLayers, 0L);
        // At half size the radii are halved before rounding (shrink 3 to 2, soften 5 to a blur of 1).
        under.effects.back() = plain;
        under.effects.back().params["shrink"] = 3.0, under.effects.back().params["soften"] = 5.0;
        RenderOptions halfSize = o;
        halfSize.scale = 0.5;
        gpu::resetStats();
        compare(20, halfSize, "screen key shrunk and softened at half size");
        QCOMPARE(gpu::stats().cpuLayers, 0L);
        under.effects.pop_back();
        // On V2's picture, partly transparent, after its grade.
        Effect onTop = plain;
        onTop.params["shrink"] = -2.0, onTop.params["soften"] = 4.0;
        edit::clipById(s, topId)->effects.push_back(onTop);
        for (int view : {0, 1, 2}) {
            edit::clipById(s, topId)->effects.back().params["view"] = double(view);
            gpu::resetStats();
            compare(20, o, QString("screen key on a transparent picture, view %1").arg(view));
            QCOMPARE(gpu::stats().cpuLayers, 0L);
        }
        edit::clipById(s, topId)->effects.pop_back();
        // An HDR sequence seen on an SDR display.
        s.colorSpace = "rec2100pq";
        o.displaySpace = "rec709";
        compare(15, o, "pq to rec709");
    }

    // The lined script: pages of the screenplay with a line per take down what it reads, ticks for what the cut uses,
    // red bars for lines nobody read, circled takes ringed; the same pages as a PDF.
    void linedScript() {
        Project p = makeDefaultProject();
        auto take = [&](const std::string& name, const std::vector<std::string>& said) {
            MediaItem m;
            m.id = p.newId();
            m.name = name;
            m.path = "/nonexistent/" + name;
            m.duration = 600;
            m.hasVideo = m.hasAudio = true;
            m.width = 1920, m.height = 1080, m.fps = {25, 1};
            auto t = std::make_shared<Transcript>();
            double at = 1;
            for (const std::string& words : said) {
                TranscriptSegment seg;
                seg.start = at;
                seg.text = words;
                for (const QString& w : QString::fromStdString(words).split(' ')) {
                    seg.words.push_back({at, at + 0.35, w.toStdString(), -1, {}});
                    at += 0.4;
                }
                seg.end = at;
                at += 1;
                t->segments.push_back(seg);
            }
            m.transcript = t;
            p.media.push_back(m);
            return m.id;
        };
        // Forty speeches (over two pages); the first take reads the even ones and the second all but the last.
        std::string script = "Title: Lined\n\nINT. HALL - DAY\n\nPeople talk.\n\n";
        std::vector<std::string> even, most;
        for (int i = 0; i < 40; ++i) {
            std::string line;  // (words of its own, so no line reads as another)
            for (const char* w : {"a", "b", "c", "d", "e", "f"}) line += (line.empty() ? "" : " ") + std::string("word") + std::to_string(i) + w;
            script += (i % 2 ? "MARY\n" : "JOHN\n") + line + ".\n\n";
            if (i % 2 == 0) even.push_back(line);
            if (i < 39) most.push_back(line);
        }
        const Id a = take("a.mov", even), b = take("b.mov", most);
        p.findMedia(a)->metadata["scene"] = "12";
        p.findMedia(a)->metadata["take"] = "3";
        p.findMedia(b)->metadata["circled"] = "Yes";
        p.findMedia(b)->metadata["camera"] = "B";
        QCOMPARE(setProjectScript(p, "lined.fountain", "", script), 39);
        const auto res = buildScriptCut(p, matchScript(p, readScreenplay(p.script).lines), "Cut");
        QVERIFY(res.sequence);
        const LinedScriptLayout l = layoutLinedScript(p, p.findSequence(res.sequence));
        QCOMPARE(l.title, QString("Lined"));
        QVERIFY(l.pages >= 2);
        for (const auto& r : l.rows) {
            QVERIFY(r.page >= 0 && r.page < l.pages);
            QVERIFY(r.x >= 0 && r.x + r.text.size() * 7.2 <= 520);
            QVERIFY(r.y >= 60 && r.y + LinedScriptLayout::kRow <= LinedScriptLayout::kPageHeight - 30);
        }
        // No cue is left alone at the foot of a page: its speech follows it.
        for (size_t i = 0; i < l.rows.size(); ++i)
            if (l.rows[i].kind == ScriptBlock::Kind::Character) {
                QVERIFY(i + 1 < l.rows.size());
                QCOMPARE(l.rows[i + 1].page, l.rows[i].page);
            }
        // Both takes on the first page, the first labelled by scene and take, the second circled.
        std::vector<const LinedScriptLayout::Column*> first;
        for (const auto& c : l.columns)
            if (c.page == 0) first.push_back(&c);
        QCOMPARE(first.size(), size_t(2));
        QCOMPARE(first[0]->label, QString("12-3"));
        QVERIFY(!first[0]->circled);
        QCOMPARE(first[1]->label, QString("b.mov"));
        QVERIFY(first[1]->circled);
        QVERIFY(first[0]->x > 500 && first[1]->x > first[0]->x);
        // Line 0 is read by both (the cut uses one), line 1 by the second take only, line 39 by none.
        auto segmentsOf = [&](int line) {
            std::vector<const LinedScriptLayout::Segment*> out;
            for (const auto& sg : l.segments)
                if (sg.line == line) out.push_back(&sg);
            return out;
        };
        const auto line0 = segmentsOf(0);
        QCOMPARE(line0.size(), size_t(2));
        QCOMPARE(int(std::count_if(line0.begin(), line0.end(), [](auto* sg) { return sg->inCut; })), 1);
        QCOMPARE(segmentsOf(1).size(), size_t(1));
        QCOMPARE(l.columns[size_t(segmentsOf(1)[0]->column)].media, b);
        QVERIFY(segmentsOf(39).empty());
        auto markOf = [&](int line) {
            for (const auto& m : l.marks)
                if (m.line == line) return m.state;
            return 99;
        };
        QCOMPARE(markOf(0), 1);
        QCOMPARE(markOf(39), -1);
        // A point on a line's row finds the line; one on a take's stretch finds that reading.
        const LinedScriptLayout::Segment* s1 = segmentsOf(1)[0];
        QCOMPARE(l.lineAt(s1->page, {200, (s1->y0 + s1->y1) / 2}), 1);
        QCOMPARE(l.lineAt(s1->page, {200, 20}), -1);
        const LinedScriptLayout::Segment* hit = l.segmentAt(s1->page, {l.columns[size_t(s1->column)].x + 2, (s1->y0 + s1->y1) / 2});
        QVERIFY(hit);
        QCOMPARE(hit->line, 1);
        QCOMPARE(hit->reading, 0);
        // The page as painted: the text in black, the takes' lines in their colours, a red bar by line 39.
        auto pageImage = [&](int page) {
            QImage img(int(LinedScriptLayout::kPageWidth), int(LinedScriptLayout::kPageHeight), QImage::Format_RGB32);
            QPainter painter(&img);
            paintLinedScriptPage(painter, l, page);
            return img;
        };
        const QImage page0 = pageImage(0);
        auto inked = [](const QImage& img, QRect r) {
            int n = 0;
            for (int y = r.top(); y <= r.bottom(); ++y)
                for (int x = r.left(); x <= r.right(); ++x) n += qGray(img.pixel(x, y)) < 160;
            return n;
        };
        QVERIFY(inked(page0, QRect(54, 72, 400, 400)) > 500);  // the script
        const QPointF c1(l.columns[size_t(s1->column)].x, (s1->y0 + s1->y1) / 2);
        QVERIFY(qGray(page0.pixel(c1.toPoint())) < 200);
        int last = -1;
        for (const auto& m : l.marks)
            if (m.line == 39) last = m.page;
        const QImage lastPage = pageImage(last);
        bool red = false;
        for (const auto& m : l.marks)
            if (m.line == 39) {
                const QColor px = lastPage.pixelColor(54 - 13, int((m.y0 + m.y1) / 2));
                red = px.red() > 150 && px.green() < 100;
            }
        QVERIFY(red);
        // The PDF: one page each.
        QTemporaryDir dir;
        const QString pdf = dir.filePath("lined.pdf");
        QString error;
        QVERIFY2(exportLinedScriptPdf(l, pdf, &error), qPrintable(error));
        QFile f(pdf);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        QVERIFY(bytes.startsWith("%PDF"));
        QCOMPARE(int(bytes.count("/Type /Page\n") + bytes.count("/Type /Page ") + bytes.count("/Type /Page/") + bytes.count("/Type /Page>")), l.pages);
        // No script: nothing to lay out or write.
        Project none = makeDefaultProject();
        QCOMPARE(layoutLinedScript(none, nullptr).pages, 0);
        QVERIFY(!exportLinedScriptPdf(layoutLinedScript(none, nullptr), dir.filePath("none.pdf")));
    }

    // A frame the GPU cannot draw (an adjustment layer) falls back to the CPU whole, and a clip it cannot grade (a
    // mask with Refine Edges) is made on the CPU and composited there; either way the picture is the CPU's.
    void gpuFallback() {
        std::string why;
        if (!gpu::initialize(&why)) QSKIP(why.c_str());
        Project p = makeDefaultProject();
        Sequence& s = *p.active();
        s.width = 96;
        s.height = 54;
        edit::overwrite(p, s, {TrackKind::Video, 0}, colorClip(p, 0.8f, 0.3f, 0.1f, 0, 30));
        Clip masked = colorClip(p, 0.1f, 0.4f, 0.9f, 0, 30);
        masked.motion.params["scale"] = 50.0;
        Effect cc = makeEffect(p, "color_correct");
        cc.params["saturation"] = 0.0;
        cc.params["mask.shape"] = 1.0;
        cc.params["mask.x"] = 0.5;
        cc.params["mask.y"] = 0.5;
        cc.params["mask.w"] = 0.5;
        cc.params["mask.h"] = 0.5;
        cc.params["mask.refine"] = 50.0;
        masked.effects.push_back(cc);
        const Id maskedId = masked.id;
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 1}, masked).ok);
        RenderOptions o;
        auto same = [&](FrameTime t) {
            o.gpu = false;
            const Image cpu = renderProgramFrame(p, s, t, o);
            o.gpu = true;
            const Image onGpu = renderProgramFrame(p, s, t, o);
            double worst = 0;
            for (int y = 0; y < cpu.height; ++y)
                for (int x = 0; x < cpu.width; ++x)
                    for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(double(cpu.at(x, y)[c]) - onGpu.at(x, y)[c]));
            return worst;
        };
        gpu::resetStats();
        QVERIFY(same(5) <= 2e-3);
        QCOMPARE(gpu::stats().gpuFrames, 1L);
        QVERIFY(gpu::stats().cpuLayers >= 1);
        QVERIFY(edit::clipById(s, maskedId));
        Clip adj = makeGeneratorClip(p, "adjustment", 30);
        adj.effects.push_back(makeEffect(p, "invert"));
        QVERIFY(edit::overwrite(p, s, {TrackKind::Video, 2}, adj).ok);
        gpu::resetStats();
        QVERIFY(same(5) <= 1e-6);
        QCOMPARE(gpu::stats().cpuFrames, 1L);
        QVERIFY(!gpu::stats().lastFallback.empty());
    }
};

QTEST_MAIN(TestRender)
#include "test_render.moc"

// (AI models run on the processor in tests, so results do not depend on the machine's GPU: media/Inference.h.)
[[maybe_unused]] static const bool kCpuInference = (qputenv("MONTAGE_INFERENCE", "cpu"), true);
