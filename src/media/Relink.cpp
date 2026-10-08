#include "Relink.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <map>

#include "Decoder.h"

namespace montage {

namespace {

// The file name of a path saved on any system ("D:\Shoot\A001.mxf" too).
QString fileNameOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return QString::fromStdString(slash == std::string::npos ? path : path.substr(slash + 1));
}

bool visual(MediaKind k) { return k == MediaKind::Video || k == MediaKind::Image; }

bool passes(const MediaItem& was, const MediaItem& now, RelinkCheck check, std::string* why) {
    auto fail = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    if (check == RelinkCheck::Replace) {
        if (visual(was.kind) != visual(now.kind)) return fail(visual(was.kind) ? "It has no picture" : "It is not a sound file");
        return true;
    }
    if (was.kind != now.kind) return fail("It is a different kind of media");
    if (was.width > 0 && now.width > 0 && (was.width != now.width || was.height != now.height))
        return fail("Its picture size is " + std::to_string(now.width) + " x " + std::to_string(now.height) + ", not " +
                    std::to_string(was.width) + " x " + std::to_string(was.height));
    if (was.duration > 0 && now.duration > 0 && std::fabs(was.duration - now.duration) > std::max(0.1, 0.005 * was.duration))
        return fail("It is a different length");
    return true;
}

// The file's details onto the item, keeping what the editor added.
void takeDetails(MediaItem& m, const MediaItem& n, bool replace) {
    m.path = n.path;
    m.kind = n.kind;
    m.width = n.width;
    m.height = n.height;
    m.fps = n.fps;
    m.hasVideo = n.hasVideo;
    m.hasAudio = n.hasAudio;
    m.sampleRate = n.sampleRate;
    m.channels = n.channels;
    m.videoCodec = n.videoCodec;
    m.audioCodec = n.audioCodec;
    m.colorSpace = n.colorSpace;
    if (!m.subclipOf) m.duration = n.duration;
    if (replace) {
        m.timecode = n.timecode;
        m.created = n.created;
        m.proxyPath.clear();
        m.transcript.reset();
        m.visual.reset();
        m.faces.reset();
        if (!m.subclipOf) m.name = n.name;
    }
}

}  // namespace

bool isOffline(const MediaItem& m) {
    return m.kind != MediaKind::Sequence && !m.path.empty() && !QFileInfo::exists(QString::fromStdString(m.path));
}

std::vector<Id> offlineMedia(const Project& p) {
    std::vector<Id> ids;
    for (const MediaItem& m : p.media)
        if (!m.subclipOf && isOffline(m)) ids.push_back(m.id);
    return ids;
}

bool relinkMedia(Project& p, Id id, const std::string& path, RelinkCheck check, std::string* why) {
    MediaItem* m = nullptr;
    for (MediaItem& it : p.media)
        if (it.id == id) m = &it;
    if (m && m->subclipOf) return relinkMedia(p, m->subclipOf, path, check, why);  // a subclip's file is its media's
    if (!m || m->kind == MediaKind::Sequence) {
        if (why) *why = "No such media";
        return false;
    }
    const QFileInfo fi(QString::fromStdString(path));
    if (!fi.isFile()) {
        if (why) *why = "The file is not there";
        return false;
    }
    MediaItem n;
    std::string err;
    if (!probeMedia(QDir::cleanPath(fi.absoluteFilePath()).toStdString(), n, &err)) {
        if (why) *why = err.empty() ? "It cannot be read" : err;
        return false;
    }
    if (!passes(*m, n, check, why)) return false;
    const bool replace = check == RelinkCheck::Replace;
    for (MediaItem& it : p.media)
        if (it.id == id || it.subclipOf == id) takeDetails(it, n, replace);
    return true;
}

std::vector<Id> relinkFromFolder(Project& p, const std::string& folder, std::vector<Id> ids, int depth) {
    if (ids.empty()) ids = offlineMedia(p);
    std::vector<Id> relinked;
    const QDir root(QString::fromStdString(folder));
    if (ids.empty() || !root.exists()) return relinked;
    // Every file down to `depth`, by lower-case name and by lower-case name without its extension.
    std::multimap<QString, QString> byName, byBase;
    QDirIterator it(root.absolutePath(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    const int rootDepth = int(root.absolutePath().count('/'));
    while (it.hasNext()) {
        const QString f = it.next();
        if (f.count('/') - rootDepth - 1 > depth) continue;
        const QFileInfo fi(f);
        byName.emplace(fi.fileName().toLower(), f);
        byBase.emplace(fi.completeBaseName().toLower(), f);
    }
    for (Id id : ids) {
        const MediaItem* m = p.findMedia(id);
        if (!m || !isOffline(*m)) continue;
        const QString name = fileNameOf(m->path).toLower();
        std::vector<QString> candidates;
        for (auto [a, b] = byName.equal_range(name); a != b; ++a) candidates.push_back(a->second);
        if (candidates.empty())
            for (auto [a, b] = byBase.equal_range(QFileInfo(name).completeBaseName()); a != b; ++a) candidates.push_back(a->second);
        std::sort(candidates.begin(), candidates.end(), [](const QString& a, const QString& b) {
            return a.count('/') != b.count('/') ? a.count('/') < b.count('/') : a < b;  // nearest the folder first
        });
        for (const QString& c : candidates)
            if (relinkMedia(p, id, c.toStdString(), RelinkCheck::Strict)) {
                relinked.push_back(id);
                break;
            }
    }
    return relinked;
}

}  // namespace montage
