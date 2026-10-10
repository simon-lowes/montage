#include "Production.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "EditOps.h"

namespace montage {

namespace {

const char* const kMarker = "production.json";

}  // namespace

bool isProduction(const std::string& folder) { return QFileInfo(QDir(QString::fromStdString(folder)).filePath(kMarker)).isFile(); }

bool createProduction(const std::string& folder, const std::string& name, std::string* error) {
    const QDir dir(QString::fromStdString(folder));
    if (!QDir().mkpath(dir.absolutePath())) {
        if (error) *error = "Cannot make " + folder;
        return false;
    }
    if (isProduction(folder)) return true;
    QSaveFile f(dir.filePath(kMarker));
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = "Cannot write in " + folder;
        return false;
    }
    const QJsonObject j{{"name", QString::fromStdString(name.empty() ? dir.dirName().toStdString() : name)},
                        {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                        {"app", "Montage"}};
    f.write(QJsonDocument(j).toJson());
    if (!f.commit()) {
        if (error) *error = "Cannot write in " + folder;
        return false;
    }
    return true;
}

std::string productionName(const std::string& folder) {
    QFile f(QDir(QString::fromStdString(folder)).filePath(kMarker));
    if (f.open(QIODevice::ReadOnly)) {
        const QString n = QJsonDocument::fromJson(f.readAll()).object().value("name").toString();
        if (!n.isEmpty()) return n.toStdString();
    }
    return QDir(QString::fromStdString(folder)).dirName().toStdString();
}

std::string productionOf(const std::string& projectPath) {
    QDir dir = QFileInfo(QString::fromStdString(projectPath)).absoluteDir();
    for (int up = 0; up < 16; ++up) {
        if (isProduction(dir.absolutePath().toStdString())) return dir.absolutePath().toStdString();
        if (!dir.cdUp()) break;
    }
    return {};
}

std::vector<ProductionProject> listProduction(const std::string& folder) {
    std::vector<ProductionProject> out;
    const QDir root(QString::fromStdString(folder));
    QDirIterator it(root.absolutePath(), {"*.montage"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        if (fi.fileName().startsWith('.')) continue;
        ProductionProject pp;
        pp.path = fi.absoluteFilePath().toStdString();
        pp.relative = root.relativeFilePath(fi.absoluteFilePath()).toStdString();
        pp.name = fi.completeBaseName().toStdString();
        pp.modified = fi.lastModified();
        pp.lock = projectLockStatus(pp.path);
        out.push_back(std::move(pp));
    }
    std::sort(out.begin(), out.end(), [](const ProductionProject& a, const ProductionProject& b) {
        return QString::fromStdString(a.relative).compare(QString::fromStdString(b.relative), Qt::CaseInsensitive) < 0;
    });
    return out;
}

std::vector<Id> importFromProject(Project& into, const Project& from, const std::vector<Id>& sequences) {
    // The sequences needed, nested ones before those that play them.
    std::vector<Id> order;
    std::set<Id> seen;
    std::function<void(Id, int)> visit = [&](Id id, int depth) {
        const Sequence* s = from.findSequence(id);
        if (!s || seen.count(id) || depth > 64) return;
        seen.insert(id);
        for (const auto* list : {&s->videoTracks, &s->audioTracks})
            for (const Track& t : *list)
                for (const Clip& c : t.clips)
                    if (const MediaItem* m = c.mediaId ? from.findMedia(c.mediaId) : nullptr; m && m->kind == MediaKind::Sequence)
                        visit(m->sequenceId, depth + 1);
        order.push_back(id);
    };
    for (Id id : sequences) visit(id, 0);
    if (order.empty()) return {};

    // The media they play: the project's own item for a file it has, else a copy with a new id.
    std::map<Id, Id> media;
    std::function<Id(Id)> bring = [&](Id id) -> Id {
        if (const auto it = media.find(id); it != media.end()) return it->second;
        const MediaItem* m = from.findMedia(id);
        if (!m || m->kind == MediaKind::Sequence) return 0;
        if (!m->path.empty())
            for (const MediaItem& own : into.media)
                if (own.kind != MediaKind::Sequence && own.path == m->path && own.subclipOf == 0 && m->subclipOf == 0) return media[id] = own.id;
        MediaItem copy = *m;
        copy.id = into.newId();
        media[id] = copy.id;
        if (m->subclipOf) copy.subclipOf = bring(m->subclipOf);
        if (!copy.bin.empty() && std::find(into.bins.begin(), into.bins.end(), copy.bin) == into.bins.end()) into.bins.push_back(copy.bin);
        into.media.push_back(std::move(copy));
        return media[id];
    };

    std::map<Id, Id> sequenceMedia;  // the source project's sequence -> the new sequence's item in the bin
    std::map<Id, Id> made;           // the source project's sequence -> the new sequence
    for (Id id : order) {
        Sequence s = *from.findSequence(id);
        for (auto* list : {&s.videoTracks, &s.audioTracks})
            for (Track& t : *list)
                for (Clip& c : t.clips) {
                    if (!c.mediaId) continue;
                    const MediaItem* m = from.findMedia(c.mediaId);
                    if (m && m->kind == MediaKind::Sequence) {
                        const auto it = sequenceMedia.find(m->sequenceId);
                        c.mediaId = it == sequenceMedia.end() ? 0 : it->second;
                    } else {
                        c.mediaId = bring(c.mediaId);
                    }
                }
        // In under a temporary id, then copied with new ids for everything in it (tracks, clips, links, effects).
        s.id = into.newId();
        const Id temp = s.id;
        into.sequences.push_back(std::move(s));
        const Id fresh = edit::duplicateSequence(into, temp, from.findSequence(id)->name);
        into.sequences.erase(std::remove_if(into.sequences.begin(), into.sequences.end(), [&](const Sequence& q) { return q.id == temp; }),
                             into.sequences.end());
        if (!fresh) continue;
        made[id] = fresh;
        for (MediaItem& m : into.media)
            if (m.kind == MediaKind::Sequence && m.sequenceId == fresh) {
                sequenceMedia[id] = m.id;
                for (const MediaItem& o : from.media)
                    if (o.kind == MediaKind::Sequence && o.sequenceId == id) m.bin = o.bin;
                if (!m.bin.empty() && std::find(into.bins.begin(), into.bins.end(), m.bin) == into.bins.end()) into.bins.push_back(m.bin);
            }
    }
    std::vector<Id> out;
    for (Id id : sequences)
        if (const auto it = made.find(id); it != made.end()) out.push_back(it->second);
    return out;
}

}  // namespace montage
