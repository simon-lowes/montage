#include "VfxPull.h"
#include "VfxShots.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Amf.h"
#include "ColorSpace.h"
#include "Compositor.h"
#include "Processing.h"
#include "VideoFx.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/Interpretation.h"
#include "media/DepthMap.h"
#include "media/VideoDepth.h"
#include "media/DeepImage.h"
#include "media/Exr.h"
#include "media/Image.h"
#include "media/LensCalibration.h"
#include "media/Matting.h"
#include "media/MediaPool.h"
#include "media/Proxy.h"

namespace montage {

namespace {

// A name safe for files: letters, digits, dash and underscore.
std::string safeName(const std::string& in) {
    QString s = QString::fromStdString(in);
    s.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")), QStringLiteral("_"));
    s.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
    return s.isEmpty() ? std::string("shot") : s.toStdString();
}

std::string csvField(const std::string& v) {
    if (v.find_first_of(",\"\n") == std::string::npos) return v;
    std::string out = "\"";
    for (char c : v) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

// `from` (w x h, n channels) in a w2 x h2 picture, from its top left corner; the rest 0.
std::vector<float> placed(const std::vector<float>& from, int w, int h, size_t n, int w2, int h2) {
    if (w == w2 && h == h2) return from;
    std::vector<float> out(size_t(w2) * size_t(h2) * n, 0.0f);
    for (int y = 0; y < std::min(h, h2); ++y)
        std::copy_n(&from[size_t(y) * size_t(w) * n], size_t(std::min(w, w2)) * n, &out[size_t(y) * size_t(w2) * n]);
    return out;
}

// `from` (w x h, n channels) stretched to w2 x h2 (an anamorphic render's stored width to its shown width): linearly,
// or for ids (a Cryptomatte's) the nearest sample.
std::vector<float> stretched(const std::vector<float>& from, int w, int h, size_t n, int w2, int h2, bool nearest) {
    if (w == w2 && h == h2) return from;
    if (w <= 0 || h <= 0) return std::vector<float>(size_t(w2) * size_t(h2) * n, 0.0f);
    std::vector<float> out(size_t(w2) * size_t(h2) * n);
    auto at = [&](int x, int y) { return &from[(size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(std::clamp(x, 0, w - 1))) * n]; };
    parallelRows(h2, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const double sy = (y + 0.5) * h / h2 - 0.5;
            for (int x = 0; x < w2; ++x) {
                const double sx = (x + 0.5) * w / w2 - 0.5;
                float* d = &out[(size_t(y) * size_t(w2) + size_t(x)) * n];
                if (nearest) {
                    std::copy_n(at(int(std::lround(sx)), int(std::lround(sy))), n, d);
                    continue;
                }
                const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                const float fx = float(sx - ix), fy = float(sy - iy);
                const float *a = at(ix, iy), *b = at(ix + 1, iy), *c = at(ix, iy + 1), *e = at(ix + 1, iy + 1);
                for (size_t k = 0; k < n; ++k) d[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + e[k] * fx) * fy;
            }
        }
    });
    return out;
}

// The layers an OpenEXR pull adds to each frame: the source render's others, and the clip's masks as mattes.
struct PullLayers {
    const Project* project = nullptr;
    const Sequence* seq = nullptr;
    const Clip* clip = nullptr;
    const MediaItem* media = nullptr;
    int64_t first = 0;  // the media frame of the pull's first
    double mediaFps = 25;
    bool sourceLayers = false;
    std::vector<std::pair<const Effect*, std::string>> mattes;
    std::vector<std::string>* names = nullptr;  // the layers written, from the first frame

    bool operator()(FrameTime f, int W, int H, std::vector<ExrOutLayer>& layers, std::map<std::string, std::string>& attributes,
                    std::string& error) const {
        const MediaItem& m = *media;
        const double sec = double(first + f) / mediaFps;
        const std::string file = exrFrameFile(m.path, sec);
        std::vector<std::string> listed;  // (as readExrInfo lists them: a Cryptomatte once, by its name)
        auto note = [&](const std::string& n) {
            if (std::find(listed.begin(), listed.end(), n) == listed.end()) listed.push_back(n);
        };
        if (sourceLayers && !file.empty()) {
            int w = 0, h = 0;
            std::vector<ExrCopiedLayer> copied;
            if (!readExrChannels(file, w, h, copied, &attributes, &error)) return false;
            for (ExrCopiedLayer& l : copied) {
                // (the picture the plate shows is in it already; the main picture, when the plate is another layer,
                // goes under "main")
                if (!l.cryptomatte && l.listedAs == clip->layer) continue;
                if (l.layer.name.empty() && !l.cryptomatte && l.listedAs.empty()) l.layer.name = l.listedAs = "main";
                note(l.listedAs);
                // (an anamorphic render's layers are stored squeezed: stretched as its picture is shown)
                // (colour blended; data such as ids, depth and vectors by the nearest sample, not mixed across edges)
                const bool colour = std::all_of(l.layer.channels.begin(), l.layer.channels.end(), [](const std::string& ch) {
                    return ch == "R" || ch == "G" || ch == "B" || ch == "A";
                });
                l.layer.data = stretched(l.layer.data, w, h, l.layer.channels.size(), W, H, l.cryptomatte || !colour);
                layers.push_back(std::move(l.layer));
            }
        }
        if (!mattes.empty()) {
            // The masks as the compositor draws them on this frame of the clip (handles: the clip's ends held).
            const Sequence& s = *seq;
            const Clip& c = *clip;
            const FrameTime lt = FrameTime(std::llround(c.localForSource(sec * s.fpsValue())));
            const int mw = std::max(1, m.width), mh = std::max(1, m.height);
            Frame16Ptr frame = MediaPool::instance().videoFrame(c.layer.empty() ? m.path : layerPath(m.path, c.layer), sec, mw, mh);
            if (!frame) {
                error = "Cannot read the frame at " + std::to_string(sec) + " s of " + m.path;
                return false;
            }
            Image src = toImage(*frame);
            convertColor(src, pictureColorSpace(m, m.path), sequenceColorSpace(s), s.hdrPeakNits);
            // (sizes in sequence pixels to source pixels, as the clip's fit scales them)
            const int fit = int(c.motion.p("fit", lt, 0));
            const double fx = fit == 0 ? std::min(s.width / double(mw), s.height / double(mh))
                              : fit == 1 ? std::max(s.width / double(mw), s.height / double(mh))
                              : fit == 2 ? s.width / double(mw)
                                         : 1.0;
            const double pixelScale = 1 / std::max(1e-6, fx);
            std::shared_ptr<const DepthMap> depth;
            std::shared_ptr<const ValueMap> people;
            for (const auto& [e, name] : mattes) {
                if (!depth && needsDepth(*e, lt)) depth = frameDepth(src, c.layer.empty() && m.kind == MediaKind::Video ? m.path : std::string(), sec);
                if (!people && needsPersonMatte(*e, lt) && mattingAvailable() && mattingModel().installed()) people = cachedPersonMatte(src);
            }
            DepthScope depthScope(depth);
            PersonScope personScope(people);
            CryptomatteScope cryptoScope(file);
            TrackedSourceScope trackedSource(m.id, false);
            for (const auto& [e, name] : mattes) {
                std::vector<float> matte = effectMatte(*e, lt, src, pixelScale, sec);
                if (matte.empty()) matte.assign(size_t(mw) * size_t(mh), 1.0f);  // (no mask on this frame: all of it)
                ExrOutLayer l;
                l.name = name;
                l.channels = {"A"};
                l.data = placed(matte, src.width, src.height, 1, W, H);
                layers.push_back(std::move(l));
                note(name);
            }
        }
        if (names && names->empty()) *names = listed;
        return true;
    }
};

}  // namespace

std::string vfxShotName(const std::string& pattern, const Project& p, const Sequence& seq, const Clip& c) {
    const MediaItem* m = p.findMedia(c.mediaId);
    const std::string clip = c.name.empty() && m ? m->name : c.name;
    if (pattern.empty()) return safeName(clip);
    // ({clip}: a file's name without its extension)
    const std::string clipWord = m && clip == m->name ? QFileInfo(QString::fromStdString(clip)).completeBaseName().toStdString() : clip;
    // Its layer, and its place among the cut's footage clips (by start, then the lower track first).
    std::string layer;
    std::vector<std::pair<FrameTime, size_t>> order;
    std::pair<FrameTime, size_t> mine{c.start, 0};
    for (size_t t = 0; t < seq.videoTracks.size(); ++t)
        for (const Clip& k : seq.videoTracks[t].clips) {
            const MediaItem* km = p.findMedia(k.mediaId);
            if (k.id == c.id) layer = seq.videoTracks[t].name, mine = {k.start, t};
            else if ((km && km->kind == MediaKind::Video && km->hasVideo) || vfxShotOfClip(seq, k.id)) order.emplace_back(k.start, t);
        }
    const long n = std::count_if(order.begin(), order.end(), [&](const auto& o) { return o < mine; });
    char shot[16];
    std::snprintf(shot, sizeof shot, "%03d", int(n + 1) * 10);
    QString out = QString::fromStdString(pattern);
    out.replace(QStringLiteral("{sequence}"), QString::fromStdString(seq.name));
    out.replace(QStringLiteral("{shot}"), QString::fromLatin1(shot));
    out.replace(QStringLiteral("{clip}"), QString::fromStdString(clipWord));
    out.replace(QStringLiteral("{layer}"), QString::fromStdString(layer));
    return safeName(out.toStdString());
}

bool exportVfxPulls(const Project& p, const Sequence& seq, const std::vector<Id>& clips, const VfxPullOptions& o, std::vector<VfxShot>* out,
                    const ExportProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (o.format != "exr" && o.format != "dpx" && o.format != "tiff") return fail("Pulls are OpenEXR, DPX or TIFF");
    if (o.stMaps && !exrAvailable()) return fail("ST maps need Montage built with the OpenEXR library");
    if (!QDir().mkpath(QString::fromStdString(o.folder))) return fail("Cannot make the folder " + o.folder);
    // The shots: footage clips on video tracks, in timeline order. A shot showing a version the vendor sent back is
    // pulled from its plate again (its plate take, where the version is placed against it), not from the version.
    std::vector<Clip> plates;
    for (const Track& t : seq.videoTracks)
        for (const Clip& c : t.clips)
            if (std::find(clips.begin(), clips.end(), c.id) != clips.end() && c.mediaId) {
                Clip plate = c;
                if (const VfxShotRecord* r = vfxShotOfClip(seq, c.id); r && r->plateMedia && c.mediaId != r->plateMedia)
                    for (const Take& take : c.takes)
                        if (take.mediaId == r->plateMedia) {
                            plate.mediaId = take.mediaId;
                            plate.sourceIn += take.offset;
                            plate.takes.clear();
                            break;
                        }
                const MediaItem* m = p.findMedia(plate.mediaId);
                if (m && m->kind == MediaKind::Video && m->hasVideo) plates.push_back(plate);
            }
    std::sort(plates.begin(), plates.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    std::vector<const Clip*> chosen;
    for (const Clip& c : plates) chosen.push_back(&c);
    if (chosen.empty()) return fail("Choose clips of footage to pull (titles and other generated clips have no source frames)");
    std::vector<VfxShot> shots;
    // (names other clips' shots have are not given again: their versions would be taken for this one's)
    std::vector<std::string> used;
    for (const VfxShotRecord& r : seq.vfxShots)
        if (std::none_of(plates.begin(), plates.end(), [&](const Clip& c) { return c.id == r.clip; })) used.push_back(r.name);
    const double seqFps = seq.fpsValue();
    for (size_t n = 0; n < chosen.size(); ++n) {
        const Clip& c = *chosen[n];
        const MediaItem& m = *p.findMedia(c.mediaId);
        const double mf = m.fps.valid() ? m.fps.toDouble() : seqFps;
        // The source the cut uses (the whole span a speed change or ramp covers), in media frames, then the handles.
        const double a = c.sourceIn, b = c.sourceIn + c.sourceExtent();
        const int64_t cutFirst = int64_t(std::floor(std::min(a, b) / seqFps * mf + 1e-6));
        const int64_t cutLast = std::max<int64_t>(cutFirst, int64_t(std::ceil(std::max(a, b) / seqFps * mf - 1e-6)) - 1);
        const int64_t total = m.duration > 0 ? int64_t(std::floor(m.duration * mf + 1e-6)) : cutLast + 1;
        const int head = int(std::min<int64_t>(o.handles, cutFirst));
        const int tail = int(std::clamp<int64_t>(total - 1 - cutLast, 0, o.handles));
        const int64_t first = cutFirst - head, frames = (cutLast + tail) - first + 1;
        // A sequence at the footage's size and rate holding just those frames, untouched.
        // (a DPX in the footage's own colour space, so log stays log; an EXR in linear light).
        Sequence alone = seq;
        alone.name = m.name;
        alone.width = m.width + (m.width & 1);
        alone.height = m.height + (m.height & 1);
        if (m.fps.valid()) alone.fps = m.fps;
        alone.videoTracks.assign(1, Track{});
        alone.videoTracks[0].kind = TrackKind::Video;
        alone.audioTracks.clear();
        alone.buses.clear();
        alone.captionTracks.clear();
        alone.markers.clear();
        Clip plate;
        plate.id = 1;
        plate.mediaId = m.id;
        plate.name = c.name;
        plate.start = 0;
        plate.duration = frames;
        plate.sourceIn = double(first);
        plate.motion = makeEffect("transform", 2);
        plate.layer = c.layer;  // (a multi-layer render's layer the cut shows)
        alone.videoTracks[0].clips.push_back(plate);
        // Names: the clip's or the pattern's, numbered when two shots share one.
        std::string name = vfxShotName(o.namePattern, p, seq, c);
        if (std::count(used.begin(), used.end(), name)) {
            const std::string base = name;
            for (int k = int(n); std::count(used.begin(), used.end(), name); ++k) name = base + "_" + std::to_string(k + 1);
        }
        used.push_back(name);
        VfxShot shot;
        shot.name = name;
        shot.media = m.path;
        shot.folder = (QDir(QString::fromStdString(o.folder)).filePath(QString::fromStdString(name))).toStdString();
        shot.headHandle = head;
        shot.tailHandle = tail;
        shot.cutIn = o.cutIn;
        shot.cutOut = o.cutIn + int(cutLast - cutFirst);
        shot.firstFrame = o.cutIn - head;
        shot.lastFrame = shot.cutOut + tail;
        shot.sourceIn = double(first) / mf;
        shot.sourceOut = double(first + frames) / mf;
        shot.clip = c.id;
        shot.width = alone.width, shot.height = alone.height;
        shot.fps = alone.fps;
        shot.cutSource = double(cutFirst) * seqFps / mf;
        if (!QDir().mkpath(QString::fromStdString(shot.folder))) return fail("Cannot make the folder " + shot.folder);
        ExportSettings st;
        st.videoCodec = o.format;
        st.audioCodec = "none";
        st.smartRender = false;
        st.colorSpace = o.colorSpace;
        if (st.colorSpace.empty() && sceneReferredWorking(seq) && o.format != "tiff") {
            // Graded scene-referred, plates come in the working space: DPX in it (in ACEScct's curve on its primaries when
            // it is linear light, which 10 bits would clip and band; ACEScct itself when the list has no such space),
            // OpenEXR in its light (ACES2065-1 when the list has no linear space on its primaries).
            const ColorSpace& working = workingColorSpace(seq);
            const ColorSpace& light = linearColorSpace(working.primaries);
            const ColorSpace& log = logColorSpace(working.primaries);
            const std::string dpx = working.transfer != Transfer::Linear ? working.id : findColorSpace(log.id) ? log.id : std::string("acescct");
            st.colorSpace = o.format == "dpx" ? dpx : findColorSpace(light.id) ? light.id : std::string("aces2065-1");
        }
        if (st.colorSpace.empty() && o.format == "dpx") st.colorSpace = m.colorOverride.empty() ? m.colorSpace : m.colorOverride;  // log stays log
        st.startNumber = shot.firstFrame;
        const char* ext = o.format == "tiff" ? "tif" : o.format.c_str();
        st.path = QDir(QString::fromStdString(shot.folder)).filePath(QString::fromStdString(name) + QStringLiteral(".%04d.") + ext).toStdString();
        st.in = 0;
        st.out = frames;
        const size_t index = n;
        auto prog = [&, index](double f, FrameTime t) {
            if (progress) progress((double(index) + f) / double(chosen.size()), t);
        };
        if (o.format == "exr") {
            st.exrCompression = o.compression;
            if (o.sourceLayers || o.mattes) {
                if (!exrAvailable()) return fail("Layers and mattes in OpenEXR pulls need Montage built with the OpenEXR library");
                PullLayers pl;
                pl.project = &p;
                pl.seq = &seq;
                pl.clip = &c;
                pl.media = &m;
                pl.first = first;
                pl.mediaFps = mf;
                pl.sourceLayers = o.sourceLayers;
                pl.names = &shot.layers;
                if (o.mattes)
                    for (const Effect* e : liveEffects(seq, c)) {
                        if (!e->enabled || !supportsMask(e->type) || (!hasMask(*e, 0) && !hasMask(*e, std::max<FrameTime>(0, c.duration - 1))))
                            continue;
                        const EffectInfo* info = findEffectInfo(e->type);
                        std::string matte = "matte_" + safeName(info ? info->displayName : e->type);
                        for (int k = 2; std::any_of(pl.mattes.begin(), pl.mattes.end(), [&](const auto& x) { return x.second == matte; }); ++k)
                            matte = "matte_" + safeName(info ? info->displayName : e->type) + "_" + std::to_string(k);
                        pl.mattes.emplace_back(e, matte);
                    }
                st.exrLayers = pl;
            }
        }
        std::string why;
        // A deep render's frames: their samples copied as they are, as deep OpenEXR (compressed as deep data can be).
        const std::string firstFile = o.format == "exr" && c.layer.empty() ? exrFrameFile(m.path, double(first) / mf) : std::string();
        const std::shared_ptr<const ExrInfo> firstInfo = firstFile.empty() ? nullptr : cachedExrInfo(firstFile);
        if (firstInfo && firstInfo->deep) {
            const std::string& cmp = o.compression;
            const std::string compression = cmp == "none" || cmp == "rle" || cmp == "zips" || cmp == "zstd" ? cmp : std::string("zips");
            for (int64_t k = 0; k < frames; ++k) {
                if (cancel && cancel->load()) return fail("Cancelled");
                DeepFrame f;
                const QString file = QDir(QString::fromStdString(shot.folder))
                                         .filePath(QStringLiteral("%1.%2.exr").arg(QString::fromStdString(name)).arg(shot.firstFrame + int(k), 4, 10, QChar('0')));
                bool full = false;
                if (!readDeepExr(exrFrameFile(m.path, double(first + k) / mf), f, &why, &full) ||
                    !writeDeepExr(file.toStdString(), f, compression, false, full, &why))
                    return fail(name + ": " + why);
                prog(double(k + 1) / double(frames), FrameTime(k));
            }
            shot.deep = true;
        } else if (!exportSequence(p, alone, st, prog, cancel, &why)) {
            return fail(name + ": " + why);
        }
        // The frames' space (as the exporter chooses it: EXR any space asked for, else linear Rec.709; DPX the space
        // asked for or the footage's; TIFF a display space asked for, else the sequence's).
        const ColorSpace* chosenSpace = findColorSpace(st.colorSpace);
        const std::string space = o.format == "exr"                                               ? (chosenSpace ? chosenSpace->id : std::string("linear-rec709"))
                                  : chosenSpace && (o.format == "dpx" || !chosenSpace->sceneReferred) ? chosenSpace->id
                                                                                                     : sequenceColorSpace(alone).id;
        // (Video made for a display, pulled from a sequence not graded scene-referred, comes in its display's light: read
        // back as that, its versions show as the plate did.)
        const ColorSpace* written = findColorSpace(space);
        shot.colorSpace = written && written->sceneReferred && !sceneReferredWorking(alone) && !mediaColorSpace(m).sceneReferred
                              ? displayLightOf(*written).id
                              : space;
        shot.plate = QDir(QString::fromStdString(shot.folder)).filePath(QString::fromStdString(name) + QStringLiteral(".####.") + ext).toStdString();
        if (o.amf) {
            // The frames' space as the input transform and the cut's grade as looks, for the frames by their numbers.
            AmfDocument amf;
            if (!clipAmf(p, seq, c, shot.folder, amf, nullptr, space, &why)) return fail(name + ": " + why);
            amf.clipName = name;
            amf.clipFile.clear();
            amf.clipUuid.clear();
            amf.clipSequence = name + ".####." + ext;
            amf.sequenceMin = shot.firstFrame;
            amf.sequenceMax = shot.lastFrame;
            shot.amf = QDir(QString::fromStdString(shot.folder)).filePath(QString::fromStdString(name) + QStringLiteral(".amf")).toStdString();
            const std::string xml = writeAmf(amf);
            QFile f(QString::fromStdString(shot.amf));
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(xml.data(), qint64(xml.size())) != qint64(xml.size()))
                return fail("Cannot write " + shot.amf);
        }
        if (o.stMaps) {
            // (The clip's first Lens Correction that is on, as it is at the cut's first frame.)
            const auto lc = std::find_if(c.effects.begin(), c.effects.end(), [](const Effect& e) { return e.type == "lens_correction" && e.enabled; });
            const auto lensOf = lc != c.effects.end() ? lens::parseLens(lc->s("profile")) : nullptr;
            if (lensOf) {
                StMaps maps;
                if (!makeStMaps(*lensOf, vfx::lensSettings(*lc, 0), alone.width, alone.height, maps, &why) ||
                    !writeStMaps(maps,
                                 shot.undistortMap = QDir(QString::fromStdString(shot.folder)).filePath(QString::fromStdString(name) + "_undistort.exr").toStdString(),
                                 shot.redistortMap = QDir(QString::fromStdString(shot.folder)).filePath(QString::fromStdString(name) + "_redistort.exr").toStdString(),
                                 &why))
                    return fail(name + ": " + why);
                shot.overscanWidth = maps.overscanWidth;
                shot.overscanHeight = maps.overscanHeight;
            }
        }
        if (o.scripts) {
            const std::string returns = o.returns.empty() ? QDir(QString::fromStdString(o.folder)).filePath(QStringLiteral("returns")).toStdString() : o.returns;
            VfxShotRecord r = vfxShotRecord(shot);
            shot.script = QDir(QString::fromStdString(shot.folder)).filePath(QString::fromStdString(name) + QStringLiteral(".nk")).toStdString();
            const std::string nk = nukeScript(r, vfxVersionPath(returns, o.returnsPattern, r, 1, o.format));
            QFile f(QString::fromStdString(shot.script));
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(nk.data(), qint64(nk.size())) != qint64(nk.size()))
                return fail("Cannot write " + shot.script);
        }
        shots.push_back(shot);
    }
    // The pull list.
    std::string csv = "Shot,Source File,Source In (s),Source Out (s),Head Handle,Tail Handle,First Frame,Cut In,Cut Out,Last Frame,Folder,Layers,Overscan,Undistort Map,Redistort Map\n";
    char num[64];
    for (const VfxShot& s : shots) {
        csv += csvField(s.name) + "," + csvField(s.media) + ",";
        std::snprintf(num, sizeof num, "%.6f,%.6f,", s.sourceIn, s.sourceOut);
        csv += num;
        csv += std::to_string(s.headHandle) + "," + std::to_string(s.tailHandle) + "," + std::to_string(s.firstFrame) + "," +
               std::to_string(s.cutIn) + "," + std::to_string(s.cutOut) + "," + std::to_string(s.lastFrame) + "," + csvField(s.folder) + ",";
        std::string layers;
        for (const std::string& l : s.layers) layers += (layers.empty() ? "" : " ") + l;
        csv += csvField(layers) + ",";
        csv += s.overscanWidth > 0 ? std::to_string(s.overscanWidth) + "x" + std::to_string(s.overscanHeight) : std::string();
        csv += "," + csvField(s.undistortMap) + "," + csvField(s.redistortMap) + "\n";
    }
    QFile list(QDir(QString::fromStdString(o.folder)).filePath(QStringLiteral("pull_list.csv")));
    if (!list.open(QIODevice::WriteOnly | QIODevice::Truncate) || list.write(csv.data(), qint64(csv.size())) != qint64(csv.size()))
        return fail("Cannot write the pull list");
    if (out) *out = std::move(shots);
    return true;
}

}  // namespace montage
