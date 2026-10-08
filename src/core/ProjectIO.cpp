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
        case Interp::Bezier: return "bezier";
    }
    return "linear";
}

Interp interpFrom(const QString& s) {
    if (s == "hold") return Interp::Hold;
    if (s == "smooth") return Interp::Smooth;
    if (s == "bezier") return Interp::Bezier;
    return Interp::Linear;
}

QJsonValue surroundToJson(const SurroundPan& p) {
    if (p == SurroundPan{}) return QJsonValue();
    return QJsonObject{{"x", p.x}, {"y", p.y}, {"width", p.width}, {"lfeDb", p.lfeDb}};
}

SurroundPan surroundFromJson(const QJsonValue& v) {
    SurroundPan p;
    const QJsonObject o = v.toObject();
    p.x = std::clamp(o.value("x").toDouble(0), -1.0, 1.0);
    p.y = std::clamp(o.value("y").toDouble(1), -1.0, 1.0);
    p.width = std::clamp(o.value("width").toDouble(1), 0.0, 1.0);
    p.lfeDb = std::clamp(o.value("lfeDb").toDouble(-100), -100.0, 12.0);
    return p;
}

QJsonValue paramToJson(const Param& p) {
    if (p.keys.empty()) return p.value;
    QJsonArray keys;
    for (const auto& k : p.keys) {
        QJsonArray a{double(k.t), k.v, interpName(k.interp)};
        if (k.inDt != 0 || k.inDv != 0 || k.outDt != 0 || k.outDv != 0) a << k.inDt << k.inDv << k.outDt << k.outDv;  // Bezier handles
        keys.append(a);
    }
    QJsonObject o{{"value", p.value}, {"keys", keys}};
    if (p.repeat != Repeat::Hold) o["repeat"] = p.repeat == Repeat::Loop ? "loop" : p.repeat == Repeat::PingPong ? "pingpong" : "offset";
    return o;
}

Param paramFromJson(const QJsonValue& v) {
    Param p;
    if (v.isDouble()) {
        p.value = v.toDouble();
        return p;
    }
    QJsonObject o = v.toObject();
    p.value = o.value("value").toDouble();
    const QString repeat = o.value("repeat").toString();
    p.repeat = repeat == "loop" ? Repeat::Loop : repeat == "pingpong" ? Repeat::PingPong : repeat == "offset" ? Repeat::Offset : Repeat::Hold;
    for (const auto& kv : o.value("keys").toArray()) {
        QJsonArray a = kv.toArray();
        if (a.size() < 2) continue;
        Keyframe k{FrameTime(a.at(0).toDouble()), a.at(1).toDouble(), a.size() > 2 ? interpFrom(a.at(2).toString()) : Interp::Linear};
        if (a.size() >= 7) k.inDt = a.at(3).toDouble(), k.inDv = a.at(4).toDouble(), k.outDt = a.at(5).toDouble(), k.outDv = a.at(6).toDouble();
        p.keys.push_back(k);
    }
    std::sort(p.keys.begin(), p.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
    return p;
}

// Object masks: prompts per frame, and each segmented frame as base64 of its packed logits.
QJsonObject objectMaskToJson(const ObjectMask& m) {
    QJsonObject prompts, frames;
    for (const auto& [f, pts] : m.prompts) {
        QJsonArray a;
        for (const ObjectPoint& pt : pts) a.append(QJsonArray{pt.x, pt.y, pt.label});
        prompts[QString::number(f)] = a;
    }
    for (const auto& [f, data] : m.frames)
        frames[QString::number(f)] = QString::fromLatin1(QByteArray::fromRawData(data.data(), qsizetype(data.size())).toBase64());
    return QJsonObject{{"fps", m.fps}, {"prompts", prompts}, {"frames", frames}};
}

std::shared_ptr<const ObjectMask> objectMaskFromJson(const QJsonObject& o) {
    auto m = std::make_shared<ObjectMask>();
    m->fps = o.value("fps").toDouble();
    const QJsonObject prompts = o.value("prompts").toObject(), frames = o.value("frames").toObject();
    for (auto it = prompts.begin(); it != prompts.end(); ++it) {
        std::vector<ObjectPoint> pts;
        for (const QJsonValue& v : it.value().toArray()) {
            const QJsonArray a = v.toArray();
            if (a.size() >= 3) pts.push_back({a[0].toDouble(), a[1].toDouble(), a[2].toInt()});
        }
        if (!pts.empty()) m->prompts[it.key().toLongLong()] = std::move(pts);
    }
    for (auto it = frames.begin(); it != frames.end(); ++it)
        m->frames[it.key().toLongLong()] = QByteArray::fromBase64(it.value().toString().toLatin1()).toStdString();
    return m;
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
    if (e.object) o["object"] = objectMaskToJson(*e.object);
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
    if (o.contains("object")) e.object = objectMaskFromJson(o.value("object").toObject());
    return e;
}

QJsonArray markersToJson(const std::vector<Marker>& markers) {
    QJsonArray m;
    for (const auto& mk : markers) {
        QJsonObject mo{{"t", double(mk.t)}, {"duration", double(mk.duration)}, {"name", qs(mk.name)}, {"comment", qs(mk.comment)}, {"color", mk.color}};
        if (mk.chapter) mo["chapter"] = true;
        m.append(mo);
    }
    return m;
}

std::vector<Marker> markersFromJson(const QJsonValue& v) {
    std::vector<Marker> out;
    for (const auto& mv : v.toArray()) {
        QJsonObject mo = mv.toObject();
        Marker mk;
        mk.t = i64(mo.value("t"));
        mk.duration = i64(mo.value("duration"));
        mk.name = ss(mo.value("name"));
        mk.comment = ss(mo.value("comment"));
        mk.color = mo.value("color").toInt(0);
        mk.chapter = mo.value("chapter").toBool(false);
        out.push_back(mk);
    }
    return out;
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
    if (!c.role.empty()) o["role"] = qs(c.role);
    if (c.blendMode != "normal") o["blend"] = qs(c.blendMode);
    if (!c.generator.empty()) o["generator"] = effectToJson(c.generator);
    if (!c.motion.empty()) o["motion"] = effectToJson(c.motion);
    if (!c.audio.empty()) o["audio"] = effectToJson(c.audio);
    if (!c.timing.empty()) o["timing"] = effectToJson(c.timing);
    QJsonArray fx;
    for (const auto& e : c.effects) fx.append(effectToJson(e));
    if (!fx.isEmpty()) o["effects"] = fx;
    if (!c.unrendered.empty()) o["unrendered"] = qs(c.unrendered);
    if (c.angle != 0) o["angle"] = c.angle;
    if (c.audioAngle != -1) o["audioAngle"] = c.audioAngle;
    if (!c.markers.empty()) o["markers"] = markersToJson(c.markers);
    if (!c.takes.empty()) {
        QJsonArray takes;
        for (const Take& t : c.takes)
            takes.append(QJsonObject{{"media", qint64(t.mediaId)}, {"offset", t.offset}, {"name", qs(t.name)}});
        o["takes"] = takes;
        o["take"] = c.take;
    }
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
    c.role = ss(o.value("role"));
    c.blendMode = o.contains("blend") ? ss(o.value("blend")) : "normal";
    c.generator = effectFromJson(o.value("generator"));
    c.motion = effectFromJson(o.value("motion"));
    c.audio = effectFromJson(o.value("audio"));
    c.timing = effectFromJson(o.value("timing"));
    for (const auto& e : o.value("effects").toArray()) c.effects.push_back(effectFromJson(e));
    c.unrendered = ss(o.value("unrendered"));
    c.angle = std::max(0, o.value("angle").toInt(0));
    c.audioAngle = std::max(-1, o.value("audioAngle").toInt(-1));
    c.markers = markersFromJson(o.value("markers"));
    for (const auto& tv : o.value("takes").toArray()) {
        const QJsonObject t = tv.toObject();
        c.takes.push_back({Id(i64(t.value("media"))), t.value("offset").toDouble(), ss(t.value("name"))});
    }
    c.take = c.takes.empty() ? 0 : std::clamp(o.value("take").toInt(), 0, int(c.takes.size()) - 1);
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
    if (!t.effects.empty()) {
        QJsonArray fx;
        for (const auto& e : t.effects) fx.append(effectToJson(e));
        o["effects"] = fx;
    }
    if (t.output) o["output"] = double(t.output);
    if (const QJsonValue sp = surroundToJson(t.surround); !sp.isUndefined()) o["surround"] = sp;
    if (t.volumeAuto.animated()) o["volumeAuto"] = paramToJson(t.volumeAuto);
    if (t.panAuto.animated()) o["panAuto"] = paramToJson(t.panAuto);
    if (t.automation != 1) o["automation"] = t.automation;
    if (!t.folder.empty()) o["folder"] = qs(t.folder);
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
    for (const auto& e : o.value("effects").toArray()) t.effects.push_back(effectFromJson(e));
    t.output = Id(i64(o.value("output")));
    if (o.contains("surround")) t.surround = surroundFromJson(o.value("surround"));
    if (o.contains("volumeAuto")) t.volumeAuto = paramFromJson(o.value("volumeAuto"));
    if (o.contains("panAuto")) t.panAuto = paramFromJson(o.value("panAuto"));
    t.automation = std::clamp(o.value("automation").toInt(1), 0, 4);
    t.folder = ss(o.value("folder"));
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
    if (st.animation) style["animation"] = st.animation;
    const CaptionStyle def;
    if (st.hiR != def.hiR || st.hiG != def.hiG || st.hiB != def.hiB) style["highlight"] = QJsonArray{st.hiR, st.hiG, st.hiB};
    QJsonArray items;
    for (const Caption& c : t.captions) {
        QJsonArray item{double(c.start), double(c.end), qs(c.text)};
        if (!c.wordTimes.empty()) {
            QJsonArray w;
            for (double f : c.wordTimes) w.append(std::round(f * 10000) / 10000);
            item.append(w);
        }
        items.append(item);
    }
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
    st.animation = so.value("animation").toInt(def.animation);
    const QJsonArray hc = so.value("highlight").toArray();
    if (hc.size() == 3) st.hiR = hc.at(0).toDouble(), st.hiG = hc.at(1).toDouble(), st.hiB = hc.at(2).toDouble();
    for (const auto& v : o.value("captions").toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() < 3) continue;
        Caption c{i64(a.at(0)), i64(a.at(1)), ss(a.at(2)), {}};
        for (const auto& w : a.at(3).toArray()) c.wordTimes.push_back(w.toDouble());
        t.captions.push_back(std::move(c));
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
    for (const auto& mk : s.markers) {
        QJsonObject mo{{"t", double(mk.t)}, {"duration", double(mk.duration)}, {"name", qs(mk.name)}, {"comment", qs(mk.comment)}, {"color", mk.color}};
        if (mk.chapter) mo["chapter"] = true;
        m.append(mo);
    }
    o["video"] = v;
    o["audio"] = a;
    o["markers"] = m;
    if (!s.captionTracks.empty()) {
        QJsonArray c;
        for (const auto& t : s.captionTracks) c.append(captionTrackToJson(t));
        o["captions"] = c;
    }
    if (!s.buses.empty()) {
        QJsonArray buses;
        for (const auto& b : s.buses) {
            QJsonArray fx;
            for (const auto& e : b.effects) fx.append(effectToJson(e));
            QJsonObject bo{{"id", double(b.id)}, {"name", qs(b.name)}, {"effects", fx},
                           {"volumeDb", b.volumeDb}, {"pan", b.pan}, {"muted", b.muted}};
            if (const QJsonValue sp = surroundToJson(b.surround); !sp.isUndefined()) bo["surround"] = sp;
            buses.append(bo);
        }
        o["buses"] = buses;
    }
    if (!s.masterEffects.empty()) {
        QJsonArray fx;
        for (const auto& e : s.masterEffects) fx.append(effectToJson(e));
        o["masterEffects"] = fx;
    }
    if (s.masterVolumeDb != 0) o["masterVolumeDb"] = s.masterVolumeDb;
    if (s.multicam) o["multicam"] = true;
    if (!s.folderGains.empty()) {
        QJsonObject gains;
        for (const auto& [k, db] : s.folderGains) gains[qs(k)] = db;
        o["folderGains"] = gains;
    }
    if (!s.mutedRoles.empty()) {
        QJsonArray roles;
        for (const std::string& r : s.mutedRoles) roles.append(qs(r));
        o["mutedRoles"] = roles;
    }
    if (!s.collapsedFolders.empty()) {
        QJsonArray folders;
        for (const std::string& f : s.collapsedFolders) folders.append(qs(f));
        o["collapsedFolders"] = folders;
    }
    if (s.colorSpace != "rec709") o["colorSpace"] = qs(s.colorSpace);
    if (s.hdrPeakNits != 1000) o["hdrPeakNits"] = s.hdrPeakNits;
    if (s.audioLayout != "stereo") o["audioLayout"] = qs(s.audioLayout);
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
    s.multicam = o.value("multicam").toBool(false);
    for (const auto& f : o.value("collapsedFolders").toArray()) s.collapsedFolders.push_back(f.toString().toStdString());
    for (const auto& r : o.value("mutedRoles").toArray()) s.mutedRoles.push_back(r.toString().toStdString());
    const QJsonObject gains = o.value("folderGains").toObject();
    for (auto it = gains.begin(); it != gains.end(); ++it) s.folderGains[it.key().toStdString()] = it.value().toDouble();
    s.colorSpace = o.contains("colorSpace") ? ss(o.value("colorSpace")) : "rec709";
    s.hdrPeakNits = std::clamp(o.value("hdrPeakNits").toDouble(1000), 100.0, 10000.0);
    s.audioLayout = o.contains("audioLayout") ? ss(o.value("audioLayout")) : "stereo";
    if (s.audioLayout != "5.1" && s.audioLayout != "7.1") s.audioLayout = "stereo";
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
        mk.chapter = mo.value("chapter").toBool(false);
        s.markers.push_back(mk);
    }
    for (const auto& c : o.value("captions").toArray()) s.captionTracks.push_back(captionTrackFromJson(c.toObject()));
    for (const auto& v : o.value("buses").toArray()) {
        const QJsonObject bo = v.toObject();
        Bus b;
        b.id = Id(i64(bo.value("id")));
        b.name = ss(bo.value("name"));
        for (const auto& e : bo.value("effects").toArray()) b.effects.push_back(effectFromJson(e));
        b.volumeDb = bo.value("volumeDb").toDouble(0);
        b.pan = bo.value("pan").toDouble(0);
        b.muted = bo.value("muted").toBool(false);
        if (bo.contains("surround")) b.surround = surroundFromJson(bo.value("surround"));
        s.buses.push_back(std::move(b));
    }
    for (const auto& e : o.value("masterEffects").toArray()) s.masterEffects.push_back(effectFromJson(e));
    s.masterVolumeDb = o.value("masterVolumeDb").toDouble(0);
    return s;
}

}  // namespace

std::string clipToJsonString(const Clip& c) {
    return QJsonDocument(clipToJson(c)).toJson(QJsonDocument::Compact).toStdString();
}

std::string captionTrackToJsonString(const CaptionTrack& t) {
    return QJsonDocument(captionTrackToJson(t)).toJson(QJsonDocument::Compact).toStdString();
}

std::string objectMaskToJsonString(const ObjectMask& m) {
    return QJsonDocument(objectMaskToJson(m)).toJson(QJsonDocument::Compact).toStdString();
}

bool clipFromJsonString(const std::string& json, Clip& out) {
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
    if (!doc.isObject()) return false;
    out = clipFromJson(doc.object());
    return true;
}

std::string effectToJsonString(const Effect& e) { return QJsonDocument(effectToJson(e)).toJson(QJsonDocument::Compact).toStdString(); }

bool effectFromJsonString(const std::string& json, Effect& out) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    out = effectFromJson(doc.object());
    return !out.type.empty();
}

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
        if (!m.colorSpace.empty()) o["colorSpace"] = qs(m.colorSpace);
        if (!m.colorOverride.empty()) o["colorOverride"] = qs(m.colorOverride);
        if (m.timecode >= 0) o["timecode"] = m.timecode;
        if (m.rating) o["rating"] = m.rating;
        if (m.label) o["label"] = m.label;
        if (!m.keywords.empty()) {
            QJsonArray k;
            for (const auto& w : m.keywords) k.append(qs(w));
            o["keywords"] = k;
        }
        if (!m.metadata.empty()) {
            QJsonObject md;
            for (const auto& [key, v] : m.metadata) md[qs(key)] = qs(v);
            o["metadata"] = md;
        }
        if (!m.created.empty()) o["created"] = qs(m.created);
        if (m.subclipOf) {
            o["subclipOf"] = double(m.subclipOf);
            o["subclipIn"] = m.subclipIn;
            o["subclipOut"] = m.subclipOut;
        }
        if (m.transcript && !m.transcript->empty())
            o["transcript"] = QJsonDocument::fromJson(QByteArray::fromStdString(transcriptToJson(*m.transcript))).object();
        if (m.visual && !m.visual->samples.empty())
            o["visual"] = QJsonDocument::fromJson(QByteArray::fromStdString(visualIndexToJson(*m.visual))).object();
        if (m.faces && !m.faces->model.empty())
            o["faces"] = QJsonDocument::fromJson(QByteArray::fromStdString(faceIndexToJson(*m.faces))).object();
        media.append(o);
    }
    root["media"] = media;
    if (!p.bins.empty()) {
        QJsonArray bins;
        for (const auto& b : p.bins) bins.append(qs(b));
        root["bins"] = bins;
    }
    if (!p.people.empty()) {
        QJsonArray people;
        for (const Person& person : p.people) people.append(QJsonObject{{"id", person.id}, {"name", qs(person.name)}});
        root["people"] = people;
    }
    if (!p.smartBins.empty()) {
        QJsonArray smart;
        for (const SmartBin& b : p.smartBins) {
            QJsonArray rules;
            for (const SmartRule& r : b.rules) rules.append(QJsonObject{{"field", qs(r.field)}, {"op", qs(r.op)}, {"value", qs(r.value)}});
            smart.append(QJsonObject{{"id", double(b.id)}, {"name", qs(b.name)}, {"all", b.matchAll}, {"rules", rules}});
        }
        root["smartBins"] = smart;
    }
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
        if (o.contains("visual")) {
            VisualIndex v;
            if (visualIndexFromJson(QJsonDocument(o.value("visual").toObject()).toJson(QJsonDocument::Compact).toStdString(), v))
                m.visual = std::make_shared<const VisualIndex>(std::move(v));
        }
        if (o.contains("faces")) {
            FaceIndex f;
            if (faceIndexFromJson(QJsonDocument(o.value("faces").toObject()).toJson(QJsonDocument::Compact).toStdString(), f))
                m.faces = std::make_shared<const FaceIndex>(std::move(f));
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
        m.colorSpace = ss(o.value("colorSpace"));
        m.colorOverride = ss(o.value("colorOverride"));
        m.timecode = o.value("timecode").toDouble(-1);
        m.rating = std::clamp(o.value("rating").toInt(0), -1, 5);
        m.label = o.value("label").toInt(0);
        for (const auto& k : o.value("keywords").toArray()) m.keywords.push_back(ss(k));
        const QJsonObject md = o.value("metadata").toObject();
        for (auto it = md.begin(); it != md.end(); ++it) m.metadata[it.key().toStdString()] = ss(it.value());
        m.created = ss(o.value("created"));
        m.subclipOf = Id(i64(o.value("subclipOf")));
        m.subclipIn = o.value("subclipIn").toDouble();
        m.subclipOut = o.value("subclipOut").toDouble();
        p.media.push_back(m);
    }
    for (const auto& b : root.value("bins").toArray()) p.bins.push_back(ss(b));
    for (const auto& pv : root.value("people").toArray()) p.people.push_back({pv.toObject().value("id").toInt(), ss(pv.toObject().value("name"))});
    for (const auto& bv : root.value("smartBins").toArray()) {
        const QJsonObject o = bv.toObject();
        SmartBin b;
        b.id = Id(i64(o.value("id")));
        b.name = ss(o.value("name"));
        b.matchAll = o.value("all").toBool(true);
        for (const auto& rv : o.value("rules").toArray()) {
            const QJsonObject r = rv.toObject();
            b.rules.push_back({ss(r.value("field")), ss(r.value("op")), ss(r.value("value"))});
        }
        p.smartBins.push_back(std::move(b));
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
    for (const auto& b : p.smartBins) bump(b.id);
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
                    bump(c.timing.id);
                    for (const auto& e : c.effects) bump(e.id);
                }
                for (const auto& tr : t.transitions) {
                    bump(tr.id);
                    bump(tr.params.id);
                }
                for (const auto& e : t.effects) bump(e.id);
            }
        for (const auto& b : s.buses) {
            bump(b.id);
            for (const auto& e : b.effects) bump(e.id);
        }
        for (const auto& e : s.masterEffects) bump(e.id);
        for (const auto& ct : s.captionTracks) bump(ct.id);
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
