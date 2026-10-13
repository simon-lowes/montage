#include "Gallery.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <mutex>
#include <set>

#include "Compositor.h"
#include "core/GradeGraph.h"
#include "core/GradeVersions.h"
#include "core/History.h"
#include "core/ProjectIO.h"

namespace montage {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

// An album's name as a folder: no separators, no leading dot.
bool validAlbum(const std::string& name) {
    const QString n = qs(name).trimmed();
    return !n.isEmpty() && n == qs(name) && !n.startsWith('.') && !n.contains('/') && !n.contains('\\') && n != "..";
}

QJsonObject objectOf(const std::string& json) { return QJsonDocument::fromJson(QByteArray::fromStdString(json)).object(); }

QString stillJson(const std::string& folder, const GalleryStill& s) { return qs(folder) + "/" + qs(s.album) + "/" + qs(s.id) + ".json"; }

bool writeStill(const std::string& folder, const GalleryStill& s, std::string* error) {
    // The grade as a clip of its own (the project file's form), with what a clip does not hold beside it.
    Clip graded;
    graded.effects = s.effects;
    graded.grade = s.graph;
    QJsonArray anchors, shared;
    for (int a : s.anchors) anchors.append(a);
    for (const SharedNode& n : s.sharedNodes) {
        QJsonArray effects;
        for (const Effect& e : n.effects) effects.append(objectOf(effectToJsonString(e)));
        shared.append(QJsonObject{{"id", double(n.id)}, {"name", qs(n.name)}, {"effects", effects}});
    }
    const QJsonObject o{{"label", qs(s.label)},       {"picture", QFileInfo(qs(s.picture)).fileName()},
                        {"clip", qs(s.clip)},         {"sequence", qs(s.sequence)},
                        {"timecode", qs(s.timecode)}, {"created", qs(s.created)},
                        {"width", s.width},           {"height", s.height},
                        {"grade", objectOf(clipToJsonString(graded))},
                        {"anchors", anchors},         {"shared_nodes", shared}};
    // (Whole or not at all: a still whose JSON is cut short is lost.)
    QSaveFile f(stillJson(folder, s));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson()) <= 0 || !f.commit()) {
        if (error) *error = "Cannot write " + stillJson(folder, s).toStdString();
        return false;
    }
    return true;
}

bool readStill(const QString& json, const std::string& album, GalleryStill& out) {
    QFile f(json);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.isEmpty()) return false;
    GalleryStill s;
    s.id = QFileInfo(json).completeBaseName().toStdString();
    s.album = album;
    s.label = o.value("label").toString().toStdString();
    // Beside the JSON whatever it says (removing a still removes its picture).
    QString picture = QFileInfo(o.value("picture").toString()).fileName();
    if (picture.isEmpty() || picture.startsWith('.')) picture = QString::fromStdString(s.id) + ".png";
    s.picture = (QFileInfo(json).absolutePath() + "/" + picture).toStdString();
    s.clip = o.value("clip").toString().toStdString();
    s.sequence = o.value("sequence").toString().toStdString();
    s.timecode = o.value("timecode").toString().toStdString();
    s.created = o.value("created").toString().toStdString();
    s.width = o.value("width").toInt();
    s.height = o.value("height").toInt();
    Clip graded;
    if (!clipFromJsonString(QJsonDocument(o.value("grade").toObject()).toJson(QJsonDocument::Compact).toStdString(), graded)) return false;
    s.effects = graded.effects;
    s.graph = graded.grade;
    for (const QJsonValue& a : o.value("anchors").toArray()) s.anchors.push_back(a.toInt());
    for (const QJsonValue& v : o.value("shared_nodes").toArray()) {
        const QJsonObject n = v.toObject();
        SharedNode node;
        node.id = Id(n.value("id").toDouble());
        node.name = n.value("name").toString().toStdString();
        for (const QJsonValue& e : n.value("effects").toArray()) {
            Effect effect;
            if (effectFromJsonString(QJsonDocument(e.toObject()).toJson(QJsonDocument::Compact).toStdString(), effect))
                node.effects.push_back(effect);
        }
        s.sharedNodes.push_back(node);
    }
    out = std::move(s);
    return true;
}

// What a shared node does, its ids aside.
std::string nodeContent(const SharedNode& n) {
    std::string k = n.name;
    for (Effect e : n.effects) {
        e.id = 0;
        k += '\n' + effectToJsonString(e);
    }
    return k;
}

// The stills grabbed while the project had no file, by this run (moveUntitledStills).
std::mutex untitledMutex;
std::set<std::pair<std::string, std::string>> untitledStills;  // album, id

}  // namespace

std::string galleryFolder(const std::string& projectPath) {
    if (projectPath.empty())
        return (QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/Montage/Gallery/Untitled").toStdString();
    const QFileInfo fi(qs(projectPath));
    return (fi.absolutePath() + "/Gallery/" + fi.completeBaseName()).toStdString();
}

std::vector<std::string> galleryAlbums(const std::string& folder) {
    std::vector<std::pair<qint64, std::string>> found;
    for (const QFileInfo& d : QDir(qs(folder)).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        found.push_back({d.birthTime().isValid() ? d.birthTime().toMSecsSinceEpoch() : d.lastModified().toMSecsSinceEpoch(),
                         d.fileName().toStdString()});
    std::sort(found.begin(), found.end());
    std::vector<std::string> albums{kDefaultAlbum};
    for (const auto& [when, name] : found)
        if (name != kDefaultAlbum) albums.push_back(name);
    return albums;
}

bool addAlbum(const std::string& folder, const std::string& name, std::string* error) {
    if (!validAlbum(name)) {
        if (error) *error = "An album needs a name without slashes";
        return false;
    }
    if (!QDir().mkpath(qs(folder) + "/" + qs(name))) {
        if (error) *error = "Cannot make " + folder + "/" + name;
        return false;
    }
    return true;
}

std::vector<GalleryStill> galleryStills(const std::string& folder, const std::string& album) {
    std::vector<GalleryStill> stills;
    if (!validAlbum(album)) return stills;
    const QDir dir(qs(folder) + "/" + qs(album));
    for (const QFileInfo& f : dir.entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
        GalleryStill s;
        if (readStill(f.absoluteFilePath(), album, s)) stills.push_back(std::move(s));
    }
    // (Ids begin with the time they were grabbed.)
    std::stable_sort(stills.begin(), stills.end(), [](const GalleryStill& a, const GalleryStill& b) { return a.created < b.created; });
    return stills;
}

bool findStill(const std::string& folder, const std::string& album, const std::string& idOrLabel, GalleryStill& out) {
    const std::vector<GalleryStill> stills = galleryStills(folder, album);
    for (const GalleryStill& s : stills)
        if (s.id == idOrLabel) {
            out = s;
            return true;
        }
    // A label names a still only when one has it.
    const GalleryStill* found = nullptr;
    for (const GalleryStill& s : stills)
        if (s.label == idOrLabel) {
            if (found) return false;
            found = &s;
        }
    if (found) out = *found;
    return found;
}

bool grabStill(const Project& p, const Sequence& s, FrameTime t, const std::string& folder, const std::string& album,
               GalleryStill& out, std::string* error, int maxWidth) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (!validAlbum(album)) return fail("An album needs a name without slashes");
    // The clip it shows: the top one with a picture there (a track matte's hidden source is not it).
    const Clip* clip = nullptr;
    const std::vector<bool> mattes = hiddenMatteTracks(s, t);
    for (int i = int(s.videoTracks.size()) - 1; i >= 0 && !clip; --i)
        if (!s.videoTracks[size_t(i)].muted && !mattes[size_t(i)])
            if (const Clip* c = edit::clipAt(s, TrackRef{TrackKind::Video, i}, t); c && c->enabled) clip = c;
    if (!clip) return fail("No clip at the playhead to grab a still of");
    if (!QDir().mkpath(qs(folder) + "/" + qs(album))) return fail("Cannot make " + folder + "/" + album);

    RenderOptions o;
    o.scale = std::min(1.0, double(std::max(16, maxWidth)) / std::max(1, s.width));
    o.highQuality = true;
    o.displaySpace = "rec709";
    const Image view = renderProgramFrame(p, s, t, o);
    if (view.width <= 0 || view.height <= 0) return fail("Nothing to grab");
    QImage picture(view.width, view.height, QImage::Format_RGBA8888);
    toRgba8(view, picture.bits(), size_t(picture.bytesPerLine()));

    GalleryStill still;
    // (Each grab a millisecond after the last at least, so stills grabbed together keep their order.)
    static std::mutex clockLock;
    static qint64 lastGrab = 0;
    QDateTime now = QDateTime::currentDateTime();
    {
        std::lock_guard lock(clockLock);
        if (now.toMSecsSinceEpoch() <= lastGrab) now = QDateTime::fromMSecsSinceEpoch(lastGrab + 1);
        lastGrab = now.toMSecsSinceEpoch();
    }
    still.id = (now.toString("yyyyMMdd-HHmmss-zzz") + QStringLiteral("-%1").arg(QRandomGenerator::global()->bounded(0x10000), 4, 16, QLatin1Char('0')))
                   .toStdString();
    still.album = album;
    int last = 0;
    for (const GalleryStill& g : galleryStills(folder, album)) last = std::max(last, QString::fromStdString(g.label).toInt());
    still.label = std::to_string(last + 1);
    still.picture = (qs(folder) + "/" + qs(album) + "/" + qs(still.id) + ".png").toStdString();
    still.clip = clip->name;
    still.sequence = s.name;
    still.timecode = formatTimecode(t, s.fps);
    still.created = now.toString(Qt::ISODateWithMs).toStdString();
    still.width = view.width;
    still.height = view.height;
    gradeOf(*clip, still.effects, still.anchors);
    still.graph = clip->grade;
    // (The shared nodes its grade shows, not its versions'.)
    for (const SharedNode& n : sharedNodesUsed(s, *clip))
        if (std::any_of(clip->grade.nodes.begin(), clip->grade.nodes.end(), [&](const GradeNode& g) { return g.shared == n.id; }))
            still.sharedNodes.push_back(n);
    if (!picture.save(qs(still.picture), "PNG")) return fail("Cannot write " + still.picture);
    if (!writeStill(folder, still, error)) {
        QFile::remove(qs(still.picture));
        return false;
    }
    if (folder == galleryFolder("")) {
        const std::lock_guard<std::mutex> lock(untitledMutex);
        untitledStills.insert({album, still.id});
    }
    out = std::move(still);
    return true;
}

int moveUntitledStills(const std::string& folder) {
    const std::string from = galleryFolder("");
    if (folder == from) return 0;
    const std::lock_guard<std::mutex> lock(untitledMutex);
    int moved = 0;
    for (auto it = untitledStills.begin(); it != untitledStills.end();) {
        const auto& [album, id] = *it;
        const QString src = qs(from) + "/" + qs(album) + "/" + qs(id), dst = qs(folder) + "/" + qs(album) + "/" + qs(id);
        if (QFileInfo::exists(src + ".json") && QDir().mkpath(qs(folder) + "/" + qs(album)) && !QFileInfo::exists(dst + ".json") &&
            QFile::rename(src + ".json", dst + ".json")) {
            QFile::rename(src + ".png", dst + ".png");
            ++moved;
        }
        it = untitledStills.erase(it);
    }
    return moved;
}

void forgetUntitledStills() {
    const std::lock_guard<std::mutex> lock(untitledMutex);
    untitledStills.clear();
}

bool removeStill(const std::string& folder, const GalleryStill& still) {
    const bool json = QFile::remove(stillJson(folder, still));
    QFile::remove(qs(still.picture));
    return json;
}

bool renameStill(const std::string& folder, GalleryStill& still, const std::string& label, std::string* error) {
    if (label.empty()) return false;
    for (const GalleryStill& g : galleryStills(folder, still.album))
        if (g.id != still.id && (g.label == label || g.id == label)) {
            if (error) *error = "Another still is called " + label;
            return false;
        }
    GalleryStill renamed = still;
    renamed.label = label;
    if (!writeStill(folder, renamed, error)) return false;
    still = std::move(renamed);
    return true;
}

namespace edit {

Result applyStillGrade(Project& p, Sequence& s, const GalleryStill& still, const std::vector<Id>& clips) {
    Result res;
    std::string why;
    // The still's shared nodes (those its graph shows) once for them all: the sequence's own when one does just the
    // same (the same node, or a copy from an earlier apply), else a copy of the still's, fresh ids and all, so one of
    // another project's (whose ids could be anything's here) or one changed since stays as the still has it.
    std::map<Id, Id> sharedIds;
    auto adopt = [&] {
        for (const SharedNode& n : still.sharedNodes) {
            if (sharedIds.count(n.id) ||
                std::none_of(still.graph.nodes.begin(), still.graph.nodes.end(), [&](const GradeNode& g) { return g.shared == n.id; }))
                continue;
            const std::string content = nodeContent(n);
            const SharedNode* same = findSharedNode(s, n.id);
            if (same && nodeContent(*same) != content) same = nullptr;
            for (size_t i = 0; i < s.sharedNodes.size() && !same; ++i)
                if (nodeContent(s.sharedNodes[i]) == content) same = &s.sharedNodes[i];
            if (same) {
                sharedIds[n.id] = same->id;
                continue;
            }
            SharedNode copy = n;
            copy.id = p.newId();
            for (Effect& e : copy.effects) e.id = p.newId();
            sharedIds[n.id] = copy.id;
            s.sharedNodes.push_back(std::move(copy));
        }
    };
    bool adopted = false;
    for (Id id : clips) {
        const auto loc = locate(s, id);
        if (!loc || loc->track.kind != TrackKind::Video) continue;
        const Track* t = trackAt(s, loc->track);
        if (!t || t->locked) {
            why = "Its track is locked";
            continue;
        }
        if (!adopted) adopt(), adopted = true;
        const Result r = replaceGrade(p, s, id, still.effects, still.anchors, still.graph, sharedIds);
        if (r.ok) ++res.applied;
        else why = r.error;
    }
    if (res.applied == 0) return Result::fail(why.empty() ? "No picture clips to grade" : why);
    return res;
}

}  // namespace edit

}  // namespace montage
