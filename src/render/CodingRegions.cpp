// Montage — Regions Coded Finer (see CodingRegions.h).
#include "render/CodingRegions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/Effects.h"
#include "media/Faces.h"
#include "render/Compositor.h"
#include "render/EncodeCompare.h"
#include "render/Exporter.h"
#include "render/Processing.h"

namespace montage {

bool encoderTakesRegions(const std::string& c) { return c == "libx264" || c == "libx265" || c == "libvpx-vp9"; }

double codingRegionOffset(double strength) { return -0.2 * std::clamp(strength, 0.0, 1.0); }

bool checkCodingRegions(const std::vector<CodingRegion>& regions, std::string* error) {
    std::string why;
    for (const CodingRegion& r : regions) {
        if (r.kind != "box" && r.kind != "mask" && r.kind != "faces") why = "A region coded finer is a box, a clip's masks or faces, not \"" + r.kind + "\"";
        else if (!(r.strength > 0 && r.strength <= 1)) why = "A region's strength is over 0 and at most 1";
        else if (r.kind == "box" && !(r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 && r.x + r.w <= 1 + 1e-9 && r.y + r.h <= 1 + 1e-9))
            why = "A box coded finer lies within the frame and is not empty";
        if (!why.empty()) break;
    }
    if (!why.empty() && error) *error = why;
    return why.empty();
}

namespace {

// A rectangle (sequence pixels, scaled by sx and sy to the coded picture) clipped to it; false when nothing is left.
bool codedRect(double x0, double y0, double x1, double y1, double sx, double sy, int W, int H, double qoffset, CodedRegion& out) {
    out.left = std::clamp(int(std::floor(x0 * sx)), 0, W), out.right = std::clamp(int(std::ceil(x1 * sx)), 0, W);
    out.top = std::clamp(int(std::floor(y0 * sy)), 0, H), out.bottom = std::clamp(int(std::ceil(y1 * sy)), 0, H);
    out.qoffset = qoffset;
    return out.right > out.left && out.bottom > out.top;
}

}  // namespace

std::vector<CodedRegion> codedRegionsAt(const Project& p, const Sequence& seq, const std::vector<CodingRegion>& regions, FrameTime f, int W, int H,
                                        const Image* picture) {
    std::vector<std::pair<double, CodedRegion>> found;  // (strength, region)
    if (W <= 0 || H <= 0 || seq.width <= 0 || seq.height <= 0) return {};
    const double sx = double(W) / seq.width, sy = double(H) / seq.height;
    std::shared_ptr<const std::vector<FaceBox>> faces;
    bool facesLooked = false;
    for (const CodingRegion& r : regions) {
        const double q = codingRegionOffset(r.strength);
        CodedRegion c;
        if (r.kind == "box") {
            if (codedRect(r.x * seq.width, r.y * seq.height, (r.x + r.w) * seq.width, (r.y + r.h) * seq.height, sx, sy, W, H, q, c))
                found.push_back({r.strength, c});
        } else if (r.kind == "faces") {
            if (!picture || picture->width <= 0 || picture->height <= 0) continue;
            if (!facesLooked) faces = facesInPicture(*picture), facesLooked = true;
            if (!faces) continue;
            for (const FaceBox& b : *faces) {
                // (a tenth larger each way: the hair line and the chin)
                const double mx = 0.1 * b.w, my = 0.1 * b.h;
                if (codedRect((b.x - mx) * W / sx, (b.y - my) * H / sy, (b.x + b.w + mx) * W / sx, (b.y + b.h + my) * H / sy, sx, sy, W, H, q, c))
                    found.push_back({r.strength, c});
            }
        } else if (r.kind == "mask") {
            for (const Track& t : seq.videoTracks) {
                if (t.muted) continue;
                for (const Clip& clip : t.clips) {
                    if ((r.clip && clip.id != r.clip) || !clip.enabled || f < clip.start || f >= clip.end()) continue;
                    double cw = 0, ch = 0;
                    if (!clipFrameSize(p, seq, clip, cw, ch) || cw <= 0 || ch <= 0) continue;
                    const FrameTime lt = f - clip.start;
                    for (const Effect& e : clip.effects) {
                        if (!e.enabled || (r.effect && e.id != r.effect)) continue;
                        // (an inverted mask is everything but its shape: no region of its own)
                        if (e.p("mask.invert", lt) > 0.5) continue;
                        const std::vector<std::array<double, 2>> outline = maskOutline(e, lt, int(std::lround(cw)), int(std::lround(ch)));
                        if (outline.empty()) continue;
                        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
                        bool placed = true;
                        for (const auto& pt : outline) {
                            double X = 0, Y = 0;
                            if (!clipFrameToSequence(p, seq, clip, f, pt[0] / cw, pt[1] / ch, X, Y)) {
                                placed = false;
                                break;
                            }
                            x0 = std::min(x0, X), y0 = std::min(y0, Y), x1 = std::max(x1, X), y1 = std::max(y1, Y);
                        }
                        if (!placed) continue;
                        // (grown by the mask's own expansion, in the clip's pixels taken as the sequence's)
                        const double grow = std::max(0.0, e.p("mask.expansion", lt));
                        if (codedRect(x0 - grow, y0 - grow, x1 + grow, y1 + grow, sx, sy, W, H, q, c)) found.push_back({r.strength, c});
                    }
                }
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<CodedRegion> out;
    for (auto& [s, c] : found) out.push_back(c);
    return out;
}

std::string codingRegionsToJson(const std::vector<CodingRegion>& regions) {
    QJsonArray a;
    for (const CodingRegion& r : regions) {
        QJsonObject o{{"kind", QString::fromStdString(r.kind)}, {"strength", r.strength}};
        if (r.kind == "box") o["x"] = r.x, o["y"] = r.y, o["w"] = r.w, o["h"] = r.h;
        if (r.kind == "mask") {
            if (r.clip) o["clip"] = double(r.clip);
            if (r.effect) o["effect"] = double(r.effect);
        }
        a.append(o);
    }
    return QJsonDocument(a).toJson(QJsonDocument::Compact).toStdString();
}

bool codingRegionsFromJson(const std::string& json, std::vector<CodingRegion>& out, std::string* error) {
    out.clear();
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(json), &pe);
    if (pe.error != QJsonParseError::NoError || !d.isArray()) {
        if (error) *error = "Regions coded finer are a list of boxes, masks and faces";
        return false;
    }
    for (const QJsonValue& v : d.array()) {
        if (!v.isObject()) {
            if (error) *error = "Each region coded finer is an object";
            return false;
        }
        const QJsonObject o = v.toObject();
        CodingRegion r;
        r.kind = o.value("kind").toString("box").toStdString();
        r.strength = o.value("strength").toDouble(0.5);
        r.x = o.value("x").toDouble(0), r.y = o.value("y").toDouble(0), r.w = o.value("w").toDouble(1), r.h = o.value("h").toDouble(1);
        r.clip = Id(o.value("clip").toDouble(0)), r.effect = Id(o.value("effect").toDouble(0));
        out.push_back(r);
    }
    return checkCodingRegions(out, error);
}

std::string RegionReport::summary() const {
    char buf[600];
    std::snprintf(buf, sizeof buf,
                  "Regions coded finer: %.1f %% of the picture over %lld frames. Inside them, PSNR %.2f dB (%.2f without them); outside, %.2f dB "
                  "(%.2f without). %lld bytes (%lld without).",
                  insideShare * 100, (long long)framesWithRegions, psnrInside, psnrInsideWithout, psnrOutside, psnrOutsideWithout, (long long)bytes,
                  (long long)bytesWithout);
    std::string s = buf;
    if (vmaf >= 0 && vmafWithout >= 0) {
        std::snprintf(buf, sizeof buf, " VMAF %.2f (%.2f without).", vmaf, vmafWithout);
        s += buf;
    }
    return s;
}

bool regionPsnr(const std::string& reference, const std::string& coded, const std::vector<std::vector<CodedRegion>>& given, double& inside,
                double& outside, double* insideShare, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    CompareOptions o;
    std::string why;
    PictureSource ref = openPictureFile(reference, nullptr, o, &why);
    if (!ref) return fail(why.empty() ? "Cannot read " + reference : why);
    QualityPicture a;
    if (!ref(a)) return fail(reference + " has no pictures");
    PictureSource enc = openPictureFile(coded, &a, o, &why);
    if (!enc) return fail(why.empty() ? "Cannot read " + coded : why);
    double seIn = 0, seOut = 0;
    int64_t nIn = 0, nOut = 0;
    QualityPicture b;
    size_t k = 0;
    std::vector<uint8_t> in;
    for (bool more = true; more; ++k, more = ref(a)) {
        if (!enc(b)) return fail(coded + " has fewer pictures than " + reference);
        if (!a.sameLayout(b) || a.rgb) return fail("The two files' pictures are not alike");
        const int W = a.width, H = a.height;
        in.assign(size_t(W) * size_t(H), 0);
        if (k < given.size())
            for (const CodedRegion& r : given[k])
                for (int y = std::max(0, r.top); y < std::min(H, r.bottom); ++y)
                    std::fill(in.begin() + y * W + std::max(0, r.left), in.begin() + y * W + std::min(W, r.right), uint8_t(1));
        for (size_t i = 0; i < in.size(); ++i) {
            const double e = double(a.plane[0][i]) - double(b.plane[0][i]);
            if (in[i]) seIn += e * e, ++nIn;
            else seOut += e * e, ++nOut;
        }
    }
    const double peak = double(a.peak());
    auto psnr = [&](double se, int64_t n) { return n == 0 ? 0.0 : se <= 0 ? 100.0 : 10 * std::log10(peak * peak / (se / double(n))); };
    inside = psnr(seIn, nIn), outside = psnr(seOut, nOut);
    if (insideShare) *insideShare = nIn + nOut > 0 ? double(nIn) / double(nIn + nOut) : 0;
    return true;
}

bool regionCodingReport(const Project& p, const Sequence& seq, const ExportSettings& s, const std::vector<std::vector<CodedRegion>>& given,
                        RegionReport& out, const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    out = RegionReport{};
    const QFileInfo fi(QString::fromStdString(s.path));
    const std::string without = fi.dir().filePath(fi.completeBaseName() + ".without-regions." + fi.suffix()).toStdString();
    const std::string reference = fi.dir().filePath(fi.completeBaseName() + ".regions-reference.mkv").toStdString();
    struct Remove {
        std::vector<std::string> paths;
        ~Remove() {
            for (const std::string& f : paths) QFile::remove(QString::fromStdString(f));
        }
    } remove{{without, reference}};
    ExportSettings plain = s;
    plain.path = without;
    plain.regions.clear();
    if (s.videoCodec == "libvpx-vp9") {
        // (VP9 with regions codes in its real-time mode: the export without them in the same mode, given a region
        // that never lies anywhere, the mask of a clip there is none of)
        CodingRegion none;
        none.kind = "mask", none.clip = std::numeric_limits<Id>::max();
        plain.regions = {none};
    }
    plain.regionsGiven = {};
    plain.credentialCertificate.clear(), plain.credentialKey.clear();
    plain.loudnessReport = false;
    std::string why;
    if (!exportSequence(p, seq, plain, {}, cancel, &why)) return fail(why.empty() ? "Cannot make the export without its regions" : why);
    // (the picture both were coded from, losslessly: the same size, sampling, depth and colour)
    int W = 0, H = 0;
    codedPictureSize(seq, s, W, H);
    ExportSettings ref;
    ref.path = reference;
    ref.videoCodec = "ffv1";
    ref.audioCodec = "none";
    ref.width = W, ref.height = H;
    ref.in = s.in, ref.out = s.out;
    ref.colorSpace = s.colorSpace, ref.toneMapping = s.toneMapping;
    ref.useProxies = s.useProxies;
    ref.pixFmt = s.pixFmt.empty() ? (s.videoCodec == "libx264" ? "yuv420p" : "") : s.pixFmt;
    ref.chapters = false, ref.embedCaptions = false;
    ref.burnIn = s.burnIn, ref.burnInCaptions = s.burnInCaptions;
    if (ref.pixFmt.empty()) {
        // (the coded file's own sampling and depth)
        CompareOptions o;
        PictureSource src = openPictureFile(s.path, nullptr, o, &why);
        QualityPicture q;
        if (!src || !src(q)) return fail("Cannot read " + s.path + " back");
        ref.pixFmt = q.bits > 8 ? (q.log2ChromaH ? "yuv420p10le" : q.log2ChromaW ? "yuv422p10le" : "yuv444p10le")
                                : (q.log2ChromaH ? "yuv420p" : q.log2ChromaW ? "yuv422p" : "yuv444p");
    }
    if (!exportSequence(p, seq, ref, {}, cancel, &why)) return fail(why.empty() ? "Cannot make the reference picture" : why);
    if (!regionPsnr(reference, s.path, given, out.psnrInside, out.psnrOutside, &out.insideShare, &why)) return fail(why);
    if (!regionPsnr(reference, without, given, out.psnrInsideWithout, out.psnrOutsideWithout, nullptr, &why)) return fail(why);
    out.bytes = QFileInfo(QString::fromStdString(s.path)).size();
    out.bytesWithout = QFileInfo(QString::fromStdString(without)).size();
    for (const auto& g : given) out.framesWithRegions += g.empty() ? 0 : 1;
    if (vmafAvailable()) {
        CompareOptions o;
        o.metrics = MetricVmaf;
        const EncodeComparison a = compareEncodeFiles(reference, s.path, o, {}, cancel);
        const EncodeComparison b = compareEncodeFiles(reference, without, o, {}, cancel);
        if (a.ok && a.vmafScored) out.vmaf = a.mean.vmaf;
        if (b.ok && b.vmafScored) out.vmafWithout = b.mean.vmaf;
    }
    return true;
}

}  // namespace montage
