#include "VfxPull.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/EditOps.h"
#include "core/Effects.h"

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

}  // namespace

bool exportVfxPulls(const Project& p, const Sequence& seq, const std::vector<Id>& clips, const VfxPullOptions& o, std::vector<VfxShot>* out,
                    const ExportProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (o.format != "exr" && o.format != "dpx" && o.format != "tiff") return fail("Pulls are OpenEXR, DPX or TIFF");
    if (!QDir().mkpath(QString::fromStdString(o.folder))) return fail("Cannot make the folder " + o.folder);
    // The shots: footage clips on video tracks, in timeline order.
    std::vector<const Clip*> chosen;
    for (const Track& t : seq.videoTracks)
        for (const Clip& c : t.clips)
            if (std::find(clips.begin(), clips.end(), c.id) != clips.end() && c.mediaId) {
                const MediaItem* m = p.findMedia(c.mediaId);
                if (m && m->kind == MediaKind::Video && m->hasVideo) chosen.push_back(&c);
            }
    std::sort(chosen.begin(), chosen.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
    if (chosen.empty()) return fail("Choose clips of footage to pull (titles and other generated clips have no source frames)");
    std::vector<VfxShot> shots;
    std::vector<std::string> used;
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
        alone.videoTracks[0].clips.push_back(plate);
        // Names: the clip's, numbered when two shots share one.
        std::string name = safeName(c.name.empty() ? m.name : c.name);
        if (std::count(used.begin(), used.end(), name)) name += "_" + std::to_string(n + 1);
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
        if (!QDir().mkpath(QString::fromStdString(shot.folder))) return fail("Cannot make the folder " + shot.folder);
        ExportSettings st;
        st.videoCodec = o.format;
        st.audioCodec = "none";
        st.smartRender = false;
        st.colorSpace = o.colorSpace;
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
        std::string why;
        if (!exportSequence(p, alone, st, prog, cancel, &why)) return fail(name + ": " + why);
        shots.push_back(shot);
    }
    // The pull list.
    std::string csv = "Shot,Source File,Source In (s),Source Out (s),Head Handle,Tail Handle,First Frame,Cut In,Cut Out,Last Frame,Folder\n";
    char num[64];
    for (const VfxShot& s : shots) {
        csv += csvField(s.name) + "," + csvField(s.media) + ",";
        std::snprintf(num, sizeof num, "%.6f,%.6f,", s.sourceIn, s.sourceOut);
        csv += num;
        csv += std::to_string(s.headHandle) + "," + std::to_string(s.tailHandle) + "," + std::to_string(s.firstFrame) + "," +
               std::to_string(s.cutIn) + "," + std::to_string(s.cutOut) + "," + std::to_string(s.lastFrame) + "," + csvField(s.folder) + "\n";
    }
    QFile list(QDir(QString::fromStdString(o.folder)).filePath(QStringLiteral("pull_list.csv")));
    if (!list.open(QIODevice::WriteOnly | QIODevice::Truncate) || list.write(csv.data(), qint64(csv.size())) != qint64(csv.size()))
        return fail("Cannot write the pull list");
    if (out) *out = std::move(shots);
    return true;
}

}  // namespace montage
