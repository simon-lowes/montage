#include "McpServer.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>

#include "core/AutoTag.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/History.h"
#include "core/Interchange.h"
#include "core/MediaLog.h"
#include "core/ProjectIO.h"
#include "core/ScriptCut.h"
#include "core/TranscriptEdit.h"
#include "media/SpeechEnhance.h"
#include "media/AutoDuck.h"
#include "media/Decoder.h"
#include "media/Transcriber.h"
#include "media/VisualSearch.h"
#include "render/ClipAnalysis.h"
#include "render/Compositor.h"
#include "render/Exporter.h"
#include "render/Processing.h"

namespace montage {

namespace {

const char* kModern = "2026-07-28";
const char* kVersionKey = "io.modelcontextprotocol/protocolVersion";
const char* kCapabilitiesKey = "io.modelcontextprotocol/clientCapabilities";
const char* kServerInfoKey = "io.modelcontextprotocol/serverInfo";

const char* kInstructions =
    "Montage is a video editor. Projects are .montage files: pass their path to every tool, and each edit is saved "
    "at once (montage_undo restores the version before the last edit). Times are seconds (numbers) or timecode "
    "strings like \"00:01:02:03\"; tracks are \"V1\", \"V2\", \"A1\"... Clips are referred to by the ids "
    "montage_project_info lists. Look at the result with montage_render_frame before rendering the whole sequence. "
    "If the project is open in the Montage app, reopen it there to see the changes.";

QJsonObject serverInfo() {
    return QJsonObject{{"name", "montage"}, {"title", "Montage"}, {"version", MONTAGE_VERSION}};
}

// ---- Tool results -----------------------------------------------------------------

struct ToolResult {
    QString text;
    QJsonObject structured;
    QByteArray png;  // an image to show (base64-encoded in the reply)
    bool error = false;
};

ToolResult fail(const QString& message) { return {message, {}, {}, true}; }
ToolResult ok(const QString& text, const QJsonObject& structured = {}) { return {text, structured, {}, false}; }

// A tool argument problem, reported as a failed tool call (so the model can correct itself).
struct ArgError {
    QString message;
};

QString str(const QJsonObject& a, const char* key, const QString& def = {}) {
    const QJsonValue v = a.value(key);
    return v.isString() ? v.toString() : def;
}
QString need(const QJsonObject& a, const char* key) {
    const QString v = str(a, key);
    if (v.isEmpty()) throw ArgError{QStringLiteral("\"%1\" is required").arg(key)};
    return v;
}

// Seconds (a number) or a timecode / frame count / "N s" string, in the sequence's frames.
FrameTime timeArg(const QJsonValue& v, const Sequence& s, const char* name) {
    if (v.isDouble()) return FrameTime(std::llround(v.toDouble() * s.fpsValue()));
    FrameTime f = 0;
    if (v.isString() && parseTimecode(v.toString().toStdString(), s.fps, f)) return f;
    throw ArgError{QStringLiteral("\"%1\" must be seconds or a timecode like 00:00:01:00").arg(name)};
}

TrackRef trackArg(const QString& name, const Sequence& s, bool mayCreate, Project* p = nullptr, Sequence* ms = nullptr) {
    const QString n = name.trimmed().toUpper();
    if (n.size() >= 2 && (n[0] == 'V' || n[0] == 'A')) {
        bool good = false;
        const int i = n.mid(1).toInt(&good) - 1;
        const TrackKind kind = n[0] == 'V' ? TrackKind::Video : TrackKind::Audio;
        const int count = int(kind == TrackKind::Video ? s.videoTracks.size() : s.audioTracks.size());
        if (good && i >= 0 && i < count) return {kind, i};
        if (good && mayCreate && p && ms && i == count) return edit::addTrack(*p, *ms, kind);
    }
    throw ArgError{QStringLiteral("Unknown track \"%1\": use V1, V2... or A1, A2...").arg(name)};
}

QString tc(FrameTime f, const Sequence& s) { return QString::fromStdString(formatTimecode(f, s.fps)); }
double secs(FrameTime f, const Sequence& s) { return double(f) / s.fpsValue(); }

QString absolute(const QString& path) { return QString::fromStdString(std::filesystem::absolute(path.toStdString()).string()); }

// ---- Projects on disk ---------------------------------------------------------------

struct Loaded {
    QString path;
    Project project;
    Sequence& seq() { return *project.active(); }
};

Loaded open(const QJsonObject& a) {
    Loaded l;
    l.path = absolute(need(a, "project"));
    std::string err;
    if (!loadProject(l.path.toStdString(), l.project, &err))
        throw ArgError{QStringLiteral("Cannot open %1: %2").arg(l.path, QString::fromStdString(err))};
    if (!l.project.active()) throw ArgError{QStringLiteral("%1 has no sequence").arg(l.path)};
    return l;
}

// Saves, keeping the previous version as <project>.bak for montage_undo.
void save(Loaded& l) {
    const QString bak = l.path + ".bak";
    if (QFileInfo::exists(l.path)) {
        QFile::remove(bak);
        QFile::copy(l.path, bak);
    }
    std::string err;
    if (!saveProject(l.project, l.path.toStdString(), &err))
        throw ArgError{QStringLiteral("Cannot save %1: %2").arg(l.path, QString::fromStdString(err))};
}

void check(const edit::Result& r) {
    if (!r.ok) throw ArgError{QString::fromStdString(r.error)};
}

Clip& clipArg(Loaded& l, const QJsonObject& a, const char* key = "clip") {
    const QJsonValue v = a.value(key);
    if (!v.isDouble()) throw ArgError{QStringLiteral("\"%1\" must be a clip id (see montage_project_info)").arg(key)};
    Clip* c = edit::clipById(l.seq(), Id(v.toDouble()));
    if (!c) throw ArgError{QStringLiteral("No clip %1 in the active sequence").arg(qulonglong(v.toDouble()))};
    return *c;
}

QJsonObject clipJson(const Project& p, const Sequence& s, const Clip& c) {
    QJsonObject o{{"id", double(c.id)},
                  {"name", QString::fromStdString(c.name)},
                  {"start", tc(c.start, s)},
                  {"end", tc(c.end(), s)},
                  {"start_seconds", secs(c.start, s)},
                  {"duration_seconds", secs(c.duration, s)}};
    if (c.isGenerator()) {
        o["generator"] = QString::fromStdString(c.generator.type);
        if (c.generator.type == "title") o["text"] = QString::fromStdString(c.generator.s("text"));
    } else if (const MediaItem* m = p.findMedia(c.mediaId)) {
        o["media"] = QString::fromStdString(m->path.empty() ? m->name : m->path);
        o["source_in_seconds"] = c.sourceIn / s.fpsValue();
    }
    if (c.speed != 1.0 || c.reverse) o["speed"] = (c.reverse ? -1 : 1) * c.speed;
    if (!c.enabled) o["enabled"] = false;
    if (c.linkGroup) o["linked_group"] = double(c.linkGroup);
    QJsonArray fx;
    for (const Effect& e : c.effects) fx.append(QJsonObject{{"id", double(e.id)}, {"type", QString::fromStdString(e.type)}});
    if (!fx.isEmpty()) o["effects"] = fx;
    return o;
}

// A media item's logging: rating, label, keywords, metadata, bin, recording date.
void logJson(const MediaItem& m, QJsonObject& o) {
    if (m.rating) o["rating"] = m.rating;
    if (m.label > 0) o["label"] = QString::fromLatin1(labelName(m.label));
    if (!m.keywords.empty()) {
        QJsonArray k;
        for (const std::string& w : m.keywords) k.append(QString::fromStdString(w));
        o["keywords"] = k;
    }
    for (const auto& [key, v] : m.metadata) o[QString::fromStdString(key)] = QString::fromStdString(v);
    if (!m.bin.empty()) o["bin"] = QString::fromStdString(m.bin);
    if (!m.created.empty()) o["recorded"] = QString::fromStdString(m.created);
    if (m.subclipOf) {
        o["subclip_of"] = double(m.subclipOf);
        o["subclip_start_seconds"] = m.subclipIn;
        o["subclip_end_seconds"] = m.subclipOut;
    }
}

QJsonObject projectJson(const Project& p) {
    const Sequence& s = *p.active();
    QJsonArray tracks;
    for (TrackRef r : allTracks(s)) {
        const Track* t = trackAt(s, r);
        QJsonArray clips;
        for (const Clip& c : t->clips) clips.append(clipJson(p, s, c));
        QJsonObject to{{"track", QString::fromStdString(t->name)}, {"kind", r.kind == TrackKind::Video ? "video" : "audio"},
                       {"clips", clips}};
        if (t->muted) to["muted"] = true;
        QJsonArray trs;
        for (const auto& tr : t->transitions)
            trs.append(QJsonObject{{"type", QString::fromStdString(tr.type)}, {"from_clip", double(tr.clipA)}, {"to_clip", double(tr.clipB)}});
        if (!trs.isEmpty()) to["transitions"] = trs;
        tracks.append(to);
    }
    QJsonArray markers;
    for (const Marker& m : s.markers)
        markers.append(QJsonObject{{"at", tc(m.t, s)}, {"name", QString::fromStdString(m.name)}, {"comment", QString::fromStdString(m.comment)}});
    QJsonArray media;
    for (const MediaItem& m : p.media) {
        QJsonObject mo{{"id", double(m.id)}, {"name", QString::fromStdString(m.name)}, {"duration_seconds", m.duration}};
        if (!m.path.empty()) mo["path"] = QString::fromStdString(m.path);
        if (m.transcript) mo["transcribed"] = true;
        logJson(m, mo);
        media.append(mo);
    }
    return QJsonObject{{"sequence", QString::fromStdString(s.name)},
                       {"width", s.width},
                       {"height", s.height},
                       {"fps", s.fpsValue()},
                       {"duration", tc(s.duration(), s)},
                       {"duration_seconds", secs(s.duration(), s)},
                       {"tracks", tracks},
                       {"markers", markers},
                       {"media", media}};
}

// Adds a media file to the project (or finds it there).
Id mediaFor(Project& p, const QString& path) {
    const std::string abs = absolute(path).toStdString();
    for (const MediaItem& m : p.media)
        if (m.path == abs) return m.id;
    MediaItem m;
    m.id = p.newId();
    std::string err;
    if (!probeMedia(abs, m, &err)) throw ArgError{QStringLiteral("Cannot read %1: %2").arg(path, QString::fromStdString(err))};
    p.media.push_back(m);
    return m.id;
}

// A media item already in the project, by file path or name.
MediaItem& projectMedia(Project& p, const QString& ref) {
    const std::string abs = absolute(ref).toStdString(), name = ref.toStdString();
    for (MediaItem& m : p.media)
        if (!m.path.empty() && m.path == abs) return m;
    for (MediaItem& m : p.media)
        if (m.name == name) return m;
    throw ArgError{QStringLiteral("No media \"%1\" in the project (see montage_project_info)").arg(ref)};
}

std::vector<std::string> stringList(const QJsonObject& a, const char* key) {
    std::vector<std::string> out;
    const QJsonValue v = a.value(key);
    if (v.isString()) return parseKeywords(v.toString().toStdString());
    for (const QJsonValue& x : v.toArray()) out.push_back(x.toString().trimmed().toStdString());
    std::erase(out, std::string());
    return out;
}

// Smart bin rules from JSON, checked against the fields and their tests.
std::vector<SmartRule> rulesArg(const QJsonArray& rules) {
    std::vector<SmartRule> out;
    for (const QJsonValue& v : rules) {
        const QJsonObject r = v.toObject();
        SmartRule rule{str(r, "field").toStdString(), str(r, "op").toStdString(), {}};
        const QJsonValue value = r.value("value");
        rule.value = value.isDouble() ? QString::number(value.toDouble()).toStdString() : value.toString().toStdString();
        const MediaField* f = mediaField(rule.field);
        if (!f || !f->rule) {
            QStringList keys;
            for (const MediaField& m : mediaFields())
                if (m.rule) keys << m.key;
            throw ArgError{QStringLiteral("Unknown field \"%1\"; fields: %2").arg(QString::fromStdString(rule.field), keys.join(", "))};
        }
        QStringList ops;
        bool known = false;
        for (const RuleOp& o : ruleOps(f->type)) {
            ops << o.id;
            known |= rule.op == o.id;
        }
        if (!known)
            throw ArgError{QStringLiteral("\"%1\" cannot be tested with \"%2\"; use %3").arg(QString::fromStdString(rule.field),
                                                                                              QString::fromStdString(rule.op), ops.join(", "))};
        out.push_back(std::move(rule));
    }
    return out;
}

QJsonObject mediaJson(const MediaItem& m) {
    QJsonObject o{{"name", QString::fromStdString(m.name)}, {"duration_seconds", m.duration}};
    if (m.hasVideo) {
        o["width"] = m.width;
        o["height"] = m.height;
        o["fps"] = m.fps.toDouble();
        o["video_codec"] = QString::fromStdString(m.videoCodec);
    }
    if (m.hasAudio) {
        o["sample_rate"] = m.sampleRate;
        o["channels"] = m.channels;
        o["audio_codec"] = QString::fromStdString(m.audioCodec);
    }
    return o;
}

QString json(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)); }

}  // namespace

// ---- Tools ------------------------------------------------------------------------------

struct McpServer::Impl {
    struct Tool {
        QString name, title, description;
        QJsonObject schema;
        bool readOnly = false;
        std::function<ToolResult(const QJsonObject&)> run;
    };
    std::vector<Tool> tools;
    QString legacyVersion;  // set by initialize (legacy clients)
    std::function<void(const QJsonObject&)> notify;  // progress notifications while a tool runs
    QJsonValue progressToken;
    std::ostream* live = nullptr;  // when serving a stream: notifications go out as they happen

    Impl() { addTools(); }

    void add(const char* name, const char* title, const char* description, const char* schema, bool readOnly,
             std::function<ToolResult(const QJsonObject&)> run) {
        tools.push_back({name, title, description, QJsonDocument::fromJson(schema).object(), readOnly, std::move(run)});
    }

    void progress(double fraction, const QString& message) {
        if (!notify || progressToken.isUndefined() || progressToken.isNull()) return;
        notify(QJsonObject{{"jsonrpc", "2.0"},
                           {"method", "notifications/progress"},
                           {"params", QJsonObject{{"progressToken", progressToken}, {"progress", fraction}, {"total", 1.0}, {"message", message}}}});
    }

    void addTools();
    // Indexes the videos (all, or these and subclips' media) that have no visual index; an error message or "".
    QString indexMissing(Project& p, const std::vector<Id>& only, bool& changed);
};

QString McpServer::Impl::indexMissing(Project& p, const std::vector<Id>& only, bool& changed) {
    std::vector<Id> want;
    for (Id id : only)
        if (const MediaItem* m = p.findMedia(id)) want.push_back(m->subclipOf ? m->subclipOf : id);
    for (MediaItem& m : p.media) {
        if (!only.empty() && std::find(want.begin(), want.end(), m.id) == want.end()) continue;
        if (m.kind != MediaKind::Video || !m.hasVideo || m.path.empty() || m.subclipOf || (m.visual && !m.visual->samples.empty())) continue;
        VisualIndex v;
        std::string err;
        if (!indexVideo(m.path, m.duration, v, 0, [&](double f) { progress(f, QStringLiteral("Indexing %1").arg(QString::fromStdString(m.name))); }, nullptr, &err))
            return QString::fromStdString(m.name + ": " + err);
        m.visual = std::make_shared<const VisualIndex>(std::move(v));
        changed = true;
    }
    return {};
}

void McpServer::Impl::addTools() {
    add("montage_probe_media", "Probe media", "Describe a video, audio or image file: duration, size, frame rate and codecs.",
        R"json({"type":"object","properties":{"path":{"type":"string","description":"Media file"}},"required":["path"]})json", true,
        [](const QJsonObject& a) {
            MediaItem m;
            std::string err;
            if (!probeMedia(absolute(need(a, "path")).toStdString(), m, &err)) return fail(QString::fromStdString(err));
            const QJsonObject o = mediaJson(m);
            return ok(json(o), o);
        });

    add("montage_create_project", "Create a project",
        "Create a .montage project with the given media laid end to end on V1/A1. The sequence takes the first video's "
        "size and frame rate unless width, height or fps are given.",
        R"json({"type":"object","properties":{
            "project":{"type":"string","description":"Path of the .montage file to write"},
            "media":{"type":"array","items":{"type":"string"},"description":"Media files, in order"},
            "width":{"type":"integer"},"height":{"type":"integer"},"fps":{"type":"number"}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l;
            l.path = absolute(need(a, "project"));
            l.project = makeDefaultProject();
            l.project.name = QFileInfo(l.path).completeBaseName().toStdString();
            std::vector<Id> ids;
            for (const QJsonValue& v : a.value("media").toArray()) ids.push_back(mediaFor(l.project, v.toString()));
            Sequence& s = l.seq();
            for (Id id : ids)
                if (const MediaItem* m = l.project.findMedia(id); m && m->kind == MediaKind::Video && m->hasVideo) {
                    if (m->width > 0) {
                        s.width = m->width;
                        s.height = m->height;
                    }
                    if (m->fps.valid()) s.fps = m->fps;
                    break;
                }
            if (a.value("width").isDouble()) s.width = a.value("width").toInt();
            if (a.value("height").isDouble()) s.height = a.value("height").toInt();
            if (a.value("fps").isDouble()) {
                const double f = a.value("fps").toDouble();
                s.fps = std::fabs(f - std::round(f)) < 1e-6 ? Rational{int(std::lround(f)), 1} : Rational{int(std::lround(f * 1001)), 1001};
            }
            FrameTime at = 0;
            for (Id id : ids) {
                check(edit::placeMedia(l.project, s, id, at, 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false));
                at = s.duration();
            }
            save(l);
            const QJsonObject o = projectJson(l.project);
            return ok(QStringLiteral("Created %1 (%2)").arg(l.path, tc(s.duration(), s)), o);
        });

    add("montage_project_info", "Project info",
        "List the active sequence of a project: size, frame rate, duration, every track with its clips (ids, times, media, "
        "effects), transitions, markers and media.",
        R"json({"type":"object","properties":{"project":{"type":"string"}},"required":["project"]})json", true,
        [](const QJsonObject& a) {
            Loaded l = open(a);
            const QJsonObject o = projectJson(l.project);
            return ok(json(o), o);
        });

    add("montage_place_media", "Place media",
        "Put a media file (or part of it) on the timeline at a time, with its sound linked on the audio track. "
        "Overwrite replaces what is there; insert pushes later clips along.",
        R"json({"type":"object","properties":{
            "project":{"type":"string"},"media":{"type":"string","description":"Media file, or the name of a subclip in the project"},
            "at":{"type":["number","string"],"description":"Timeline time; default: the end of the sequence"},
            "track":{"type":"string","description":"Video track, default V1 (a new one is made if it is the next number)"},
            "audio_track":{"type":"string","description":"Audio track, default A1"},
            "in":{"type":["number","string"],"description":"Source in, seconds (of the subclip, for one)"},
            "out":{"type":["number","string"],"description":"Source out, seconds (of the subclip, for one)"},
            "insert":{"type":"boolean","default":false}},
            "required":["project","media"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            // A subclip (by name) places its range of its media, under its name.
            const MediaItem* sub = nullptr;
            for (const MediaItem& m : l.project.media)
                if (m.subclipOf && m.name == need(a, "media").toStdString()) sub = &m;
            const std::string subName = sub ? sub->name : std::string();
            const Id media = sub ? sub->subclipOf : mediaFor(l.project, need(a, "media"));
            const double base = sub ? sub->subclipIn : 0;
            const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : s.duration();
            const TrackRef v = trackArg(str(a, "track", "V1"), s, true, &l.project, &s);
            const TrackRef au = trackArg(str(a, "audio_track", "A1"), s, true, &l.project, &s);
            auto seconds = [&](const char* k, double def) {
                const QJsonValue x = a.value(k);
                if (x.isDouble()) return x.toDouble();
                if (x.isString()) return double(timeArg(x, s, k)) / s.fpsValue();
                return def;
            };
            // Seconds of the media (of the subclip, for one) into sequence frames, as placeMedia takes them.
            const double in = (base + seconds("in", 0)) * s.fpsValue();
            const double outSec = seconds("out", -1);
            const double out = outSec >= 0 ? (base + outSec) * s.fpsValue() : sub ? sub->subclipOut * s.fpsValue() : -1.0;
            const auto r = edit::placeMedia(l.project, s, media, at, in, out, v, au, a.value("insert").toBool());
            check(r);
            if (sub)
                for (Id id : r.created)
                    if (Clip* c = edit::clipById(s, id)) c->name = subName;
            save(l);
            QJsonArray created;
            for (Id id : r.created)
                if (const Clip* c = edit::clipById(s, id)) created.append(clipJson(l.project, s, *c));
            return ok(QStringLiteral("Placed %1 clip(s) at %2").arg(r.created.size()).arg(tc(at, s)), QJsonObject{{"clips", created}});
        });

    add("montage_split", "Split clips",
        "Cut clips in two at a timeline time: on one track, or on every track.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"at":{"type":["number","string"]},
            "track":{"type":"string","description":"Only this track (default: all)"}},"required":["project","at"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const FrameTime at = timeArg(a.value("at"), s, "at");
            if (a.contains("track")) check(edit::razor(l.project, s, trackArg(str(a, "track"), s, false), at));
            else check(edit::razorAll(l.project, s, at));
            save(l);
            return ok(QStringLiteral("Split at %1").arg(tc(at, s)), projectJson(l.project));
        });

    add("montage_remove_clips", "Remove clips",
        "Remove clips by id. With ripple, later clips move up to close the gap (keeping sound in sync).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clips":{"type":"array","items":{"type":"number"}},
            "ripple":{"type":"boolean","default":false}},"required":["project","clips"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            std::vector<Id> ids;
            for (const QJsonValue& v : a.value("clips").toArray()) ids.push_back(Id(v.toDouble()));
            if (ids.empty()) throw ArgError{"\"clips\" must list clip ids"};
            check(edit::removeClips(l.project, l.seq(), ids, a.value("ripple").toBool()));
            save(l);
            return ok(QStringLiteral("Removed %1 clip(s)").arg(ids.size()), projectJson(l.project));
        });

    add("montage_move_clip", "Move a clip",
        "Move a clip (and the clips linked to it) to a new start time, optionally to another video track.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "start":{"type":["number","string"]},"track":{"type":"string"}},"required":["project","clip","start"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            Clip& c = clipArg(l, a);
            const Id id = c.id;
            const FrameTime delta = timeArg(a.value("start"), s, "start") - c.start;
            int videoDelta = 0, audioDelta = 0;
            if (a.contains("track")) {
                const auto loc = edit::locate(s, id);
                const TrackRef to = trackArg(str(a, "track"), s, true, &l.project, &s);
                if (!loc || to.kind != loc->track.kind) throw ArgError{"A clip moves only between tracks of its own kind"};
                (to.kind == TrackKind::Video ? videoDelta : audioDelta) = to.index - loc->track.index;
            }
            check(edit::moveClips(l.project, s, {id}, delta, videoDelta, audioDelta));
            save(l);
            const Clip* moved = edit::clipById(s, id);
            return ok(QStringLiteral("Moved to %1").arg(tc(moved ? moved->start : 0, s)), moved ? clipJson(l.project, s, *moved) : QJsonObject{});
        });

    add("montage_trim_clip", "Trim a clip",
        "Move a clip's in or out point by a number of seconds (positive: later). Ripple moves later clips with it.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "edge":{"type":"string","enum":["in","out"]},"by":{"type":"number","description":"Seconds"},
            "ripple":{"type":"boolean","default":false}},"required":["project","clip","edge","by"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Id id = clipArg(l, a).id;
            const FrameTime by = FrameTime(std::llround(a.value("by").toDouble() * s.fpsValue()));
            const auto r = edit::trim(l.project, s, id, str(a, "edge") == "in" ? edit::Edge::In : edit::Edge::Out, by,
                                      a.value("ripple").toBool() ? edit::TrimMode::Ripple : edit::TrimMode::Normal);
            check(r);
            save(l);
            const Clip* c = edit::clipById(s, id);
            return ok(QStringLiteral("Trimmed by %1 frame(s)").arg(r.applied), c ? clipJson(l.project, s, *c) : QJsonObject{});
        });

    add("montage_set_speed", "Set clip speed",
        "Change a clip's playback speed (1 = normal, 0.5 = half speed, 2 = double; negative plays backwards). "
        "Its length changes to match, and later clips ripple.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},"speed":{"type":"number"}},
            "required":["project","clip","speed"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Id id = clipArg(l, a).id;
            const double sp = a.value("speed").toDouble();
            if (std::fabs(sp) < 0.01 || std::fabs(sp) > 100) throw ArgError{"Speed must be between 0.01 and 100 (or -0.01 and -100)"};
            check(edit::setSpeed(l.project, l.seq(), id, std::fabs(sp), true, sp < 0));
            save(l);
            const Clip* c = edit::clipById(l.seq(), id);
            return ok(QStringLiteral("Speed set to %1").arg(sp), c ? clipJson(l.project, l.seq(), *c) : QJsonObject{});
        });

    add("montage_add_title", "Add a title",
        "Add a text title over the picture at a time, for a duration (default 3 s), on a video track (default: the "
        "track above the top one in use). template picks a ready-made, animated design (a lower third takes two lines: "
        "name, newline, role); the text replaces its sample text.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"text":{"type":"string"},
            "at":{"type":["number","string"]},"duration":{"type":["number","string"],"default":3},
            "track":{"type":"string"},"size":{"type":"number","description":"Font size in pixels"},
            "template":{"type":"string","enum":["plain","lower_third","lower_third_box","centred","chapter","callout","typewriter","end_card"],"default":"plain"}},
            "required":["project","text","at"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            TrackRef t{TrackKind::Video, 0};
            if (a.contains("track")) t = trackArg(str(a, "track"), s, true, &l.project, &s);
            else {
                int top = -1;
                for (int i = 0; i < int(s.videoTracks.size()); ++i)
                    if (!s.videoTracks[size_t(i)].clips.empty()) top = i;
                t = top + 1 < int(s.videoTracks.size()) ? TrackRef{TrackKind::Video, top + 1} : edit::addTrack(l.project, s, TrackKind::Video);
            }
            const FrameTime len = a.contains("duration") ? timeArg(a.value("duration"), s, "duration") : FrameTime(std::llround(3 * s.fpsValue()));
            const QString tpl = str(a, "template", "plain");
            const std::string type = tpl == "plain" ? std::string("title") : "title_" + tpl.toStdString();
            if (type != "title" && !findTitleTemplate(type)) throw ArgError{QStringLiteral("Unknown template \"%1\"").arg(tpl)};
            Clip c = makeGeneratorClip(l.project, type, std::max<FrameTime>(1, len));
            c.generator.strings["text"] = need(a, "text").toStdString();
            if (a.value("size").isDouble()) c.generator.params["size"] = Param(a.value("size").toDouble());
            c.start = timeArg(a.value("at"), s, "at");
            c.name = need(a, "text").left(40).toStdString();
            const auto r = edit::overwrite(l.project, s, t, c);
            check(r);
            save(l);
            const Clip* made = r.created.empty() ? nullptr : edit::clipById(s, r.created[0]);
            return ok(QStringLiteral("Added a title on %1").arg(QString::fromStdString(trackAt(s, t)->name)),
                      made ? clipJson(l.project, s, *made) : QJsonObject{});
        });

    add("montage_list_effects", "List effects",
        "The effects and transitions Montage has, with their parameters (name, range, default).",
        R"json({"type":"object","properties":{"kind":{"type":"string","enum":["video","audio","transition"],"default":"video"}}})json", true,
        [](const QJsonObject& a) {
            const QString kind = str(a, "kind", "video");
            QJsonArray list;
            for (const EffectInfo& e : effectCatalog()) {
                const bool want = kind == "audio"        ? e.category == EffectCategory::AudioFilter
                                  : kind == "transition" ? e.category == EffectCategory::VideoTransition || e.category == EffectCategory::AudioTransition
                                                         : e.category == EffectCategory::VideoFilter;
                if (!want || e.hidden) continue;
                QJsonArray params;
                for (const ParamInfo& pi : e.params) {
                    QJsonObject po{{"name", QString::fromStdString(pi.name)}, {"label", QString::fromStdString(pi.label)},
                                   {"min", pi.min}, {"max", pi.max}, {"default", pi.def}};
                    if (!pi.choices.empty()) {
                        QJsonArray ch;
                        for (const auto& c : pi.choices) ch.append(QString::fromStdString(c));
                        po["choices"] = ch;
                    }
                    params.append(po);
                }
                list.append(QJsonObject{{"type", QString::fromStdString(e.type)}, {"name", QString::fromStdString(e.displayName)},
                                        {"group", QString::fromStdString(e.group)}, {"params", params}});
            }
            const QJsonObject o{{"effects", list}};
            return ok(json(o), o);
        });

    add("montage_add_effect", "Add an effect",
        "Add an effect to a clip (see montage_list_effects), with parameter values. Video effects can be limited to a "
        "mask: mask.shape 1 ellipse or 2 rectangle, mask.x / mask.y centre and mask.w / mask.h size as fractions of the frame.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},"effect":{"type":"string"},
            "params":{"type":"object","additionalProperties":{"type":"number"}}},"required":["project","clip","effect"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Clip& c = clipArg(l, a);
            const std::string type = need(a, "effect").toStdString();
            const EffectInfo* info = findEffectInfo(type);
            if (!info || info->hidden || (info->category != EffectCategory::VideoFilter && info->category != EffectCategory::AudioFilter))
                throw ArgError{QStringLiteral("Unknown effect \"%1\" (see montage_list_effects)").arg(QString::fromStdString(type))};
            if (type == "enhance_speech" && (!speechEnhancerAvailable() || !speechModel().installed()))
                return fail("Enhance Speech needs its model: run `scripts/fetch-models.sh` or add the effect once in the app");
            Effect e = makeEffect(l.project, type);
            const QJsonObject params = a.value("params").toObject();
            for (auto it = params.begin(); it != params.end(); ++it) {
                const std::string name = it.key().toStdString();
                const bool known = std::any_of(info->params.begin(), info->params.end(), [&](const ParamInfo& p) { return p.name == name; }) ||
                                   (name.rfind("mask.", 0) == 0 && supportsMask(type));
                if (!known) throw ArgError{QStringLiteral("\"%1\" has no parameter \"%2\"").arg(QString::fromStdString(type), it.key())};
                e.params[name] = Param(it.value().toDouble());
            }
            const auto loc = edit::locate(l.seq(), c.id);
            const bool audioClip = loc && loc->track.kind == TrackKind::Audio;
            if (audioClip != (info->category == EffectCategory::AudioFilter))
                throw ArgError{audioClip ? QStringLiteral("That is an audio clip: choose an audio effect")
                                         : QStringLiteral("That is a video clip: choose a video effect")};
            c.effects.push_back(e);
            save(l);
            return ok(QStringLiteral("Added %1 to %2").arg(QString::fromStdString(info->displayName), QString::fromStdString(c.name)),
                      QJsonObject{{"effect_id", double(e.id)}});
        });

    add("montage_add_transition", "Add a transition",
        "Add a transition at a clip's start or end (cross_dissolve by default, centred on the cut).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "edge":{"type":"string","enum":["in","out"],"default":"in"},"type":{"type":"string","default":"cross_dissolve"},
            "duration":{"type":["number","string"],"default":1}},"required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Id id = clipArg(l, a).id;
            const FrameTime len = a.contains("duration") ? timeArg(a.value("duration"), s, "duration") : FrameTime(std::llround(s.fpsValue()));
            check(edit::addTransition(l.project, s, id, str(a, "edge", "in") == "out" ? edit::Edge::Out : edit::Edge::In,
                                      str(a, "type", "cross_dissolve").toStdString(), std::max<FrameTime>(1, len)));
            save(l);
            return ok(QStringLiteral("Added the transition"));
        });

    add("montage_add_marker", "Add a marker", "Add a timeline marker with a name and comment.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"at":{"type":["number","string"]},
            "name":{"type":"string"},"comment":{"type":"string"}},"required":["project","at"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const FrameTime at = timeArg(a.value("at"), s, "at");
            edit::addMarker(s, Marker{at, 0, str(a, "name").toStdString(), str(a, "comment").toStdString(), 0});
            save(l);
            return ok(QStringLiteral("Marker at %1").arg(tc(at, s)));
        });

    add("montage_transcribe", "Transcribe",
        "Turn the speech in a media file into word-timed text (whisper.cpp, on this computer), optionally labelling who "
        "speaks. With a project, the transcript is kept on that media item (for montage_find_phrase and captions).",
        R"json({"type":"object","properties":{"media":{"type":"string"},"project":{"type":"string"},
            "model":{"type":"string","default":"base.en","description":"A downloaded whisper model (montage-cli models)"},
            "language":{"type":"string","default":"auto"},"speakers":{"type":"boolean","default":false}},"required":["media"]})json",
        false, [this](const QJsonObject& a) {
            TranscribeOptions o;
            o.model = str(a, "model", "base.en").toStdString();
            o.language = str(a, "language", "auto").toStdString();
            o.speakers = a.value("speakers").toBool();
            const QString media = absolute(need(a, "media"));
            auto t = std::make_shared<Transcript>();
            std::string err;
            if (!transcribeMedia(media.toStdString(), o, *t, [this](double f) { progress(f, "Transcribing"); }, nullptr, &err))
                return fail(QString::fromStdString(err));
            QString text;
            for (const auto& seg : t->segments) {
                const QString who = QString::fromStdString(speakerName(*t, seg.speaker));
                text += QStringLiteral("[%1 - %2] ").arg(seg.start, 0, 'f', 2).arg(seg.end, 0, 'f', 2) + (who.isEmpty() ? QString() : who + ": ") +
                        QString::fromStdString(seg.text) + "\n";
            }
            if (a.contains("project")) {
                Loaded l = open(a);
                const Id id = mediaFor(l.project, media);
                l.project.findMedia(id)->transcript = t;
                save(l);
            }
            return ok(text.isEmpty() ? QStringLiteral("(no speech)") : text,
                      QJsonDocument::fromJson(QByteArray::fromStdString(transcriptToJson(*t))).object());
        });

    add("montage_find_phrase", "Find spoken words",
        "Find where a phrase is spoken in the cut (from the transcripts of the clips' media), as timeline times.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"phrase":{"type":"string"}},"required":["project","phrase"]})json", true,
        [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            Transcript cut;
            TranscriptSegment seg;
            seg.words = sequenceTranscriptWords(l.project, s);
            cut.segments.push_back(seg);
            QJsonArray hits;
            QString text;
            for (const auto& [from, to] : findPhrase(cut, need(a, "phrase").toStdString())) {
                const FrameTime f0 = FrameTime(std::floor(from * s.fpsValue())), f1 = FrameTime(std::ceil(to * s.fpsValue()));
                hits.append(QJsonObject{{"start", tc(f0, s)}, {"end", tc(f1, s)}, {"start_seconds", from}, {"end_seconds", to}});
                text += tc(f0, s) + " - " + tc(f1, s) + "\n";
            }
            if (seg.words.empty()) return ok("Nothing in the sequence is transcribed (use montage_transcribe with the project)");
            return ok(hits.isEmpty() ? QStringLiteral("Not found") : text, QJsonObject{{"hits", hits}});
        });

    add("montage_cut_speech", "Cut by transcript",
        "Edit the cut by what is said, as in a text-based editor: remove every place a phrase is spoken, the filler words "
        "(um, uh, er...) and/or pauses longer than pauses_longer_than seconds (shortened to keep_pause). Every track is cut "
        "the same way and closed up, captions included. smooth_cuts puts a Smooth Cut (an optical-flow morph) on each join "
        "in the picture, to hide the jump. One undoable edit; use montage_find_phrase first to see what a phrase matches.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "phrases":{"type":"array","items":{"type":"string"},"description":"Phrases to cut, every time they are said"},
            "fillers":{"type":"boolean","default":false},
            "pauses_longer_than":{"type":"number","description":"Seconds; omit to keep pauses"},
            "keep_pause":{"type":"number","default":0.3},
            "smooth_cuts":{"type":"boolean","default":false}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const double fps = s.fpsValue();
            const std::vector<TranscriptWord> words = sequenceTranscriptWords(l.project, s);
            if (words.empty()) return fail("Nothing in the sequence is transcribed (use montage_transcribe with the project)");
            std::vector<FrameRange> ranges;
            QJsonArray found;
            Transcript cut;
            cut.segments.emplace_back();
            cut.segments.back().words = words;
            for (const QJsonValue& v : a.value("phrases").toArray()) {
                int n = 0;
                for (const auto& [from, to] : findPhrase(cut, v.toString().toStdString())) {
                    // As the Transcript panel deletes words: up to the next word when the pause after is short.
                    double end = to + 0.1;
                    for (const TranscriptWord& w : words)
                        if (w.start >= to - 1e-6) {
                            end = w.start - end < 0.5 ? w.start : std::min(end, w.start);
                            break;
                        }
                    ranges.emplace_back(FrameTime(std::llround(from * fps)), FrameTime(std::llround(end * fps)));
                    ++n;
                }
                found.append(QJsonObject{{"phrase", v.toString()}, {"times", n}});
            }
            int fillers = 0;
            if (a.value("fillers").toBool()) {
                for (const FrameRange& r : fillerWordRanges(words, fps)) ranges.push_back(r);
                fillers = int(std::count_if(words.begin(), words.end(), [](const TranscriptWord& w) { return isFillerWord(w.text); }));
            }
            int pauses = 0;
            if (a.value("pauses_longer_than").isDouble()) {
                const double longer = std::max(0.1, a.value("pauses_longer_than").toDouble());
                const double keep = std::clamp(a.value("keep_pause").toDouble(0.3), 0.0, longer);
                const auto p = pauseRanges(words, fps, longer, keep);
                pauses = int(p.size());
                ranges.insert(ranges.end(), p.begin(), p.end());
            }
            if (mergeRanges(ranges).empty()) return ok("Nothing to cut", QJsonObject{{"phrases", found}, {"removed_seconds", 0}});
            const FrameTime smooth = a.value("smooth_cuts").toBool() ? std::max<FrameTime>(2, FrameTime(std::lround(fps * 0.2))) : 0;
            const edit::Result r = rippleDeleteRanges(l.project, s, ranges, smooth);
            if (!r.ok) return fail(QString::fromStdString(r.error));
            save(l);
            int joins = 0;
            for (const Track& t : s.videoTracks)
                joins += int(std::count_if(t.transitions.begin(), t.transitions.end(), [](const Transition& tr) { return tr.type == "smooth_cut"; }));
            const double secs = double(r.applied) / fps;
            return ok(QStringLiteral("Cut %1 s: %2 phrase match(es), %3 filler word(s), %4 pause(s)%5")
                          .arg(secs, 0, 'f', 2)
                          .arg(std::accumulate(found.begin(), found.end(), 0, [](int n, const QJsonValue& v) { return n + v.toObject().value("times").toInt(); }))
                          .arg(fillers)
                          .arg(pauses)
                          .arg(smooth ? QStringLiteral(", with Smooth Cuts") : QString()),
                      QJsonObject{{"phrases", found}, {"fillers", fillers}, {"pauses", pauses}, {"removed_seconds", secs},
                                  {"smooth_cuts", joins}, {"duration", tc(s.duration(), s)}});
        });

    add("montage_script_cut", "Build a cut from a script",
        "Find each line of a script in the project's transcribed takes and build a new sequence with the best reading of "
        "each line in script order on V1/A1 (most of the line said, fewest extra words, the named speaker when the "
        "transcript names speakers), other readings disabled on the tracks above, and a marker per line. Pass the script "
        "as text or a file (plain text, Fountain or Final Draft .fdx); a name in capitals above the words or \"Name:\" "
        "before them gives the speaker. dry_run reports what was found without building. Transcribe the takes first.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "script":{"type":"string","description":"The script's text"},
            "script_file":{"type":"string","description":"Or a file holding it"},
            "name":{"type":"string","default":"Script Cut"},
            "min_coverage":{"type":"number","default":0.6,"description":"Share of a line a reading must say (0.2-1)"},
            "alternates":{"type":"integer","default":3},
            "handle":{"type":"number","default":0.15,"description":"Seconds kept around each reading"},
            "markers":{"type":"boolean","default":true},
            "dry_run":{"type":"boolean","default":false}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            std::string text = a.value("script").toString().toStdString();
            if (const QString file = a.value("script_file").toString(); !file.isEmpty()) {
                QFile f(absolute(file));
                if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Cannot open %1").arg(file));
                text = f.readAll().toStdString();
                if (file.endsWith(QStringLiteral(".fdx"), Qt::CaseInsensitive)) text = fdxToScript(text);
            }
            const auto lines = parseScript(text);
            if (lines.empty()) return fail("No script lines (pass script or script_file)");
            ScriptCutOptions o;
            o.minCoverage = std::clamp(a.value("min_coverage").toDouble(0.6), 0.2, 1.0);
            o.maxAlternates = std::clamp(a.value("alternates").toInt(3), 0, 8);
            o.handle = std::clamp(a.value("handle").toDouble(0.15), 0.0, 2.0);
            o.markers = a.value("markers").toBool(true);
            const auto matches = matchScript(l.project, lines, o);
            QJsonArray report;
            int found = 0;
            for (const ScriptMatch& m : matches) {
                QJsonObject line{{"line", QString::fromStdString(m.line.text)}, {"takes", int(m.takes.size())}};
                if (!m.line.speaker.empty()) line["speaker"] = QString::fromStdString(m.line.speaker);
                if (!m.takes.empty()) {
                    ++found;
                    const ScriptTake& t = m.takes.front();
                    const MediaItem* media = l.project.findMedia(t.mediaId);
                    line["best"] = QJsonObject{{"media", media ? QString::fromStdString(media->name) : QString()},
                                               {"start", t.start}, {"end", t.end}, {"extra_words", t.extraWords},
                                               {"coverage", t.coverage}};
                }
                report.append(line);
            }
            if (a.value("dry_run").toBool())
                return ok(QStringLiteral("Found %1 of %2 line(s)").arg(found).arg(matches.size()), QJsonObject{{"lines", report}});
            if (found == 0) return fail("None of the lines were found in the transcribed takes (use montage_transcribe first)");
            const QString name = a.value("name").toString(QStringLiteral("Script Cut"));
            const ScriptCutResult r = buildScriptCut(l.project, matches, name.toStdString(), o);
            if (!r.sequence) return fail("Nothing could be placed");
            l.project.activeSequence = r.sequence;
            save(l);
            const Sequence& s = l.seq();
            return ok(QStringLiteral("Built \"%1\": %2 line(s) placed, %3 alternate(s), %4 not found")
                          .arg(name)
                          .arg(r.placed)
                          .arg(r.alternates)
                          .arg(r.missing),
                      QJsonObject{{"sequence", name}, {"placed", r.placed}, {"alternates", r.alternates}, {"missing", r.missing},
                                  {"duration", tc(s.duration(), s)}, {"lines", report}});
        });

    add("montage_find_shots", "Find shots by description",
        "Search the project's footage by what it shows (\"a dog on a beach\", \"close-up of hands\"), with CLIP running on "
        "this computer. Videos not indexed yet are indexed first (once; the index is saved in the project). Returns the "
        "best moments: media file and media times, best first.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"query":{"type":"string"},
            "max":{"type":"integer","default":10}},"required":["project","query"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            if (!visualSearchAvailable()) return fail("This build of Montage cannot search footage (no ONNX Runtime)");
            if (!visualModel().installed())
                return fail("The visual search model is not downloaded: run `scripts/fetch-models.sh` or open Find Shots in the app once");
            std::string err;
            bool changed = false;
            if (const QString e = indexMissing(l.project, {}, changed); !e.isEmpty()) return fail(e);
            if (changed) save(l);
            auto clip = ClipModel::load(&err);
            const std::vector<float> q = clip ? clip->text(need(a, "query").toStdString(), &err) : std::vector<float>{};
            if (q.empty()) return fail(QString::fromStdString(err));
            const auto hits = findShots(l.project, q, size_t(std::clamp(a.value("max").toInt(10), 1, 100)));
            QJsonArray list;
            QString text;
            for (const ShotMatch& h : hits) {
                const MediaItem* m = l.project.findMedia(h.media);
                if (!m) continue;
                list.append(QJsonObject{{"media", QString::fromStdString(m->path)}, {"start_seconds", h.start}, {"end_seconds", h.end},
                                        {"best_seconds", h.best}, {"score", double(h.score)}});
                text += QStringLiteral("%1  %2-%3 s (best %4 s, score %5)\n")
                            .arg(QString::fromStdString(m->name))
                            .arg(h.start, 0, 'f', 1)
                            .arg(h.end, 0, 'f', 1)
                            .arg(h.best, 0, 'f', 1)
                            .arg(double(h.score), 0, 'f', 3);
            }
            return ok(text.isEmpty() ? QStringLiteral("No indexed video") : text, QJsonObject{{"moments", list}});
        });

    add("montage_log_media", "Log media",
        "Log media in a project as an editor does, to find it again: a rating (-1 rejects, 0 unrated, 1-5 stars), a colour "
        "label, keywords to add or remove, metadata fields (scene, shot, take, camera, device, description, comment, or name) "
        "and the bin it is in (\"Interviews/Day 1\"; \"\" for the top level).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "media":{"type":["string","array"],"items":{"type":"string"},"description":"Media files or names in the project"},
            "rating":{"type":"integer","minimum":-1,"maximum":5},
            "label":{"type":"string","description":"None, Violet, Iris, Caribbean, Lavender, Cerulean, Forest, Rose, Mango, Yellow, Tan or Red"},
            "add_keywords":{"type":["array","string"],"items":{"type":"string"}},
            "remove_keywords":{"type":["array","string"],"items":{"type":"string"}},
            "fields":{"type":"object","additionalProperties":{"type":"string"},"description":"Field name to text; empty text clears it"},
            "bin":{"type":"string"}},"required":["project","media"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            std::vector<MediaItem*> items;
            const QJsonValue mv = a.value("media");
            if (mv.isString()) items.push_back(&projectMedia(l.project, mv.toString()));
            for (const QJsonValue& v : mv.toArray()) items.push_back(&projectMedia(l.project, v.toString()));
            if (items.empty()) throw ArgError{"\"media\" is required"};
            const QJsonObject fields = a.value("fields").toObject();
            for (auto it = fields.begin(); it != fields.end(); ++it) {
                const MediaField* f = mediaField(it.key().toStdString());
                if (!f || !f->editable || it.key() == "rating" || it.key() == "label" || it.key() == "keywords")
                    throw ArgError{QStringLiteral("\"%1\" is not a field to set; use scene, shot, take, camera, device, description, comment or name").arg(it.key())};
            }
            for (MediaItem* m : items) {
                if (a.value("rating").isDouble()) {
                    const int r = a.value("rating").toInt();
                    if (r < -1 || r > 5) throw ArgError{"\"rating\" must be -1 (rejected) to 5"};
                    m->rating = r;
                }
                if (a.contains("label") && !setMediaField(*m, "label", str(a, "label").toStdString()))
                    throw ArgError{QStringLiteral("Unknown label \"%1\"").arg(str(a, "label"))};
                addKeywords(m->keywords, stringList(a, "add_keywords"));
                removeKeywords(m->keywords, stringList(a, "remove_keywords"));
                for (auto it = fields.begin(); it != fields.end(); ++it)
                    if (!setMediaField(*m, it.key().toStdString(), it.value().toString().toStdString()))
                        throw ArgError{QStringLiteral("Cannot set %1 to \"%2\"").arg(it.key(), it.value().toString())};
            }
            if (a.value("bin").isString()) {
                std::vector<Id> ids;
                for (MediaItem* m : items) ids.push_back(m->id);
                std::string bin;
                for (const QString& part : str(a, "bin").split('/', Qt::SkipEmptyParts)) bin = joinBin(bin, part.simplified().toStdString());
                moveMediaToBin(l.project, ids, bin);
            }
            save(l);
            QJsonArray out;
            for (MediaItem* m : items) {
                QJsonObject o{{"name", QString::fromStdString(m->name)}};
                logJson(*m, o);
                out.append(o);
            }
            return ok(QStringLiteral("Logged %1 media item(s)").arg(items.size()), QJsonObject{{"media", out}});
        });

    add("montage_find_media", "Find media",
        "Find media in a project by text (names, keywords, metadata, speech; \"quoted phrases\") and/or rules on fields, as a "
        "smart bin does. Rule fields: any, name, rating, label, duration (seconds), kind (video, audio, image, sequence), "
        "keywords, usage (clips using it), scene, shot, take, camera, device, description, comment, created, width, height, "
        "fps, videoCodec, audioCodec, channels, colour, transcript, proxy, bin, path. Tests: contains, !contains, is, !is, "
        "starts, empty, !empty for text; >, >=, <, <=, is, !is for numbers and ratings; includes, !includes, empty, !empty "
        "for keywords. Optionally saves the rules as a smart bin the app shows.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "text":{"type":"string"},
            "rules":{"type":"array","items":{"type":"object","properties":{"field":{"type":"string"},"op":{"type":"string"},
                "value":{"type":["string","number"]}},"required":["field","op"]}},
            "match":{"type":"string","enum":["all","any"],"default":"all"},
            "save_as":{"type":"string","description":"Also save the rules as a smart bin with this name"}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            SmartBin b;
            b.name = str(a, "save_as").toStdString();
            b.matchAll = str(a, "match", "all") != "any";
            b.rules = rulesArg(a.value("rules").toArray());
            const std::string text = str(a, "text").toStdString();
            const std::map<Id, int> usage = mediaUsage(l.project);
            QJsonArray list;
            QString lines;
            for (const MediaItem& m : l.project.media) {
                if (!smartBinMatches(b, m, usage, &l.project) || !mediaMatchesSearch(m, text, &l.project)) continue;
                QJsonObject o{{"name", QString::fromStdString(m.name)}, {"duration_seconds", m.duration}};
                if (!m.path.empty()) o["path"] = QString::fromStdString(m.path);
                o["usage"] = usage.count(m.id) ? usage.at(m.id) : 0;
                logJson(m, o);
                list.append(o);
                lines += QString::fromStdString(m.name) + (m.rating > 0 ? "  " + QString(m.rating, QChar(0x2605)) : m.rating < 0 ? "  rejected" : "") +
                         (m.keywords.empty() ? QString() : "  [" + QString::fromStdString(joinKeywords(m.keywords)) + "]") + "\n";
            }
            if (!b.name.empty()) {
                if (b.rules.empty()) throw ArgError{"A smart bin needs rules"};
                b.id = l.project.newId();
                l.project.smartBins.push_back(b);
                save(l);
            }
            return ok(lines.isEmpty() ? QStringLiteral("No media matches") : lines, QJsonObject{{"media", list}});
        });

    add("montage_make_subclip", "Make a subclip",
        "Save a range of a media item in the project as a subclip: a bin item of its own (to log, find and place) that "
        "plays that part of the media. Placing it with montage_place_media (by its name) places the range.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "media":{"type":"string","description":"A media file or name in the project"},
            "start_seconds":{"type":"number"},"end_seconds":{"type":"number"},
            "name":{"type":"string"}},"required":["project","media","start_seconds","end_seconds"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            MediaItem& src = projectMedia(l.project, need(a, "media"));
            auto sub = makeSubclip(l.project, src.id, a.value("start_seconds").toDouble(), a.value("end_seconds").toDouble(),
                                   str(a, "name").toStdString());
            if (!sub) return fail("Cannot make that subclip: it needs a video or audio file and a range inside it");
            sub->id = l.project.newId();
            l.project.media.push_back(*sub);
            save(l);
            QJsonObject o{{"name", QString::fromStdString(sub->name)}, {"id", double(sub->id)}};
            logJson(*sub, o);
            return ok(QStringLiteral("Made subclip \"%1\" (%2-%3 s)").arg(QString::fromStdString(sub->name)).arg(sub->subclipIn, 0, 'f', 2).arg(sub->subclipOut, 0, 'f', 2), o);
        });

    add("montage_auto_tag", "Auto-tag shots",
        "Tag videos (and subclips) with keywords for what they show, with CLIP on this computer: Close-up, Medium shot or "
        "Wide shot; Interior or Exterior; Day or Night; People. Videos are indexed first if needed. Returns each item's "
        "keywords and the stretches of footage each tag covers.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "media":{"type":"array","items":{"type":"string"},"description":"Media files or names; default: every video"}},
            "required":["project"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            if (!visualSearchAvailable()) return fail("This build of Montage cannot look at footage (no ONNX Runtime)");
            if (!visualModel().installed())
                return fail("The visual search model is not downloaded: run `scripts/fetch-models.sh` or open Find Shots in the app once");
            std::vector<Id> ids;
            for (const QJsonValue& v : a.value("media").toArray()) ids.push_back(projectMedia(l.project, v.toString()).id);
            if (ids.empty())
                for (const MediaItem& m : l.project.media)
                    if (m.kind == MediaKind::Video && m.hasVideo) ids.push_back(m.id);
            bool changed = false;
            if (const QString e = indexMissing(l.project, ids, changed); !e.isEmpty()) return fail(e);
            std::string err;
            auto clip = ClipModel::load(&err);
            const LabelEmbeddings labels = clip ? clip->labels(&err) : LabelEmbeddings{};
            if (labels.empty()) return fail(QString::fromStdString(err));
            QJsonArray list;
            QString text;
            for (Id id : ids) {
                MediaItem* m = l.project.findMedia(id);
                if (!m || m->kind != MediaKind::Video) continue;
                const AutoTags t = autoTagMedia(l.project, *m, labels);
                changed |= addKeywords(m->keywords, t.keywords);
                QJsonArray kw, runs;
                for (const std::string& k : t.keywords) kw.append(QString::fromStdString(k));
                for (const TagRun& r : t.runs)
                    runs.append(QJsonObject{{"keyword", QString::fromStdString(r.keyword)}, {"start_seconds", r.start}, {"end_seconds", r.end}});
                list.append(QJsonObject{{"name", QString::fromStdString(m->name)}, {"keywords", kw}, {"runs", runs}});
                text += QString::fromStdString(m->name) + ": " + (t.keywords.empty() ? QStringLiteral("(no tags)") : QString::fromStdString(joinKeywords(t.keywords))) + "\n";
            }
            if (changed) save(l);
            return ok(text.isEmpty() ? QStringLiteral("No videos") : text, QJsonObject{{"media", list}});
        });

    add("montage_auto_duck", "Duck music under dialogue",
        "Lower music under speech: wherever someone speaks on the dialogue tracks (transcript words where the media is "
        "transcribed, else loudness), the music clips' volume dips by amount_db, fading down before and up after. Written as "
        "volume keyframes on the music clips (existing volume keyframes on them are replaced).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "music_track":{"type":"string","description":"Audio track with the music, e.g. A2 (or give clips)"},
            "clips":{"type":"array","items":{"type":"number"},"description":"Music clip ids"},
            "dialogue_tracks":{"type":"array","items":{"type":"string"},"description":"Audio tracks with dialogue, e.g. [\"A1\"]"},
            "amount_db":{"type":"number","default":-15},"fade_down":{"type":"number","default":0.3},
            "fade_up":{"type":"number","default":0.8},"threshold_db":{"type":"number","default":-40}},
            "required":["project","dialogue_tracks"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            std::vector<Id> music;
            for (const QJsonValue& v : a.value("clips").toArray()) music.push_back(clipArg(l, QJsonObject{{"clip", v}}).id);
            if (a.contains("music_track")) {
                const TrackRef t = trackArg(str(a, "music_track"), s, false);
                if (t.kind != TrackKind::Audio) throw ArgError{"\"music_track\" must be an audio track"};
                for (const Clip& c : trackAt(s, t)->clips) music.push_back(c.id);
            }
            if (music.empty()) throw ArgError{"Give the music as \"music_track\" or \"clips\""};
            std::vector<int> tracks;
            for (const QJsonValue& v : a.value("dialogue_tracks").toArray()) {
                const TrackRef t = trackArg(v.toString(), s, false);
                if (t.kind != TrackKind::Audio) throw ArgError{"Dialogue tracks must be audio tracks"};
                tracks.push_back(t.index);
            }
            if (tracks.empty()) throw ArgError{"\"dialogue_tracks\" is required"};
            DuckOptions o;
            if (a.value("amount_db").isDouble()) o.amountDb = std::clamp(a.value("amount_db").toDouble(), -60.0, 0.0);
            if (a.value("fade_down").isDouble()) o.fadeDown = std::max(0.0, a.value("fade_down").toDouble());
            if (a.value("fade_up").isDouble()) o.fadeUp = std::max(0.0, a.value("fade_up").toDouble());
            if (a.value("threshold_db").isDouble()) o.thresholdDb = a.value("threshold_db").toDouble();
            std::string err;
            const Spans spans = dialogueSpans(l.project, s, tracks, o, &err);
            if (!err.empty()) return fail(QString::fromStdString(err));
            int changed = 0;
            for (Id id : music)
                if (Clip* c = edit::clipById(s, id)) changed += duckClip(*c, s, spans, o) ? 1 : 0;
            if (changed) save(l);
            QJsonArray list;
            for (const auto& [from, to] : spans) list.append(QJsonObject{{"start_seconds", from}, {"end_seconds", to}});
            return ok(QStringLiteral("Ducked %1 clip(s) under %2 stretch(es) of dialogue").arg(changed).arg(spans.size()),
                      QJsonObject{{"dialogue", list}, {"clips_changed", changed}});
        });

    add("montage_add_adjustment_layer", "Add an adjustment layer",
        "Add an adjustment layer: a clip whose effects (montage_add_effect on the returned clip id), opacity and blend mode "
        "apply to everything on the tracks below it, for a grade or a look over many clips at once.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"at":{"type":["number","string"]},
            "duration":{"type":["number","string"],"description":"Default: to the end of the sequence"},
            "track":{"type":"string","description":"Default: a new video track on top"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : 0;
            FrameTime len = a.contains("duration") ? timeArg(a.value("duration"), s, "duration") : s.duration() - at;
            if (len <= 0) throw ArgError{"The layer needs a length (the sequence is empty after \"at\")"};
            const TrackRef track = a.contains("track") ? trackArg(str(a, "track"), s, true, &l.project, &s)
                                                       : edit::addTrack(l.project, s, TrackKind::Video);
            if (track.kind != TrackKind::Video) throw ArgError{"An adjustment layer goes on a video track"};
            Clip c = makeGeneratorClip(l.project, "adjustment", len);
            c.start = at;
            const Id id = c.id;
            check(edit::overwrite(l.project, s, track, c));
            save(l);
            const Clip* placed = edit::clipById(s, id);
            const QJsonObject o = placed ? clipJson(l.project, s, *placed) : QJsonObject{};
            return ok(QStringLiteral("Adjustment layer %1 on V%2 from %3").arg(qulonglong(id)).arg(track.index + 1).arg(tc(at, s)), o);
        });

    add("montage_match_color", "Match colour to a shot",
        "Shot matching: grade clips so they look like the picture at reference_at (the cut as it plays there, with its "
        "grade). Each clip's own frame at reference_at, or its middle frame, is matched per channel (shadows, mid-tones, "
        "highlights) into a Color Correct put first in its effects, replacing an earlier match. Check the result with "
        "montage_render_frame.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "reference_at":{"type":["number","string"],"description":"Timeline time of the look to match"},
            "clips":{"type":"array","items":{"type":"number"},"description":"Clip ids to grade"}},
            "required":["project","reference_at","clips"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const FrameTime at = timeArg(a.value("reference_at"), s, "reference_at");
            std::vector<Id> clips;
            for (const QJsonValue& v : a.value("clips").toArray()) clips.push_back(clipArg(l, QJsonObject{{"clip", v}}).id);
            if (clips.empty()) throw ArgError{"\"clips\" is required"};
            const Image ref = colourReferenceFrame(l.project, s, at);
            const int n = matchClipColour(l.project, s, clips, ref, at);
            if (n == 0) return fail("None of the clips has a picture to match");
            save(l);
            return ok(QStringLiteral("Matched %1 clip(s) to the picture at %2").arg(n).arg(tc(at, s)), QJsonObject{{"matched", n}});
        });

    add("montage_auto_reframe", "Auto reframe for another shape",
        "Make the cut in another aspect ratio (9:16 for Reels, Shorts and TikTok; 1:1; 4:5; 16:9): a copy of the active "
        "sequence at the new shape where every video and still clip fills the frame and follows its subject (found from "
        "what moves and what stands out). The copy becomes the active sequence, so later edits and montage_render apply to "
        "it. motion: slower (interviews), default, or faster (sport).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "aspect":{"type":"string","enum":["9:16","1:1","4:5","16:9"],"default":"9:16"},
            "motion":{"type":"string","enum":["slower","default","faster"],"default":"default"},
            "name":{"type":"string"}},"required":["project"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            const QString aspect = str(a, "aspect", "9:16");
            const QStringList parts = aspect.split(':');
            const int aw = parts.size() == 2 ? parts[0].toInt() : 0, ah = parts.size() == 2 ? parts[1].toInt() : 0;
            if (aw <= 0 || ah <= 0) throw ArgError{"\"aspect\" must look like 9:16"};
            const QString motion = str(a, "motion", "default");
            const int speed = motion == "slower" ? 0 : motion == "faster" ? 2 : 1;
            std::map<Id, std::vector<ReframeKey>> paths;
            std::string err;
            if (!analyzeSequenceReframe(l.project, l.seq(), speed, paths, [this](double f) { progress(f, "Finding subjects"); }, nullptr, &err))
                return fail(QString::fromStdString(err));
            int w = 0, h = 0;
            reframeSize(l.seq(), aw, ah, w, h);
            const Id made = makeReframedSequence(l.project, l.seq().id, w, h, paths, str(a, "name").toStdString());
            if (!made) return fail("Could not copy the sequence");
            l.project.activeSequence = made;
            save(l);
            const Sequence& s = l.seq();
            return ok(QStringLiteral("Made \"%1\" (%2 x %3) with %4 clip(s) following their subject; it is now the active sequence")
                          .arg(QString::fromStdString(s.name)).arg(w).arg(h).arg(paths.size()),
                      QJsonObject{{"sequence", double(made)}, {"name", QString::fromStdString(s.name)}, {"width", w}, {"height", h},
                                  {"clips_reframed", int(paths.size())}});
        });

    add("montage_render_frame", "Look at a frame",
        "Render the program at a timeline time and return it as an image (to check an edit), optionally saving a PNG.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"at":{"type":["number","string"]},
            "width":{"type":"integer","default":640,"description":"Width of the returned image"},
            "output":{"type":"string","description":"Also save the full-size frame here (PNG)"}},"required":["project","at"]})json", true,
        [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const FrameTime at = timeArg(a.value("at"), s, "at");
            const int w = std::clamp(a.value("width").toInt(640), 64, 1920);
            RenderOptions ro;
            ro.scale = std::min(1.0, double(w) / std::max(1, s.width));
            ro.captions = true;
            Image img = renderProgramFrame(l.project, s, at, ro);
            flattenOver(img, 0, 0, 0);
            QImage q(img.width, img.height, QImage::Format_RGBA8888);
            toRgba8(img, q.bits(), size_t(q.bytesPerLine()));
            QByteArray png;
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            q.save(&buf, "PNG");
            if (a.contains("output")) {
                std::string err;
                if (!exportStill(l.project, s, at, absolute(str(a, "output")).toStdString(), &err)) return fail(QString::fromStdString(err));
            }
            ToolResult r = ok(QStringLiteral("Frame at %1 (%2x%3)").arg(tc(at, s)).arg(img.width).arg(img.height));
            r.png = png;
            return r;
        });

    add("montage_list_presets", "List export presets", "The export presets montage_render accepts.",
        R"json({"type":"object","properties":{}})json", true, [](const QJsonObject&) {
            QJsonArray list;
            QString text;
            for (const auto& p : exportPresets()) {
                list.append(QJsonObject{{"name", QString::fromStdString(p.name)}, {"extension", QString::fromStdString(p.extension)},
                                        {"description", QString::fromStdString(p.description)}});
                text += QString::fromStdString(p.name + " (." + p.extension + "): " + p.description) + "\n";
            }
            return ok(text, QJsonObject{{"presets", list}});
        });

    add("montage_render", "Render",
        "Render the active sequence (or its in-out range) to a file with an export preset (default \"H.264 - High Quality\"), "
        "optionally normalising the mix's loudness for where it is going.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"output":{"type":"string"},
            "preset":{"type":"string"},"in":{"type":["number","string"]},"out":{"type":["number","string"]},
            "loudness_lufs":{"type":"number","description":"Normalise the mix to this loudness, e.g. -14 (streaming) or -23 (EBU R128)"},
            "peak_ceiling":{"type":"number","default":-1,"description":"True peak ceiling (dBTP) when normalising"},
            "burn_in":{"type":"object","description":"Overlays for a review copy","properties":{
                "timecode":{"type":"boolean"},"clip_name":{"type":"boolean"},"text":{"type":"string"},
                "corner":{"type":"string","enum":["top_left","top_centre","top_right","bottom_left","bottom_centre","bottom_right"],"default":"top_left"},
                "watermark":{"type":"string","description":"Image file, e.g. a logo"},
                "watermark_corner":{"type":"string","enum":["top_left","top_centre","top_right","bottom_left","bottom_centre","bottom_right"],"default":"bottom_right"},
                "watermark_opacity":{"type":"number","default":0.6}}}},
            "required":["project","output"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const ExportPreset* pr = findExportPreset(str(a, "preset", "H.264 - High Quality").toStdString());
            if (!pr) throw ArgError{"Unknown preset (see montage_list_presets)"};
            ExportSettings st = pr->settings;
            st.path = absolute(need(a, "output")).toStdString();
            if (a.contains("in")) st.in = timeArg(a.value("in"), s, "in");
            if (a.contains("out")) st.out = timeArg(a.value("out"), s, "out");
            if (a.value("loudness_lufs").isDouble()) {
                st.loudnessTarget = a.value("loudness_lufs").toDouble();
                if (st.loudnessTarget >= 0 || st.loudnessTarget < -70) throw ArgError{"\"loudness_lufs\" must be between -70 and 0"};
                st.peakCeiling = std::min(0.0, a.value("peak_ceiling").toDouble(-1));
            }
            if (a.value("burn_in").isObject()) {
                const QJsonObject b = a.value("burn_in").toObject();
                static const QStringList corners = {"top_left", "top_centre", "top_right", "bottom_left", "bottom_centre", "bottom_right"};
                auto corner = [&](const char* key, int def) {
                    if (!b.contains(key)) return def;
                    const int i = int(corners.indexOf(b.value(key).toString()));
                    if (i < 0) throw ArgError{QStringLiteral("\"%1\" must be one of %2").arg(key, corners.join(", "))};
                    return i;
                };
                st.burnIn.timecode = b.value("timecode").toBool();
                st.burnIn.clipName = b.value("clip_name").toBool();
                st.burnIn.text = b.value("text").toString().toStdString();
                st.burnIn.corner = corner("corner", 0);
                if (b.contains("watermark")) st.burnIn.watermark = absolute(b.value("watermark").toString()).toStdString();
                st.burnIn.watermarkCorner = corner("watermark_corner", 5);
                st.burnIn.watermarkOpacity = std::clamp(b.value("watermark_opacity").toDouble(0.6), 0.0, 1.0);
            }
            std::string err;
            if (!exportSequence(l.project, s, st, [this](double f, FrameTime) { progress(f, "Rendering"); }, nullptr, &err))
                return fail(QString::fromStdString(err));
            return ok(QStringLiteral("Wrote %1").arg(QString::fromStdString(st.path)), QJsonObject{{"output", QString::fromStdString(st.path)}});
        });

    add("montage_export_timeline", "Export the timeline",
        "Write the active sequence as an EDL, OpenTimelineIO, Final Cut Pro 7 XML (Premiere, Resolve) or FCPXML (Final Cut Pro).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"format":{"type":"string","enum":["edl","otio","xml","fcpxml"]},
            "output":{"type":"string"}},"required":["project","format","output"]})json",
        true, [](const QJsonObject& a) {
            Loaded l = open(a);
            const QString f = need(a, "format");
            const std::string text = f == "otio" ? exportOtio(l.project, l.seq())
                                     : f == "xml" ? exportFcp7Xml(l.project, l.seq())
                                     : f == "fcpxml" ? exportFcpXml(l.project, l.seq())
                                     : f == "edl" ? exportEdl(l.project, l.seq())
                                                  : throw ArgError{"format must be edl, otio, xml or fcpxml"};
            const QString out = absolute(need(a, "output"));
            QFile file(out);
            if (!file.open(QIODevice::WriteOnly) || file.write(text.data(), qint64(text.size())) != qint64(text.size()))
                return fail(QStringLiteral("Cannot write %1").arg(out));
            return ok(QStringLiteral("Wrote %1").arg(out));
        });

    add("montage_import_timeline", "Import a timeline",
        "Make a project from an EDL, OpenTimelineIO, Final Cut Pro 7 XML or FCPXML file (media found by path).",
        R"json({"type":"object","properties":{"input":{"type":"string"},"project":{"type":"string","description":"The .montage file to write"},
            "fps":{"type":"number","description":"Frame rate for an EDL (default 30)"}},"required":["input","project"]})json",
        false, [](const QJsonObject& a) {
            std::string in = absolute(need(a, "input")).toStdString();
            if (std::filesystem::is_directory(in)) in += "/Info.fcpxml";
            std::ifstream f(in, std::ios::binary);
            if (!f) return fail(QStringLiteral("Cannot read %1").arg(QString::fromStdString(in)));
            const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            Loaded l;
            l.path = absolute(need(a, "project"));
            l.project.name = std::filesystem::path(in).stem().string();
            const MediaProber prober = [](const std::string& file, MediaItem& m) { return probeMedia(file, m, nullptr); };
            const std::string ext = std::filesystem::path(in).extension().string();
            const double fps = a.value("fps").toDouble(30);
            const Rational rate = std::fabs(fps - std::round(fps)) < 1e-6 ? Rational{int(std::lround(fps)), 1} : Rational{int(std::lround(fps * 1001)), 1001};
            const ImportResult r = ext == ".edl" ? importEdl(l.project, text, rate, prober, std::filesystem::path(in).parent_path().string())
                                   : ext == ".xml" || ext == ".fcpxml" ? importXmlTimeline(l.project, text, prober)
                                                                       : importOtio(l.project, text, prober);
            if (!r.ok) return fail(QString::fromStdString(r.error));
            save(l);
            QString note = QStringLiteral("Imported %1 clip(s) into %2").arg(r.clips).arg(l.path);
            for (const auto& o : r.offline) note += "\nOffline: " + QString::fromStdString(o);
            for (const auto& w : r.warnings) note += "\nWarning: " + QString::fromStdString(w);
            return ok(note, projectJson(l.project));
        });

    add("montage_undo", "Undo the last edit",
        "Put the project back as it was before the last edit made through these tools (one step).",
        R"json({"type":"object","properties":{"project":{"type":"string"}},"required":["project"]})json", false,
        [](const QJsonObject& a) {
            const QString path = absolute(need(a, "project")), bak = path + ".bak";
            if (!QFileInfo::exists(bak)) return fail("Nothing to undo");
            QFile::remove(path);
            if (!QFile::rename(bak, path)) return fail(QStringLiteral("Cannot restore %1").arg(path));
            return ok(QStringLiteral("Restored %1").arg(path));
        });
}

// ---- Protocol -------------------------------------------------------------------------------

McpServer::McpServer() : d_(new Impl) {}
McpServer::~McpServer() { delete d_; }

const std::vector<std::string>& McpServer::protocolVersions() {
    static const std::vector<std::string> v = {kModern, "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
    return v;
}

std::vector<std::string> McpServer::handle(const std::string& message) {
    std::vector<std::string> out;
    auto send = [&](const QJsonObject& o) { out.push_back(QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString()); };
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(message), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        send(QJsonObject{{"jsonrpc", "2.0"}, {"id", QJsonValue()}, {"error", QJsonObject{{"code", -32700}, {"message", "Parse error"}}}});
        return out;
    }
    const QJsonObject msg = doc.object();
    const QString method = msg.value("method").toString();
    const bool isRequest = msg.contains("id") && !msg.value("id").isNull();
    if (method.isEmpty()) {
        if (isRequest) send(QJsonObject{{"jsonrpc", "2.0"}, {"id", msg.value("id")}, {"error", QJsonObject{{"code", -32600}, {"message", "Invalid request"}}}});
        return out;  // a response to us: nothing to do
    }
    if (!isRequest) return out;  // notifications (initialized, cancelled...) need no reply
    const QJsonValue id = msg.value("id");
    const QJsonObject params = msg.value("params").toObject();
    const QJsonObject meta = params.value("_meta").toObject();
    auto error = [&](int code, const QString& text, const QJsonValue& data = QJsonValue()) {
        QJsonObject e{{"code", code}, {"message", text}};
        if (!data.isUndefined() && !data.isNull()) e["data"] = data;
        send(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"error", e}});
        return out;
    };
    // Modern requests carry their protocol version; legacy ones follow initialize.
    const bool modern = meta.contains(kVersionKey);
    QJsonArray supported;
    for (const auto& v : protocolVersions()) supported.append(QString::fromStdString(v));
    if (modern) {
        const QString version = meta.value(kVersionKey).toString();
        if (version != kModern)
            return error(-32022, "Unsupported protocol version", QJsonObject{{"supported", supported}, {"requested", version}});
        if (!meta.contains(kCapabilitiesKey)) return error(-32602, "Missing io.modelcontextprotocol/clientCapabilities in _meta");
    }
    auto reply = [&](QJsonObject result) {
        if (modern) {
            result["resultType"] = "complete";
            QJsonObject m = result.value("_meta").toObject();
            m[kServerInfoKey] = serverInfo();
            result["_meta"] = m;
        }
        send(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        return out;
    };

    if (method == "initialize") {
        const QString asked = params.value("protocolVersion").toString();
        QString chosen = "2025-11-25";
        for (const auto& v : protocolVersions())
            if (QString::fromStdString(v) == asked && v != kModern) chosen = asked;
        d_->legacyVersion = chosen;
        return reply(QJsonObject{{"protocolVersion", chosen},
                                 {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
                                 {"serverInfo", serverInfo()},
                                 {"instructions", kInstructions}});
    }
    if (method == "server/discover")
        return reply(QJsonObject{{"supportedVersions", supported},
                                 {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                                 {"instructions", kInstructions}});
    if (method == "ping") return reply(QJsonObject{});
    if (method == "tools/list") {
        QJsonArray list;
        for (const auto& t : d_->tools) {
            QJsonObject o{{"name", t.name}, {"title", t.title}, {"description", t.description}, {"inputSchema", t.schema}};
            o["annotations"] = QJsonObject{{"readOnlyHint", t.readOnly}, {"destructiveHint", false}, {"openWorldHint", false}};
            list.append(o);
        }
        return reply(QJsonObject{{"tools", list}});
    }
    if (method == "tools/call") {
        const QString name = params.value("name").toString();
        auto tool = std::find_if(d_->tools.begin(), d_->tools.end(), [&](const auto& t) { return t.name == name; });
        if (tool == d_->tools.end()) return error(-32602, QStringLiteral("Unknown tool: %1").arg(name));
        d_->progressToken = meta.value("progressToken");
        d_->notify = [&](const QJsonObject& n) {
            if (!d_->live) return send(n);
            *d_->live << QJsonDocument(n).toJson(QJsonDocument::Compact).toStdString() << '\n';
            d_->live->flush();
        };
        ToolResult r;
        try {
            r = tool->run(params.value("arguments").toObject());
        } catch (const ArgError& e) {
            r = fail(e.message);
        } catch (const std::exception& e) {
            r = fail(QString::fromUtf8(e.what()));
        }
        d_->notify = nullptr;
        QJsonArray content;
        if (!r.png.isEmpty())
            content.append(QJsonObject{{"type", "image"}, {"data", QString::fromLatin1(r.png.toBase64())}, {"mimeType", "image/png"}});
        content.append(QJsonObject{{"type", "text"}, {"text", r.text}});
        QJsonObject result{{"content", content}, {"isError", r.error}};
        if (!r.structured.isEmpty()) result["structuredContent"] = r.structured;
        return reply(result);
    }
    return error(-32601, QStringLiteral("Method not found: %1").arg(method));
}

int McpServer::run(std::istream& in, std::ostream& out) {
    d_->live = &out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        for (const std::string& reply : handle(line)) out << reply << '\n';
        out.flush();
    }
    return 0;
}

}  // namespace montage
