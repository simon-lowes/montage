#include "McpServer.h"

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QDir>
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
#include "core/Automation.h"
#include "core/CaptionTools.h"
#include "core/Captions.h"
#include "core/Chapters.h"
#include "core/MarkerList.h"
#include "core/MaskPath.h"
#include "core/Bleep.h"
#include "core/Checkerboard.h"
#include "core/EditOps.h"
#include "core/TimelineCompare.h"
#include "core/Effects.h"
#include "core/History.h"
#include "core/Interchange.h"
#include "core/MediaLog.h"
#include "core/ProjectIO.h"
#include "core/ScriptCut.h"
#include "core/Surround.h"
#include "core/TranscriptEdit.h"
#include "media/Relink.h"
#include "media/SpeechEnhance.h"
#include "media/SuperScale.h"
#include "media/Translator.h"
#include "render/AafExport.h"
#include "render/AutoMix.h"
#include "render/AutoBroll.h"
#include "render/Highlights.h"
#include "render/MusicEdit.h"
#include "render/VoiceMatch.h"
#include "media/Analysis.h"
#include "media/AutoDuck.h"
#include "core/Slate.h"
#include "render/PaperEdit.h"
#include "render/LutExport.h"
#include "render/ProjectManager.h"
#include "render/QualityCheck.h"
#include "media/Decoder.h"
#include "media/Faces.h"
#include "media/DepthMap.h"
#include "media/Matting.h"
#include "media/Inpaint.h"
#include "media/Rife.h"
#include "media/TextToSpeech.h"
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
    if (!c.role.empty()) o["role"] = QString::fromStdString(c.role);
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
    QJsonObject out{{"sequence", QString::fromStdString(s.name)},
                       {"width", s.width},
                       {"height", s.height},
                       {"fps", s.fpsValue()},
                       {"duration", tc(s.duration(), s)},
                       {"duration_seconds", secs(s.duration(), s)},
                       {"tracks", tracks},
                       {"markers", markers},
                       {"media", media}};
    if (!s.mutedRoles.empty()) {
        QJsonArray muted;
        for (const std::string& r : s.mutedRoles) muted.append(QString::fromStdString(r));
        out["muted_roles"] = muted;
    }
    return out;
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
    struct SpeechLine {
        std::string text;
        FrameTime at = 0, fit = -1;  // fit: the room it has, in frames (-1: any)
    };
    // A caption track's cues, each with the room until the next one starts.
    static std::vector<SpeechLine> captionSpeech(const CaptionTrack& t) {
        std::vector<SpeechLine> lines;
        for (size_t i = 0; i < t.captions.size(); ++i) {
            std::string text = t.captions[i].text;
            std::replace(text.begin(), text.end(), '\n', ' ');
            if (!text.empty())
                lines.push_back({text, t.captions[i].start, (i + 1 < t.captions.size() ? t.captions[i + 1].start : t.captions[i].end) - t.captions[i].start});
        }
        return lines;
    }
    // Speaks each line (Kokoro) into a WAV in `folder`, a little faster where it has too little room, and places it at its time
    // on audio track `track`, adding `placed` entries; an error message or "".
    QString speakLines(Project& p, Sequence& s, const std::vector<SpeechLine>& lines, const std::string& voice, double speed, TrackRef track,
                       const QString& folder, QJsonArray& placed, double& seconds);
    // Indexes the videos (all, or these and subclips' media) that have no visual index; an error message or "".
    QString indexMissing(Project& p, const std::vector<Id>& only, bool& changed);
};

namespace {
// A person by id or (case-insensitively) by name.
int personId(const Project& p, const QJsonValue& v) {
    for (const PersonSummary& s : peopleIn(p))
        if ((v.isDouble() && v.toInt() == s.id) || (v.isString() && QString::fromStdString(s.name).compare(v.toString().trimmed(), Qt::CaseInsensitive) == 0) ||
            (v.isString() && v.toString().trimmed() == QString::number(s.id)))
            return s.id;
    throw ArgError{QStringLiteral("No person \"%1\" (montage_find_people lists them)").arg(v.isDouble() ? QString::number(v.toInt()) : v.toString())};
}
}  // namespace

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

QString McpServer::Impl::speakLines(Project& p, Sequence& s, const std::vector<SpeechLine>& lines, const std::string& voice, double speed,
                                    TrackRef track, const QString& folder, QJsonArray& placed, double& seconds) {
    QDir().mkpath(folder);
    int n = 1;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::vector<float> audio;
        std::string err;
        progress(double(i) / lines.size(), QStringLiteral("Speaking"));
        if (!synthesizeSpeech(lines[i].text, voice, speed, audio, &err)) return QString::fromStdString(err);
        const double room = lines[i].fit > 0 ? lines[i].fit / s.fpsValue() : 0, said = double(audio.size()) / kTtsSampleRate;
        if (room > 0 && said > room * 1.02) {
            const double faster = std::min(1.6, speed * said / room);
            if (faster > speed * 1.02 && !synthesizeSpeech(lines[i].text, voice, faster, audio, &err)) return QString::fromStdString(err);
        }
        QString path;
        do path = folder + '/' + QString::fromStdString(s.name) + QStringLiteral(" Speech ") + QString::number(n++) + QStringLiteral(".wav");
        while (QFileInfo::exists(path));
        if (!writeSpeechWav(path.toStdString(), audio, &err)) return QString::fromStdString(err);
        const Id media = mediaFor(p, path);
        const edit::Result r = edit::placeMedia(p, s, media, lines[i].at, 0, -1, {TrackKind::Video, 0}, track, false);
        if (!r.ok) return QString::fromStdString(r.error);
        seconds += double(audio.size()) / kTtsSampleRate;
        placed.append(QJsonObject{{"file", path}, {"at_seconds", lines[i].at / s.fpsValue()}, {"seconds", double(audio.size()) / kTtsSampleRate},
                                  {"clip", r.created.empty() ? 0.0 : double(r.created.front())}});
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
            "width":{"type":"integer"},"height":{"type":"integer"},"fps":{"type":"number"},
            "audio_layout":{"type":"string","enum":["stereo","5.1","7.1"],"default":"stereo"}},
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
            if (a.contains("audio_layout")) {
                const std::string layout = a.value("audio_layout").toString().toStdString();
                if (std::find(audioLayouts().begin(), audioLayouts().end(), layout) == audioLayouts().end())
                    throw ArgError{"\"audio_layout\" must be stereo, 5.1 or 7.1"};
                s.audioLayout = layout;
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
        "Overwrite replaces what is there; insert pushes later clips along; place_on_top puts it on the first tracks "
        "above with room; ripple_overwrite replaces the clip at that time and moves later clips by the difference; "
        "smart_insert inserts at the edit nearest that time.",
        R"json({"type":"object","properties":{
            "project":{"type":"string"},"media":{"type":"string","description":"Media file, or the name of a subclip in the project"},
            "at":{"type":["number","string"],"description":"Timeline time; default: the end of the sequence"},
            "track":{"type":"string","description":"Video track, default V1 (a new one is made if it is the next number)"},
            "audio_track":{"type":"string","description":"Audio track, default A1"},
            "in":{"type":["number","string"],"description":"Source in, seconds (of the subclip, for one)"},
            "out":{"type":["number","string"],"description":"Source out, seconds (of the subclip, for one)"},
            "insert":{"type":"boolean","default":false},
            "mode":{"type":"string","enum":["overwrite","insert","place_on_top","ripple_overwrite","smart_insert"],
                    "description":"Default overwrite (insert, when insert is true)"}},
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
            const QString mode = str(a, "mode", a.value("insert").toBool() ? "insert" : "overwrite");
            static const QStringList modes{"overwrite", "insert", "place_on_top", "ripple_overwrite", "smart_insert"};
            if (!modes.contains(mode)) throw ArgError{QStringLiteral("Unknown mode \"%1\"").arg(mode)};
            FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : s.duration();
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
            edit::Result r;
            if (mode == "place_on_top") {
                r = edit::placeOnTop(l.project, s, media, at, in, out, v, au);
            } else if (mode == "ripple_overwrite") {
                const Clip* c = edit::clipAt(s, v, at);
                if (!c) c = edit::clipAt(s, au, at);
                if (!c) throw ArgError{QStringLiteral("No clip at %1 to replace").arg(tc(at, s))};
                at = c->start;
                r = edit::rippleOverwrite(l.project, s, c->id, media, in, out, v, au);
            } else {
                if (mode == "smart_insert") at = edit::nearestEdit(s, v, at);
                r = edit::placeMedia(l.project, s, media, at, in, out, v, au, mode != "overwrite");
            }
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

    add("montage_swap_clip", "Swap a clip with its neighbour",
        "Swap a clip with the one before or after it on its track: they change places within the span they share (a gap "
        "between them stays between them), with the clips linked to each.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "with":{"type":"string","enum":["next","previous"],"default":"next"}},"required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Id id = clipArg(l, a).id;
            const QString with = str(a, "with", "next");
            if (with != "next" && with != "previous") throw ArgError{"\"with\" is next or previous"};
            check(edit::swapClip(l.project, s, id, with == "next"));
            save(l);
            const Clip* c = edit::clipById(s, id);
            return ok(QStringLiteral("Swapped; the clip now starts at %1").arg(tc(c ? c->start : 0, s)), c ? clipJson(l.project, s, *c) : QJsonObject{});
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
        "Its length changes to match, and later clips ripple. frames picks how slow motion makes the frames between "
        "source frames: nearest (repeat), blend, optical_flow, or ai (RIFE, needs its model).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},"speed":{"type":"number"},
            "frames":{"type":"string","enum":["nearest","blend","optical_flow","ai"]}},
            "required":["project","clip","speed"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Id id = clipArg(l, a).id;
            const double sp = a.value("speed").toDouble();
            if (std::fabs(sp) < 0.01 || std::fabs(sp) > 100) throw ArgError{"Speed must be between 0.01 and 100 (or -0.01 and -100)"};
            int sampling = -1;
            if (a.contains("frames")) {
                const QString f = a.value("frames").toString();
                sampling = f == "nearest" ? 0 : f == "blend" ? 1 : f == "optical_flow" ? 2 : f == "ai" ? 3 : -2;
                if (sampling == -2) throw ArgError{"frames is nearest, blend, optical_flow or ai"};
                if (sampling == 3 && (!rifeAvailable() || !rifeModel().installed()))
                    return fail("AI frames need the RIFE model: run `scripts/fetch-models.sh` or choose them once in the app");
            }
            check(edit::setSpeed(l.project, l.seq(), id, std::fabs(sp), true, sp < 0));
            if (sampling >= 0)
                if (Clip* c = edit::clipById(l.seq(), id)) {
                    if (c->timing.empty()) c->timing = makeEffect(l.project, "time");
                    c->timing.params["sampling"] = Param(double(sampling));
                }
            save(l);
            const Clip* c = edit::clipById(l.seq(), id);
            return ok(QStringLiteral("Speed set to %1").arg(sp), c ? clipJson(l.project, l.seq(), *c) : QJsonObject{});
        });

    add("montage_add_title", "Add a title",
        "Add a text title over the picture at a time, for a duration (default 3 s), on a video track (default: the "
        "track above the top one in use). template picks a ready-made, animated design (a lower third takes two lines: "
        "name, newline, role; credits roll up the frame, a crawl runs along the bottom); the text replaces its sample text. "
        "motion makes any title roll or crawl, starting and ending off screen.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"text":{"type":"string"},
            "at":{"type":["number","string"]},"duration":{"type":["number","string"],"default":3},
            "track":{"type":"string"},"size":{"type":"number","description":"Font size in pixels"},
            "template":{"type":"string","enum":["plain","lower_third","lower_third_box","centred","chapter","callout","typewriter","end_card","credits","crawl"],"default":"plain"},
            "motion":{"type":"string","enum":["still","roll","crawl_left","crawl_right"],"description":"Move the whole title through the frame over its duration (credits, a ticker)"}},
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
            if (a.contains("motion")) {
                const QStringList kinds{"still", "roll", "crawl_left", "crawl_right"};
                const int m = int(kinds.indexOf(str(a, "motion")));
                if (m < 0) throw ArgError{"\"motion\" is still, roll, crawl_left or crawl_right"};
                c.generator.params["motion"] = Param(double(m));
            }
            c.start = timeArg(a.value("at"), s, "at");
            c.name = need(a, "text").left(40).toStdString();
            const auto r = edit::overwrite(l.project, s, t, c);
            check(r);
            save(l);
            const Clip* made = r.created.empty() ? nullptr : edit::clipById(s, r.created[0]);
            return ok(QStringLiteral("Added a title on %1").arg(QString::fromStdString(trackAt(s, t)->name)),
                      made ? clipJson(l.project, s, *made) : QJsonObject{});
        });

    add("montage_add_shape", "Add a shape",
        "Add a shape layer over the picture: a rectangle, ellipse, polygon, star, line or arrow, filled (a colour, or a "
        "gradient to a second colour) and/or outlined. Sizes and positions are in sequence pixels, from the centre. "
        "draw_on animates the outline drawing itself on over that many seconds (Trim End keyframed 0 to 100 %). Lottie "
        "animations (.json) and SVG graphics are placed like other media, with montage_place_media.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "shape":{"type":"string","enum":["rectangle","ellipse","polygon","star","line","arrow"],"default":"rectangle"},
            "at":{"type":["number","string"]},"duration":{"type":["number","string"],"default":3},"track":{"type":"string"},
            "width":{"type":"number","default":400},"height":{"type":"number","default":300},
            "x":{"type":"number","default":0},"y":{"type":"number","default":0},"rotation":{"type":"number","default":0},
            "fill":{"type":"string","description":"#rrggbb, or \"none\""},"gradient_to":{"type":"string","description":"#rrggbb"},
            "stroke_width":{"type":"number","default":0},"stroke_color":{"type":"string","default":"#ffffff"},
            "roundness":{"type":"number","default":0},"points":{"type":"integer","description":"Sides of a polygon, points of a star"},
            "opacity":{"type":"number","default":100},"draw_on":{"type":"number","description":"Seconds"}},
            "required":["project","at"]})json",
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
            static const QStringList shapes = {"rectangle", "ellipse", "polygon", "star", "line", "arrow"};
            const int kind = int(shapes.indexOf(str(a, "shape", "rectangle")));
            if (kind < 0) throw ArgError{QStringLiteral("\"shape\" must be one of %1").arg(shapes.join(", "))};
            const FrameTime len = a.contains("duration") ? timeArg(a.value("duration"), s, "duration") : FrameTime(std::llround(3 * s.fpsValue()));
            Clip c = makeGeneratorClip(l.project, "shape", std::max<FrameTime>(1, len));
            Effect& g = c.generator;
            auto setColor = [&](const char* name, const QString& hex) {
                const QColor col(hex);
                if (!col.isValid()) throw ArgError{QStringLiteral("\"%1\" is not a colour (use #rrggbb)").arg(hex)};
                g.params[std::string(name) + ".r"] = Param(col.redF());
                g.params[std::string(name) + ".g"] = Param(col.greenF());
                g.params[std::string(name) + ".b"] = Param(col.blueF());
            };
            g.params["shape"] = Param(double(kind));
            for (const char* k : {"width", "height", "rotation", "roundness", "opacity"})
                if (a.value(k).isDouble()) g.params[k] = Param(a.value(k).toDouble());
            if (a.value("x").isDouble()) g.params["pos_x"] = Param(a.value("x").toDouble());
            if (a.value("y").isDouble()) g.params["pos_y"] = Param(a.value("y").toDouble());
            if (a.value("points").isDouble()) g.params["points"] = Param(double(std::clamp(a.value("points").toInt(), 3, 64)));
            if (a.contains("fill")) {
                if (str(a, "fill") == "none") g.params["fill"] = Param(0.0);
                else setColor("fill_color", str(a, "fill"));
            }
            if (a.contains("gradient_to")) {
                setColor("fill_color2", str(a, "gradient_to"));
                g.params["gradient"] = Param(1.0);
            }
            if (a.value("stroke_width").isDouble()) g.params["stroke"] = Param(std::max(0.0, a.value("stroke_width").toDouble()));
            if (a.contains("stroke_color")) setColor("stroke_color", str(a, "stroke_color"));
            if (a.value("draw_on").isDouble()) {
                const FrameTime over = std::max<FrameTime>(1, FrameTime(std::llround(a.value("draw_on").toDouble() * s.fpsValue())));
                Param p;
                p.addKey(0, 0.0);
                p.addKey(std::min(over, c.duration - 1), 100.0);
                g.params["trim_end"] = p;
            }
            c.start = timeArg(a.value("at"), s, "at");
            c.name = findEffectInfo("shape")->params[0].choices[size_t(kind)];
            const auto r = edit::overwrite(l.project, s, t, c);
            check(r);
            save(l);
            const Clip* made = r.created.empty() ? nullptr : edit::clipById(s, r.created[0]);
            return ok(QStringLiteral("Added a %1 on %2").arg(shapes[kind], QString::fromStdString(trackAt(s, t)->name)),
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
        "mask: mask.shape 1 ellipse or 2 rectangle, mask.x / mask.y centre and mask.w / mask.h size as fractions of the frame; "
        "6 a gradient across that box (full effect above it, none below, mask.rotation turning it); or a drawn Bézier "
        "path through `mask_path` (points as frame fractions, [x, y] or {x, y, in: [dx, dy], out: [dx, dy]} with handles; "
        "`mask_smooth` gives points without handles smooth automatic ones); "
        "or to a range of distances: mask.depth 1, with mask.depth_low and mask.depth_high from 0 (farthest) to 100 (nearest). "
        "depth_blur (lens blur keeping one distance sharp), depth_fog, depth_map and relight (a virtual light) work from "
        "the picture's depth. "
        "remove_background cuts people out (keep 1 keeps the background instead), and mask.shape 4 limits any effect to "
        "the people in the picture. object_removal paints out whatever its mask covers and fills it in. stabilize and "
        "rolling_shutter (straightening the skew a rolling shutter gives pans; readout as a percentage of a frame) measure "
        "the camera's movement when added.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},"effect":{"type":"string"},
            "params":{"type":"object","additionalProperties":{"type":"number"}},
            "mask_path":{"type":"array","items":{"type":["array","object"]},"description":"A closed Bezier mask: three or more points, fractions of the clip's frame"},
            "mask_smooth":{"type":"boolean","default":false}},"required":["project","clip","effect"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Clip& c = clipArg(l, a);
            const std::string type = need(a, "effect").toStdString();
            const EffectInfo* info = findEffectInfo(type);
            if (!info || info->hidden || (info->category != EffectCategory::VideoFilter && info->category != EffectCategory::AudioFilter))
                throw ArgError{QStringLiteral("Unknown effect \"%1\" (see montage_list_effects)").arg(QString::fromStdString(type))};
            if (type == "enhance_speech" && (!speechEnhancerAvailable() || !speechModel().installed()))
                return fail("Enhance Speech needs its model: run `scripts/fetch-models.sh` or add the effect once in the app");
            if (type == "super_scale" && (!upscalerAvailable() || !upscaleModel().installed()))
                return fail("Super Scale needs its model: run `scripts/fetch-models.sh` or add the effect once in the app");
            Effect e = makeEffect(l.project, type);
            const QJsonObject params = a.value("params").toObject();
            for (auto it = params.begin(); it != params.end(); ++it) {
                const std::string name = it.key().toStdString();
                const bool known = std::any_of(info->params.begin(), info->params.end(), [&](const ParamInfo& p) { return p.name == name; }) ||
                                   (name.rfind("mask.", 0) == 0 && supportsMask(type));
                if (!known) throw ArgError{QStringLiteral("\"%1\" has no parameter \"%2\"").arg(QString::fromStdString(type), it.key())};
                e.params[name] = Param(it.value().toDouble());
            }
            if (a.contains("mask_path")) {
                if (!supportsMask(type)) throw ArgError{QStringLiteral("\"%1\" cannot have a mask").arg(QString::fromStdString(type))};
                const QJsonArray arr = a.value("mask_path").toArray();
                if (arr.size() < 3) throw ArgError{QStringLiteral("mask_path needs at least three points")};
                std::vector<PathPoint> pts;
                auto pair = [](const QJsonValue& v, double& x, double& y) {
                    const QJsonArray xy = v.toArray();
                    if (xy.size() != 2) return false;
                    x = xy[0].toDouble();
                    y = xy[1].toDouble();
                    return true;
                };
                for (const QJsonValue& v : arr) {
                    PathPoint p;
                    if (v.isArray()) {
                        if (!pair(v, p.x, p.y)) throw ArgError{QStringLiteral("Each mask_path point is [x, y] or {x, y, in, out}")};
                    } else {
                        const QJsonObject o = v.toObject();
                        if (!o.contains("x") || !o.contains("y")) throw ArgError{QStringLiteral("Each mask_path point needs x and y")};
                        p.x = o.value("x").toDouble();
                        p.y = o.value("y").toDouble();
                        if (o.contains("in") && !pair(o.value("in"), p.ix, p.iy)) throw ArgError{QStringLiteral("A point's \"in\" handle is [dx, dy]")};
                        if (o.contains("out") && !pair(o.value("out"), p.ox, p.oy)) throw ArgError{QStringLiteral("A point's \"out\" handle is [dx, dy]")};
                    }
                    pts.push_back(p);
                }
                double fw = 1, fh = 1;
                if (!clipFrameSize(l.project, l.seq(), c, fw, fh)) fw = l.seq().width, fh = l.seq().height;
                setMaskPathFromFrame(e, pts, fw, fh, a.value("mask_smooth").toBool());
            }
            if (type == "object_removal" && (!inpaintAvailable() || !inpaintModel().installed()))
                return fail("Object Removal needs its model: run `scripts/fetch-models.sh` or add it once in the app");
            if (needsFaces(e) && (!faceSearchAvailable() || !faceModel().installed()))
                return fail("Face Refinement needs the face models: run `scripts/fetch-models.sh` or add it once in the app");
            if ((needsPersonMatte(e, 0) || type == "behind_people") && (!mattingAvailable() || !mattingModel().installed()))
                return fail("Remove Background, Behind People and People masks need their model: run `scripts/fetch-models.sh` or add one once in the app");
            if (needsDepth(e, 0) && (!depthAvailable() || !depthModel().installed()))
                return fail("Depth effects need the depth model: run `scripts/fetch-models.sh` or add one once in the app");
            const auto loc = edit::locate(l.seq(), c.id);
            const bool audioClip = loc && loc->track.kind == TrackKind::Audio;
            if (audioClip != (info->category == EffectCategory::AudioFilter))
                throw ArgError{audioClip ? QStringLiteral("That is an audio clip: choose an audio effect")
                                         : QStringLiteral("That is a video clip: choose a video effect")};
            if (type == "stabilize" || type == "rolling_shutter") {
                // They work from the camera's movement, measured now, and move the whole frame (so they go first).
                std::string motion, err;
                if (!analyzeClipStabilization(l.project, l.seq(), c, motion, {}, nullptr, &err))
                    return fail(QStringLiteral("Could not measure the camera's movement: %1").arg(QString::fromStdString(err)));
                e.strings["motion"] = motion;
                c.effects.insert(c.effects.begin(), e);
                save(l);
                return ok(QStringLiteral("Added %1 to %2").arg(QString::fromStdString(info->displayName), QString::fromStdString(c.name)),
                          QJsonObject{{"effect_id", double(e.id)}});
            }
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

    add("montage_add_marker", "Add a marker",
        "Add a timeline marker with a name and comment. A chapter marker also becomes a chapter in exported MP4, MOV and "
        "MKV files and in montage_chapters' list. With a clip, the marker goes on the clip at the moment of its media shown "
        "at `at`, and travels with the clip.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"at":{"type":["number","string"]},
            "name":{"type":"string"},"comment":{"type":"string"},"chapter":{"type":"boolean","default":false},
            "clip":{"type":"number","description":"A clip id (montage_project_info) for a clip marker"}},"required":["project","at"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const FrameTime at = timeArg(a.value("at"), s, "at");
            const bool chapter = a.value("chapter").toBool();
            const Marker m{at, 0, str(a, "name").toStdString(), str(a, "comment").toStdString(), 0, chapter};
            if (a.contains("clip")) {
                const Clip& c = clipArg(l, a);
                if (!edit::addClipMarker(s, c.id, at, m)) throw ArgError{QStringLiteral("The clip is not at %1").arg(tc(at, s))};
                save(l);
                return ok(QStringLiteral("Clip marker on \"%1\" at %2").arg(QString::fromStdString(c.name), tc(at, s)));
            }
            edit::addMarker(s, m);
            save(l);
            return ok(QStringLiteral("%1 at %2").arg(chapter ? QStringLiteral("Chapter marker") : QStringLiteral("Marker"), tc(at, s)));
        });

    add("montage_chapters", "List chapters",
        "The sequence's chapter markers as YouTube's chapter list for a video description (\"0:00 Intro\", a line each), "
        "timed from `from` when the export starts there, with a warning when YouTube would not show them.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"from":{"type":["number","string"]},
            "to":{"type":["number","string"]}},"required":["project"]})json",
        true, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Sequence& s = l.seq();
            const FrameTime from = a.contains("from") ? timeArg(a.value("from"), s, "from") : 0;
            const FrameTime to = a.contains("to") ? timeArg(a.value("to"), s, "to") : -1;
            std::string warning;
            const std::string text = youtubeChapters(s, from, to, &warning);
            if (text.empty()) return fail(QString::fromStdString(warning));
            return ok(QString::fromStdString(text) + (warning.empty() ? QString() : QStringLiteral("\nNote: ") + QString::fromStdString(warning)));
        });

    add("montage_automate_track", "Automate a track's fader",
        "Set an audio track's fader automation, as written from the mixer: its mode (off, read, write, latch, touch) and "
        "volume (dB) and pan (-1 left to 1 right) points at times; points replace the lane's points between the first and last "
        "given. clear removes the lanes. Lists the lanes afterwards.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"track":{"type":"string","default":"A1"},
            "mode":{"type":"string","enum":["off","read","write","latch","touch"]},
            "volume":{"type":"array","items":{"type":"array","items":{"type":["number","string"]},"minItems":2,"maxItems":2},
                      "description":"[[time, dB], ...]"},
            "pan":{"type":"array","items":{"type":"array","items":{"type":["number","string"]},"minItems":2,"maxItems":2},
                   "description":"[[time, pan], ...]"},
            "clear":{"type":"boolean","default":false}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const TrackRef ref = trackArg(str(a, "track", "A1"), s, false);
            if (ref.kind != TrackKind::Audio) throw ArgError{QStringLiteral("Automation is for audio tracks (A1, A2...)")};
            Track& t = s.audioTracks.at(size_t(ref.index));
            if (a.value("clear").toBool()) t.volumeAuto = Param(), t.panAuto = Param();
            if (a.contains("mode")) {
                const QString m = str(a, "mode").toLower();
                const QStringList names{"off", "read", "write", "latch", "touch"};
                if (!names.contains(m)) throw ArgError{QStringLiteral("\"mode\" must be off, read, write, latch or touch")};
                t.automation = int(names.indexOf(m));
            }
            auto points = [&](const char* key, Param& lane, double lo, double hi) {
                const QJsonArray pts = a.value(key).toArray();
                if (pts.isEmpty()) return;
                std::vector<Keyframe> keys;
                for (const auto& v : pts) {
                    const QJsonArray pt = v.toArray();
                    if (pt.size() != 2) throw ArgError{QStringLiteral("\"%1\" points are [time, value]").arg(QString::fromLatin1(key))};
                    keys.push_back({timeArg(pt.at(0), s, key), std::clamp(pt.at(1).toDouble(), lo, hi)});
                }
                std::sort(keys.begin(), keys.end(), [](const Keyframe& x, const Keyframe& y) { return x.t < y.t; });
                std::erase_if(lane.keys, [&](const Keyframe& k) { return k.t >= keys.front().t && k.t <= keys.back().t; });
                for (const Keyframe& k : keys) lane.keys.push_back(k);
                std::sort(lane.keys.begin(), lane.keys.end(), [](const Keyframe& x, const Keyframe& y) { return x.t < y.t; });
            };
            points("volume", t.volumeAuto, -60, 12);
            points("pan", t.panAuto, -1, 1);
            save(l);
            auto describe = [&](const Param& lane, const char* unit) {
                QStringList parts;
                for (const Keyframe& k : lane.keys) parts << QStringLiteral("%1 %2%3").arg(tc(k.t, s)).arg(k.v, 0, 'f', 2).arg(QString::fromLatin1(unit));
                return parts.isEmpty() ? QStringLiteral("none") : parts.join(QStringLiteral(", "));
            };
            return ok(QStringLiteral("%1: %2 automation\nVolume: %3\nPan: %4")
                          .arg(str(a, "track", "A1"), QString::fromLatin1(automationModeName(trackAutomation(t))), describe(t.volumeAuto, " dB"),
                               describe(t.panAuto, "")));
        });

    add("montage_export_lut", "Export a clip's grade as a LUT",
        "Bake a clip's colour effects (Color Correct, Curves, Hue Curves, Levels, LUTs, colour space transforms...) into a "
        ".cube 3D LUT for monitors, cameras or other applications. Spatial and masked effects cannot be held by a LUT and "
        "are listed as left out.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "path":{"type":"string","description":"The .cube file to write"},
            "at":{"type":["number","string"],"description":"Sequence time for keyframed grades (default: the clip's start)"},
            "size":{"type":"integer","default":33,"minimum":2,"maximum":129}},"required":["project","clip","path"]})json",
        true, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Sequence& s = l.seq();
            const Clip& c = clipArg(l, a);
            const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : c.start;
            const FrameTime t = std::clamp<FrameTime>(at - c.start, 0, std::max<FrameTime>(0, c.duration - 1));
            std::vector<std::string> skipped;
            const Lut3D lut = bakeLut(c.effects, t, a.value("size").toInt(33), &skipped);
            const QString path = absolute(need(a, "path"));
            std::string err;
            if (!writeCubeLut(lut, path.toStdString(), c.name, &err)) return fail(QString::fromStdString(err));
            QStringList left;
            for (const std::string& n : skipped) left << QString::fromStdString(n);
            return ok(QStringLiteral("Wrote %1 (%2-point cube)%3").arg(path).arg(lut.size)
                          .arg(left.isEmpty() ? QString() : QStringLiteral("; left out: ") + left.join(QStringLiteral(", "))));
        });

    add("montage_audition", "Audition takes on a clip",
        "Auditions (Final Cut's auditions, Resolve's take selector): alternative takes held by a clip and tried in its "
        "place, keeping its position, length, effects and keyframes (linked sound follows). add puts media files in as "
        "takes (from in seconds); pick chooses a take by number (1 = first) or \"next\"/\"previous\"; finalize keeps "
        "the pick and drops the rest. Without these, lists the clip's takes.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "add":{"type":"array","items":{"type":"string"},"description":"Media files to add as takes"},
            "in":{"type":"number","default":0,"description":"Where the added takes start, in seconds of their media"},
            "pick":{"type":["integer","string"],"description":"A take number (1 = first), or next / previous"},
            "finalize":{"type":"boolean","default":false}},"required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Id clip = clipArg(l, a).id;
            auto check = [](const edit::Result& r) {
                if (!r.ok) throw ArgError{QString::fromStdString(r.error)};
            };
            bool changed = false;
            if (a.contains("add")) {
                std::vector<std::pair<Id, double>> media;
                for (const QJsonValue& v : a.value("add").toArray())
                    media.push_back({mediaFor(l.project, v.toString()), a.value("in").toDouble() * s.fpsValue()});
                check(edit::addTakes(l.project, s, clip, media));
                changed = true;
            }
            if (a.contains("pick")) {
                const QJsonValue pv = a.value("pick");
                if (pv.isString() && (pv.toString() == "next" || pv.toString() == "previous"))
                    check(edit::cycleTake(l.project, s, clip, pv.toString() == "next" ? 1 : -1));
                else
                    check(edit::pickTake(l.project, s, clip, (pv.isString() ? pv.toString().toInt() : pv.toInt()) - 1));
                changed = true;
            }
            if (a.value("finalize").toBool()) {
                check(edit::finalizeAudition(l.project, s, clip));
                changed = true;
            }
            if (changed) save(l);
            const Clip* c = edit::clipById(s, clip);
            if (c->takes.empty()) return ok(changed ? QStringLiteral("Kept %1; no other takes.").arg(QString::fromStdString(c->name))
                                                    : QStringLiteral("That clip has no takes."));
            QStringList lines;
            for (size_t i = 0; i < c->takes.size(); ++i)
                lines << QStringLiteral("%1%2. %3").arg(int(i) == c->take ? "* " : "  ").arg(i + 1).arg(QString::fromStdString(c->takes[i].name));
            return ok(QStringLiteral("Take %1 of %2 is in the cut:\n").arg(c->take + 1).arg(c->takes.size()) + lines.join('\n'));
        });

    add("montage_relink", "Find and relink offline media",
        "Media whose files have moved or gone show as Media Offline. Without arguments, lists them. With folder, looks "
        "there (and below) for their files by name, or by name with another extension for transcodes, checking each is "
        "the same footage (kind, picture size, length), and relinks what it finds. With media and path, points one item "
        "at a file; replace swaps in different footage (Replace Footage), keeping its clips' edits.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "folder":{"type":"string","description":"A folder to search for the offline files"},
            "media":{"type":"string","description":"The item's last known file path, or its name"},
            "path":{"type":"string","description":"The file to link it to"},
            "replace":{"type":"boolean","default":false,"description":"Replace Footage: accept different footage"}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            auto list = [&]() {
                QStringList lines;
                for (Id id : offlineMedia(l.project))
                    if (const MediaItem* m = l.project.findMedia(id))
                        lines << QStringLiteral("%1 (%2)").arg(QString::fromStdString(m->name), QString::fromStdString(m->path));
                return lines;
            };
            QString done;
            if (a.contains("folder")) {
                const QString folder = absolute(need(a, "folder"));
                if (!QDir(folder).exists()) return fail(QStringLiteral("No folder %1").arg(folder));
                const auto found = relinkFromFolder(l.project, folder.toStdString());
                if (!found.empty()) save(l);
                done = QStringLiteral("Relinked %1 from %2.").arg(found.size()).arg(folder);
            } else if (a.contains("media") || a.contains("path")) {
                MediaItem& m = projectMedia(l.project, need(a, "media"));
                std::string why;
                if (!relinkMedia(l.project, m.id, absolute(need(a, "path")).toStdString(),
                                 a.value("replace").toBool() ? RelinkCheck::Replace : RelinkCheck::Strict, &why))
                    return fail(QStringLiteral("Not relinked: %1").arg(QString::fromStdString(why)));
                save(l);
                done = QStringLiteral("Linked %1 to %2.").arg(QString::fromStdString(m.name), QString::fromStdString(m.path));
            }
            const QStringList offline = list();
            if (offline.isEmpty()) return ok((done.isEmpty() ? QString() : done + ' ') + QStringLiteral("No media is offline."));
            return ok((done.isEmpty() ? QString() : done + ' ') + QStringLiteral("%1 offline:\n").arg(offline.size()) + offline.join('\n'));
        });

    add("montage_quality_check", "Quality check",
        "Check the sequence (or from..to) before delivery, as broadcasters' QC does: flashing that can trigger seizures "
        "(ITU-R BT.1702 / Ofcom / WCAG: more than three flashes a second over a quarter of the screen, or saturated red), "
        "levels outside EBU R103, black or frozen picture, silence, clipping, and loudness against a target. Lists each "
        "problem with its timecodes; with markers, puts a red \"QC:\" marker on each (replacing earlier ones).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"from":{"type":["number","string"]},
            "to":{"type":["number","string"]},"flashing":{"type":"boolean","default":true},"levels":{"type":"boolean","default":true},
            "black_seconds":{"type":"number","default":1,"description":"0 = not checked"},
            "freeze_seconds":{"type":"number","default":5,"description":"0 = not checked"},
            "silence_seconds":{"type":"number","default":2,"description":"0 = not checked"},
            "clipping":{"type":"boolean","default":true},
            "loudness_target":{"type":"number","description":"LUFS, e.g. -14 (streaming) or -23 (EBU R128); omitted = not checked"},
            "peak_ceiling":{"type":"number","default":-1,"description":"dBTP, checked with the loudness"},
            "markers":{"type":"boolean","default":false}},"required":["project"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const FrameTime from = a.contains("from") ? timeArg(a.value("from"), s, "from") : 0;
            const FrameTime to = a.contains("to") ? timeArg(a.value("to"), s, "to") : -1;
            QcSettings q;
            q.flashing = a.value("flashing").toBool(true);
            q.levels = a.value("levels").toBool(true);
            q.blackSeconds = std::max(0.0, a.value("black_seconds").toDouble(1));
            q.freezeSeconds = std::max(0.0, a.value("freeze_seconds").toDouble(5));
            q.silenceSeconds = std::max(0.0, a.value("silence_seconds").toDouble(2));
            q.clipping = a.value("clipping").toBool(true);
            q.loudnessTarget = a.value("loudness_target").toDouble(0);
            q.peakCeiling = a.value("peak_ceiling").toDouble(-1);
            const std::vector<QcIssue> issues = qualityCheck(l.project, s, from, to, q, [this](double f) { progress(f, "Checking"); });
            QString text;
            for (const QcIssue& i : issues)
                text += QStringLiteral("%1 - %2  %3: %4\n").arg(tc(i.start, s), tc(i.end, s), QString::fromLatin1(qcKindName(i.kind)),
                                                             QString::fromStdString(i.text));
            if (a.value("markers").toBool()) {
                addQcMarkers(s, issues);
                save(l);
            }
            return ok(issues.empty() ? QStringLiteral("No problems found.") : QStringLiteral("%1 problem(s):\n").arg(issues.size()) + text);
        });

    add("montage_export_markers", "Export markers",
        "The sequence's markers as a marker list: csv (Premiere's columns: name, description, in, out, duration, type, colour), "
        "avid (Media Composer locators) or edl (a marker EDL DaVinci Resolve imports). Written to `path` when given.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"format":{"type":"string","enum":["csv","avid","edl"],"default":"csv"},
            "path":{"type":"string"}},"required":["project"]})json",
        true, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Sequence& s = l.seq();
            const QString format = str(a, "format", "csv").toLower();
            if (format != "csv" && format != "avid" && format != "edl") throw ArgError{QStringLiteral("\"format\" must be csv, avid or edl")};
            const std::string text = format == "avid" ? markersToAvidLocators(s) : format == "edl" ? markersToResolveEdl(s) : markersToCsv(s);
            if (a.contains("path")) {
                QFile f(absolute(need(a, "path")));
                if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(text.data(), qint64(text.size())) != qint64(text.size()))
                    return fail(QStringLiteral("Cannot write %1").arg(f.fileName()));
                return ok(QStringLiteral("Wrote %1 marker(s) to %2").arg(s.markers.size()).arg(f.fileName()));
            }
            return ok(QString::fromStdString(text));
        });

    add("montage_import_markers", "Import markers",
        "Add markers from a marker list: a CSV with a header row (name, description or notes, in or timecode, out or duration, "
        "type, colour), as review tools and Premiere write them, or Avid locator lines. Give the file's `path` or its `text`.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"path":{"type":"string"},"text":{"type":"string"}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            std::string text = str(a, "text").toStdString();
            if (a.contains("path")) {
                QFile f(absolute(need(a, "path")));
                if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Cannot read %1").arg(f.fileName()));
                text = f.readAll().toStdString();
            }
            std::vector<Marker> markers;
            std::string err;
            if (!parseMarkerList(text, s, markers, &err)) return fail(QString::fromStdString(err));
            for (const Marker& m : markers) edit::addMarker(s, m);
            save(l);
            return ok(QStringLiteral("Added %1 marker(s)").arg(markers.size()));
        });

    add("montage_consolidate", "Copy the project and its media",
        "Copy the project to a folder with the media it uses (Premiere's Project Manager): collect copies each used file "
        "whole; with trim, only the parts of videos the sequences use are transcoded (plus handles) and the clips point at "
        "the new files. Media nothing uses is left out unless keep_unused. Returns the new project's path.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"folder":{"type":"string"},"name":{"type":"string"},
            "sequences":{"type":"string","enum":["all","active"],"default":"all"},"trim":{"type":"boolean","default":false},
            "handles":{"type":"number","default":1,"description":"Seconds kept either side, when trimming"},
            "codec":{"type":"string","enum":["prores","h264"],"default":"prores"},"keep_unused":{"type":"boolean","default":false}},
            "required":["project","folder"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            ConsolidateOptions o;
            o.folder = absolute(need(a, "folder")).toStdString();
            o.name = str(a, "name", QFileInfo(l.path).completeBaseName()).toStdString();
            if (str(a, "sequences", "all") == "active") o.sequences = {l.project.activeSequence};
            o.trim = a.value("trim").toBool();
            o.handles = std::max(0.0, a.value("handles").toDouble(1));
            o.codec = str(a, "codec", "prores") == "h264" ? "libx264" : "prores_ks";
            o.keepUnused = a.value("keep_unused").toBool();
            ConsolidateResult res;
            std::string err;
            if (!consolidateProject(l.project, o, &res, [this](double f) { progress(f, "Copying"); }, nullptr, &err))
                return fail(QString::fromStdString(err));
            QString text = QStringLiteral("Wrote %1: %2 file(s) copied, %3 consolidated, %4 MB")
                               .arg(QString::fromStdString(res.projectPath)).arg(res.copied).arg(res.trimmed).arg(double(res.bytes) / 1e6, 0, 'f', 1);
            for (const std::string& m : res.missing) text += QStringLiteral("\nMissing: ") + QString::fromStdString(m);
            return ok(text);
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
        "(um, uh, er...), retakes (broken-off attempts the speaker started again) and/or pauses longer than "
        "pauses_longer_than seconds (shortened to keep_pause). Every track is cut "
        "the same way and closed up, captions included. smooth_cuts puts a Smooth Cut (an optical-flow morph) on each join "
        "in the picture, to hide the jump. One undoable edit; use montage_find_phrase first to see what a phrase matches.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "phrases":{"type":"array","items":{"type":"string"},"description":"Phrases to cut, every time they are said"},
            "fillers":{"type":"boolean","default":false},
            "retakes":{"type":"boolean","default":false,"description":"Where the speaker broke off and started the same words again, keep only the last take"},
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
            int retakes = 0;
            if (a.value("retakes").toBool()) {
                std::vector<std::pair<size_t, size_t>> takes;
                for (const FrameRange& r : retakeRanges(words, fps, 3, 30, &takes)) ranges.push_back(r);
                retakes = int(takes.size());
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
            return ok(QStringLiteral("Cut %1 s: %2 phrase match(es), %3 filler word(s), %6 retake(s), %4 pause(s)%5")
                          .arg(secs, 0, 'f', 2)
                          .arg(std::accumulate(found.begin(), found.end(), 0, [](int n, const QJsonValue& v) { return n + v.toObject().value("times").toInt(); }))
                          .arg(fillers)
                          .arg(pauses)
                          .arg(smooth ? QStringLiteral(", with Smooth Cuts") : QString())
                          .arg(retakes),
                      QJsonObject{{"phrases", found}, {"fillers", fillers}, {"retakes", retakes}, {"pauses", pauses}, {"removed_seconds", secs},
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

    add("montage_beat_markers", "Mark the beat",
        "Find the beat of a music clip and put sequence markers on its bars (\"Bar 12\"), or on every beat (\"12.3\" = bar 12, "
        "beat 3), where the clip plays them. Use the markers to cut picture on the beat. Returns the tempo.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "every_beat":{"type":"boolean","default":false}},"required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Clip& c = clipArg(l, a);
            BeatGrid g;
            std::string err;
            if (!mediaBeats(l.project, c.mediaId, g, nullptr, &err)) return fail(QString::fromStdString(err));
            const int n = addBeatMarkers(l.seq(), c, g, a.value("every_beat").toBool());
            save(l);
            return ok(QStringLiteral("%1 marker(s) at %2 BPM").arg(n).arg(g.tempo, 0, 'f', 1),
                      QJsonObject{{"markers", n}, {"tempo", g.tempo}});
        });

    add("montage_paper_edit", "Assemble a paper edit",
        "Build a new sequence from lines of the media's transcripts, in the order given, as Premiere's Paper Edit does: "
        "each line is a quote (the words as said, found in that media's transcript; `after` seconds to skip an earlier "
        "saying) or a from/to range in seconds, laid out back to back with picture and sound and `handle` seconds of air.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "lines":{"type":"array","items":{"type":"object","properties":{"media":{"type":"string"},"text":{"type":"string"},
                "after":{"type":"number"},"from":{"type":"number"},"to":{"type":"number"}},"required":["media"]}},
            "name":{"type":"string","default":"Paper Edit"},"handle":{"type":"number","default":0.1}},
            "required":["project","lines"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            std::vector<PaperLine> lines;
            for (const QJsonValue& v : a.value("lines").toArray()) {
                const QJsonObject o = v.toObject();
                const MediaItem& m = projectMedia(l.project, o.value("media").toString());
                if (o.contains("text")) {
                    if (!m.transcript) throw ArgError{QStringLiteral("%1 has no transcript: transcribe it first").arg(QString::fromStdString(m.name))};
                    const auto line = findLine(l.project, m.id, o.value("text").toString().toStdString(), o.value("after").toDouble(0));
                    if (!line)
                        throw ArgError{QStringLiteral("\"%1\" is not said in %2").arg(o.value("text").toString(), QString::fromStdString(m.name))};
                    lines.push_back(*line);
                } else if (o.value("from").isDouble() && o.value("to").isDouble()) {
                    lines.push_back({m.id, o.value("from").toDouble(), o.value("to").toDouble(), {}});
                } else {
                    throw ArgError{"Each line needs \"text\", or \"from\" and \"to\""};
                }
            }
            if (lines.empty()) throw ArgError{"\"lines\" is empty"};
            const Id id = makePaperEdit(l.project, lines, str(a, "name", "Paper Edit").toStdString(), std::max(0.0, a.value("handle").toDouble(0.1)));
            if (!id) return fail("Nothing could be placed");
            save(l);
            const Sequence* s = l.project.findSequence(id);
            QJsonArray placed;
            for (const PaperLine& line : lines)
                placed.append(QJsonObject{{"media", QString::fromStdString(l.project.findMedia(line.media)->name)}, {"from", line.in},
                                          {"to", line.out}, {"text", QString::fromStdString(line.text)}});
            return ok(QStringLiteral("Assembled %1 line(s) into \"%2\" (%3)").arg(lines.size()).arg(QString::fromStdString(s->name), tc(s->duration(), *s)),
                      QJsonObject{{"sequence", double(id)}, {"lines", placed}, {"duration_seconds", s->duration() / s->fpsValue()}});
        });

    add("montage_highlights", "Make a highlight edit",
        "Find the liveliest moments of long footage and lay them out as a new sequence of about `seconds`: each half "
        "second is scored by how much louder it is than the clip usually is, how much moves in the picture and, with "
        "`look_for` and indexed footage, how much it looks like that description. Moments play in the order they "
        "happened, cut off words. `media` defaults to every video in the project.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"media":{"type":"array","items":{"type":"number"}},
            "seconds":{"type":"number","default":30},"look_for":{"type":"string"},"name":{"type":"string","default":"Highlights"}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            std::vector<Id> media;
            for (const QJsonValue& v : a.value("media").toArray()) media.push_back(Id(v.toDouble()));
            if (media.empty())
                for (const MediaItem& m : l.project.media)
                    if (m.kind == MediaKind::Video) media.push_back(m.id);
            HighlightOptions o;
            o.seconds = std::max(2.0, a.value("seconds").toDouble(30));
            std::string err;
            if (const QString look = a.value("look_for").toString(); !look.isEmpty()) {
                if (!visualSearchAvailable() || !visualModel().installed())
                    return fail("look_for needs the visual search model: run `scripts/fetch-models.sh`");
                auto clip = ClipModel::load(&err);
                if (!clip) return fail(QString::fromStdString(err));
                o.lookFor = clip->text(look.toStdString(), &err);
            }
            const std::vector<HighlightMoment> moments = findHighlights(l.project, media, o, {}, nullptr, &err);
            if (moments.empty()) return fail(QString::fromStdString(err));
            const Id seq = makeHighlightSequence(l.project, moments, a.value("name").toString(QStringLiteral("Highlights")).toStdString());
            if (!seq) return fail("The highlights could not be laid out");
            save(l);
            QJsonArray list;
            double total = 0;
            for (const HighlightMoment& m : moments) {
                list.append(QJsonObject{{"media", double(m.media)}, {"in", m.in}, {"out", m.out}, {"score", m.score}});
                total += m.out - m.in;
            }
            return ok(QStringLiteral("%1 moment(s), %2 s, in a new sequence").arg(moments.size()).arg(total, 0, 'f', 1),
                      QJsonObject{{"sequence", double(seq)}, {"seconds", total}, {"moments", list}});
        });

    add("montage_cut_to_beat", "Cut clips to the beat",
        "Lay media (videos or stills, in order, repeating if the music outlasts them) back to back on a video track with a "
        "cut every `every` beats or bars of a music clip, from its first beat to its end. Each piece is the middle of its "
        "media; a video too short for a piece is passed over. Picture only (the music is the sound).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number","description":"the music clip"},
            "media":{"type":"array","items":{"type":"number"}},"every":{"type":"number","default":1},
            "unit":{"type":"string","enum":["beat","bar"],"default":"bar"},"track":{"type":"number","default":0}},
            "required":["project","clip","media"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Clip c = clipArg(l, a);
            BeatGrid g;
            std::string err;
            if (!mediaBeats(l.project, c.mediaId, g, nullptr, &err)) return fail(QString::fromStdString(err));
            std::vector<Id> media;
            for (const QJsonValue& v : a.value("media").toArray()) media.push_back(Id(v.toDouble()));
            const edit::Result r = cutToBeat(l.project, l.seq(), c, g, media, std::max(1, a.value("every").toInt(1)),
                                             a.value("unit").toString(QStringLiteral("bar")) == QLatin1String("bar"),
                                             std::max(0, a.value("track").toInt(0)));
            if (!r.ok) return fail(QString::fromStdString(r.error));
            save(l);
            QJsonArray clips;
            for (Id id : r.created) clips.append(double(id));
            return ok(QStringLiteral("%1 clip(s) cut to the music at %2 BPM").arg(r.created.size()).arg(g.tempo, 0, 'f', 1),
                      QJsonObject{{"clips", clips}, {"tempo", g.tempo}});
        });

    add("montage_fit_music", "Fit music to a length",
        "Re-edit a music clip (audio, normal speed, not linked to picture) to last about `seconds`: whole bars are skipped "
        "or repeated where the music matches itself best, keeping its start and ending, with short crossfades on the beat. "
        "The result is within about half a bar of the length asked for.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "seconds":{"type":"number"}},"required":["project","clip","seconds"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Clip c = clipArg(l, a);
            Sequence& s = l.seq();
            const double secs = a.value("seconds").toDouble();
            if (secs < 5) return fail("seconds must be 5 or more");
            MusicFit fit;
            std::string err;
            if (!analyzeMusicFit(l.project, s, c, FrameTime(std::llround(secs * s.fpsValue())), fit, nullptr, &err))
                return fail(QString::fromStdString(err));
            const edit::Result r = applyMusicFit(l.project, s, c.id, fit);
            if (!r.ok) return fail(QString::fromStdString(r.error));
            save(l);
            QJsonArray pieces;
            for (const MusicSegment& seg : fit.segments) pieces.append(QJsonArray{seg.in, seg.out});
            return ok(QStringLiteral("The music now lasts %1 s in %2 piece(s)").arg(fit.duration, 0, 'f', 2).arg(fit.segments.size()),
                      QJsonObject{{"seconds", fit.duration}, {"pieces", pieces}, {"clips", int(r.created.size())},
                                  {"join_similarity", fit.similarity}});
        });

    add("montage_auto_mix", "Mix the audio",
        "A first mix in one step: every audio clip is recognised as dialogue, music or effects (from transcripts, the "
        "rhythm of speech and a steady beat), set to a loudness for its kind, dialogue is evened out with volume keyframes, "
        "and music dips under speech. Pass roles to correct what a clip is. dry_run returns the plan only.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "dialogue_lufs":{"type":"number","default":-18},"music_lufs":{"type":"number","default":-22},
            "effects_lufs":{"type":"number","default":-24},"ride":{"type":"boolean","default":true},
            "duck_db":{"type":"number","default":-12,"description":"0 = no ducking"},
            "roles":{"type":"object","description":"clip id -> dialogue, music, effects or silence",
                     "additionalProperties":{"type":"string","enum":["dialogue","music","effects","silence"]}},
            "dry_run":{"type":"boolean","default":false}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            MixOptions o;
            o.dialogueLufs = std::clamp(a.value("dialogue_lufs").toDouble(-18), -40.0, -6.0);
            o.musicLufs = std::clamp(a.value("music_lufs").toDouble(-22), -40.0, -6.0);
            o.effectsLufs = std::clamp(a.value("effects_lufs").toDouble(-24), -40.0, -6.0);
            o.ride = a.value("ride").toBool(true);
            o.duckDb = std::clamp(a.value("duck_db").toDouble(-12), -30.0, 0.0);
            o.duck = o.duckDb < 0;
            std::string err;
            auto plan = planMix(l.project, s, o, {}, nullptr, &err);
            if (plan.empty()) return fail(err.empty() ? QStringLiteral("There is no audio to mix") : QString::fromStdString(err));
            const QJsonObject roles = a.value("roles").toObject();
            for (ClipMix& m : plan) {
                const QString want = roles.value(QString::number(m.clip)).toString().toLower();
                if (want.isEmpty()) continue;
                for (AudioRole r : {AudioRole::Dialogue, AudioRole::Music, AudioRole::Effects, AudioRole::Silence})
                    if (want == QString::fromLatin1(audioRoleName(r)).toLower()) m.role = r;
                replanClip(m, o);
            }
            QJsonArray clips;
            for (const ClipMix& m : plan) {
                const Clip* c = edit::clipById(s, m.clip);
                clips.append(QJsonObject{{"clip", double(m.clip)}, {"name", c ? QString::fromStdString(c->name) : QString()},
                                         {"role", QString::fromLatin1(audioRoleName(m.role)).toLower()},
                                         {"heard_as", QString::fromLatin1(audioRoleName(m.guess.role)).toLower()},
                                         {"loudness", m.guess.loudness}, {"gain_db", m.gainDb}, {"ride_keys", int(m.ride.size())}});
            }
            if (a.value("dry_run").toBool()) return ok(QStringLiteral("Planned %1 clip(s)").arg(plan.size()), QJsonObject{{"clips", clips}});
            const int n = applyMix(l.project, s, plan, o);
            save(l);
            return ok(QStringLiteral("Mixed %1 clip(s)").arg(n), QJsonObject{{"clips", clips}, {"changed", n}});
        });

    add("montage_compare_sequences", "Compare two versions of a cut",
        "What changed from one sequence to another (Resolve's timeline comparison): clips added, removed, trimmed (in "
        "and out points, in source frames), moved (to another track, or changed places) and changed (speed, effects, "
        "picture, sound, a title's text). Clips that only slid along with the edits around them are not changes. "
        "add_markers puts a coloured marker on the newer sequence for each change.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "before":{"type":"string","description":"The earlier sequence's name"},
            "after":{"type":"string","description":"The later sequence's name (default: the active one)"},
            "add_markers":{"type":"boolean","default":false}},"required":["project","before"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            auto byName = [&](const QString& name) -> Sequence* {
                for (Sequence& sq : l.project.sequences)
                    if (QString::fromStdString(sq.name) == name) return &sq;
                throw ArgError{QStringLiteral("No sequence named \"%1\"").arg(name)};
            };
            const Sequence* before = byName(need(a, "before"));
            Sequence* after = a.contains("after") ? byName(str(a, "after")) : &l.seq();
            if (before == after) throw ArgError{"Compare two different sequences"};
            const auto changes = compareSequences(l.project, *before, *after);
            QJsonArray list;
            for (const TimelineChange& c : changes)
                list.append(QJsonObject{{"change", QString::fromLatin1(changeKindName(c.kind)).toLower()},
                                        {"clip", QString::fromStdString(c.name)},
                                        {"before_clip", double(c.before)},
                                        {"after_clip", double(c.after)},
                                        {"track", QStringLiteral("%1%2").arg(c.track.kind == TrackKind::Video ? "V" : "A").arg(c.track.index + 1)},
                                        {"at", tc(c.at, *after)},
                                        {"at_seconds", secs(c.at, *after)},
                                        {"details", QString::fromStdString(c.details)}});
            if (a.value("add_markers").toBool() && !changes.empty()) {
                static const std::map<ChangeKind, const char*> colours{{ChangeKind::Added, "Forest"}, {ChangeKind::Removed, "Red"},
                                                                        {ChangeKind::Trimmed, "Yellow"}, {ChangeKind::Moved, "Cerulean"},
                                                                        {ChangeKind::Changed, "Violet"}};
                for (const TimelineChange& c : changes)
                    edit::addMarker(*after, Marker{c.at, c.kind == ChangeKind::Removed ? 0 : c.length,
                                                   std::string(changeKindName(c.kind)) + (c.name.empty() ? "" : ": " + c.name), c.details,
                                                   labelFromName(colours.at(c.kind))});
                save(l);
            }
            return ok(changes.empty() ? QStringLiteral("No differences") : QStringLiteral("%1 change(s)").arg(changes.size()),
                      QJsonObject{{"changes", list}});
        });

    add("montage_layout", "Arrange clips in a layout",
        "Make video clips share the frame: picture in picture (the lowest clip full frame, the others whole in the "
        "corners), side by side, top and bottom, three across or a 2 x 2 grid (each filling its cell, cropped from the "
        "middle), or back to full frame. Clips go from the lowest track up; by default the video clips at a time.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "layout":{"type":"string","enum":["picture_in_picture","side_by_side","top_and_bottom","three_across","grid","full_frame"]},
            "clips":{"type":"array","items":{"type":"number"},"description":"Video clip ids; default: those at \"at\""},
            "at":{"type":["number","string"],"description":"Timeline time whose video clips to arrange (default 0)"},
            "gap":{"type":"number","default":0,"description":"Pixels between and around the cells"},
            "corner":{"type":"string","enum":["top_left","top_right","bottom_left","bottom_right"],"default":"bottom_right"},
            "size":{"type":"number","default":0.3,"description":"Picture in picture: share of the frame's width"}},
            "required":["project","layout"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            static const QStringList names{"full_frame", "picture_in_picture", "side_by_side", "top_and_bottom", "three_across", "grid"};
            const int li = int(names.indexOf(need(a, "layout")));
            if (li < 0) throw ArgError{QStringLiteral("\"layout\" must be one of %1").arg(names.join(", "))};
            std::vector<Id> ids;
            if (a.contains("clips")) {
                for (const QJsonValue& v : a.value("clips").toArray()) ids.push_back(Id(v.toDouble()));
            } else {
                const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : 0;
                for (int i = 0; i < int(s.videoTracks.size()); ++i)
                    if (const Clip* c = edit::clipAt(s, {TrackKind::Video, i}, at)) ids.push_back(c->id);
            }
            static const QStringList corners{"top_left", "top_right", "bottom_left", "bottom_right"};
            edit::LayoutOptions o;
            o.gap = std::clamp(a.value("gap").toDouble(0), 0.0, double(std::min(s.width, s.height)) / 4);
            o.corner = a.contains("corner") ? int(corners.indexOf(str(a, "corner"))) : 3;
            if (o.corner < 0) throw ArgError{QStringLiteral("\"corner\" must be one of %1").arg(corners.join(", "))};
            o.pipSize = std::clamp(a.value("size").toDouble(0.3), 0.05, 1.0);
            check(edit::arrangeLayout(l.project, s, ids, edit::Layout(li), o));
            save(l);
            QJsonArray clips;
            for (Id id : ids)
                if (const Clip* c = edit::clipById(s, id))
                    clips.append(QJsonObject{{"clip", double(id)},
                                             {"scale", c->motion.p("scale", 0, 100)},
                                             {"position", QJsonArray{c->motion.p("pos_x", 0), c->motion.p("pos_y", 0)}},
                                             {"crop", QJsonArray{c->motion.p("crop_left", 0), c->motion.p("crop_right", 0),
                                                                 c->motion.p("crop_top", 0), c->motion.p("crop_bottom", 0)}}});
            return ok(QStringLiteral("Arranged %1 clip(s)").arg(clips.size()), QJsonObject{{"clips", clips}});
        });

    add("montage_set_roles", "Set audio roles",
        "Audio roles, as in Final Cut: tag audio clips Dialogue, Music, Effects or a role of your own (a video clip's "
        "linked sound takes it), mute or unmute a role across the sequence, or detect the roles of untagged clips by "
        "listening. Roles drive stems by role (montage_export stems roles) and are kept by montage_auto_mix.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "clips":{"type":"array","items":{"type":"number"},"description":"Clip ids to tag with role"},
            "role":{"type":"string","description":"Dialogue, Music, Effects or any name; empty clears"},
            "mute":{"type":"array","items":{"type":"string"},"description":"Roles to mute"},
            "unmute":{"type":"array","items":{"type":"string"},"description":"Roles to hear again"},
            "detect":{"type":"boolean","default":false,"description":"Listen to untagged audio clips and tag them"}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            QStringList done;
            if (a.contains("clips")) {
                if (!a.contains("role")) throw ArgError{"\"role\" is needed with \"clips\""};
                std::vector<Id> ids;
                for (const QJsonValue& v : a.value("clips").toArray()) {
                    if (!edit::clipById(s, Id(v.toDouble()))) throw ArgError{QStringLiteral("No clip %1 in the active sequence").arg(qulonglong(v.toDouble()))};
                    ids.push_back(Id(v.toDouble()));
                }
                const int n = edit::setClipRole(s, ids, str(a, "role").trimmed().toStdString());
                done << QStringLiteral("tagged %1 audio clip(s)").arg(n);
            }
            for (const QJsonValue& v : a.value("mute").toArray()) edit::setRoleMuted(s, v.toString().toStdString(), true);
            for (const QJsonValue& v : a.value("unmute").toArray()) edit::setRoleMuted(s, v.toString().toStdString(), false);
            if (a.contains("mute") || a.contains("unmute")) done << QStringLiteral("%1 role(s) muted").arg(s.mutedRoles.size());
            if (a.value("detect").toBool()) {
                std::string err;
                const auto plan = planMix(l.project, s, MixOptions{}, {}, nullptr, &err);
                int n = 0;
                for (const ClipMix& m : plan)
                    if (Clip* c = edit::clipById(s, m.clip); c && c->role.empty() && m.guess.role != AudioRole::Silence) {
                        c->role = audioRoleName(m.guess.role);
                        ++n;
                    }
                done << QStringLiteral("detected %1 role(s)").arg(n);
            }
            if (done.isEmpty()) throw ArgError{"Give clips and a role, mute, unmute or detect"};
            save(l);
            QJsonArray roles;
            for (const std::string& r : edit::sequenceRoles(s)) {
                int clips = 0;
                for (const Track& t : s.audioTracks)
                    for (const Clip& c : t.clips) clips += c.role == r;
                roles.append(QJsonObject{{"role", QString::fromStdString(r)}, {"clips", clips}, {"muted", edit::roleMuted(s, r)}});
            }
            return ok(done.join(QStringLiteral("; ")), QJsonObject{{"roles", roles}});
        });

    add("montage_bleep", "Bleep words",
        "Cover spoken words with a bleep tone (or silence), as broadcasters do: every swear word in the transcribed "
        "dialogue (profanity), every time a phrase is said, or given stretches of the timeline. Kept on each clip in source "
        "time, so later trims keep it on the word; the words are masked in the captions too (\"f***\").",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "profanity":{"type":"boolean","description":"Every common English swear word"},
            "phrase":{"type":"string","description":"Every time this word or phrase is said"},
            "times":{"type":"array","items":{"type":"array","items":{"type":"number"}},"description":"[[start, end], ...] timeline seconds"},
            "mode":{"type":"string","enum":["tone","silence"],"default":"tone"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const std::vector<TranscriptWord> all = sequenceTranscriptWords(l.project, s);
            std::vector<TranscriptWord> chosen;
            if (a.value("profanity").toBool()) chosen = profanity(all);
            if (a.contains("phrase")) {
                std::vector<std::string> want;
                for (const QString& w : str(a, "phrase").toLower().split(' ', Qt::SkipEmptyParts)) {
                    QString b;
                    for (QChar c : w)
                        if (c.isLetterOrNumber() || c == '\'') b += c;
                    if (!b.isEmpty()) want.push_back(b.toStdString());
                }
                auto bareOf = [](const std::string& t) {
                    QString b;
                    for (QChar c : QString::fromStdString(t).toLower())
                        if (c.isLetterOrNumber() || c == '\'') b += c;
                    return b.toStdString();
                };
                for (size_t i = 0; !want.empty() && i + want.size() <= all.size(); ++i) {
                    bool match = true;
                    for (size_t k = 0; k < want.size() && match; ++k) match = bareOf(all[i + k].text) == want[k];
                    if (match)
                        for (size_t k = 0; k < want.size(); ++k) chosen.push_back(all[i + k]);
                }
            }
            for (const QJsonValue& v : a.value("times").toArray()) {
                const QJsonArray r = v.toArray();
                if (r.size() != 2) throw ArgError{"Each of \"times\" is [start, end] in seconds"};
                TranscriptWord w;
                w.start = r[0].toDouble();
                w.end = r[1].toDouble();
                if (w.end > w.start) chosen.push_back(w);
            }
            if (chosen.empty()) return fail("Nothing to bleep: no such words in the transcribed dialogue");
            const edit::Result r = bleepWords(l.project, s, chosen);
            if (!r.ok) return fail(QString::fromStdString(r.error));
            if (str(a, "mode", "tone") == "silence")
                for (Id id : r.created)
                    if (Clip* c = edit::clipById(s, id))
                        for (Effect& e : c->effects)
                            if (e.type == "bleep") e.params["mode"] = Param(1.0);
            save(l);
            QJsonArray words;
            for (const TranscriptWord& w : chosen)
                words.append(QJsonObject{{"text", QString::fromStdString(w.text)}, {"start", w.start}, {"end", w.end}});
            return ok(QStringLiteral("Bleeped %1 word(s) on %2 clip(s)").arg(chosen.size()).arg(r.created.size()),
                      QJsonObject{{"words", words}, {"clips", int(r.created.size())}});
        });

    add("montage_checkerboard", "Split dialogue by speaker",
        "Checkerboard dialogue, as dialogue editors do before a mix: each audio clip is split where the speaker changes "
        "(in the silence between them) and each person's parts go to an audio track of their own (a free track below, or a "
        "new one named after them). Needs a transcript with speaker labels (montage_transcribe with speakers).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "clips":{"type":"array","items":{"type":"number"},"description":"Audio clip ids"},
            "track":{"type":"string","description":"Or every clip on this audio track, e.g. A1"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            std::vector<Id> clips;
            for (const QJsonValue& v : a.value("clips").toArray()) clips.push_back(Id(v.toDouble()));
            if (a.contains("track")) {
                const TrackRef t = trackArg(str(a, "track"), s, false);
                if (t.kind != TrackKind::Audio) throw ArgError{"\"track\" must be an audio track"};
                for (const Clip& c : trackAt(s, t)->clips) clips.push_back(c.id);
            }
            if (clips.empty()) throw ArgError{"Give \"clips\" or \"track\""};
            int people = 0, split = 0;
            QStringList errors;
            for (Id id : clips) {
                int n = 0;
                const edit::Result r = checkerboardBySpeaker(l.project, s, id, &n);
                if (!r.ok) {
                    errors << QStringLiteral("%1: %2").arg(id).arg(QString::fromStdString(r.error));
                    continue;
                }
                people = std::max(people, n);
                split += int(r.created.size());
            }
            if (split == 0) return fail(errors.join("\n"));
            save(l);
            QJsonArray tracks;
            for (const Track& t : s.audioTracks) tracks.append(QJsonObject{{"name", QString::fromStdString(t.name)}, {"clips", int(t.clips.size())}});
            return ok(QStringLiteral("%1 pieces, %2 people").arg(split).arg(people) + (errors.isEmpty() ? QString() : "\n" + errors.join("\n")),
                      QJsonObject{{"pieces", split}, {"people", people}, {"audio_tracks", tracks}});
        });

    add("montage_match_voice", "Match voices",
        "Make dialogue recorded on another microphone or in another room sound like a reference clip: each clip gets a "
        "Parametric EQ (first in its effects, replacing an earlier match) fitted to the difference between the voices' "
        "long-term spectra. Levels are left alone (see montage_auto_mix).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"reference":{"type":"number","description":"The clip that sounds right"},
            "clips":{"type":"array","items":{"type":"number"}}},"required":["project","reference","clips"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Clip& ref = clipArg(l, a, "reference");
            std::vector<double> reference;
            std::string err;
            if (!clipSpeechSpectrum(l.project, s, ref, reference, &err)) return fail(QString::fromStdString(err));
            QJsonArray out;
            int n = 0;
            for (const QJsonValue& v : a.value("clips").toArray()) {
                Clip* c = edit::clipById(s, Id(v.toDouble()));
                auto loc = c ? edit::locate(s, c->id) : std::nullopt;
                if (!c || !loc || loc->track.kind != TrackKind::Audio) throw ArgError{QStringLiteral("%1 is not an audio clip").arg(v.toDouble())};
                std::vector<double> spectrum;
                if (!clipSpeechSpectrum(l.project, s, *c, spectrum, &err)) return fail(QString::fromStdString(err));
                const VoiceEq eq = fitVoiceEq(spectrum, reference);
                applyVoiceEq(l.project, *c, eq);
                ++n;
                out.append(QJsonObject{{"clip", double(c->id)}, {"low_db", eq.lowDb}, {"b300_db", eq.b1Db}, {"b1200_db", eq.b2Db},
                                       {"b4000_db", eq.b3Db}, {"high_db", eq.highDb}, {"difference_before_db", eq.beforeDb},
                                       {"difference_after_db", eq.afterDb}});
            }
            if (n == 0) return fail("No clips to match");
            save(l);
            return ok(QStringLiteral("Matched %1 clip(s) to %2").arg(n).arg(QString::fromStdString(ref.name)), QJsonObject{{"clips", out}});
        });

    add("montage_set_surround", "Set up a surround mix",
        "Mix the active sequence in stereo, 5.1 (L R C LFE Ls Rs) or 7.1 (L R C LFE Lb Rb Ls Rs), and place audio tracks "
        "among the speakers. A position is an angle (0 straight ahead, 90 right, -90 left, 180 behind) and a distance (1 at "
        "the speakers, 0 spread over all of them); width narrows a stereo track to a point (0, e.g. dialogue in the centre "
        "speaker); lfe_db sends it to the subwoofer (-100 off). A track routed to a bus is placed by its bus. Export with "
        "montage_render (downmix_stereo for a stereo copy).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "layout":{"type":"string","enum":["stereo","5.1","7.1"]},
            "tracks":{"type":"array","items":{"type":"object","properties":{
                "track":{"type":"string","description":"Audio track, e.g. A1"},"angle":{"type":"number","default":0},
                "distance":{"type":"number","default":1},"width":{"type":"number","default":1},"lfe_db":{"type":"number","default":-100}},
                "required":["track"]}}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            if (a.contains("layout")) {
                const std::string layout = a.value("layout").toString().toStdString();
                if (std::find(audioLayouts().begin(), audioLayouts().end(), layout) == audioLayouts().end())
                    throw ArgError{"\"layout\" must be stereo, 5.1 or 7.1"};
                s.audioLayout = layout;
            }
            QJsonArray out;
            for (const QJsonValue& v : a.value("tracks").toArray()) {
                const QJsonObject t = v.toObject();
                const TrackRef r = trackArg(t.value("track").toString(), s, false);
                if (r.kind != TrackKind::Audio) throw ArgError{"Surround placement is for audio tracks"};
                Track* tr = trackAt(s, r);
                const double angle = t.value("angle").toDouble(0) * M_PI / 180;
                const double dist = std::clamp(t.value("distance").toDouble(1), 0.0, 1.0);
                SurroundPan& p = tr->surround;
                p.x = dist * std::sin(angle);
                p.y = dist * std::cos(angle);
                p.width = std::clamp(t.value("width").toDouble(1), 0.0, 1.0);
                p.lfeDb = std::clamp(t.value("lfe_db").toDouble(-100), -100.0, 12.0);
                out.append(QJsonObject{{"track", QString::fromStdString(tr->name)}, {"x", p.x}, {"y", p.y}, {"width", p.width},
                                       {"lfe_db", p.lfeDb}});
            }
            save(l);
            return ok(QStringLiteral("%1 mix, %2 track(s) placed").arg(QString::fromStdString(s.audioLayout)).arg(out.size()),
                      QJsonObject{{"layout", QString::fromStdString(s.audioLayout)},
                                  {"channels", layoutChannels(s.audioLayout)}, {"tracks", out}});
        });

    add("montage_super_scale", "Super Scale a file",
        "Write a copy of a video or still enlarged 2, 3 or 4 times with Real-ESRGAN, which redraws edges and texture "
        "instead of blurring them. A video keeps its sound (output .mov is ProRes 422 HQ, others H.264); a still is "
        "written as the output's image type (.png, .jpg, .tif). For clips in a project, montage_add_effect "
        "\"super_scale\" enlarges them in place whenever they are shown larger than they were shot. Needs the model "
        "(scripts/fetch-models.sh, or the app downloads it).",
        R"json({"type":"object","properties":{"input":{"type":"string"},"output":{"type":"string"},
            "factor":{"type":"integer","enum":[2,3,4],"default":2},
            "strength":{"type":"number","default":1,"description":"0..1: how much of the model, the rest plain scaling"}},
            "required":["input","output"]})json",
        false, [this](const QJsonObject& a) {
            const std::string in = absolute(need(a, "input")).toStdString(), out = absolute(need(a, "output")).toStdString();
            const int factor = a.value("factor").toInt(2);
            if (factor < 2 || factor > 4) throw ArgError{"\"factor\" must be 2, 3 or 4"};
            std::string err;
            if (!createSuperScaled(in, out, factor, std::clamp(a.value("strength").toDouble(1), 0.0, 1.0),
                                   [this](double f) { progress(f, "Super Scale"); }, nullptr, &err))
                return fail(QString::fromStdString(err));
            MediaItem m;
            probeMedia(out, m);
            return ok(QStringLiteral("Wrote %1 (%2 x %3)").arg(QString::fromStdString(out)).arg(m.width).arg(m.height),
                      QJsonObject{{"output", QString::fromStdString(out)}, {"width", m.width}, {"height", m.height}});
        });

    add("montage_captions", "Import or export captions",
        "Write a caption track to a file in the format its extension names: .srt (SubRip), .vtt (WebVTT), .scc "
        "(Scenarist, CEA-608), .ttml / .xml / .dfxp (TTML, IMSC 1.1 Text profile, with the track's colours), .stl "
        "(EBU Tech 3264, 25 or 30 fps) or .ass / .ssa (SubStation Alpha, with the track's style); or read any of them "
        "as a new caption track (`import`). `track` is a caption track index (default: the visible one).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"export":{"type":"string","description":"A file to write"},
            "import":{"type":"string","description":"A file to read as a new track"},"track":{"type":"integer"},
            "language":{"type":"string","description":"ISO 639-1 code for an imported track"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            if (a.contains("import")) {
                const QString path = need(a, "import");
                QFile in(path);
                if (!in.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Cannot read %1").arg(path));
                const std::string data = in.readAll().toStdString();
                std::vector<Caption> caps;
                std::string err;
                if (!parseSubtitles(data, s.fps, caps, &err)) return fail(QString::fromStdString(err));
                CaptionTrack t;
                t.id = l.project.newId();
                const std::string stem = QFileInfo(path).completeBaseName().toStdString();
                t.name = stem;
                // "film.fr.srt" names its language.
                if (const size_t dot = stem.rfind('.'); dot != std::string::npos && stem.size() - dot == 3) t.language = stem.substr(dot + 1);
                if (a.contains("language")) t.language = a.value("language").toString().toLower().toStdString();
                t.captions = std::move(caps);
                const int n = int(t.captions.size());
                const QString name = QString::fromStdString(t.name);
                s.captionTracks.push_back(std::move(t));
                save(l);
                return ok(QStringLiteral("Imported %1 captions as \"%2\"").arg(n).arg(name),
                          QJsonObject{{"track", int(s.captionTracks.size()) - 1}, {"name", name}, {"captions", n}});
            }
            const QString path = need(a, "export");
            const CaptionTrack* t = nullptr;
            if (a.contains("track")) {
                const int index = a.value("track").toInt(-1);
                if (index < 0 || index >= int(s.captionTracks.size())) return fail("No such caption track");
                t = &s.captionTracks[size_t(index)];
            } else {
                t = captionTrackFor(s);
                if (!t && !s.captionTracks.empty()) t = &s.captionTracks.front();
            }
            if (!t) return fail("The sequence has no captions");
            const std::string ext = "." + QFileInfo(path).suffix().toStdString();
            if (!captionFormatKnown(ext))
                throw ArgError{QStringLiteral("Unknown caption format \"%1\": use .srt, .vtt, .scc, .ttml, .stl or .ass")
                                   .arg(QString::fromStdString(ext))};
            const std::string data = exportCaptions(*t, s, ext);
            QFile o(path);
            if (!o.open(QIODevice::WriteOnly) || o.write(data.data(), qint64(data.size())) != qint64(data.size()))
                return fail(QStringLiteral("Cannot write %1").arg(path));
            return ok(QStringLiteral("Wrote %1 captions to %2").arg(t->captions.size()).arg(path),
                      QJsonObject{{"path", path}, {"captions", int(t->captions.size())}, {"bytes", double(data.size())}});
        });

    add("montage_edit_captions", "Check and fix captions",
        "Check a caption track against reading limits (by default the Netflix Timed Text Style Guide's: 20 characters a "
        "second, 42 characters a line, two lines, 5/6 s to 7 s on screen, 2 frames between captions) or change it as a "
        "whole. `action`: check (what each caption breaks), fix_timing (short or fast captions stay up longer into the "
        "time after them, each ends the minimum gap before the next, short pauses close up), shift (by `by`: seconds or a "
        "timecode, negative for earlier), sync (the first and last of `captions`, or of the track, start at `first` and "
        "`last`, the rest stretched between: subtitles timed for another cut or frame rate) or replace (`find` with "
        "`replace`, optionally `case_sensitive` and `whole_words`). `captions` (indices) limits shift and replace. Limits "
        "can be changed: max_cps, max_line_chars, max_lines, min_seconds, max_seconds, min_gap_frames.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"track":{"type":"integer","default":0},
            "action":{"type":"string","enum":["check","fix_timing","shift","sync","replace"]},
            "captions":{"type":"array","items":{"type":"integer"}},"by":{"type":["number","string"]},
            "first":{"type":["number","string"]},"last":{"type":["number","string"]},
            "find":{"type":"string"},"replace":{"type":"string"},"case_sensitive":{"type":"boolean"},"whole_words":{"type":"boolean"},
            "max_cps":{"type":"number"},"max_line_chars":{"type":"integer"},"max_lines":{"type":"integer"},
            "min_seconds":{"type":"number"},"max_seconds":{"type":"number"},"min_gap_frames":{"type":"integer"}},
            "required":["project","action"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const int index = a.value("track").toInt(0);
            if (index < 0 || index >= int(s.captionTracks.size())) return fail("No such caption track");
            CaptionTrack& t = s.captionTracks[size_t(index)];
            CaptionLimits lim;
            if (a.contains("max_cps")) lim.maxCps = a.value("max_cps").toDouble();
            if (a.contains("max_line_chars")) lim.maxLineChars = a.value("max_line_chars").toInt();
            if (a.contains("max_lines")) lim.maxLines = a.value("max_lines").toInt();
            if (a.contains("min_seconds")) lim.minSeconds = a.value("min_seconds").toDouble();
            if (a.contains("max_seconds")) lim.maxSeconds = a.value("max_seconds").toDouble();
            if (a.contains("min_gap_frames")) lim.minGapFrames = a.value("min_gap_frames").toInt();
            std::vector<size_t> chosen;
            for (const QJsonValue& v : a.value("captions").toArray()) {
                const int i = v.toInt(-1);
                if (i < 0 || i >= int(t.captions.size())) throw ArgError{QStringLiteral("No caption %1").arg(v.toInt())};
                chosen.push_back(size_t(i));
            }
            const QString action = need(a, "action");
            if (action == "check") {
                const std::vector<unsigned> issues = checkCaptions(t.captions, s.fps, lim);
                QJsonArray list;
                QStringList lines;
                for (size_t i = 0; i < issues.size(); ++i) {
                    if (!issues[i]) continue;
                    const QString what = QString::fromStdString(describeCaptionIssues(issues[i], t.captions[i], s.fps, lim));
                    list.append(QJsonObject{{"caption", int(i)}, {"at", tc(t.captions[i].start, s)},
                                            {"text", QString::fromStdString(t.captions[i].text)}, {"issues", what}});
                    lines << QStringLiteral("#%1 %2: %3").arg(i).arg(tc(t.captions[i].start, s), QString(what).replace('\n', QStringLiteral("; ")));
                }
                const QString head = list.isEmpty() ? QStringLiteral("All %1 captions are within the limits").arg(t.captions.size())
                                                     : QStringLiteral("%1 of %2 captions break the limits:").arg(list.size()).arg(t.captions.size());
                return ok(lines.isEmpty() ? head : head + "\n" + lines.join('\n'),
                          QJsonObject{{"captions", int(t.captions.size())}, {"issues", list}});
            }
            QString done;
            if (action == "fix_timing") {
                const int n = fixCaptionTiming(t.captions, s.fps, lim);
                if (!n) return ok(QStringLiteral("No caption needed retiming"), QJsonObject{{"changed", 0}});
                done = QStringLiteral("Retimed %1 captions").arg(n);
            } else if (action == "shift") {
                if (!a.contains("by")) throw ArgError{QStringLiteral("shift needs \"by\"")};
                const QJsonValue by = a.value("by");
                FrameTime delta = 0;
                if (by.isString() && by.toString().trimmed().startsWith('-'))
                    delta = -timeArg(QJsonValue(by.toString().trimmed().mid(1)), s, "by");
                else
                    delta = timeArg(by, s, "by");
                if (!shiftCaptions(t.captions, chosen, delta)) return fail("Nothing to shift");
                done = QStringLiteral("Shifted %1 captions by %2 frames").arg(chosen.empty() ? t.captions.size() : chosen.size()).arg(delta);
            } else if (action == "sync") {
                if (t.captions.size() < 2) return fail("Syncing needs at least two captions");
                if (chosen.size() < 2) chosen = {0, t.captions.size() - 1};
                const FrameTime fromA = t.captions[chosen.front()].start, fromB = t.captions[chosen.back()].start;
                if (!syncCaptions(t.captions, fromA, timeArg(a.value("first"), s, "first"), fromB, timeArg(a.value("last"), s, "last")))
                    return fail("The two captions start together");
                done = QStringLiteral("Synced %1 captions").arg(t.captions.size());
            } else if (action == "replace") {
                const int n = replaceInCaptions(t.captions, chosen, need(a, "find").toStdString(), a.value("replace").toString().toStdString(),
                                                a.value("case_sensitive").toBool(), a.value("whole_words").toBool());
                if (!n) return fail(QStringLiteral("\"%1\" was not found").arg(a.value("find").toString()));
                done = QStringLiteral("Replaced %1").arg(n);
            } else {
                throw ArgError{QStringLiteral("Unknown action \"%1\"").arg(action)};
            }
            save(l);
            int flagged = 0;
            for (unsigned v : checkCaptions(t.captions, s.fps, lim)) flagged += v != 0;
            return ok(done + QStringLiteral("; %1 captions still break the limits").arg(flagged),
                      QJsonObject{{"captions", int(t.captions.size())}, {"still_flagged", flagged}});
        });

    add("montage_translate_captions", "Translate captions",
        "Translate a caption track into another language on this computer (Opus-MT), as a new track with the same "
        "timings (hidden until chosen). Languages are ISO 639-1 codes (de, fr, es, ja...); pairs without a direct model go "
        "through English. The models must be downloaded (the app asks the first time; or scripts/fetch-models.sh).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"to":{"type":"string"},
            "track":{"type":"integer","default":0,"description":"Caption track index"},
            "from":{"type":"string","description":"The track's language, if its setting is wrong"}},"required":["project","to"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const int index = a.value("track").toInt(0);
            if (index < 0 || index >= int(s.captionTracks.size())) return fail("No such caption track");
            const CaptionTrack source = s.captionTracks[size_t(index)];
            const std::string to = need(a, "to").toLower().toStdString();
            const std::string from = a.contains("from") ? a.value("from").toString().toLower().toStdString()
                                                        : (source.language.empty() ? "en" : source.language);
            const auto route = translationRoute(from, to);
            if (route.empty())
                return fail(QStringLiteral("There is no translation from %1 to %2").arg(QString::fromStdString(from), QString::fromStdString(to)));
            for (const ModelPack* pack : route)
                if (!pack->installed())
                    return fail(QStringLiteral("The %1 is not downloaded (%2 MB): translate once in the app, or fetch it into %3")
                                    .arg(QString::fromStdString(pack->title))
                                    .arg(pack->bytes() / 1000000)
                                    .arg(QString::fromStdString(pack->directory())));
            std::vector<std::string> out;
            std::string err;
            if (!translateTexts(captionTexts(source), from, to, out, {}, nullptr, &err)) return fail(QString::fromStdString(err));
            CaptionTrack t = translatedTrack(source, out, l.project.newId(), to, translationLanguageName(to));
            t.visible = false;
            const QString name = QString::fromStdString(t.name);
            s.captionTracks.push_back(std::move(t));
            save(l);
            QJsonArray sample;
            for (size_t i = 0; i < std::min<size_t>(3, out.size()); ++i) sample.append(QString::fromStdString(out[i]));
            return ok(QStringLiteral("Added \"%1\" (%2 captions)").arg(name).arg(out.size()),
                      QJsonObject{{"track", int(s.captionTracks.size()) - 1}, {"name", name}, {"first", sample}});
        });

    add("montage_auto_broll", "Add B-roll by what is said",
        "Cutaways chosen by the dialogue: the cut's transcribed speech is split into sentences, each compared (CLIP, on "
        "this computer) with what the footage shows, and over the best-matching sentences (`coverage`, a share) a few "
        "seconds of the best-matching shot go on a video track above, picture only. `media` lists the footage to choose "
        "from (default: every video not used in the sequence). Footage not indexed yet is indexed first.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"media":{"type":"array","items":{"type":"number"}},
            "coverage":{"type":"number","default":0.5},"track":{"type":"number","default":1}},"required":["project"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            if (!visualSearchAvailable()) return fail("This build of Montage cannot search footage (no ONNX Runtime)");
            if (!visualModel().installed())
                return fail("The visual search model is not downloaded: run `scripts/fetch-models.sh` or open Find Shots in the app once");
            Sequence& s = l.seq();
            std::vector<Id> media;
            for (const QJsonValue& v : a.value("media").toArray()) media.push_back(Id(v.toDouble()));
            if (media.empty()) {
                std::vector<Id> used;
                for (TrackRef r : allTracks(s))
                    for (const Clip& c : trackAt(s, r)->clips) used.push_back(c.mediaId);
                for (const MediaItem& m : l.project.media)
                    if (m.kind == MediaKind::Video && std::find(used.begin(), used.end(), m.id) == used.end()) media.push_back(m.id);
            }
            if (media.empty()) return fail("There is no footage to choose cutaways from");
            bool changed = false;
            if (const QString e = indexMissing(l.project, media, changed); !e.isEmpty()) return fail(e);
            std::string err;
            auto clip = ClipModel::load(&err);
            if (!clip) return fail(QString::fromStdString(err));
            BrollOptions o;
            o.coverage = std::clamp(a.value("coverage").toDouble(0.5), 0.05, 1.0);
            const std::vector<BrollPick> picks =
                planBroll(l.project, s, media, [&](const std::string& t) { return clip->text(t); }, o, &err);
            if (picks.empty()) {
                if (changed) save(l);
                return fail(QString::fromStdString(err));
            }
            const edit::Result r = placeBroll(l.project, s, picks, std::max(1, a.value("track").toInt(1)));
            if (!r.ok) return fail(QString::fromStdString(r.error));
            save(l);
            QJsonArray list;
            for (const BrollPick& b : picks) {
                const MediaItem* m = l.project.findMedia(b.media);
                list.append(QJsonObject{{"sentence", QString::fromStdString(b.sentence)}, {"at", tc(b.at, s)}, {"seconds", double(b.length) / s.fpsValue()},
                                        {"media", m ? QString::fromStdString(m->name) : QString()}, {"media_id", double(b.media)}, {"score", b.score}});
            }
            return ok(QStringLiteral("%1 cutaway(s) added").arg(picks.size()), QJsonObject{{"cutaways", list}});
        });

    add("montage_find_shots", "Find shots by description",
        "Search the project's footage by what it shows (\"a dog on a beach\", \"close-up of hands\"), with CLIP running on "
        "this computer. Videos not indexed yet are indexed first (once; the index is saved in the project). Returns the "
        "best moments: media file and media times, best first. Give like instead of query to find moments that look like "
        "a frame of the footage (the other takes of a shot, cutaways of the same place).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"query":{"type":"string"},
            "like":{"type":"object","properties":{"media":{"type":"string","description":"Media file or name in the project"},
                "seconds":{"type":"number","description":"Media time of the frame"}},"required":["media","seconds"]},
            "max":{"type":"integer","default":10}},"required":["project"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            if (!visualSearchAvailable()) return fail("This build of Montage cannot search footage (no ONNX Runtime)");
            if (!visualModel().installed())
                return fail("The visual search model is not downloaded: run `scripts/fetch-models.sh` or open Find Shots in the app once");
            std::string err;
            bool changed = false;
            if (const QString e = indexMissing(l.project, {}, changed); !e.isEmpty()) return fail(e);
            if (changed) save(l);
            const size_t max = size_t(std::clamp(a.value("max").toInt(10), 1, 100));
            std::vector<ShotMatch> hits;
            if (a.value("like").isObject()) {
                const QJsonObject like = a.value("like").toObject();
                const QString which = like.value("media").toString();
                const MediaItem* from = nullptr;
                for (const MediaItem& m : l.project.media)
                    if (QString::fromStdString(m.name) == which || QString::fromStdString(m.path) == absolute(which)) from = &m;
                if (!from) throw ArgError{QStringLiteral("No media \"%1\" in the project").arg(which)};
                std::vector<float> image;
                const double at = like.value("seconds").toDouble();
                if (!embedFrame(from->path, at, image, &err)) return fail(QString::fromStdString(err));
                hits = findSimilarShots(l.project, image, from->id, at, max);
            } else {
                auto clip = ClipModel::load(&err);
                const std::vector<float> q = clip ? clip->text(need(a, "query").toStdString(), &err) : std::vector<float>{};
                if (q.empty()) return fail(QString::fromStdString(err));
                hits = findShots(l.project, q, max);
            }
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

    add("montage_find_people", "Find people",
        "Find who is in the project's footage, on this computer: faces in the videos and stills not looked through yet "
        "are found (YuNet) and told apart (SFace), then grouped into people across the project (saved in the project). "
        "Lists everyone, most seen first, with an id and a name (\"Person N\" until named with montage_name_person). "
        "Give person (an id or name) for the moments they are seen: media file and media times.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "person":{"type":["integer","string"],"description":"A person's id or name"},
            "max":{"type":"integer","default":50,"description":"At most this many moments"}},"required":["project"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            if (!faceSearchAvailable()) return fail("This build of Montage cannot find people (no ONNX Runtime)");
            bool changed = false;
            for (MediaItem& m : l.project.media) {
                if ((m.kind != MediaKind::Video && m.kind != MediaKind::Image) || !m.hasVideo || m.path.empty() || m.subclipOf || m.faces)
                    continue;
                if (!faceModel().installed())
                    return fail("The face models are not downloaded: run `scripts/fetch-models.sh` or use Find People in the app once");
                FaceIndex f;
                std::string err;
                if (!indexFaces(m.path, m.kind == MediaKind::Image ? 0.0 : m.duration, f, 0, 8, 32,
                                [&](double x) { progress(x, QStringLiteral("Looking for faces in %1").arg(QString::fromStdString(m.name))); },
                                nullptr, &err))
                    return fail(QString::fromStdString(m.name + ": " + err));
                m.faces = std::make_shared<const FaceIndex>(std::move(f));
                changed = true;
            }
            if (changed) {
                groupPeople(l.project);
                save(l);
            }
            const auto people = peopleIn(l.project);
            QJsonArray list;
            QString text;
            for (const PersonSummary& s : people) {
                list.append(QJsonObject{{"id", s.id}, {"name", QString::fromStdString(s.name)}, {"clips", s.media}, {"faces", s.faces}});
                text += QStringLiteral("%1 (id %2): in %3 clip(s)\n").arg(QString::fromStdString(s.name)).arg(s.id).arg(s.media);
            }
            QJsonObject out{{"people", list}};
            if (a.contains("person")) {
                const int id = personId(l.project, a.value("person"));
                QJsonArray moments;
                const auto found = findPerson(l.project, id);
                const size_t max = size_t(std::clamp(a.value("max").toInt(50), 1, 1000));
                text += QStringLiteral("\n%1 is seen in:\n").arg(QString::fromStdString(personName(l.project, id)));
                for (size_t i = 0; i < found.size() && i < max; ++i) {
                    const PersonMoment& pm = found[i];
                    const MediaItem* m = l.project.findMedia(pm.media);
                    if (!m) continue;
                    moments.append(QJsonObject{{"media", QString::fromStdString(m->path)}, {"start_seconds", pm.start},
                                               {"end_seconds", pm.end}, {"best_seconds", pm.best}});
                    text += QStringLiteral("%1  %2-%3 s\n").arg(QString::fromStdString(m->name)).arg(pm.start, 0, 'f', 1).arg(pm.end, 0, 'f', 1);
                }
                out["moments"] = moments;
            }
            return ok(text.isEmpty() ? QStringLiteral("No faces found") : text, out);
        });

    add("montage_name_person", "Name a person",
        "Name a person found by montage_find_people (\"\" goes back to \"Person N\"), or, with same_as, join them with another "
        "person found separately (the same person in different light, say). Smart bins that name them follow.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "person":{"type":["integer","string"],"description":"Their id or current name"},
            "name":{"type":"string"},
            "same_as":{"type":["integer","string"],"description":"Another person's id or name: merge into them"}},
            "required":["project","person"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            int id = personId(l.project, a.value("person"));
            QString text;
            if (a.contains("same_as")) {
                const int into = personId(l.project, a.value("same_as"));
                if (!mergePeople(l.project, id, into)) return fail("Those are the same person");
                text = QStringLiteral("Joined with %1").arg(QString::fromStdString(personName(l.project, into)));
                id = into;
            }
            if (a.contains("name")) {
                renamePerson(l.project, id, a.value("name").toString().trimmed().toStdString());
                text += (text.isEmpty() ? "" : "; ") + QStringLiteral("named %1").arg(QString::fromStdString(personName(l.project, id)));
            }
            if (text.isEmpty()) throw ArgError{"Give name or same_as"};
            save(l);
            return ok(text, QJsonObject{{"id", id}, {"name", QString::fromStdString(personName(l.project, id))}});
        });

    add("montage_generate_speech", "Generate a voiceover",
        "Speak English text with an AI voice on this computer (Kokoro) and place it on an audio track: text at a time "
        "(default the start), or a caption track spoken cue by cue at each cue's time, a little faster where a cue is "
        "short. Voices: af_heart, af_bella, af_sarah, am_michael, am_adam, am_puck (American), bf_emma, bf_isabella, "
        "bm_george, bm_lewis (British). The WAV files go in a Voiceover folder beside the project.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"text":{"type":"string"},
            "captions":{"type":"integer","description":"Caption track index to speak instead of text"},
            "voice":{"type":"string","default":"af_heart"},"speed":{"type":"number","default":1},
            "at":{"type":["number","string"]},"track":{"type":"string","description":"Audio track, default A1"}},
            "required":["project"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            if (!ttsAvailable()) return fail("This build of Montage cannot speak (no ONNX Runtime)");
            if (!ttsModel().installed())
                return fail("The speech model is not downloaded: run `scripts/fetch-models.sh` or generate a voiceover once in the app");
            const std::string voice = str(a, "voice", "af_heart").toStdString();
            if (!findTtsVoice(voice)) throw ArgError{QStringLiteral("Unknown voice \"%1\"").arg(QString::fromStdString(voice))};
            const double speed = std::clamp(a.value("speed").toDouble(1), 0.5, 2.0);
            std::vector<SpeechLine> lines;
            if (a.contains("captions")) {
                const int index = a.value("captions").toInt();
                if (index < 0 || index >= int(s.captionTracks.size())) throw ArgError{"No such caption track"};
                lines = captionSpeech(s.captionTracks[size_t(index)]);
            } else {
                lines.push_back({need(a, "text").toStdString(), a.contains("at") ? timeArg(a.value("at"), s, "at") : 0, -1});
            }
            if (lines.empty()) return fail("Nothing to say");
            const TrackRef au = trackArg(str(a, "track", "A1"), s, true, &l.project, &s);
            QJsonArray placed;
            double total = 0;
            const QString err = speakLines(l.project, s, lines, voice, speed, au,
                                           QFileInfo(absolute(need(a, "project"))).absolutePath() + QStringLiteral("/Voiceover"), placed, total);
            if (!err.isEmpty()) return fail(err);
            save(l);
            return ok(QStringLiteral("Placed %1 voiceover clip(s), %2 s in all").arg(lines.size()).arg(total, 0, 'f', 1),
                      QJsonObject{{"clips", placed}});
        });

    add("montage_dub", "Dub into English",
        "Dub a caption track into English on this computer: translated from its language (Opus-MT; skipped if it is English), "
        "each cue spoken at its time with an AI voice (Kokoro; a little faster where a cue is short) on a new audio track "
        "\"Dub (English)\", and every other audio clip lowered by duck_db while the dub speaks (volume keyframes, replacing "
        "theirs; 0 leaves them), as in a voice-over translation. The English captions are added as a hidden track. Needs the "
        "translation and speech models (the app asks the first time; or scripts/fetch-models.sh).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "track":{"type":"integer","default":0,"description":"Caption track index"},
            "from":{"type":"string","description":"The track's language, if its setting is wrong"},
            "voice":{"type":"string","default":"af_heart"},"speed":{"type":"number","default":1},
            "duck_db":{"type":"number","default":-18}},"required":["project"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const int index = a.value("track").toInt(0);
            if (index < 0 || index >= int(s.captionTracks.size())) return fail("No such caption track");
            if (s.captionTracks[size_t(index)].captions.empty()) return fail("That caption track is empty");
            if (!ttsAvailable() || !translatorAvailable()) return fail("This build of Montage cannot dub (no ONNX Runtime)");
            if (!ttsModel().installed())
                return fail("The speech model is not downloaded: run `scripts/fetch-models.sh` or generate a voiceover once in the app");
            const std::string voice = str(a, "voice", "af_heart").toStdString();
            if (!findTtsVoice(voice)) throw ArgError{QStringLiteral("Unknown voice \"%1\"").arg(QString::fromStdString(voice))};
            const double speed = std::clamp(a.value("speed").toDouble(1), 0.5, 2.0);
            const double duckDb = std::clamp(a.value("duck_db").toDouble(-18), -60.0, 0.0);
            const CaptionTrack source = s.captionTracks[size_t(index)];
            const std::string from = a.contains("from") ? a.value("from").toString().toLower().toStdString()
                                                        : (source.language.empty() ? "en" : source.language);
            int english = index;
            if (from != "en") {
                const auto route = translationRoute(from, "en");
                if (route.empty()) return fail(QStringLiteral("There is no translation from %1 to English").arg(QString::fromStdString(from)));
                for (const ModelPack* pack : route)
                    if (!pack->installed())
                        return fail(QStringLiteral("The %1 is not downloaded (%2 MB): translate once in the app, or fetch it into %3")
                                        .arg(QString::fromStdString(pack->title))
                                        .arg(pack->bytes() / 1000000)
                                        .arg(QString::fromStdString(pack->directory())));
                std::vector<std::string> out;
                std::string err;
                progress(0, QStringLiteral("Translating"));
                if (!translateTexts(captionTexts(source), from, "en", out, {}, nullptr, &err)) return fail(QString::fromStdString(err));
                CaptionTrack t = translatedTrack(source, out, l.project.newId(), "en", translationLanguageName("en"));
                t.visible = false;
                s.captionTracks.push_back(std::move(t));
                english = int(s.captionTracks.size()) - 1;
            }
            const std::vector<SpeechLine> lines = captionSpeech(s.captionTracks[size_t(english)]);
            if (lines.empty()) return fail("Nothing to say");
            const TrackRef dub = edit::addTrack(l.project, s, TrackKind::Audio);
            s.audioTracks[size_t(dub.index)].name = "Dub (English)";
            QJsonArray placed;
            double total = 0;
            const QString err = speakLines(l.project, s, lines, voice, speed, dub,
                                           QFileInfo(absolute(need(a, "project"))).absolutePath() + QStringLiteral("/Voiceover"), placed, total);
            if (!err.isEmpty()) return fail(err);
            int ducked = 0;
            if (duckDb < 0) {
                DuckOptions o;
                o.amountDb = duckDb;
                const Spans spans = clipSpans(s, dub.index, o.minPause);
                for (int i = 0; i < int(s.audioTracks.size()); ++i)
                    if (i != dub.index)
                        for (Clip& c : s.audioTracks[size_t(i)].clips) ducked += duckClip(c, s, spans, o) ? 1 : 0;
            }
            save(l);
            QJsonArray sample;
            for (size_t i = 0; i < std::min<size_t>(3, lines.size()); ++i) sample.append(QString::fromStdString(lines[i].text));
            return ok(QStringLiteral("Dubbed %1 caption(s) onto A%2 (%3 s of speech); lowered %4 other clip(s) under it")
                          .arg(lines.size())
                          .arg(dub.index + 1)
                          .arg(total, 0, 'f', 1)
                          .arg(ducked),
                      QJsonObject{{"captions_track", english}, {"audio_track", QStringLiteral("A%1").arg(dub.index + 1)}, {"clips", placed},
                                  {"ducked", ducked}, {"first", sample}});
        });

    add("montage_log_media", "Log media",
        "Log media in a project as an editor does, to find it again: a rating (-1 rejects, 0 unrated, 1-5 stars), a colour "
        "label, keywords to add or remove, metadata fields (scene, shot, take, camera, device, description, comment, or name), "
        "the scene, shot and take read from the slate called at the head of a transcribed take, and the bin it is in "
        "(\"Interviews/Day 1\"; \"\" for the top level).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "media":{"type":["string","array"],"items":{"type":"string"},"description":"Media files or names in the project"},
            "rating":{"type":"integer","minimum":-1,"maximum":5},
            "label":{"type":"string","description":"None, Violet, Iris, Caribbean, Lavender, Cerulean, Forest, Rose, Mango, Yellow, Tan or Red"},
            "add_keywords":{"type":["array","string"],"items":{"type":"string"}},
            "remove_keywords":{"type":["array","string"],"items":{"type":"string"}},
            "fields":{"type":"object","additionalProperties":{"type":"string"},"description":"Field name to text; empty text clears it"},
            "from_slate":{"type":"boolean","description":"Set scene, shot and take from the slate called at the head of each (\"Scene 12 apple, take 3\"), from its transcript"},
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
            int slates = 0;
            if (a.value("from_slate").toBool()) {
                std::vector<Id> ids;
                for (MediaItem* m : items) {
                    if (!m->transcript) throw ArgError{QStringLiteral("%1 has no transcript: transcribe it first").arg(QString::fromStdString(m->name))};
                    ids.push_back(m->id);
                }
                slates = logFromSlates(l.project, ids);
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
            return ok(QStringLiteral("Logged %1 media item(s)%2").arg(items.size()).arg(a.value("from_slate").toBool()
                                                                                         ? QStringLiteral(", %1 from spoken slates").arg(slates)
                                                                                         : QString()),
                      QJsonObject{{"media", out}, {"from_slates", slates}});
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
                "watermark_opacity":{"type":"number","default":0.6}}},
            "downmix_stereo":{"type":"boolean","default":false,"description":"A 5.1/7.1 sequence: fold the mix down to stereo"},
            "stems":{"type":"string","enum":["none","tracks","buses","roles"],"default":"none",
                "description":"Also write 24-bit WAV stems beside the output, one per audio track, per bus (plus Main) or per audio role"}},
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
            st.downmixStereo = a.value("downmix_stereo").toBool();
            const QString stems = str(a, "stems", "none");
            if (stems != "none" && stems != "tracks" && stems != "buses" && stems != "roles")
                throw ArgError{"\"stems\" must be none, tracks, buses or roles"};
            std::string err;
            if (!exportSequence(l.project, s, st, [this](double f, FrameTime) { progress(f, "Rendering"); }, nullptr, &err))
                return fail(QString::fromStdString(err));
            QJsonObject o{{"output", QString::fromStdString(st.path)}};
            QString text = QStringLiteral("Wrote %1").arg(QString::fromStdString(st.path));
            if (stems != "none") {
                std::vector<StemFile> files;
                if (!exportStems(l.project, s, st, stems == "buses" ? StemsByBus : stems == "roles" ? StemsByRole : StemsByTrack, &files, [this](double f, FrameTime) { progress(f, "Stems"); },
                                 nullptr, &err))
                    return fail(QString::fromStdString(err));
                QJsonArray list;
                for (const StemFile& f : files) {
                    list.append(QJsonObject{{"name", QString::fromStdString(f.name)}, {"path", QString::fromStdString(f.path)}});
                    text += QStringLiteral("\nWrote %1").arg(QString::fromStdString(f.path));
                }
                o["stems"] = list;
            }
            return ok(text, o);
        });

    add("montage_export_timeline", "Export the timeline",
        "Write the active sequence as an EDL, OpenTimelineIO, Final Cut Pro 7 XML (Premiere, Resolve), FCPXML (Final Cut "
        "Pro) or AAF for audio post (Pro Tools, Fairlight: the audio tracks, linked to mono WAVs written to a \"<name> "
        "Media\" folder beside it, with crossfades, fades and clip gain).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"format":{"type":"string","enum":["edl","otio","xml","fcpxml","aaf"]},
            "output":{"type":"string"}},"required":["project","format","output"]})json",
        true, [this](const QJsonObject& a) {
            Loaded l = open(a);
            const QString f = need(a, "format");
            if (f == "aaf") {
                const QString out = absolute(need(a, "output"));
                AafExportResult r;
                std::string err;
                if (!exportAaf(l.project, l.seq(), out.toStdString(), &r, [this](double x, FrameTime) { progress(x, "AAF"); }, nullptr, &err))
                    return fail(QString::fromStdString(err));
                QJsonArray files, warnings;
                for (const std::string& m : r.mediaFiles) files.append(QString::fromStdString(m));
                for (const std::string& w : r.warnings) warnings.append(QString::fromStdString(w));
                return ok(QStringLiteral("Wrote %1: %2 audio tracks, %3 clips, %4 crossfades, %5 WAV files")
                              .arg(out).arg(r.audioTracks).arg(r.clips).arg(r.transitions).arg(files.size()),
                          QJsonObject{{"output", out}, {"audio_tracks", r.audioTracks}, {"clips", r.clips},
                                      {"crossfades", r.transitions}, {"media", files}, {"warnings", warnings}});
            }
            const std::string text = f == "otio" ? exportOtio(l.project, l.seq())
                                     : f == "xml" ? exportFcp7Xml(l.project, l.seq())
                                     : f == "fcpxml" ? exportFcpXml(l.project, l.seq())
                                     : f == "edl" ? exportEdl(l.project, l.seq())
                                                  : throw ArgError{"format must be edl, otio, xml, fcpxml or aaf"};
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
