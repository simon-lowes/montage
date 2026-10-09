#include "Versions.h"

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <map>
#include <numeric>

#include "core/EditOps.h"
#include "render/ClipAnalysis.h"

namespace montage {

bool parseVersionShape(const std::string& text, VersionShape& out) {
    const QString t = QString::fromStdString(text).trimmed().toLower().replace('x', ':');
    const QStringList parts = t.split(':');
    if (parts.size() != 2) return false;
    bool okW = false, okH = false;
    const int w = parts[0].toInt(&okW), h = parts[1].toInt(&okH);
    if (!okW || !okH || w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
    const int g = std::gcd(w, h);  // a frame size (1920x1080) reduces to its shape
    if (w / g > 64 || h / g > 64) return false;
    out.aspectW = w / g, out.aspectH = h / g;
    out.label = std::to_string(out.aspectW) + "x" + std::to_string(out.aspectH);
    return true;
}

std::vector<VersionShape> standardVersionShapes() { return {{16, 9, "16x9"}, {9, 16, "9x16"}, {4, 5, "4x5"}, {1, 1, "1x1"}}; }

bool makeVersionSequences(Project& p, Id source, const std::vector<VersionShape>& shapes, std::vector<Id>& out, int speed,
                          const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    out.clear();
    const Sequence* src = p.findSequence(source);
    if (!src || src->duration() == 0) {
        if (error) *error = "The sequence is empty";
        return false;
    }
    const int g0 = std::gcd(src->width, src->height);
    const int srcW = src->width / std::max(1, g0), srcH = src->height / std::max(1, g0);
    // Each shot's subject, found once for all the shapes that need reframing.
    std::map<Id, std::vector<ReframeKey>> paths;
    const bool any = std::any_of(shapes.begin(), shapes.end(), [&](const VersionShape& v) { return v.aspectW * srcH != v.aspectH * srcW; });
    if (any && !analyzeSequenceReframe(p, *src, speed, paths, progress, cancel, error)) return false;
    for (const VersionShape& v : shapes) {
        src = p.findSequence(source);
        if (v.aspectW * srcH == v.aspectH * srcW) {  // already that shape
            out.push_back(source);
            continue;
        }
        int w = 0, h = 0;
        reframeSize(*src, v.aspectW, v.aspectH, w, h);
        const std::string name = src->name + " " + v.label;
        // A version made before is replaced (with its sequence media).
        for (const Sequence& old : p.sequences)
            if (old.name == name && old.id != source) {
                const Id gone = old.id;
                std::erase_if(p.media, [&](const MediaItem& m) { return m.kind == MediaKind::Sequence && m.sequenceId == gone; });
                std::erase_if(p.sequences, [&](const Sequence& s) { return s.id == gone; });
                break;
            }
        const Id made = makeReframedSequence(p, source, w, h, paths, name);
        if (!made) {
            if (error) *error = "The sequence could not be copied";
            return false;
        }
        // Tall frames: captions clear of the platforms' buttons and description.
        if (Sequence* s = p.findSequence(made); s && h > w)
            for (CaptionTrack& t : s->captionTracks) t.style.position = std::min(t.style.position, 0.75);
        out.push_back(made);
    }
    return true;
}

ExportSettings versionSettings(const Sequence& version, const ExportSettings& base, const std::string& folder, bool burnCaptions,
                               double loudnessLufs) {
    ExportSettings st = base;
    st.width = st.height = 0;  // the version's own size
    QString ext = QFileInfo(QString::fromStdString(base.path)).suffix();
    if (ext.isEmpty()) ext = QStringLiteral("mp4");
    QString file = QString::fromStdString(version.name);
    for (QChar& c : file)
        if (QStringLiteral("\\/:*?\"<>|").contains(c)) c = '-';
    st.path = QDir(QString::fromStdString(folder)).filePath(file + '.' + ext).toStdString();
    st.burnInCaptions = burnCaptions && std::any_of(version.captionTracks.begin(), version.captionTracks.end(),
                                                    [](const CaptionTrack& t) { return t.visible && !t.captions.empty(); });
    st.loudnessTarget = loudnessLufs;
    return st;
}

}  // namespace montage
