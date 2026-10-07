#include "ProjectIO.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <utility>

namespace montage {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string ss(const QJsonValue& v) { return v.toString().toStdString(); }
// 64-bit ids/frames are stored as doubles (exact up to 2^53).
qint64 i64(const QJsonValue& v, qint64 def = 0) { return v.isDouble() ? qint64(v.toDouble()) : def; }

const char* kindName(MediaKind k) {
    switch (k) {
        case MediaKind::Video: return "video";
        case MediaKind::Audio: return "audio";
        case MediaKind::Image: return "image";
        case MediaKind::Sequence: return "sequence";
    }
    return "video";
}

MediaKind kindFrom(const QString& s) {
    if (s == "audio") return MediaKind::Audio;
    if (s == "image") return MediaKind::Image;
    if (s == "sequence") return MediaKind::Sequence;
    return MediaKind::Video;
}

const char* interpName(Interp i) {
    switch (i) {
        case Interp::Linear: return "linear";
        case Interp::Hold: return "hold";
        case Interp::Smooth: return "smooth";
    }
    return "linear";
}

Interp interpFrom(const QString& s) {
    if (s == "hold") return Interp::Hold;
    if (s == "smooth") return Interp::Smooth;
    return Interp::Linear;
}

QJsonValue paramToJson(const Param& p) {
    if (p.keys.empty()) return p.value;
    QJsonArray keys;
    for (const auto& k : p.keys) keys.append(QJsonArray{double(k.t), k.v, interpName(k.interp)});
    return QJsonObject{{"value", p.value}, {"keys", keys}};
}

Param paramFromJson(const QJsonValue& v) {
    Param p;
    if (v.isDouble()) {
        p.value = v.toDouble();
        return p;
    }
    QJsonObject o = v.toObject();
    p.value = o.value("value").toDouble();
    for (const auto& kv : o.value("keys").toArray()) {
        QJsonArray a = kv.toArray();
        if (a.size() < 2) continue;
        p.keys.push_back(Keyframe{FrameTime(a.at(0).toDouble()), a.at(1).toDouble(),
                                  a.size() > 2 ? interpFrom(a.at(2).toString()) : Interp::Linear});
    }
    std::sort(p.keys.begin(), p.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
    return p;
}

QJsonObject effectToJson(const Effect& e) {
    QJsonObject o{{"id", double(e.id)}, {"type", qs(e.type)}};
    if (!e.enabled) o["enabled"] = false;
    QJsonObject params;
    for (const auto& [k, v] : e.params) params[qs(k)] = paramToJson(v);
    if (!params.isEmpty()) o["params"] = params;
    QJsonObject strings;
    for (const auto& [k, v] : e.strings) strings[qs(k)] = qs(v);
    if (!strings.isEmpty()) o["strings"] = strings;
    return o;
}

Effect effectFromJson(const QJsonValue& v) {
    Effect e;
    QJsonObject o = v.toObject();
    if (o.isEmpty()) return e;
    e.id = Id(i64(o.value("id")));
    e.type = ss(o.value("type"));
    e.enabled = o.value("enabled").toBool(true);
    QJsonObject params = o.value("params").toObject();
    for (auto it = params.begin(); it != params.end(); ++it) e.params[it.key().toStdString()] = paramFromJson(it.value());
    QJsonObject strings = o.value("strings").toObject();
    for (auto it = strings.begin(); it != strings.end(); ++it) e.strings[it.key().toStdString()] = ss(it.value());
    return e;
}

QJsonObject clipToJson(const Clip& c) {
    QJsonObject o{{"id", double(c.id)},
                  {"media", double(c.mediaId)},
                  {"name", qs(c.name)},
                  {"start", double(c.start)},
                  {"duration", double(c.duration)},
                  {"sourceIn", c.sourceIn},
                  {"speed", c.speed}};
    if (c.reverse) o["reverse"] = true;
    if (!c.enabled) o["enabled"] = false;
    if (c.linkGroup) o["link"] = double(c.linkGroup);
    if (c.colorLabel) o["label"] = c.colorLabel;
    if (c.blendMode != "normal") o["blend"] = qs(c.blendMode);
    if (!c.generator.empty()) o["generator"] = effectToJson(c.generator);
    if (!c.motion.empty()) o["motion"] = effectToJson(c.motion);
    if (!c.audio.empty()) o["audio"] = effectToJson(c.audio);
    QJsonArray fx;
    for (const auto& e : c.effects) fx.append(effectToJson(e));
    if (!fx.isEmpty()) o["effects"] = fx;
    return o;
}

Clip clipFromJson(const QJsonObject& o) {
    Clip c;
    c.id = Id(i64(o.value("id")));
    c.mediaId = Id(i64(o.value("media")));
    c.name = ss(o.value("name"));
    c.start = i64(o.value("start"));
    c.duration = std::max<qint64>(1, i64(o.value("duration"), 1));
    c.sourceIn = o.value("sourceIn").toDouble();
    c.speed = o.value("speed").toDouble(1.0);
    if (!(c.speed > 0)) c.speed = 1.0;
    c.reverse = o.value("reverse").toBool(false);
    c.enabled = o.value("enabled").toBool(true);
    c.linkGroup = Id(i64(o.value("link")));
    c.colorLabel = o.value("label").toInt(0);
    c.blendMode = o.contains("blend") ? ss(o.value("blend")) : "normal";
    c.generator = effectFromJson(o.value("generator"));
    c.motion = effectFromJson(o.value("motion"));
    c.audio = effectFromJson(o.value("audio"));
    for (const auto& e : o.value("effects").toArray()) c.effects.push_back(effectFromJson(e));
    return c;
}

QJsonObject trackToJson(const Track& t) {
    QJsonObject o{{"id", double(t.id)}, {"name", qs(t.name)}};
    QJsonArray clips;
    for (const auto& c : t.clips) clips.append(clipToJson(c));
    o["clips"] = clips;
    QJsonArray trs;
    for (const auto& tr : t.transitions) {
        trs.append(QJsonObject{{"id", double(tr.id)},
                               {"type", qs(tr.type)},
                               {"a", double(tr.clipA)},
                               {"b", double(tr.clipB)},
                               {"duration", double(tr.duration)},
                               {"params", effectToJson(tr.params)}});
    }
    if (!trs.isEmpty()) o["transitions"] = trs;
    if (t.muted) o["muted"] = true;
    if (t.solo) o["solo"] = true;
    if (t.locked) o["locked"] = true;
    if (!t.syncLock) o["syncLock"] = false;
    if (t.volumeDb != 0) o["volumeDb"] = t.volumeDb;
    if (t.pan != 0) o["pan"] = t.pan;
    if (t.height) o["height"] = t.height;
    return o;
}

Track trackFromJson(const QJsonObject& o, TrackKind kind) {
    Track t;
    t.kind = kind;
    t.id = Id(i64(o.value("id")));
    t.name = ss(o.value("name"));
    for (const auto& c : o.value("clips").toArray()) t.clips.push_back(clipFromJson(c.toObject()));
    for (const auto& v : o.value("transitions").toArray()) {
        QJsonObject to = v.toObject();
        Transition tr;
        tr.id = Id(i64(to.value("id")));
        tr.type = ss(to.value("type"));
        tr.clipA = Id(i64(to.value("a")));
        tr.clipB = Id(i64(to.value("b")));
        tr.duration = std::max<qint64>(1, i64(to.value("duration"), 15));
        tr.params = effectFromJson(to.value("params"));
        t.transitions.push_back(tr);
    }
    t.muted = o.value("muted").toBool(false);
    t.solo = o.value("solo").toBool(false);
    t.locked = o.value("locked").toBool(false);
    t.syncLock = o.value("syncLock").toBool(true);
    t.volumeDb = o.value("volumeDb").toDouble(0);
    t.pan = o.value("pan").toDouble(0);
    t.height = o.value("height").toInt(0);
    std::sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    return t;
}

QJsonObject captionTrackToJson(const CaptionTrack& t) {
    const CaptionStyle& st = t.style;
    QJsonObject style{{"font", qs(st.font)},
                      {"size", st.size},
                      {"bold", st.bold},
                      {"text", QJsonArray{st.textR, st.textG, st.textB}},
                      {"boxOpacity", st.boxOpacity},
                      {"box", QJsonArray{st.boxR, st.boxG, st.boxB}},
                      {"outline", st.outline},
                      {"position", st.position}};
    QJsonArray items;
    for (const Caption& c : t.captions) items.append(QJsonArray{double(c.start), double(c.end), qs(c.text)});
    return QJsonObject{{"id", double(t.id)},        {"name", qs(t.name)}, {"language", qs(t.language)},
                       {"visible", t.visible},      {"style", style},     {"captions", items}};
}

CaptionTrack captionTrackFromJson(const QJsonObject& o) {
    CaptionTrack t;
    t.id = Id(i64(o.value("id")));
    t.name = ss(o.value("name"));
    t.language = ss(o.value("language"));
    t.visible = o.value("visible").toBool(true);
    const QJsonObject so = o.value("style").toObject();
    CaptionStyle& st = t.style;
    const CaptionStyle def;
    st.font = so.contains("font") ? ss(so.value("font")) : def.font;
    st.size = so.value("size").toDouble(def.size);
    st.bold = so.value("bold").toBool(def.bold);
    const QJsonArray tc = so.value("text").toArray(), bc = so.value("box").toArray();
    if (tc.size() == 3) st.textR = tc.at(0).toDouble(), st.textG = tc.at(1).toDouble(), st.textB = tc.at(2).toDouble();
    if (bc.size() == 3) st.boxR = bc.at(0).toDouble(), st.boxG = bc.at(1).toDouble(), st.boxB = bc.at(2).toDouble();
    st.boxOpacity = so.value("boxOpacity").toDouble(def.boxOpacity);
    st.outline = so.value("outline").toDouble(def.outline);
    st.position = so.value("position").toDouble(def.position);
    for (const auto& v : o.value("captions").toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() < 3) continue;
        t.captions.push_back({i64(a.at(0)), i64(a.at(1)), ss(a.at(2))});
    }
    normalizeCaptions(t.captions);
    return t;
}

QJsonObject sequenceToJson(const Sequence& s) {
    QJsonObject o{{"id", double(s.id)},
                  {"name", qs(s.name)},
                  {"width", s.width},
                  {"height", s.height},
                  {"fps", QJsonArray{s.fps.num, s.fps.den}},
                  {"sampleRate", s.sampleRate},
                  {"in", double(s.inPoint)},
                  {"out", double(s.outPoint)},
                  {"playhead", double(s.playhead)}};
    QJsonArray v, a, m;
    for (const auto& t : s.videoTracks) v.append(trackToJson(t));
    for (const auto& t : s.audioTracks) a.append(trackToJson(t));
    for (const auto& mk : s.markers)
        m.append(QJsonObject{{"t", double(mk.t)},
                             {"duration", double(mk.duration)},
                             {"name", qs(mk.name)},
                             {"comment", qs(mk.comment)},
                             {"color", mk.color}});
    o["video"] = v;
    o["audio"] = a;
    o["markers"] = m;
    if (!s.captionTracks.empty()) {
        QJsonArray c;
        for (const auto& t : s.captionTracks) c.append(captionTrackToJson(t));
        o["captions"] = c;
    }
    return o;
}

Sequence sequenceFromJson(const QJsonObject& o) {
    Sequence s;
    s.id = Id(i64(o.value("id")));
    s.name = ss(o.value("name"));
    s.width = std::max(16, o.value("width").toInt(1920));
    s.height = std::max(16, o.value("height").toInt(1080));
    QJsonArray fps = o.value("fps").toArray();
    if (fps.size() == 2) s.fps = Rational{fps.at(0).toInt(30), fps.at(1).toInt(1)};
    if (!s.fps.valid()) s.fps = Rational{30, 1};
    s.sampleRate = o.value("sampleRate").toInt(48000);
    s.inPoint = i64(o.value("in"), -1);
    s.outPoint = i64(o.value("out"), -1);
    s.playhead = i64(o.value("playhead"), 0);
    for (const auto& t : o.value("video").toArray()) s.videoTracks.push_back(trackFromJson(t.toObject(), TrackKind::Video));
    for (const auto& t : o.value("audio").toArray()) s.audioTracks.push_back(trackFromJson(t.toObject(), TrackKind::Audio));
    for (const auto& mv : o.value("markers").toArray()) {
        QJsonObject mo = mv.toObject();
        Marker mk;
        mk.t = i64(mo.value("t"));
        mk.duration = i64(mo.value("duration"));
        mk.name = ss(mo.value("name"));
        mk.comment = ss(mo.value("comment"));
        mk.color = mo.value("color").toInt(0);
        s.markers.push_back(mk);
    }
    for (const auto& c : o.value("captions").toArray()) s.captionTracks.push_back(captionTrackFromJson(c.toObject()));
    return s;
}

}  // namespace

std::string projectToJson(const Project& p, const std::string& projectPath) {
    QDir base = projectPath.empty() ? QDir() : QFileInfo(qs(projectPath)).absoluteDir();
    QJsonObject root{{"format", "montage-project"},
                     {"version", kProjectFormatVersion},
                     {"name", qs(p.name)},
                     {"activeSequence", double(p.activeSequence)},
                     {"nextId", double(p.nextId)}};
    QJsonArray media;
    for (const auto& m : p.media) {
        QJsonObject o{{"id", double(m.id)},
                      {"kind", kindName(m.kind)},
                      {"name", qs(m.name)},
                      {"path", qs(m.path)},
                      {"duration", m.duration},
                      {"width", m.width},
                      {"height", m.height},
                      {"fps", QJsonArray{m.fps.num, m.fps.den}},
                      {"hasVideo", m.hasVideo},
                      {"hasAudio", m.hasAudio},
                      {"sampleRate", m.sampleRate},
                      {"channels", m.channels},
                      {"videoCodec", qs(m.videoCodec)},
                      {"audioCodec", qs(m.audioCodec)}};
        if (!m.path.empty() && !projectPath.empty()) o["relPath"] = base.relativeFilePath(qs(m.path));
        if (!m.proxyPath.empty()) o["proxy"] = qs(m.proxyPath);
        if (m.sequenceId) o["sequence"] = double(m.sequenceId);
        if (!m.bin.empty()) o["bin"] = qs(m.bin);
        if (m.transcript && !m.transcript->empty())
            o["transcript"] = QJsonDocument::fromJson(QByteArray::fromStdString(transcriptToJson(*m.transcript))).object();
        media.append(o);
    }
    root["media"] = media;
    QJsonArray seqs;
    for (const auto& s : p.sequences) seqs.append(sequenceToJson(s));
    root["sequences"] = seqs;
    return QJsonDocument(root).toJson(QJsonDocument::Indented).toStdString();
}

bool projectFromJson(const std::string& json, Project& out, std::string* error, const std::string& projectPath) {
    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &perr);
    if (doc.isNull() || !doc.isObject()) {
        if (error) *error = "Invalid project file: " + perr.errorString().toStdString();
        return false;
    }
    QJsonObject root = doc.object();
    if (root.value("format").toString() != "montage-project") {
        if (error) *error = "Not a Montage project";
        return false;
    }
    if (root.value("version").toInt() > kProjectFormatVersion) {
        if (error) *error = "Project was saved by a newer version of Montage";
        return false;
    }
    QDir base = projectPath.empty() ? QDir() : QFileInfo(qs(projectPath)).absoluteDir();
    Project p;
    p.name = ss(root.value("name"));
    p.activeSequence = Id(i64(root.value("activeSequence")));
    p.nextId = Id(i64(root.value("nextId"), 1));
    for (const auto& mv : root.value("media").toArray()) {
        QJsonObject o = mv.toObject();
        MediaItem m;
        m.id = Id(i64(o.value("id")));
        m.kind = kindFrom(o.value("kind").toString());
        m.name = ss(o.value("name"));
        m.path = ss(o.value("path"));
        // Relink: prefer the relative path if the absolute one is gone.
        if (!m.path.empty() && !QFileInfo::exists(qs(m.path)) && o.contains("relPath") && !projectPath.empty()) {
            QString rel = base.absoluteFilePath(o.value("relPath").toString());
            if (QFileInfo::exists(rel)) m.path = QDir::cleanPath(rel).toStdString();
        }
        m.proxyPath = ss(o.value("proxy"));
        if (o.contains("transcript")) {
            Transcript t;
            if (transcriptFromJson(QJsonDocument(o.value("transcript").toObject()).toJson(QJsonDocument::Compact).toStdString(), t))
                m.transcript = std::make_shared<const Transcript>(std::move(t));
        }
        m.duration = o.value("duration").toDouble();
        m.width = o.value("width").toInt();
        m.height = o.value("height").toInt();
        QJsonArray fps = o.value("fps").toArray();
        if (fps.size() == 2) m.fps = Rational{fps.at(0).toInt(), fps.at(1).toInt(1)};
        m.hasVideo = o.value("hasVideo").toBool();
        m.hasAudio = o.value("hasAudio").toBool();
        m.sampleRate = o.value("sampleRate").toInt();
        m.channels = o.value("channels").toInt();
        m.videoCodec = ss(o.value("videoCodec"));
        m.audioCodec = ss(o.value("audioCodec"));
        m.sequenceId = Id(i64(o.value("sequence")));
        m.bin = ss(o.value("bin"));
        p.media.push_back(m);
    }
    for (const auto& sv : root.value("sequences").toArray()) p.sequences.push_back(sequenceFromJson(sv.toObject()));
    if (p.sequences.empty()) {
        if (error) *error = "Project has no sequences";
        return false;
    }
    if (!p.findSequence(p.activeSequence)) p.activeSequence = p.sequences.front().id;
    // Guarantee nextId is above every id in the file.
    Id maxId = 0;
    auto bump = [&](Id id) { maxId = std::max(maxId, id); };
    for (const auto& m : p.media) bump(m.id);
    for (const auto& s : p.sequences) {
        bump(s.id);
        for (const auto* list : {&s.videoTracks, &s.audioTracks})
            for (const auto& t : *list) {
                bump(t.id);
                for (const auto& c : t.clips) {
                    bump(c.id);
                    bump(c.linkGroup);
                    bump(c.motion.id);
                    bump(c.audio.id);
                    bump(c.generator.id);
                    for (const auto& e : c.effects) bump(e.id);
                }
                for (const auto& tr : t.transitions) {
                    bump(tr.id);
                    bump(tr.params.id);
                }
            }
    }
    p.nextId = std::max(p.nextId, maxId + 1);
    out = std::move(p);
    return true;
}

bool saveProject(const Project& p, const std::string& path, std::string* error) {
    QSaveFile f(qs(path));
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString().toStdString();
        return false;
    }
    std::string json = projectToJson(p, path);
    f.write(json.data(), qint64(json.size()));
    if (!f.commit()) {
        if (error) *error = f.errorString().toStdString();
        return false;
    }
    return true;
}

bool loadProject(const std::string& path, Project& out, std::string* error) {
    QFile f(qs(path));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString().toStdString();
        return false;
    }
    QByteArray data = f.readAll();
    return projectFromJson(data.toStdString(), out, error, path);
}

}  // namespace montage
