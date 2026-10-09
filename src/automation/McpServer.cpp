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
#include <optional>
#include <numeric>
#include <sstream>

#include "core/ClipAnimation.h"
#include "core/GradeVersions.h"
#include "core/AudioChannels.h"
#include "core/TranscriptCorrect.h"
#include "core/SpellCheck.h"
#include "core/ColorGroups.h"
#include "render/ExtendClip.h"
#include "render/ReviewExport.h"
#include "render/RoomTone.h"
#include "render/Versions.h"
#include "render/Dcp.h"
#include "render/Imf.h"
#include "render/LightLevel.h"
#include "render/Spherical.h"
#include "render/ClipPlacement.h"
#include "media/SpeechSearch.h"
#include "media/TextReader.h"
#include "media/ImageSequence.h"
#include "media/Interpret.h"
#include "media/MediaPool.h"
#include "render/Ofx.h"
#include "media/Psd.h"
#include "core/AutoTag.h"
#include "core/Automation.h"
#include "core/CaptionTools.h"
#include "core/Captions.h"
#include "core/ColorWarp.h"
#include "core/Chapters.h"
#include "core/ChapterSuggest.h"
#include "core/MarkerList.h"
#include "core/MaskPath.h"
#include "core/Bleep.h"
#include "audio/SpectralRepair.h"
#include "core/Checkerboard.h"
#include "core/EditOps.h"
#include "core/TimelineCompare.h"
#include "core/Reconform.h"
#include "core/Effects.h"
#include "core/History.h"
#include "core/Interchange.h"
#include "core/MediaLog.h"
#include "core/ProjectIO.h"
#include "core/ScriptCut.h"
#include "core/Surround.h"
#include "core/TranscriptEdit.h"
#include "media/DualSystem.h"
#include "media/Relink.h"
#include "media/SpeechEnhance.h"
#include "media/SuperScale.h"
#include "media/Translator.h"
#include "render/AudioReactive.h"
#include "render/VfxPull.h"
#include "render/AafExport.h"
#include "render/AutoMix.h"
#include "render/AutoBroll.h"
#include "render/Highlights.h"
#include "render/MusicEdit.h"
#include "render/VoiceMatch.h"
#include "media/Analysis.h"
#include "media/AutoDuck.h"
#include "media/MicBleed.h"
#include "core/Slate.h"
#include "render/PaperEdit.h"
#include "render/LutExport.h"
#include "render/ProjectManager.h"
#include "render/QualityCheck.h"
#include "media/Decoder.h"
#include "media/Faces.h"
#include "media/FaceTracks.h"
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
#include "render/Shorts.h"
#include "render/Letterbox.h"
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

// OpenFX plugins installed here (render/Ofx.h), found once per process.
const std::vector<ofx::PluginDesc>& openFxPlugins() {
    static const std::vector<ofx::PluginDesc> found = [] {
        if (ofx::Registry::instance().plugins().empty()) ofx::Registry::instance().scan();
        return ofx::Registry::instance().plugins();
    }();
    return found;
}
bool openFxPlugin(const std::string& id, ofx::PluginDesc& out) {
    for (const ofx::PluginDesc& d : openFxPlugins())
        if (d.id == id) {
            out = d;
            return true;
        }
    return ofx::Registry::instance().find(id, out);
}

// How a media item is read (Interpret Footage), for reports; empty as the file says.
QJsonObject interpretationJson(const MediaItem& m) {
    const Interpretation i = interpretationOf(m);
    QJsonObject o;
    if (i.conformed()) {
        o["frame_rate"] = i.fps.toDouble();
        o["file_frame_rate"] = i.fileFps.toDouble();
        if (i.keepPitch) o["keep_pitch"] = true;
    }
    if (i.par > 0) o["pixel_aspect"] = i.par;
    if (!i.alpha.empty()) o["alpha"] = QString::fromStdString(i.alpha);
    if (!i.fields.empty()) o["field_order"] = QString::fromStdString(i.fields);
    if (i.rawExposure != 0) o["raw_exposure"] = i.rawExposure;
    if (i.rawTemperature > 0) o["raw_temperature"] = i.rawTemperature;
    if (i.rawTint != 0) o["raw_tint"] = i.rawTint;
    if (!i.rawHighlights.empty()) o["raw_highlights"] = QString::fromStdString(i.rawHighlights);
    if (i.rawHalf) o["raw_half"] = true;
    return o;
}

// A media item's entry in reports: its file (without how it is read) and, when interpreted, how.
void mediaPathJson(const MediaItem& m, QJsonObject& o) {
    if (m.path.empty()) return;
    o["path"] = QString::fromStdString(uninterpretedPath(m.path));
    if (const QJsonObject i = interpretationJson(m); !i.isEmpty()) o["interpretation"] = i;
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
        mediaPathJson(m, mo);
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
    for (const MediaItem& m : p.media)  // the file, however it is read
        if (!m.subclipOf && m.kind != MediaKind::Sequence && uninterpretedPath(m.path) == abs) return m.id;
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
        if (!m.path.empty() && !m.subclipOf && uninterpretedPath(m.path) == abs) return m;
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

    add("montage_merge_clips", "Merge picture and separate sound",
        "Dual-system sound (Premiere's Merge Clips, Resolve's Auto Sync Audio): join a camera clip's picture with a "
        "field recorder's sound files into one merged clip in the bin, lined up by `sync`: auto (timecode when both are "
        "stamped and overlap, else by matching the camera's own sound), timecode, waveform or starts (both start "
        "together). The recorder's sound is trimmed to the picture; the camera's sound is kept, muted, unless "
        "keep_camera_audio is false. A recorder's BWF/iXML timecode, scene, take and channel names are read on import. "
        "Files are paths (imported if new) or names in the project. place appends the merged clip to the sequence.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"video":{"type":"string"},
            "sounds":{"type":"array","items":{"type":"string"}},
            "sync":{"type":"string","enum":["auto","timecode","waveform","starts"],"default":"auto"},
            "keep_camera_audio":{"type":"boolean","default":true},"name":{"type":"string"},"place":{"type":"boolean","default":false}},
            "required":["project","video","sounds"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            auto ref = [&](const QString& r) {
                const std::string name = r.toStdString();
                for (const MediaItem& m : l.project.media)
                    if (m.name == name) return m.id;
                return mediaFor(l.project, r);
            };
            const Id video = ref(need(a, "video"));
            std::vector<Id> sounds;
            for (const QJsonValue& v : a.value("sounds").toArray()) sounds.push_back(ref(v.toString()));
            if (sounds.empty()) throw ArgError{"\"sounds\" lists the recorder's files"};
            const QString by = a.value("sync").toString("auto");
            if (by != "auto" && by != "timecode" && by != "waveform" && by != "starts") throw ArgError{"sync is auto, timecode, waveform or starts"};
            std::vector<double> offsets;
            QJsonArray how;
            for (Id s : sounds) {
                SoundSync found;
                if (by == "starts") found.found = true;
                else found = syncSound(l.project, video, s, by == "timecode" ? SyncBy::Timecode : by == "waveform" ? SyncBy::Waveform : SyncBy::Auto);
                if (!found.found)
                    return fail(QStringLiteral("Could not line up %1 with the picture by %2")
                                    .arg(QString::fromStdString(l.project.findMedia(s)->name), by == "timecode" ? "timecode" : "timecode or sound"));
                offsets.push_back(found.offset);
                how.append(QJsonObject{{"sound", QString::fromStdString(l.project.findMedia(s)->name)},
                                       {"offset", std::round(found.offset * 1000) / 1000},
                                       {"by", by == "starts" ? "starts" : found.byTimecode ? "timecode" : "waveform"}});
            }
            MergeOptions o;
            o.name = a.value("name").toString().toStdString();
            o.keepCameraAudio = a.value("keep_camera_audio").toBool(true);
            std::string err;
            const Id made = mergeClips(l.project, video, sounds, offsets, o, &err);
            if (!made) return fail(QString::fromStdString(err));
            if (a.value("place").toBool()) {
                Sequence& s = l.seq();
                check(edit::placeMedia(l.project, s, made, s.duration(), 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false));
            }
            save(l);
            QJsonObject o2 = mediaJson(*l.project.findMedia(made));
            o2["synced"] = how;
            return ok(QStringLiteral("Merged \"%1\"").arg(QString::fromStdString(l.project.findMedia(made)->name)), o2);
        });

    add("montage_sync_dailies", "Sync dailies",
        "Merge every camera clip with the field recorder file that belongs to it, in one go (Resolve's Auto Sync Audio "
        "on a bin): pairs found by timecode overlap, else by matching each camera's own sound against the sound files. "
        "`media` limits it to those items (paths or names; default: every video and sound file in the project). The "
        "camera's sound is kept, muted, unless keep_camera_audio is false. place appends the merged clips to the sequence "
        "in order (a synced dailies reel).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"media":{"type":"array","items":{"type":"string"}},
            "keep_camera_audio":{"type":"boolean","default":true},"place":{"type":"boolean","default":false}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            std::vector<Id> media;
            if (a.contains("media")) {
                for (const QJsonValue& v : a.value("media").toArray()) {
                    const std::string name = v.toString().toStdString();
                    Id id = 0;
                    for (const MediaItem& m : l.project.media)
                        if (m.name == name) id = m.id;
                    media.push_back(id ? id : mediaFor(l.project, v.toString()));
                }
            } else {
                for (const MediaItem& m : l.project.media) media.push_back(m.id);
            }
            std::vector<std::string> report;
            const std::vector<Id> made = syncDailies(l.project, media, a.value("keep_camera_audio").toBool(true), &report);
            QStringList lines;
            for (const std::string& r : report) lines << QString::fromStdString(r);
            if (made.empty()) return fail(QStringLiteral("No camera clip could be matched with a sound file") + (lines.isEmpty() ? "" : ":\n" + lines.join('\n')));
            if (a.value("place").toBool()) {
                Sequence& s = l.seq();
                for (Id id : made) check(edit::placeMedia(l.project, s, id, s.duration(), 0, -1, {TrackKind::Video, 0}, {TrackKind::Audio, 0}, false));
            }
            save(l);
            QJsonArray items;
            for (Id id : made) items.append(mediaJson(*l.project.findMedia(id)));
            return ok(QStringLiteral("Merged %1 clips:\n%2").arg(made.size()).arg(lines.join('\n')), QJsonObject{{"merged", items}});
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
            "image_sequence":{"type":"boolean","default":false,"description":"media is one frame of a numbered image sequence (EXR, DPX, PNG...): place the whole run"},
            "fps":{"type":"number","description":"The image sequence's frame rate; default the sequence's"},
            "psd_mode":{"type":"string","enum":["merged","layer","sequence"],"default":"merged",
                        "description":"A Photoshop file: merged as one still, one `layer` of it, or its layers as a nested sequence (a track per layer with its blend mode, opacity, visibility, group and clipping)"},
            "layer":{"type":["number","string"],"description":"With psd_mode layer: the layer's name or index (0 = the bottom)"},
            "mode":{"type":"string","enum":["overwrite","insert","place_on_top","ripple_overwrite","smart_insert"],
                    "description":"Default overwrite (insert, when insert is true)"}},
            "required":["project","media"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            // A Photoshop file's layers (before the sequence is taken: a nested sequence adds one).
            Id psdMedia = 0;
            if (const QString pm = str(a, "psd_mode", "merged"); pm != QLatin1String("merged")) {
                const std::string file = absolute(need(a, "media")).toStdString();
                if (!isPsdFile(file)) throw ArgError{"psd_mode is for Photoshop files (.psd, .psb)"};
                std::string err;
                if (pm == QLatin1String("sequence")) {
                    const std::vector<Id> ids = importPsd(l.project, file, PsdImport::Sequence, 5, &err);
                    if (ids.empty()) throw ArgError{QString::fromStdString(err)};
                    psdMedia = ids.back();
                } else if (pm == QLatin1String("layer")) {
                    PsdInfo info;
                    if (!readPsdInfo(file, info, &err)) throw ArgError{QString::fromStdString(err)};
                    const QJsonValue want = a.value("layer");
                    int index = -1;
                    for (const PsdLayer& layer : info.layers)
                        if (layer.hasPixels() && (want.isDouble() ? layer.index == want.toInt() : QString::fromStdString(layer.name) == want.toString()))
                            index = layer.index;
                    if (index < 0) throw ArgError{QStringLiteral("No layer %1 with pixels in %2").arg(want.toVariant().toString(), need(a, "media"))};
                    const std::string key = psdLayerPath(file, index);
                    for (const MediaItem& m : l.project.media)
                        if (m.path == key) psdMedia = m.id;
                    if (!psdMedia) {
                        MediaItem m;
                        if (!probeMedia(key, m, &err)) throw ArgError{QString::fromStdString(err)};
                        m.name = QFileInfo(QString::fromStdString(file)).completeBaseName().toStdString() + " - " + info.layers[size_t(index)].name;
                        m.id = psdMedia = l.project.newId();
                        l.project.media.push_back(m);
                    }
                } else {
                    throw ArgError{"\"psd_mode\" is merged, layer or sequence"};
                }
            }
            Sequence& s = l.seq();
            // A subclip (by name) places its range of its media, under its name.
            const MediaItem* sub = nullptr;
            for (const MediaItem& m : l.project.media)
                if (m.subclipOf && m.name == need(a, "media").toStdString()) sub = &m;
            const std::string subName = sub ? sub->name : std::string();
            Id media = psdMedia;
            if (media) {
            } else if (a.value("image_sequence").toBool()) {
                ImageSequence seq;
                if (!detectImageSequence(absolute(need(a, "media")).toStdString(), seq))
                    throw ArgError{QStringLiteral("%1 is not part of a numbered sequence").arg(need(a, "media"))};
                seq.fps = a.contains("fps") ? rateFor(a.value("fps").toDouble()) : s.fps;
                const std::string key = imageSequencePath(seq);
                for (const MediaItem& m : l.project.media)
                    if (m.path == key) media = m.id;
                if (!media) {
                    MediaItem m;
                    std::string err;
                    if (!probeMedia(key, m, &err)) throw ArgError{QString::fromStdString(err)};
                    m.id = media = l.project.newId();
                    l.project.media.push_back(m);
                }
            } else {
                media = sub ? sub->subclipOf : mediaFor(l.project, need(a, "media"));
            }
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

    add("montage_delete_gaps", "Delete gaps",
        "Close every stretch of the timeline where no track has anything (Resolve's Delete Gaps), so nothing slips out of "
        "sync: what follows moves up with its captions, markers and track automation. With leading, the empty start too. "
        "Gaps that a locked track's clips would have to cross stay.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"leading":{"type":"boolean","default":false}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            int closed = 0;
            FrameTime frames = 0;
            check(edit::deleteGaps(l.project, l.seq(), a.value("leading").toBool(), &closed, &frames));
            save(l);
            return ok(QStringLiteral("Closed %1 gap(s), %2 in all").arg(closed).arg(tc(frames, l.seq())),
                      QJsonObject{{"gaps", closed}, {"frames", double(frames)}, {"duration", tc(l.seq().duration(), l.seq())}});
        });

    add("montage_fill_room_tone", "Fill a gap with room tone",
        "Fill a gap in a dialogue track with room tone (iZotope RX's Ambience Match): the background of a clip (`source`, "
        "else the one before the gap, else after it) is learned from its quietest moments and new sound with the same "
        "spectrum, width and level is written as a WAV in a Room Tone folder beside the project and placed in the gap "
        "around `at` on `track` (or from `at` to `to`).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"track":{"type":"string","default":"A1"},
            "at":{"type":["number","string"]},"to":{"type":["number","string"]},"source":{"type":"number","description":"Clip id to learn from"}},
            "required":["project","at"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const TrackRef track = trackArg(str(a, "track", "A1"), s, false);
            if (track.kind != TrackKind::Audio) throw ArgError{"Room tone goes on an audio track"};
            const FrameTime at = timeArg(a.value("at"), s, "at");
            TrackGap gap;
            if (!gapAt(s, track, at, gap)) return fail("There is a clip there: room tone fills a gap");
            const FrameTime from = a.contains("to") ? at : gap.start;
            const FrameTime end = a.contains("to") ? std::min(timeArg(a.value("to"), s, "to"), gap.end) : gap.end;
            if (end <= from) throw ArgError{"\"to\" comes after \"at\""};
            const Id sourceId = a.contains("source") ? Id(a.value("source").toDouble()) : (gap.before ? gap.before : gap.after);
            const Clip* source = sourceId ? edit::clipById(s, sourceId) : nullptr;
            if (!source) return fail("There is no clip to learn the room from");
            const QString sourceName = QString::fromStdString(source->name);  // placing the fill can move the track's clips
            RoomToneProfile prof;
            std::string err;
            if (!clipRoomTone(l.project, s, *source, prof, &err)) return fail(QString::fromStdString(err));
            const int64_t samples = int64_t(std::llround(double(end - from) / s.fpsValue() * prof.sampleRate));
            const QString folder = QFileInfo(absolute(need(a, "project"))).absolutePath() + QStringLiteral("/Room Tone");
            QDir().mkpath(folder);
            QString path;
            int n = 1;
            do path = folder + '/' + QString::fromStdString(s.name) + QStringLiteral(" Room Tone %1.wav").arg(n++);
            while (QFileInfo::exists(path));
            if (!writeStereoWav(path.toStdString(), synthesizeRoomTone(prof, samples, uint32_t(from + 1)), prof.sampleRate, &err))
                return fail(QString::fromStdString(err));
            const Id media = mediaFor(l.project, path);
            check(edit::placeMedia(l.project, s, media, from, 0, -1, {TrackKind::Video, 0}, track, false));
            Id made = 0;
            for (const Clip& c : trackAt(s, track)->clips)
                if (c.start == from && c.mediaId == media) made = c.id;
            save(l);
            return ok(QStringLiteral("Filled %1 to %2 on %3 with room tone learned from %4 (%5 dBFS)")
                          .arg(tc(from, s), tc(end, s), str(a, "track", "A1"), sourceName)
                          .arg(20 * std::log10(prof.rms), 0, 'f', 1),
                      QJsonObject{{"clip", double(made)}, {"path", path}, {"level_db", 20 * std::log10(prof.rms)},
                                  {"start", tc(from, s)}, {"end", tc(end, s)}});
        });

    add("montage_extend_clip", "Extend a clip past its end",
        "Carry a video clip on past the end of its media (a local counterpart to Premiere's Generative Extend): its last "
        "frame is held while the camera's motion over the shot's final second (pan, drift, push) carries on and settles, "
        "scaled so the frame stays filled, and the linked sound gets the room's own tone (a WAV in a Room Tone folder "
        "beside the project). By `seconds` (default 1), or `to` a timecode. With `ripple` (default true) what follows "
        "moves along; without it the space after the clip must be empty.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "seconds":{"type":"number","default":1},"to":{"type":["number","string"]},"ripple":{"type":"boolean","default":true},
            "sound":{"type":"boolean","default":true}},"required":["project","clip"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Id id = clipArg(l, a).id;
            const Clip* c = edit::clipById(s, id);
            const auto loc = edit::locate(s, id);
            if (!c || !loc || loc->track.kind != TrackKind::Video) throw ArgError{"\"clip\" is a video clip"};
            FrameTime frames = FrameTime(std::llround(a.value("seconds").toDouble(1) * s.fpsValue()));
            if (a.contains("to")) frames = timeArg(a.value("to"), s, "to") - c->end();
            if (frames <= 0 || frames > FrameTime(std::llround(10 * s.fpsValue()))) throw ArgError{"Extend by more than nothing and at most 10 seconds"};
            const Clip clip = *c;
            EndMotion motion;
            std::string err;
            if (!measureEndMotion(l.project, s, clip, motion, &err, [this](double f) { progress(f * 0.8, "Measuring"); })) return fail(QString::fromStdString(err));
            // The room under the linked sound.
            std::vector<std::pair<Id, Id>> fills;  // audio clip, room tone media
            if (a.value("sound").toBool(true)) {
                const QString folder = QFileInfo(absolute(need(a, "project"))).absolutePath() + QStringLiteral("/Room Tone");
                for (Id other : edit::linkedClips(s, id)) {
                    const auto ol = edit::locate(s, other);
                    if (!ol || ol->track.kind != TrackKind::Audio) continue;
                    RoomToneProfile prof;
                    if (!clipRoomTone(l.project, s, *edit::clipById(s, other), prof, nullptr)) continue;
                    QDir().mkpath(folder);
                    QString path;
                    int n = 1;
                    do path = folder + '/' + QString::fromStdString(s.name) + QStringLiteral(" Room Tone %1.wav").arg(n++);
                    while (QFileInfo::exists(path));
                    const int64_t samples = int64_t(std::llround(double(frames) / s.fpsValue() * prof.sampleRate));
                    if (!writeStereoWav(path.toStdString(), synthesizeRoomTone(prof, samples, uint32_t(other)), prof.sampleRate, &err))
                        return fail(QString::fromStdString(err));
                    fills.push_back({other, mediaFor(l.project, path)});
                }
            }
            Id video = 0;
            if (!extendClip(l.project, s, id, frames, motion, a.value("ripple").toBool(true), &video, &err)) return fail(QString::fromStdString(err));
            const Id group = l.project.newId();
            edit::clipById(s, video)->linkGroup = group;
            QJsonArray made{double(video)};
            for (const auto& [audio, media] : fills) {
                const auto al = edit::locate(s, audio);
                const Clip* ac = edit::clipById(s, audio);
                if (!al || !ac) continue;
                const FrameTime at = ac->end();
                const Track* tr = trackAt(s, al->track);
                if (std::any_of(tr->clips.begin(), tr->clips.end(), [&](const Clip& o) { return o.start < at + frames && o.end() > at; })) continue;
                check(edit::placeMedia(l.project, s, media, at, 0, frames, {TrackKind::Video, 0}, al->track, false));
                for (Clip& o : trackAt(s, al->track)->clips)
                    if (o.start == at && o.mediaId == media) o.linkGroup = group, made.append(double(o.id));
            }
            save(l);
            return ok(QStringLiteral("Extended \"%1\" by %2 (%3)")
                          .arg(QString::fromStdString(clip.name), tc(frames, s),
                               motion.samples ? QStringLiteral("carrying on %1, %2 px a frame").arg(motion.dx, 0, 'f', 1).arg(motion.dy, 0, 'f', 1)
                                              : QStringLiteral("held")),
                      QJsonObject{{"clips", made}, {"dx", motion.dx}, {"dy", motion.dy}, {"zoom", motion.zoom}, {"turn", motion.turn},
                                  {"frames", double(frames)}});
        });

    add("montage_key_screen", "Key out a green or blue screen",
        "Key a green or blue screen out of video clips: each gets the Keyer (Keylight-style: screen dominance with balance and "
        "gain, matte clip black and white, shrink or grow, soften, despill with brightness restored, edge desaturation) set to "
        "the screen colour read from its picture at `at` (default: the middle of each clip). Then montage_add_effect "
        "or montage_set_effect_param can tune it (type screen_key).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clips":{"type":"array","items":{"type":"number"}},
            "at":{"type":["number","string"]}},"required":["project","clips"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : -1;
            QJsonArray keyed;
            std::string err;
            for (const QJsonValue& v : a.value("clips").toArray()) {
                const Id id = Id(v.toDouble());
                if (keyScreen(l.project, s, id, at, &err)) {
                    const Clip* c = edit::clipById(s, id);
                    const auto it = std::find_if(c->effects.begin(), c->effects.end(), [](const Effect& e) { return e.type == "screen_key"; });
                    keyed.append(QJsonObject{{"clip", double(id)},
                                             {"screen", QJsonArray{it->p("key.r", 0), it->p("key.g", 0), it->p("key.b", 0)}}});
                }
            }
            if (keyed.isEmpty()) return fail(err.empty() ? QStringLiteral("No clips keyed") : QString::fromStdString(err));
            save(l);
            return ok(QStringLiteral("Keyed %1 clip(s)").arg(keyed.size()), QJsonObject{{"keyed", keyed}});
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
        "Move a clip's in or out point by a number of seconds (positive: later). Ripple moves later clips with it. "
        "With `extend_to` (a timeline time) instead, Extend Edit: the clip's edge nearest it moves there, rolling with "
        "the clip that meets it or trimming into a gap.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "edge":{"type":"string","enum":["in","out"]},"by":{"type":"number","description":"Seconds"},
            "ripple":{"type":"boolean","default":false},
            "extend_to":{"type":["number","string"],"description":"Seconds or timecode on the timeline"}},"required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Id id = clipArg(l, a).id;
            if (a.contains("extend_to")) {
                const auto r = edit::extendEdit(l.project, s, id, timeArg(a.value("extend_to"), s, "extend_to"));
                if (!r.ok) return fail(r.error.empty() ? QStringLiteral("The edge is already there") : QString::fromStdString(r.error));
                save(l);
                const Clip* c = edit::clipById(s, id);
                return ok(QStringLiteral("Extended by %1 frame(s)").arg(r.applied), c ? clipJson(l.project, s, *c) : QJsonObject{});
            }
            if (!a.contains("edge") || !a.contains("by")) throw ArgError{"Give edge and by, or extend_to"};
            const FrameTime by = FrameTime(std::llround(a.value("by").toDouble() * s.fpsValue()));
            const auto r = edit::trim(l.project, s, id, str(a, "edge") == "in" ? edit::Edge::In : edit::Edge::Out, by,
                                      a.value("ripple").toBool() ? edit::TrimMode::Ripple : edit::TrimMode::Normal);
            check(r);
            save(l);
            const Clip* c = edit::clipById(s, id);
            return ok(QStringLiteral("Trimmed by %1 frame(s)").arg(r.applied), c ? clipJson(l.project, s, *c) : QJsonObject{});
        });

    add("montage_grade_version", "Grade versions",
        "Several named grades on one picture clip (Resolve's local versions): `action` list, add (a copy of the current "
        "grade, or none with `empty`; named `name`), switch (to `index`, from 0), remove (`index`) or rename (`index`, "
        "`name`). A grade is the clip's colour effects (Color Correct, Curves, Hue Curves, Colour Warper, LUTs...); "
        "switching swaps them and leaves its other effects alone.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "action":{"type":"string","enum":["list","add","switch","remove","rename"]},"index":{"type":"integer"},
            "name":{"type":"string"},"empty":{"type":"boolean"}},"required":["project","clip","action"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Id id = clipArg(l, a).id;
            const QString action = need(a, "action");
            edit::Result r;
            if (action == "add") r = edit::addGradeVersion(l.project, l.seq(), id, a.value("name").toString().toStdString(), a.value("empty").toBool());
            else if (action == "switch") r = edit::switchGradeVersion(l.seq(), id, a.value("index").toInt(-1));
            else if (action == "remove") r = edit::removeGradeVersion(l.seq(), id, a.value("index").toInt(-1));
            else if (action == "rename") r = edit::renameGradeVersion(l.seq(), id, a.value("index").toInt(-1), a.value("name").toString().toStdString());
            else if (action != "list") throw ArgError{QStringLiteral("Unknown action \"%1\"").arg(action)};
            if (!r.ok) return fail(r.error.empty() ? QStringLiteral("Nothing to change") : QString::fromStdString(r.error));
            if (action != "list") save(l);
            const Clip* c = edit::clipById(l.seq(), id);
            QJsonArray list;
            QStringList lines;
            if (c->gradeVersions.empty()) list.append(QJsonObject{{"name", "Version 1"}, {"current", true}});
            for (size_t i = 0; i < c->gradeVersions.size(); ++i) {
                const bool cur = int(i) == c->gradeVersion;
                int effects = 0;
                if (cur) {
                    for (const Effect& e : c->effects) effects += isGradeEffect(e);
                } else {
                    effects = int(c->gradeVersions[i].effects.size());
                }
                list.append(QJsonObject{{"name", QString::fromStdString(c->gradeVersions[i].name)}, {"current", cur}, {"effects", effects}});
                lines << QStringLiteral("%1%2: %3 (%4 effects)").arg(cur ? "* " : "  ").arg(i).arg(QString::fromStdString(c->gradeVersions[i].name)).arg(effects);
            }
            return ok(lines.isEmpty() ? QStringLiteral("One grade (Version 1)") : lines.join('\n'), QJsonObject{{"versions", list}});
        });

    add("montage_animate_clip", "Animate a clip",
        "Give a picture clip (video, still, title, shape) an animation preset, as CapCut's In / Out / Combo: in plays "
        "over its first seconds, out over its last, combo repeats all through. Drawn on top of the clip's own transform, "
        "so it survives moves and trims. In and out: fade, slide_left (in from the right, out to the left), slide_right, "
        "slide_up, slide_down, zoom_in, zoom_out, pop, spin, drop, rise. Combo: wiggle, pulse, shake, float, swing, "
        "push_in. none removes one. *_seconds: how long (in, out) or each repeat (combo), 0.1-10 s.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "in":{"type":"string"},"out":{"type":"string"},"combo":{"type":"string"},
            "in_seconds":{"type":"number","default":0.5},"out_seconds":{"type":"number","default":0.5},
            "combo_seconds":{"type":"number","default":1}},
            "required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Id id = clipArg(l, a).id;
            int changed = 0;
            const struct {
                const char* key;
                const char* seconds;
                AnimationSlot slot;
                double def;
            } parts[] = {{"in", "in_seconds", AnimationSlot::In, 0.5}, {"out", "out_seconds", AnimationSlot::Out, 0.5},
                         {"combo", "combo_seconds", AnimationSlot::Combo, 1.0}};
            for (const auto& part : parts) {
                if (!a.contains(part.key)) continue;
                const edit::Result r = edit::setClipAnimation(l.seq(), id, part.slot, a.value(part.key).toString().toStdString(),
                                                              a.value(part.seconds).toDouble(part.def));
                if (!r.ok && !r.error.empty()) throw ArgError{QString::fromStdString(r.error)};
                changed += r.ok;
            }
            if (!changed) return fail("Nothing to change: give in, out or combo (or they are set already)");
            save(l);
            const Clip* c = edit::clipById(l.seq(), id);
            QJsonObject anim;
            for (const auto& [key, ca] : {std::pair{"in", c->animIn}, std::pair{"out", c->animOut}, std::pair{"combo", c->animLoop}})
                if (!ca.type.empty()) anim[key] = QJsonObject{{"type", QString::fromStdString(ca.type)}, {"seconds", ca.seconds}};
            return ok(QStringLiteral("Animated \"%1\"").arg(QString::fromStdString(c->name)), QJsonObject{{"animation", anim}});
        });

    add("montage_speed_ramp", "Speed ramp a clip",
        "Give a clip a speed ramp preset (CapCut's speed curves): its Time Remapping curve shaped and eased so it plays "
        "the same footage in the same length, paced differently; linked sound follows. Presets: montage (quick, slow, "
        "quick, slow, quick), hero (speeds up, lingers on the middle, speeds away), bullet (fast, almost stopped through "
        "the middle, fast), jump_cut (a burst of speed in the middle), flash_in, flash_out, slow_in (eases into slow "
        "motion), fast_out (out of it); none removes the ramp. For smooth slow parts set frames on montage_set_speed "
        "(optical_flow or ai); maintain_pitch keeps the sound at its pitch through the ramp.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "preset":{"type":"string","enum":["montage","hero","bullet","jump_cut","flash_in","flash_out","slow_in","fast_out","none"]},
            "maintain_pitch":{"type":"boolean"}},
            "required":["project","clip","preset"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Clip& c = clipArg(l, a);
            const QString preset = need(a, "preset");
            check(edit::applySpeedRamp(l.project, l.seq(), c.id, preset.toStdString()));
            if (a.contains("maintain_pitch")) edit::setMaintainPitch(l.project, l.seq(), c.id, a.value("maintain_pitch").toBool());
            save(l);
            const Clip* after = edit::clipById(l.seq(), c.id);
            QJsonArray speeds;
            for (int i = 0; i <= 4; ++i)
                speeds.append(std::round(after->speedAt(double(after->duration - 1) * i / 4) * 1000) / 1000);
            return ok(preset == "none" ? QStringLiteral("Removed the ramp") : QStringLiteral("Ramped \"%1\" (%2)").arg(QString::fromStdString(c.name), preset),
                      QJsonObject{{"speeds", speeds}});
        });

    add("montage_set_speed", "Set clip speed",
        "Change a clip's playback speed (1 = normal, 0.5 = half speed, 2 = double; negative plays backwards). "
        "Its length changes to match, and later clips ripple. frames picks how slow motion makes the frames between "
        "source frames: nearest (repeat), blend, optical_flow, or ai (RIFE, needs its model). maintain_pitch keeps the "
        "sound (the clip's and its linked sound's) at its pitch instead of rising or falling with the speed.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},"speed":{"type":"number"},
            "frames":{"type":"string","enum":["nearest","blend","optical_flow","ai"]},"maintain_pitch":{"type":"boolean"}},
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
            if (a.contains("maintain_pitch")) edit::setMaintainPitch(l.project, l.seq(), id, a.value("maintain_pitch").toBool());
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
            "template":{"type":"string","enum":["plain","lower_third","lower_third_box","centred","chapter","callout","typewriter","cascade","pop_words","drop","wave","decode","end_card","credits","crawl"],"default":"plain"},
            "text_animation":{"type":"string","enum":["none","rise","fade","pop","drop","wave","scramble"],"description":"The text coming on a letter, word or line at a time"},
            "animate_by":{"type":"string","enum":["letter","word","line"],"default":"letter"},
            "animation_seconds":{"type":"number","default":1,"description":"How long the text animation takes across the whole text"},
            "animate_out":{"type":"boolean","default":false,"description":"The text animation also plays out at the end"},
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
            if (a.contains("text_animation")) {
                const QStringList kinds{"none", "rise", "fade", "pop", "drop", "wave", "scramble"};
                const int k = int(kinds.indexOf(str(a, "text_animation")));
                if (k < 0) throw ArgError{"\"text_animation\" is none, rise, fade, pop, drop, wave or scramble"};
                c.generator.params["text_anim"] = Param(double(k));
                const QStringList units{"letter", "word", "line"};
                const int by = int(units.indexOf(str(a, "animate_by", "letter")));
                if (by < 0) throw ArgError{"\"animate_by\" is letter, word or line"};
                c.generator.params["text_anim_by"] = Param(double(by));
                if (a.value("animation_seconds").isDouble()) c.generator.params["text_anim_dur"] = Param(std::clamp(a.value("animation_seconds").toDouble(), 0.1, 10.0));
                c.generator.params["text_anim_out"] = Param(a.value("animate_out").toBool() ? 1.0 : 0.0);
            }
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
            // OpenFX video plugins installed on this computer.
            if (kind == "video") {
                Project scratch = makeDefaultProject();
                for (const ofx::PluginDesc& d : openFxPlugins()) {
                    const Effect e = ofx::makeEffect(scratch, d);
                    QJsonArray params;
                    for (const ParamInfo& pi : effectParams(e)) {
                        QJsonObject po{{"name", QString::fromStdString(pi.name)}, {"label", QString::fromStdString(pi.label)},
                                       {"min", pi.min}, {"max", pi.max}, {"default", pi.def}};
                        if (!pi.choices.empty()) {
                            QJsonArray ch;
                            for (const auto& c : pi.choices) ch.append(QString::fromStdString(c));
                            po["choices"] = ch;
                        }
                        if (pi.kind == ParamKind::Color) po["channels"] = QJsonArray{QString::fromStdString(pi.name + ".r"), QString::fromStdString(pi.name + ".g"),
                                                                                      QString::fromStdString(pi.name + ".b")};
                        params.append(po);
                    }
                    list.append(QJsonObject{{"type", QString::fromStdString(ofx::kTypePrefix + d.id)}, {"name", QString::fromStdString(d.label)},
                                            {"group", QString::fromStdString("OpenFX" + (d.group.empty() ? std::string() : "/" + d.group))},
                                            {"params", params}});
                }
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
            "strings":{"type":"object","additionalProperties":{"type":"string"},"description":"Text settings: curves (\"x,y x,y\"), hue curves, a LUT file, the Colour Warper's mesh (\"spoke,ring,hue,sat,luma;...\": spokes 0-11 every 30 degrees from red, rings 1-4 for saturation 25-100 %, hue moved in degrees, saturation in 0-1 units, brightness in stops)..."},
            "mask_path":{"type":"array","items":{"type":["array","object"]},"description":"A closed Bezier mask: three or more points, fractions of the clip's frame"},
            "mask_smooth":{"type":"boolean","default":false},
            "group_stage":{"type":"string","enum":["pre","post"],"description":"Add it to the clip's colour group instead: its pre-clip grade (before each member's own effects) or post-clip grade (after them)"}},
            "required":["project","clip","effect"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Clip& c = clipArg(l, a);
            std::string type = need(a, "effect").toStdString();
            // An OpenFX video plugin ("ofx:<id>", see montage_list_effects) is an "ofx" effect with the plugin's parameters.
            std::optional<Effect> plugin;
            if (ofx::isOfxType(type)) {
                ofx::PluginDesc d;
                if (!openFxPlugin(type.substr(std::char_traits<char>::length(ofx::kTypePrefix)), d))
                    throw ArgError{QStringLiteral("No OpenFX plugin \"%1\" is installed (see montage_list_effects)").arg(QString::fromStdString(type))};
                plugin = ofx::makeEffect(l.project, d);
                type = "ofx";
            }
            const EffectInfo* info = findEffectInfo(type);
            if (!info || (info->hidden && !plugin) || (info->category != EffectCategory::VideoFilter && info->category != EffectCategory::AudioFilter))
                throw ArgError{QStringLiteral("Unknown effect \"%1\" (see montage_list_effects)").arg(QString::fromStdString(type))};
            const std::string displayName = plugin ? ofx::effectName(*plugin) : info->displayName;
            if (type == "enhance_speech" && (!speechEnhancerAvailable() || !speechModel().installed()))
                return fail("Enhance Speech needs its model: run `scripts/fetch-models.sh` or add the effect once in the app");
            if (type == "super_scale" && (!upscalerAvailable() || !upscaleModel().installed()))
                return fail("Super Scale needs its model: run `scripts/fetch-models.sh` or add the effect once in the app");
            Effect e = plugin ? *plugin : makeEffect(l.project, type);
            const QJsonObject params = a.value("params").toObject();
            for (auto it = params.begin(); it != params.end(); ++it) {
                std::string name = it.key().toStdString();
                if (plugin && name.rfind("param.", 0) != 0 && name.rfind("mask.", 0) != 0) name = "param." + name;  // "amount" for "param.amount"
                const bool known = (plugin ? e.params.count(name) > 0
                                           : std::any_of(info->params.begin(), info->params.end(), [&](const ParamInfo& p) { return p.name == name; })) ||
                                   (name.rfind("mask.", 0) == 0 && supportsMask(type));
                if (!known) throw ArgError{QStringLiteral("\"%1\" has no parameter \"%2\"").arg(QString::fromStdString(type), it.key())};
                e.params[name] = Param(it.value().toDouble());
            }
            const QJsonObject strings = a.value("strings").toObject();
            for (auto it = strings.begin(); it != strings.end(); ++it) {
                const std::string name = it.key().toStdString();
                if (plugin) {
                    const std::string key = name.rfind("str.", 0) == 0 ? name : "str." + name;
                    if (!e.strings.count(key))
                        throw ArgError{QStringLiteral("\"%1\" has no text setting \"%2\"").arg(QString::fromStdString(displayName), it.key())};
                    e.strings[key] = it.value().toString().toStdString();
                    continue;
                }
                const auto si = std::find_if(info->strings.begin(), info->strings.end(), [&](const StringParamInfo& x) { return x.name == name; });
                if (si == info->strings.end())
                    throw ArgError{QStringLiteral("\"%1\" has no text setting \"%2\"").arg(QString::fromStdString(type), it.key())};
                const std::string value = it.value().toString().toStdString();
                if (si->kind == StringKind::ColorWarp) {
                    ColorWarp w;
                    if (!parseColorWarp(value, w)) throw ArgError{QStringLiteral("The mesh is \"spoke,ring,hue,sat,luma\" groups separated by ';'")};
                }
                e.strings[name] = value;
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
                return fail("Face Refinement, Blemish Remover and Redact Faces need the face models: run `scripts/fetch-models.sh` or add them once in the app");
            if ((needsPersonMatte(e, 0) || type == "behind_people") && (!mattingAvailable() || !mattingModel().installed()))
                return fail("Remove Background, Behind People and People masks need their model: run `scripts/fetch-models.sh` or add one once in the app");
            if (needsDepth(e, 0) && (!depthAvailable() || !depthModel().installed()))
                return fail("Depth effects need the depth model: run `scripts/fetch-models.sh` or add one once in the app");
            const auto loc = edit::locate(l.seq(), c.id);
            const bool audioClip = loc && loc->track.kind == TrackKind::Audio;
            if (audioClip != (info->category == EffectCategory::AudioFilter))
                throw ArgError{audioClip ? QStringLiteral("That is an audio clip: choose an audio effect")
                                         : QStringLiteral("That is a video clip: choose a video effect")};
            if (a.contains("group_stage")) {
                const QString stage = str(a, "group_stage");
                ColorGroup* g = findColorGroup(l.seq(), c.colorGroup);
                if (!g) throw ArgError{"That clip is in no colour group (see montage_color_group)"};
                if (stage != "pre" && stage != "post") throw ArgError{"\"group_stage\" is pre or post"};
                if (audioClip || type == "stabilize" || type == "rolling_shutter")
                    throw ArgError{"A colour group takes picture effects that work the same on each clip"};
                (stage == "pre" ? g->pre : g->post).push_back(e);
                save(l);
                return ok(QStringLiteral("Added %1 to the %2-clip grade of %3").arg(QString::fromStdString(displayName), stage, QString::fromStdString(g->name)),
                          QJsonObject{{"effect_id", double(e.id)}});
            }
            if (type == "stabilize" || type == "rolling_shutter") {
                // They work from the camera's movement, measured now, and move the whole frame (so they go first).
                std::string motion, err;
                if (!analyzeClipStabilization(l.project, l.seq(), c, motion, {}, nullptr, &err))
                    return fail(QStringLiteral("Could not measure the camera's movement: %1").arg(QString::fromStdString(err)));
                e.strings["motion"] = motion;
                c.effects.insert(c.effects.begin(), e);
                save(l);
                return ok(QStringLiteral("Added %1 to %2").arg(QString::fromStdString(displayName), QString::fromStdString(c.name)),
                          QJsonObject{{"effect_id", double(e.id)}});
            }
            c.effects.push_back(e);
            save(l);
            return ok(QStringLiteral("Added %1 to %2").arg(QString::fromStdString(displayName), QString::fromStdString(c.name)),
                      QJsonObject{{"effect_id", double(e.id)}});
        });

    add("montage_add_transition", "Add a transition",
        "Add a transition at a clip's start or end (cross_dissolve by default, centred on the cut). With `clips` "
        "instead (Premiere's Apply Default Transitions to Selection), one at both ends of every clip listed: each edit "
        "point once, a fade where a clip meets a gap; `type` on picture clips and `audio_type` (crossfade) on sound.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "clips":{"type":"array","items":{"type":"number"}},
            "edge":{"type":"string","enum":["in","out"],"default":"in"},"type":{"type":"string","default":"cross_dissolve"},
            "audio_type":{"type":"string","default":"crossfade"},
            "duration":{"type":["number","string"],"default":1}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const FrameTime len = a.contains("duration") ? timeArg(a.value("duration"), s, "duration") : FrameTime(std::llround(s.fpsValue()));
            if (a.contains("clips")) {
                std::vector<Id> ids;
                for (const QJsonValue& v : a.value("clips").toArray()) ids.push_back(Id(v.toDouble()));
                const edit::Result r = edit::addTransitionsToClips(l.project, s, ids, str(a, "type", "cross_dissolve").toStdString(),
                                                                   str(a, "audio_type", "crossfade").toStdString(), std::max<FrameTime>(1, len));
                check(r);
                save(l);
                QJsonArray made;
                for (Id t : r.created) made.append(double(t));
                return ok(QStringLiteral("Added %1 transitions").arg(r.created.size()), QJsonObject{{"transitions", made}});
            }
            const Id id = clipArg(l, a).id;
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

    add("montage_suggest_chapters", "Suggest chapters from what is said",
        "Find where the talk in the sequence moves on to something new (TextTiling over the transcribed words of the "
        "cut: the sentences either side of each break compared, the breaks where they share least taken, none closer than "
        "`min_seconds`) and title each chapter with the phrase it says more than the others do. With `apply` (the "
        "default) they become chapter markers, replacing the chapter markers there were unless `replace` is false. "
        "Returns the chapters and YouTube's chapter list.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"min_seconds":{"type":"number","default":30},
            "max_chapters":{"type":"number","default":0,"description":"0 = as many as the talk has"},
            "apply":{"type":"boolean","default":true},"replace":{"type":"boolean","default":true}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            ChapterOptions o;
            o.minSeconds = std::max(10.0, a.value("min_seconds").toDouble(30));
            o.maxChapters = std::max(0, a.value("max_chapters").toInt(0));
            std::string err;
            const std::vector<SuggestedChapter> chapters = suggestChapters(l.project, s, o, &err);
            if (chapters.empty()) return fail(QString::fromStdString(err));
            QJsonArray list;
            for (const SuggestedChapter& c : chapters)
                list.append(QJsonObject{{"start", c.start / s.fpsValue()}, {"timecode", tc(c.start, s)}, {"title", QString::fromStdString(c.title)}});
            QJsonObject out{{"chapters", list}};
            if (!a.value("apply").toBool(true)) return ok(QStringLiteral("%1 chapter(s) suggested").arg(chapters.size()), out);
            const edit::Result r = edit::addSuggestedChapters(s, chapters, a.value("replace").toBool(true));
            if (!r.ok) return fail(QString::fromStdString(r.error));
            save(l);
            std::string warning;
            const QString youtube = QString::fromStdString(youtubeChapters(s, 0, -1, &warning));
            out["youtube"] = youtube;
            if (!warning.empty()) out["warning"] = QString::fromStdString(warning);
            return ok(QStringLiteral("%1 chapter marker(s) added:\n%2").arg(chapters.size()).arg(youtube), out);
        });

    add("montage_measure_hdr", "Measure HDR light levels",
        "Measure an HDR (PQ or HLG) sequence's light levels as HDR10 states them: MaxCLL, the brightest channel of the "
        "brightest pixel in any frame, and MaxFALL, the highest frame-average light, in nits (CTA-861.3), each with where "
        "it happens, over `from` to `to` (default the whole sequence). With `save` (the default) they are kept with the "
        "sequence so exports that state them before the first frame (x265, Matroska) use them; MP4 and MOV exports "
        "measure what they render anyway. Warns when MaxCLL is above the sequence's mastering peak.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"from":{"type":["number","string"]},
            "to":{"type":["number","string"]},"save":{"type":"boolean","default":true}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            if (!sequenceColorSpace(s).hdr())
                return fail(QStringLiteral("Light levels are measured on HDR sequences; this one is %1").arg(QString::fromStdString(sequenceColorSpace(s).label)));
            const FrameTime from = a.contains("from") ? timeArg(a.value("from"), s, "from") : 0;
            const FrameTime to = a.contains("to") ? timeArg(a.value("to"), s, "to") : 0;
            LightLevels light;
            std::string err;
            if (!measureLightLevels(l.project, s, from, to, light, &err)) return fail(QString::fromStdString(err));
            unsigned cll = 0, fall = 0;
            hdr10LightLevels(light, cll, fall);
            QJsonObject out{{"max_cll", int(cll)}, {"max_fall", int(fall)}, {"max_cll_at", tc(light.maxCllFrame, s)},
                            {"max_fall_at", tc(light.maxFallFrame, s)}, {"frames", double(light.frames)}};
            QString text = QStringLiteral("MaxCLL %1 nits (at %2), MaxFALL %3 nits (at %4)")
                               .arg(cll).arg(tc(light.maxCllFrame, s)).arg(fall).arg(tc(light.maxFallFrame, s));
            if (sequenceColorSpace(s).transfer == Transfer::Pq && cll > s.hdrPeakNits + 0.5) {
                out["warning"] = QStringLiteral("Brighter than the %1-nit mastering peak").arg(s.hdrPeakNits);
                text += QStringLiteral("; brighter than the %1-nit mastering peak").arg(s.hdrPeakNits);
            }
            if (a.value("save").toBool(true)) {
                s.hdrMaxCll = cll, s.hdrMaxFall = fall;
                save(l);
            }
            return ok(text, out);
        });

    add("montage_color_group", "Colour groups",
        "Grade shots together, as Resolve's groups do: a colour group's pre-clip grade runs on each member before the "
        "clip's own effects (to match the shots) and its post-clip grade after them (the group's look). `action`: list "
        "(the groups, their members and grades), create (a group of `clips`, named `name`), add (`clips` join `group`), "
        "remove (`clips` leave their groups), rename (`group` to `name`) or delete (`group`; its clips keep their own "
        "grades). Add grades with montage_add_effect and `group_stage`.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"action":{"type":"string","enum":["list","create","add","remove","rename","delete"],"default":"list"},
            "clips":{"type":"array","items":{"type":"number"}},"group":{"type":["number","string"],"description":"Its id or name"},
            "name":{"type":"string"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const QString action = str(a, "action", "list");
            std::vector<Id> clips;
            for (const QJsonValue& v : a.value("clips").toArray()) clips.push_back(Id(v.toDouble()));
            auto groupArg = [&]() -> Id {
                const QJsonValue v = a.value("group");
                for (const ColorGroup& g : s.colorGroups)
                    if ((v.isDouble() && g.id == Id(v.toDouble())) || (v.isString() && QString::fromStdString(g.name).compare(v.toString(), Qt::CaseInsensitive) == 0))
                        return g.id;
                throw ArgError{"No such colour group (see action list)"};
            };
            if (action == "create") {
                Id created = 0;
                check(edit::makeColorGroup(l.project, s, clips, str(a, "name").toStdString(), &created));
                save(l);
                return ok(QStringLiteral("Made the colour group %1").arg(QString::fromStdString(findColorGroup(s, created)->name)),
                          QJsonObject{{"group", double(created)}});
            }
            if (action == "add") check(edit::addToColorGroup(s, clips, groupArg()));
            else if (action == "remove") check(edit::removeFromColorGroup(s, clips));
            else if (action == "rename") {
                const Id g = groupArg();
                const std::string name = need(a, "name").toStdString();
                const edit::Result r = edit::renameColorGroup(s, g, name);
                if (!r.ok && !r.error.empty()) return fail(QString::fromStdString(r.error));
            } else if (action == "delete") check(edit::deleteColorGroup(s, groupArg()));
            else if (action != "list") throw ArgError{"\"action\" is list, create, add, remove, rename or delete"};
            if (action != "list") save(l);
            QJsonArray groups;
            QString text = s.colorGroups.empty() ? QStringLiteral("No colour groups.") : QString();
            for (const ColorGroup& g : s.colorGroups) {
                QJsonArray members, pre, post;
                for (Id id : colorGroupMembers(s, g.id)) members.append(double(id));
                for (const Effect& e : g.pre) pre.append(QJsonObject{{"id", double(e.id)}, {"type", QString::fromStdString(e.type)}});
                for (const Effect& e : g.post) post.append(QJsonObject{{"id", double(e.id)}, {"type", QString::fromStdString(e.type)}});
                groups.append(QJsonObject{{"id", double(g.id)}, {"name", QString::fromStdString(g.name)}, {"clips", members}, {"pre", pre}, {"post", post}});
                text += QStringLiteral("%1 (%2): %3 clip(s), %4 pre-clip and %5 post-clip effect(s)\n")
                            .arg(QString::fromStdString(g.name)).arg(g.id).arg(members.size()).arg(pre.size()).arg(post.size());
            }
            return ok(text.trimmed(), QJsonObject{{"groups", groups}});
        });

    add("montage_spell_check", "Check spelling",
        "Check the spelling of the active sequence's captions (each track in its language) and titles (`language`, "
        "default en-US), or of `text` alone, in English (US or UK; SCOWL's word lists). The project's vocabulary is "
        "always right: `learn` adds words to it (Add to Dictionary) and `forget` takes them out, before checking. "
        "Returns each misspelt word with where it is and suggestions, best first.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"text":{"type":"string"},
            "language":{"type":"string","default":"en-US","description":"For titles and text: en, en-US, en-GB..."},
            "scope":{"type":"string","enum":["all","captions","titles"],"default":"all"},
            "learn":{"type":"array","items":{"type":"string"}},"forget":{"type":"array","items":{"type":"string"}}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Sequence& s = l.seq();
            int changed = 0;
            for (const QJsonValue& v : a.value("learn").toArray()) changed += learnWord(l.project, v.toString().toStdString());
            for (const QJsonValue& v : a.value("forget").toArray()) changed += forgetWord(l.project, v.toString().toStdString());
            if (changed) save(l);
            const QString language = str(a, "language", "en-US");
            const SpellChecker* main = SpellChecker::forLanguage(language.toStdString());
            QJsonArray list;
            auto report = [&](const std::vector<Misspelling>& bad, QJsonObject where) {
                for (const Misspelling& m : bad) {
                    QJsonArray sug;
                    for (const std::string& x : m.suggestions) sug.append(QString::fromStdString(x));
                    QJsonObject o = where;
                    o["word"] = QString::fromStdString(m.word);
                    o["suggestions"] = sug;
                    list.append(o);
                }
            };
            if (a.contains("text")) {
                if (!main) return fail(QStringLiteral("No dictionary for \"%1\" (English, US or UK)").arg(language));
                report(main->check(str(a, "text"), l.project.vocabulary, true), QJsonObject{});
            } else {
                const QString scope = str(a, "scope", "all");
                if (scope != "all" && scope != "captions" && scope != "titles") throw ArgError{"\"scope\" is all, captions or titles"};
                if (scope != "titles")
                    for (const CaptionTrack& t : s.captionTracks) {
                        const SpellChecker* sc = SpellChecker::forLanguage(t.language);
                        if (!sc) continue;
                        for (size_t i = 0; i < t.captions.size(); ++i)
                            report(sc->check(t.captions[i].text, l.project.vocabulary, true),
                                   QJsonObject{{"track", QString::fromStdString(t.name)}, {"caption", int(i + 1)}, {"at", tc(t.captions[i].start, s)}});
                    }
                if (scope != "captions" && main)
                    for (const Track& t : s.videoTracks)
                        for (const Clip& c : t.clips) {
                            const auto it = c.generator.strings.find("text");
                            if (c.generator.type.rfind("title", 0) != 0 || it == c.generator.strings.end()) continue;
                            report(main->check(it->second, l.project.vocabulary, true),
                                   QJsonObject{{"clip", double(c.id)}, {"title", QString::fromStdString(c.name)}, {"at", tc(c.start, s)}});
                        }
            }
            QString text = list.isEmpty() ? QStringLiteral("No spelling mistakes found.") : QStringLiteral("%1 misspelt word(s):").arg(list.size());
            for (const QJsonValue& v : list) {
                const QJsonObject o = v.toObject();
                QStringList sug;
                for (const QJsonValue& x : o.value("suggestions").toArray()) sug << x.toString();
                text += QStringLiteral("\n%1%2 (%3)").arg(o.contains("at") ? o.value("at").toString() + "  " : QString(), o.value("word").toString(),
                                                         sug.isEmpty() ? QStringLiteral("no suggestions") : sug.join(", "));
            }
            return ok(text, QJsonObject{{"misspellings", list}, {"vocabulary", int(l.project.vocabulary.size())}});
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
        "levels outside EBU R103, black or frozen picture, silence, clipping, loudness against a target, and spelling in "
        "captions (each track's language) and titles (`title_language`, default en-US), the project's vocabulary allowed. "
        "Lists each problem with its timecodes; with markers, puts a red \"QC:\" marker on each (replacing earlier ones).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"from":{"type":["number","string"]},
            "to":{"type":["number","string"]},"flashing":{"type":"boolean","default":true},"levels":{"type":"boolean","default":true},
            "black_seconds":{"type":"number","default":1,"description":"0 = not checked"},
            "freeze_seconds":{"type":"number","default":5,"description":"0 = not checked"},
            "silence_seconds":{"type":"number","default":2,"description":"0 = not checked"},
            "clipping":{"type":"boolean","default":true},
            "loudness_target":{"type":"number","description":"LUFS, e.g. -14 (streaming) or -23 (EBU R128); omitted = not checked"},
            "peak_ceiling":{"type":"number","default":-1,"description":"dBTP, checked with the loudness"},
            "spelling":{"type":"boolean","default":true},"title_language":{"type":"string","default":"en-US"},
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
            q.spelling = a.value("spelling").toBool(true);
            q.titleLanguage = str(a, "title_language", "en-US").toStdString();
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
        "type, colour), as review tools and Premiere write them, Avid locator lines, or a notes file saved from a review "
        "page (montage_export_review; one marker a note, named and coloured by reviewer). Give the file's `path` or its `text`.",
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
            "language":{"type":"string","default":"auto"},"speakers":{"type":"boolean","default":false},
            "vocabulary":{"type":"array","items":{"type":"string"},"description":"Names and terms to expect; default the project's"}},"required":["media"]})json",
        false, [this](const QJsonObject& a) {
            TranscribeOptions o;
            o.model = str(a, "model", "base.en").toStdString();
            o.language = str(a, "language", "auto").toStdString();
            o.speakers = a.value("speakers").toBool();
            for (const QJsonValue& v : a.value("vocabulary").toArray()) o.vocabulary.push_back(v.toString().toStdString());
            if (o.vocabulary.empty() && a.contains("project")) {
                Project vp;
                if (loadProject(absolute(str(a, "project")).toStdString(), vp)) o.vocabulary = vp.vocabulary;
            }
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

    add("montage_read_text", "Read text in the picture",
        "Read the text in pictures (OCR, PP-OCR, runs locally; downloaded on first use in the app or by "
        "scripts/fetch-models.sh). `action`: frame (the lines of text in the sequence's frame at `at`, or in `media` at "
        "`seconds`, each with its box as fractions of the picture), subtitles (the burned-in subtitles of video clip "
        "`clip` read into a new caption track where the clip plays them; `where` bottom, top or whole) or slate (the "
        "scene, shot and take on a slate in the first seconds of each of `media`, logged in their metadata). "
        "`language`: en, or another language in Latin script (fr, de, es...).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"action":{"type":"string","enum":["frame","subtitles","slate"],"default":"frame"},
            "at":{"type":["number","string"]},"media":{"type":["string","array"]},"seconds":{"type":"number","default":0},
            "clip":{"type":"number"},"where":{"type":"string","enum":["bottom","top","whole"],"default":"bottom"},
            "language":{"type":"string","default":"en"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            if (!ocrAvailable()) return fail("This build of Montage cannot read text in pictures (no ONNX Runtime)");
            if (!ocrModel().installed()) return fail("Reading text needs its model: run `scripts/fetch-models.sh` or read text once in the app");
            const QString action = str(a, "action", "frame");
            const std::string language = str(a, "language", "en").toStdString();
            std::string err;
            if (action == "frame") {
                auto reader = TextReader::load(language, &err);
                if (!reader) return fail(QString::fromStdString(err));
                std::vector<TextLine> lines;
                if (a.value("media").isString()) {
                    const MediaItem& m = projectMedia(l.project, str(a, "media"));
                    if (m.kind != MediaKind::Video && m.kind != MediaKind::Image) throw ArgError{"That media has no picture"};
                    VideoDecoder dec;
                    if (!dec.open(m.path, &err)) return fail(QString::fromStdString(err));
                    Frame16Ptr f = dec.frameAt(a.value("seconds").toDouble(0));
                    if (!f) return fail("No picture there");
                    lines = reader->read(*f, {}, &err);
                } else {
                    const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : s.playhead;
                    RenderOptions ro;
                    ro.displaySpace = "rec709";
                    const Image img = renderProgramFrame(l.project, s, at, ro);
                    QImage q(img.width, img.height, QImage::Format_RGBA8888);
                    toRgba8(img, q.bits(), size_t(q.bytesPerLine()));
                    lines = reader->read(q, {}, &err);
                }
                if (lines.empty() && !err.empty()) return fail(QString::fromStdString(err));
                QJsonArray list;
                for (const TextLine& t : lines)
                    list.append(QJsonObject{{"text", QString::fromStdString(t.text)}, {"confidence", double(t.confidence)},
                                            {"box", QJsonArray{t.x0, t.y0, t.x1, t.y1}}});
                return ok(lines.empty() ? QStringLiteral("No text there") : QString::fromStdString(textOf(lines, 0)), QJsonObject{{"lines", list}});
            }
            if (action == "subtitles") {
                Clip& c = clipArg(l, a);
                const MediaItem* m = l.project.findMedia(c.mediaId);
                if (!m || m->kind != MediaKind::Video) throw ArgError{"That is not a video clip"};
                const QString where = str(a, "where", "bottom");
                if (where != "bottom" && where != "top" && where != "whole") throw ArgError{"\"where\" is bottom, top or whole"};
                const TextRegion region = where == "top" ? TextRegion{0, 0, 1, 0.4} : where == "whole" ? TextRegion{} : TextRegion{0, 0.6, 1, 1};
                const double fps = s.fpsValue();
                const double from = std::min(c.sourceAt(0), c.sourceAt(double(c.duration))) / fps;
                const double to = std::max(c.sourceAt(0), c.sourceAt(double(c.duration))) / fps + 1 / fps;
                std::vector<Caption> found;
                if (!readBurnedInSubtitles(m->path, from, to, region, s.fps, found, language, 4, {}, nullptr, &err))
                    return fail(QString::fromStdString(err));
                const std::vector<Caption> placed = captionsThroughClip(c, found);
                if (placed.empty()) return ok("No subtitles were read in the clip's picture");
                CaptionTrack t;
                t.id = l.project.newId();
                t.name = "Burned-In - " + c.name;
                t.language = language;
                t.captions = placed;
                s.captionTracks.push_back(t);
                save(l);
                QJsonArray list;
                QString text = QStringLiteral("%1 subtitle(s) read into the caption track \"%2\":").arg(placed.size()).arg(QString::fromStdString(t.name));
                for (const Caption& cap : placed) {
                    list.append(QJsonObject{{"start", tc(cap.start, s)}, {"end", tc(cap.end, s)}, {"text", QString::fromStdString(cap.text)}});
                    text += QStringLiteral("\n%1  %2").arg(tc(cap.start, s), QString::fromStdString(cap.text));
                }
                return ok(text, QJsonObject{{"track", double(t.id)}, {"captions", list}});
            }
            if (action == "slate") {
                std::vector<MediaItem*> items;
                const QJsonValue mv = a.value("media");
                if (mv.isString()) items.push_back(&projectMedia(l.project, mv.toString()));
                for (const QJsonValue& v : mv.toArray()) items.push_back(&projectMedia(l.project, v.toString()));
                if (items.empty()) throw ArgError{"\"media\" is required"};
                QJsonArray list;
                int logged = 0;
                for (MediaItem* m : items) {
                    SlateInfo slate;
                    std::string why;
                    const bool found = m->kind == MediaKind::Video && readSlateFromPicture(m->path, slate, &why);
                    if (found) {
                        if (!slate.scene.empty()) setMediaField(*m, "scene", slate.scene);
                        if (!slate.shot.empty()) setMediaField(*m, "shot", slate.shot);
                        if (!slate.take.empty()) setMediaField(*m, "take", slate.take);
                        ++logged;
                    }
                    list.append(QJsonObject{{"media", QString::fromStdString(m->name)}, {"found", found}, {"scene", QString::fromStdString(slate.scene)},
                                            {"shot", QString::fromStdString(slate.shot)}, {"take", QString::fromStdString(slate.take)}});
                }
                if (logged) save(l);
                return ok(QStringLiteral("Logged %1 of %2 from their slates").arg(logged).arg(items.size()), QJsonObject{{"slates", list}});
            }
            throw ArgError{"\"action\" is frame, subtitles or slate"};
        });

    add("montage_search_speech", "Search what is said by meaning",
        "Find the moments of the project's transcripts that talk about something, by meaning rather than exact words "
        "(\"where they talk about money\" finds \"the budget was too tight\"): passages of each transcript and the "
        "query are compared by a sentence model (multi-qa-MiniLM, runs locally; downloaded on first use in the app or "
        "by scripts/fetch-models.sh). Returns up to `max` moments, best first, with the media, its time range and the "
        "words; `media` limits it to some media items (names or ids). For exact words use montage_find_phrase.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"query":{"type":"string"},
            "max":{"type":"integer","default":10},"media":{"type":"array","items":{"type":["string","number"]}}},
            "required":["project","query"]})json",
        true, [](const QJsonObject& a) {
            Loaded l = open(a);
            if (!speechSearchAvailable()) return fail("This build of Montage cannot search speech by meaning (no ONNX Runtime)");
            if (!sentenceModel().installed())
                return fail("Speech search needs its model: run `scripts/fetch-models.sh` or search What's Said once in the app");
            SpokenSearchOptions o;
            o.max = size_t(std::clamp(a.value("max").toInt(10), 1, 100));
            for (const QJsonValue& v : a.value("media").toArray()) {
                const MediaItem* byId = v.isDouble() ? l.project.findMedia(Id(v.toDouble())) : nullptr;
                o.media.push_back(byId ? byId->id : projectMedia(l.project, v.toString()).id);
            }
            std::string err;
            const std::vector<SpokenHit> hits = searchSpoken(l.project, need(a, "query").toStdString(), o, &err);
            if (hits.empty() && !err.empty()) return fail(QString::fromStdString(err));
            const Sequence& s = l.seq();
            QJsonArray list;
            QString text = hits.empty() ? QStringLiteral("Nothing said about that") : QString();
            for (const SpokenHit& h : hits) {
                const MediaItem* m = l.project.findMedia(h.media);
                const QString name = m ? QString::fromStdString(m->name) : QString();
                const FrameTime f0 = FrameTime(std::floor(h.start * s.fpsValue())), f1 = FrameTime(std::ceil(h.end * s.fpsValue()));
                list.append(QJsonObject{{"media", name}, {"media_id", double(h.media)}, {"start_seconds", h.start}, {"end_seconds", h.end},
                                        {"start", tc(f0, s)}, {"end", tc(f1, s)}, {"text", QString::fromStdString(h.text)}, {"score", double(h.score)}});
                text += QStringLiteral("%1 %2-%3: %4\n").arg(name, tc(f0, s), tc(f1, s), QString::fromStdString(h.text));
            }
            return ok(text.trimmed(), QJsonObject{{"hits", list}});
        });

    add("montage_edit_transcript", "Correct transcripts",
        "Fix what speech-to-text got wrong, keeping each word's timing and what was heard. `action`: correct (words "
        "`first` to `last` of `media`'s transcript, counted from 0 as montage_find_phrase reports, become `text`), "
        "revert (the correction at word `first` back to what was heard), revert_all (on `media`), replace (every "
        "whole-word `find` in every transcript of the project becomes `replace`), vocabulary (set the project's "
        "names and terms, `terms`, used by montage_transcribe and here), suggest (the near misses of the vocabulary) or "
        "fix_vocabulary (correct them all).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"media":{"type":"string"},
            "action":{"type":"string","enum":["correct","revert","revert_all","replace","vocabulary","suggest","fix_vocabulary"]},
            "first":{"type":"integer"},"last":{"type":"integer"},"text":{"type":"string"},
            "find":{"type":"string"},"replace":{"type":"string"},
            "terms":{"type":"array","items":{"type":"string"}}},"required":["project","action"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const QString action = need(a, "action");
            auto transcriptOf = [&]() -> MediaItem& {
                MediaItem& m = projectMedia(l.project, need(a, "media"));
                if (!m.transcript) throw ArgError{QStringLiteral("%1 has no transcript").arg(QString::fromStdString(m.name))};
                return m;
            };
            auto wordsText = [](const Transcript& t, size_t from, size_t to) {
                QStringList out;
                size_t k = 0;
                for (const auto& seg : t.segments)
                    for (const auto& w : seg.words) {
                        if (k >= from && k <= to) out << QString::fromStdString(w.text);
                        ++k;
                    }
                return out.join(' ');
            };
            if (action == "correct" || action == "revert" || action == "revert_all") {
                MediaItem& m = transcriptOf();
                auto t = std::make_shared<Transcript>(*m.transcript);
                const size_t first = size_t(std::max(0, a.value("first").toInt(-1)));
                bool changed = false;
                if (action == "correct") {
                    const size_t last = size_t(std::max(a.value("last").toInt(int(first)), int(first)));
                    changed = correctWords(*t, first, last, need(a, "text").toStdString());
                    if (!changed) throw ArgError{"No such words, or no text"};
                } else if (action == "revert") {
                    changed = revertCorrection(*t, first);
                    if (!changed) return fail("That word was never corrected");
                } else {
                    changed = revertAllCorrections(*t) > 0;
                    if (!changed) return fail("Nothing in it was corrected");
                }
                m.transcript = t;
                save(l);
                return ok(QStringLiteral("Done"), QJsonObject{{"words", wordsText(*t, first > 3 ? first - 3 : 0, first + 6)}});
            }
            if (action == "replace") {
                const int n = replaceInTranscripts(l.project, need(a, "find").toStdString(), need(a, "replace").toStdString());
                if (n) save(l);
                return ok(QStringLiteral("Replaced %1 time(s)").arg(n), QJsonObject{{"replaced", n}});
            }
            if (action == "vocabulary") {
                l.project.vocabulary.clear();
                for (const QJsonValue& v : a.value("terms").toArray())
                    if (!v.toString().trimmed().isEmpty()) l.project.vocabulary.push_back(v.toString().trimmed().toStdString());
                save(l);
                return ok(QStringLiteral("%1 term(s)").arg(l.project.vocabulary.size()));
            }
            if (action == "suggest" || action == "fix_vocabulary") {
                const auto found = vocabularySuggestions(l.project, l.project.vocabulary);
                QJsonArray list;
                for (const VocabularySuggestion& v : found) {
                    const MediaItem* m = l.project.findMedia(v.media);
                    list.append(QJsonObject{{"media", QString::fromStdString(m ? m->name : std::string())}, {"first", int(v.first)},
                                            {"last", int(v.last)}, {"heard", QString::fromStdString(v.heard)}, {"term", QString::fromStdString(v.term)}});
                }
                if (action == "suggest") return ok(QStringLiteral("%1 near miss(es)").arg(found.size()), QJsonObject{{"suggestions", list}});
                const int n = applyVocabularySuggestions(l.project, found);
                if (n) save(l);
                return ok(QStringLiteral("Corrected %1 near miss(es)").arg(n), QJsonObject{{"corrected", n}, {"suggestions", list}});
            }
            throw ArgError{QStringLiteral("Unknown action \"%1\"").arg(action)};
        });

    add("montage_cut_speech", "Cut by transcript",
        "Edit the cut by what is said, as in a text-based editor: remove every place a phrase is spoken, the filler words "
        "(um, uh, er... in the transcript's language, from English to Japanese; with discourse_fillers also \"like\", "
        "\"you know\", \"I mean\"... where set off by commas or pauses; plus the project's own filler_words, which "
        "filler_words also sets), retakes (broken-off attempts the speaker started again) and/or pauses longer than "
        "pauses_longer_than seconds (shortened to keep_pause). Every track is cut "
        "the same way and closed up, captions included. smooth_cuts puts a Smooth Cut (an optical-flow morph) on each join "
        "in the picture, to hide the jump. One undoable edit; use montage_find_phrase first to see what a phrase matches.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "phrases":{"type":"array","items":{"type":"string"},"description":"Phrases to cut, every time they are said"},
            "fillers":{"type":"boolean","default":false},
            "discourse_fillers":{"type":"boolean","default":false},
            "filler_words":{"type":"array","items":{"type":"string"},"description":"The project's own filler words or phrases (replaces the list)"},
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
            if (a.contains("filler_words")) {
                l.project.fillerWords.clear();
                for (const QJsonValue& v : a.value("filler_words").toArray())
                    if (!v.toString().trimmed().isEmpty()) l.project.fillerWords.push_back(v.toString().trimmed().toStdString());
            }
            if (a.value("fillers").toBool()) {
                FillerOptions fo;
                fo.language = sequenceTranscriptLanguage(l.project, s);
                fo.discourse = a.value("discourse_fillers").toBool();
                fo.custom = l.project.fillerWords;
                for (const FrameRange& r : fillerWordRanges(words, fps, fo)) ranges.push_back(r);
                const std::vector<bool> mask = fillerWordMask(words, fo);
                fillers = int(std::count(mask.begin(), mask.end(), true));
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
            if (mergeRanges(ranges).empty()) {
                if (a.contains("filler_words")) save(l);  // the list is kept for next time
                return ok("Nothing to cut", QJsonObject{{"phrases", found}, {"removed_seconds", 0}});
            }
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

    add("montage_make_shorts", "Make shorts from long footage",
        "Cut a podcast, interview or talk into short clips ready to post (CapCut's long video to shorts). Sentence-aligned "
        "windows of the transcripts between `min_seconds` and `max_seconds` are scored on how the first sentence hooks a "
        "viewer (a question, \"you\", a number, \"the secret\"...), how lively the stretch is, how much it is about "
        "`topic`, and how cleanly it starts and ends, with fillers and silence counting against it; the best `count` that "
        "do not overlap each become a new sequence at `aspect`, framed round the subject, with fillers cut and pauses "
        "shortened, captions in `caption_look` (\"none\" for none) and, with `hook_title`, the opening line as a title. "
        "`preview` only lists the moments. With `render_folder`, each is rendered there as H.264 at -14 LUFS with its "
        "captions burned in. `media` (names or paths) defaults to every transcribed media.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"media":{"type":"array","items":{"type":"string"}},
            "count":{"type":"number","default":5},"min_seconds":{"type":"number","default":15},"max_seconds":{"type":"number","default":60},
            "topic":{"type":"string"},"aspect":{"type":"string","enum":["9:16","1:1","4:5","16:9"],"default":"9:16"},
            "caption_look":{"type":"string","default":"creator_pop"},"remove_fillers":{"type":"boolean","default":true},
            "remove_pauses":{"type":"boolean","default":true},"hook_title":{"type":"boolean","default":false},
            "reframe":{"type":"boolean","default":true},"liveliness":{"type":"boolean","default":true},
            "preview":{"type":"boolean","default":false},"render_folder":{"type":"string"}},"required":["project"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            std::vector<Id> media;
            for (const QJsonValue& v : a.value("media").toArray()) media.push_back(projectMedia(l.project, v.toString()).id);
            if (media.empty())
                for (const MediaItem& m : l.project.media)
                    if (!m.subclipOf && m.transcript && !m.transcript->empty()) media.push_back(m.id);
            ShortsOptions o;
            o.count = std::clamp(a.value("count").toInt(5), 1, 50);
            o.minSeconds = std::max(1.0, a.value("min_seconds").toDouble(15));
            o.maxSeconds = std::max(o.minSeconds, a.value("max_seconds").toDouble(60));
            o.topic = a.value("topic").toString().toStdString();
            o.liveliness = a.value("liveliness").toBool(true);
            o.fillers.custom = l.project.fillerWords;
            ShortBuild b;
            const QString aspect = str(a, "aspect", "9:16");
            const QStringList wh = aspect.split(':');
            if (wh.size() != 2 || wh[0].toInt() <= 0 || wh[1].toInt() <= 0) throw ArgError{"\"aspect\" is like 9:16"};
            b.aspectW = wh[0].toInt();
            b.aspectH = wh[1].toInt();
            b.captionLook = str(a, "caption_look", "creator_pop").toStdString();
            if (b.captionLook == "none") b.captionLook.clear();
            if (!b.captionLook.empty() && !findCaptionLook(b.captionLook)) {
                QStringList ids;
                for (const CaptionLook& c : captionLooks()) ids << QString::fromStdString(c.id);
                throw ArgError{QStringLiteral("No caption look \"%1\" (%2, or none)").arg(QString::fromStdString(b.captionLook), ids.join(", "))};
            }
            b.removeFillers = a.value("remove_fillers").toBool(true);
            b.removePauses = a.value("remove_pauses").toBool(true);
            b.hookTitle = a.value("hook_title").toBool(false);
            b.reframe = a.value("reframe").toBool(true);
            b.fillers.custom = l.project.fillerWords;
            std::string err;
            const std::vector<ShortMoment> moments = findShorts(l.project, media, o, [this](double f) { progress(f * 0.3, "Finding moments"); }, nullptr, &err);
            if (moments.empty()) return fail(QString::fromStdString(err));
            const bool preview = a.value("preview").toBool(false);
            const QString folder = a.contains("render_folder") ? absolute(a.value("render_folder").toString()) : QString();
            if (!folder.isEmpty() && !QDir().mkpath(folder)) return fail(QStringLiteral("Cannot make the folder %1").arg(folder));
            QJsonArray list;
            std::map<Id, int> counts;
            int rendered = 0;
            for (size_t i = 0; i < moments.size(); ++i) {
                const ShortMoment& m = moments[i];
                const MediaItem* src = l.project.findMedia(m.media);
                QJsonObject row{{"media", QString::fromStdString(src->name)}, {"in", m.in}, {"out", m.out}, {"score", m.score},
                                {"hook", m.hook}, {"hook_line", QString::fromStdString(m.hookLine)}, {"text", QString::fromStdString(m.text)}};
                if (!preview) {
                    const std::string name = QFileInfo(QString::fromStdString(src->name)).completeBaseName().toStdString() + " - Short " +
                                             std::to_string(++counts[m.media]);
                    const Id id = makeShortSequence(l.project, m, b, name, nullptr, &err);
                    if (!id) return fail(QString::fromStdString(err));
                    const Sequence* s = l.project.findSequence(id);
                    row["sequence"] = double(id);
                    row["name"] = QString::fromStdString(s->name);
                    row["width"] = s->width;
                    row["height"] = s->height;
                    row["seconds"] = double(s->duration()) / s->fpsValue();
                    row["captions"] = s->captionTracks.empty() ? 0 : int(s->captionTracks[0].captions.size());
                    if (!folder.isEmpty()) {
                        const ExportPreset* p = findExportPreset("Social - TikTok / Reels / Shorts");
                        ExportSettings st = p ? p->settings : ExportSettings{};
                        st.burnInCaptions = !s->captionTracks.empty();
                        st.path = QDir(folder).filePath(QString::fromStdString(s->name) + ".mp4").toStdString();
                        const double base = 0.3 + 0.7 * double(i) / double(moments.size()), span = 0.7 / double(moments.size());
                        if (!exportSequence(l.project, *s, st, [&](double f, FrameTime) { progress(base + span * f, "Rendering"); }, nullptr, &err))
                            return fail(QString::fromStdString(err));
                        row["output"] = QString::fromStdString(st.path);
                        ++rendered;
                    }
                }
                list.append(row);
            }
            if (!preview) save(l);
            const QString what = preview ? QStringLiteral("%1 moment(s) found").arg(moments.size())
                                         : rendered ? QStringLiteral("%1 short(s) made and rendered to %2").arg(moments.size()).arg(folder)
                                                    : QStringLiteral("%1 short(s) made as new sequences").arg(moments.size());
            return ok(what, QJsonObject{{"shorts", list}});
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

    add("montage_reconform", "Change list and re-conform to a new cut",
        "The picture of a new version of a cut matched frame by frame to the old one (Avid's Change List tool, the "
        "Conformalizer): the stretches it keeps (in their old order, or moved, and how far they slid), its new material "
        "(inserted shots, extended shots) and what it took out (deleted shots, trimmed ones). `path` writes the list as a "
        "change EDL (.edl: CMX 3600 with the old cut as reel OLDCUT, new material as NEWCUT) or a CSV. With `source` (a "
        "sequence cut to the old version: a mix, a grade, effects and titles) it also makes a new sequence of it rebuilt to "
        "play against the new cut, every track carried over with fades, automation, markers and captions, the new "
        "material filled in from the new cut unless fill is false.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "before":{"type":"string","description":"The old cut's sequence name"},
            "after":{"type":"string","description":"The new cut's sequence name (default: the active one)"},
            "path":{"type":"string","description":"Write the change list here (.edl or .csv)"},
            "source":{"type":"string","description":"The sequence to re-conform (cut to the old version)"},
            "name":{"type":"string","description":"The re-conformed sequence's name (default: \"<source> (Conformed)\")"},
            "fill":{"type":"boolean","default":true},"markers":{"type":"boolean","default":true}},
            "required":["project","before"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            auto byName = [&](const QString& name) -> Sequence* {
                for (Sequence& sq : l.project.sequences)
                    if (QString::fromStdString(sq.name) == name) return &sq;
                throw ArgError{QStringLiteral("No sequence named \"%1\"").arg(name)};
            };
            const Sequence* before = byName(need(a, "before"));
            const Sequence* after = a.contains("after") ? byName(str(a, "after")) : &l.seq();
            if (before == after) throw ArgError{"Compare two different sequences"};
            std::string err;
            const CutChanges changes = cutChanges(l.project, *before, *after, &err);
            if (!err.empty()) return fail(QString::fromStdString(err));
            QJsonArray list;
            for (const CutEvent& e : changes.events) {
                QJsonObject o{{"change", QString::fromLatin1(cutEventName(e.kind)).toLower()},
                              {"shot", QString::fromStdString(e.shot)},
                              {"shots", e.shots},
                              {"new_in", tc(e.newIn, *after)},
                              {"new_out", tc(e.newOut, *after)},
                              {"length", double(e.length())}};
                if (e.kind != CutEventKind::Inserted && e.kind != CutEventKind::Extended) {
                    o["old_in"] = tc(e.oldIn, *after);
                    o["old_out"] = tc(e.oldOut, *after);
                }
                if (e.kind == CutEventKind::Same || e.kind == CutEventKind::Moved) o["shift"] = double(e.shift());
                list.append(o);
            }
            QJsonObject result{{"events", list}, {"changed", changes.changed()}};
            if (a.contains("path")) {
                const QString file = absolute(need(a, "path"));
                const std::string text = QFileInfo(file).suffix().compare("edl", Qt::CaseInsensitive) == 0 ? changeEdl(changes) : changeListCsv(changes);
                QFile f(file);
                if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(text.data(), qint64(text.size())) != qint64(text.size()))
                    return fail(QStringLiteral("Cannot write %1").arg(f.fileName()));
                result["path"] = f.fileName();
            }
            QString message = changes.changed() ? QStringLiteral("%1 change(s)").arg(changes.changed()) : QStringLiteral("The picture is the same");
            if (a.contains("source")) {
                const Id source = byName(need(a, "source"))->id, newCut = after->id;
                ReconformOptions o;
                o.name = str(a, "name").toStdString();
                o.fillFromNewCut = a.value("fill").toBool(true);
                o.markers = a.value("markers").toBool(true);
                const ReconformResult r = reconformSequence(l.project, source, changes, newCut, o, &err);
                if (!r.sequence) return fail(QString::fromStdString(err));
                const Sequence* made = l.project.findSequence(r.sequence);
                result["sequence"] = QString::fromStdString(made->name);
                result["clips"] = r.clips;
                save(l);
                message += QStringLiteral("; made %1").arg(QString::fromStdString(made->name));
            }
            return ok(message, result);
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

    add("montage_spectral_repair", "Spectral repair",
        "Take a sound out of an audio clip where it shares the moment with the dialogue but not its frequencies (a phone, a "
        "squeak, a whistle, a siren, a hum that comes and goes), as Audition's spectral healing and iZotope RX's Spectral "
        "Repair do: each region, a box of time and frequency, is healed (each frequency brought down to the level heard "
        "just before and after) or turned down by gain_db, and everything outside it is left as it was. Times are timeline "
        "seconds; regions are kept on the clip in source time. Leave out low_hz and high_hz to use the band that stands out "
        "most in that stretch (reported back); find_only reports the bands without changing anything. clear removes the "
        "clip's repairs first.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "clip":{"type":"number","description":"Audio clip id"},
            "regions":{"type":"array","items":{"type":"object","properties":{
                "start":{"type":"number","description":"Timeline seconds"},"end":{"type":"number"},
                "low_hz":{"type":"number"},"high_hz":{"type":"number"},
                "mode":{"type":"string","enum":["heal","attenuate"],"default":"heal"},
                "gain_db":{"type":"number","description":"attenuate: how far down (default -20)"},
                "channel":{"type":"string","enum":["both","left","right"],"default":"both"}},"required":["start","end"]}},
            "find_only":{"type":"boolean"},
            "clear":{"type":"boolean"}},"required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            Clip* c = edit::clipById(s, Id(a.value("clip").toDouble()));
            if (!c || !c->mediaId) throw ArgError{"No such clip"};
            const auto where = edit::locate(s, c->id);
            if (!where || where->track.kind != TrackKind::Audio) throw ArgError{"Give an audio clip (its sound is on an audio track)"};
            const MediaItem* m = l.project.findMedia(c->mediaId);
            if (!m || !m->hasAudio) throw ArgError{"That clip has no sound"};
            const double fps = s.fpsValue(), clipStart = double(c->start) / fps, clipEnd = double(c->end()) / fps;
            const bool findOnly = a.value("find_only").toBool();
            AudioBufferPtr sound;
            auto source = [&]() -> const AudioBuffer& {
                if (!sound) {
                    sound = MediaPool::instance().audio(audioKey(m->path, c->channels), 48000);  // as the clip plays it
                    if (!sound) throw ArgError{"Cannot read the clip's sound"};
                }
                return *sound;
            };
            std::vector<SpectralRegion> regions;
            QJsonArray found, made;
            for (const QJsonValue& v : a.value("regions").toArray()) {
                const QJsonObject o = v.toObject();
                const double t0 = o.value("start").toDouble(), t1 = o.value("end").toDouble();
                if (!(t1 > t0)) throw ArgError{"Each region's end must be after its start"};
                if (t1 <= clipStart || t0 >= clipEnd) throw ArgError{"A region is outside the clip"};
                SpectralRegion r;
                const double a0 = clipSourceSeconds(s, *c, std::max(t0, clipStart)), a1 = clipSourceSeconds(s, *c, std::min(t1, clipEnd));
                r.start = std::min(a0, a1), r.end = std::max(a0, a1);
                r.mode = o.value("mode").toString("heal").toStdString();
                if (!validSpectralMode(r.mode)) throw ArgError{"mode is heal or attenuate"};
                r.gainDb = o.value("gain_db").toDouble(-20);
                if (r.gainDb > 0) throw ArgError{"gain_db turns the region down: give a negative number of decibels"};
                const QString ch = o.value("channel").toString("both");
                r.channel = ch == "left" ? 0 : ch == "right" ? 1 : -1;
                if (o.contains("low_hz") || o.contains("high_hz")) {
                    r.low = o.value("low_hz").toDouble(0), r.high = o.value("high_hz").toDouble(0);
                    if (r.low < 0 || r.low >= 24000 || (r.high > 0 && r.high <= r.low)) throw ArgError{"low_hz and high_hz must be 0 to 24000 Hz, high above low"};
                } else {
                    const std::vector<SpectralBand> bands = prominentBands(source(), r.start, r.end, 3, r.channel);
                    QJsonArray list;
                    for (const SpectralBand& b : bands) list.append(QJsonObject{{"low_hz", b.low}, {"high_hz", b.high}, {"excess_db", b.excessDb}});
                    found.append(QJsonObject{{"start", t0}, {"end", t1}, {"bands", list}});
                    if (bands.empty()) {
                        if (findOnly) continue;
                        throw ArgError{QStringLiteral("Nothing stands out between %1 and %2 s: give low_hz and high_hz").arg(t0).arg(t1)};
                    }
                    r.low = bands.front().low, r.high = bands.front().high;
                }
                regions.push_back(r);
                made.append(QJsonObject{{"source_start", r.start}, {"source_end", r.end}, {"low_hz", r.low}, {"high_hz", r.high},
                                        {"mode", QString::fromStdString(r.mode)}, {"gain_db", r.gainDb}});
            }
            if (findOnly) return ok(json(QJsonObject{{"found", found}}), QJsonObject{{"found", found}});
            if (regions.empty() && !a.value("clear").toBool()) throw ArgError{"Give \"regions\" (or \"clear\")"};
            std::vector<SpectralRegion> all = a.value("clear").toBool() ? std::vector<SpectralRegion>{} : spectralRegionsOf(*c);
            all.insert(all.end(), regions.begin(), regions.end());
            if (all.empty()) {
                c->effects.erase(std::remove_if(c->effects.begin(), c->effects.end(), [](const Effect& e) { return e.type == "spectral_repair"; }),
                                 c->effects.end());
            } else {
                spectralRepairEffect(l.project, *c, true)->strings["regions"] = spectralRegionsToString(all);
            }
            save(l);
            QJsonObject out{{"regions", made}, {"total", int(all.size())}};
            if (!found.isEmpty()) out["found"] = found;
            return ok(QStringLiteral("%1 region(s) on the clip").arg(all.size()), out);
        });

    add("montage_redact_faces", "Redact faces",
        "Hide faces in a video clip, as news and documentary editors must for people who have not agreed to be shown: "
        "every face is found, followed from frame to frame (a face lost for a few frames stays covered) and grouped by "
        "person, then blurred, pixelated or covered with a solid colour. The first call analyses the clip (kept on the "
        "clip, so later calls are quick) and covers everyone; it reports the faces found as numbered groups, named when "
        "they match people found by People search. Then \"show\" leaves the given groups (numbers, or names of people) "
        "uncovered, or \"cover_only\" covers just them. reanalyse looks again (after the clip was lengthened).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "clip":{"type":"number","description":"Video clip id"},
            "show":{"type":"array","items":{"type":["number","string"]},"description":"Groups (1-based) or people's names left uncovered"},
            "cover_only":{"type":"array","items":{"type":["number","string"]},"description":"Only these groups or people are covered"},
            "style":{"type":"string","enum":["blur","pixelate","solid"]},
            "strength":{"type":"number","description":"0-100 (default 70)"},
            "hold":{"type":"number","description":"Frames a lost face stays covered (default 12)"},
            "reanalyse":{"type":"boolean"}},"required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            Clip* c = edit::clipById(s, Id(a.value("clip").toDouble()));
            if (!c || !c->mediaId) throw ArgError{"No such clip"};
            const auto where = edit::locate(s, c->id);
            const MediaItem* m = l.project.findMedia(c->mediaId);
            if (!where || where->track.kind != TrackKind::Video || !m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image))
                throw ArgError{"Give a video clip"};
            if (a.contains("show") && a.contains("cover_only")) throw ArgError{"Give \"show\" or \"cover_only\", not both"};
            const QString styleName = a.value("style").toString();
            if (!styleName.isEmpty() && styleName != "blur" && styleName != "pixelate" && styleName != "solid")
                throw ArgError{"style is blur, pixelate or solid"};
            if (a.contains("strength") && !(a.value("strength").toDouble() >= 0 && a.value("strength").toDouble() <= 100))
                throw ArgError{"strength is 0 to 100"};
            if (a.contains("hold") && !(a.value("hold").toDouble() >= 0 && a.value("hold").toDouble() <= 60)) throw ArgError{"hold is 0 to 60 frames"};
            double start = 0, end = 0;
            clipMediaSpan(s, *c, m->kind == MediaKind::Image, start, end);
            FaceTracks tracks;
            const Effect* existing = redactFacesEffectOf(l.project, *c, false);
            bool have = existing && faceTracksFromString(existing->s("tracks"), tracks) &&
                        (existing->s("media").empty() || existing->s("media") == std::to_string(m->id));
            if (have && tracks.step > 0 && (start < tracks.start - 1 / tracks.fps || end > tracks.end + 1 / tracks.fps)) have = false;
            bool analysed = false;
            if (!have || a.value("reanalyse").toBool()) {
                if (!faceSearchAvailable() || !faceModel().installed())
                    return fail("Redact Faces needs the face models: run `scripts/fetch-models.sh` or add them once in the app");
                std::string err;
                if (!trackFaces(m->path, start, end, tracks, {}, &err)) return fail(QString::fromStdString("Could not look for faces: " + err));
                analysed = true;
            }
            std::vector<FaceGroup> groups = groupFaceTracks(tracks);
            matchProjectPeople(l.project, groups);
            // Which groups the list names: a number, or a person's name.
            auto pick = [&](const QJsonArray& list) {
                std::set<size_t> chosen;
                for (const QJsonValue& v : list) {
                    if (v.isDouble()) {
                        const int n = v.toInt();
                        if (n < 1 || n > int(groups.size())) throw ArgError{QStringLiteral("There is no face group %1 (1 to %2)").arg(n).arg(groups.size())};
                        chosen.insert(size_t(n - 1));
                        continue;
                    }
                    const QString name = v.toString().trimmed();
                    bool found = false;
                    for (size_t g = 0; g < groups.size(); ++g)
                        if (groups[g].person && QString::fromStdString(personName(l.project, groups[g].person)).compare(name, Qt::CaseInsensitive) == 0)
                            chosen.insert(g), found = true;
                    if (!found) throw ArgError{QStringLiteral("No face in the clip is %1").arg(name)};
                }
                return chosen;
            };
            std::set<int> keep;
            if (a.contains("show")) {
                for (size_t g : pick(a.value("show").toArray())) keep.insert(groups[g].tracks.begin(), groups[g].tracks.end());
            } else if (a.contains("cover_only")) {
                const std::set<size_t> cover = pick(a.value("cover_only").toArray());
                for (size_t g = 0; g < groups.size(); ++g)
                    if (!cover.count(g)) keep.insert(groups[g].tracks.begin(), groups[g].tracks.end());
            } else if (existing && !analysed) {
                keep = trackIdsFromString(existing->s("keep"));
            }
            Effect* e = redactFacesEffectOf(l.project, *c, true);
            e->enabled = true;
            e->strings["tracks"] = faceTracksToString(tracks);
            e->strings["media"] = std::to_string(m->id);
            if (keep.empty()) e->strings.erase("keep");
            else e->strings["keep"] = trackIdsToString(keep);
            if (!styleName.isEmpty()) e->params["style"] = Param(styleName == "blur" ? 0.0 : styleName == "pixelate" ? 1.0 : 2.0);
            if (a.contains("strength")) e->params["strength"] = Param(a.value("strength").toDouble());
            if (a.contains("hold")) e->params["hold"] = Param(std::round(a.value("hold").toDouble()));
            save(l);
            QJsonArray list;
            int covered = 0;
            for (size_t g = 0; g < groups.size(); ++g) {
                const bool shown = std::all_of(groups[g].tracks.begin(), groups[g].tracks.end(), [&](int id) { return keep.count(id) > 0; });
                covered += shown ? 0 : 1;
                QJsonObject o{{"group", int(g + 1)}, {"seconds", std::round(groups[g].seconds * 10) / 10}, {"tracks", int(groups[g].tracks.size())},
                              {"covered", !shown}, {"best_time", groups[g].bestTime}};
                if (groups[g].person) o["person"] = QString::fromStdString(personName(l.project, groups[g].person));
                list.append(o);
            }
            return ok(QStringLiteral("%1 face group(s) in the clip, %2 covered%3").arg(groups.size()).arg(covered).arg(analysed ? " (analysed)" : ""),
                      QJsonObject{{"groups", list}, {"analysed", analysed}, {"source_start", tracks.start}, {"source_end", tracks.end}});
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
        "as a new caption track (`import`), or the CEA-608 closed captions inside a video clip's file (`import_embedded`). "
        "`track` is a caption track index (default: the visible one).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"export":{"type":"string","description":"A file to write"},
            "import":{"type":"string","description":"A file to read as a new track"},"track":{"type":"integer"},
            "import_embedded":{"type":"number","description":"A video clip (id): the CEA-608 closed captions inside its file as a new track, where the clip plays them"},
            "language":{"type":"string","description":"ISO 639-1 code for an imported track"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            if (a.contains("import_embedded")) {
                const Id id = Id(a.value("import_embedded").toDouble());
                const Clip* c = edit::clipById(s, id);
                const MediaItem* m = c && c->mediaId ? l.project.findMedia(c->mediaId) : nullptr;
                if (!m || m->kind != MediaKind::Video) return fail("import_embedded names a video clip (see montage_project_info)");
                std::vector<Caption> found;
                std::string err;
                if (!readEmbeddedCaptions(m->path, s.fps, found, {}, nullptr, &err)) return fail(QString::fromStdString(err));
                CaptionTrack t;
                t.id = l.project.newId();
                t.name = "CC1 - " + c->name;
                t.captions = captionsThroughClip(*c, found);
                if (t.captions.empty()) return fail("None of the clip's closed captions fall within it");
                const int n = int(t.captions.size());
                const QString name = QString::fromStdString(t.name);
                QJsonArray list;
                for (const Caption& cap : t.captions)
                    list.append(QJsonObject{{"start", tc(cap.start, s)}, {"end", tc(cap.end, s)}, {"text", QString::fromStdString(cap.text)}});
                s.captionTracks.push_back(std::move(t));
                save(l);
                return ok(QStringLiteral("Imported %1 closed captions as \"%2\"").arg(n).arg(name),
                          QJsonObject{{"track", int(s.captionTracks.size()) - 1}, {"name", name}, {"captions", list}});
            }
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

    add("montage_caption_style", "Style captions",
        "Set how a caption track looks in the viewer, burn-ins and ASS files: a ready-made `look` (classic, broadcast, "
        "bold_yellow, creator_pop, karaoke, one_word, minimal, paper, neon), then any of: font, size (% of the frame "
        "height), bold, color, box_color, box_opacity (0-1), outline (% of the text), outline_color, shadow (% of the "
        "text), all_caps, position (bottom edge, % down), animation (none, word, highlight, pop, one_word) and "
        "highlight_color. Colours are \"#rrggbb\". With nothing to set it lists the looks.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"track":{"type":"integer","default":0},
            "look":{"type":"string"},"font":{"type":"string"},"size":{"type":"number"},"bold":{"type":"boolean"},
            "color":{"type":"string"},"box_color":{"type":"string"},"box_opacity":{"type":"number"},
            "outline":{"type":"number"},"outline_color":{"type":"string"},"shadow":{"type":"number"},"all_caps":{"type":"boolean"},
            "position":{"type":"number"},"animation":{"type":"string","enum":["none","word","highlight","pop","one_word"]},
            "highlight_color":{"type":"string"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            QJsonArray looks;
            for (const CaptionLook& lk : captionLooks())
                looks.append(QJsonObject{{"id", QString::fromStdString(lk.id)}, {"name", QString::fromStdString(lk.name)}});
            const int index = a.value("track").toInt(0);
            if (index < 0 || index >= int(s.captionTracks.size())) return fail("No such caption track");
            CaptionStyle st = s.captionTracks[size_t(index)].style;
            bool any = false;
            if (a.contains("look")) {
                const CaptionLook* lk = findCaptionLook(a.value("look").toString().toStdString());
                if (!lk) throw ArgError{QStringLiteral("Unknown look \"%1\"").arg(a.value("look").toString())};
                st = lk->style;
                any = true;
            }
            auto colour = [&](const char* key, double& r, double& g, double& b) {
                if (!a.contains(key)) return;
                const QColor c(a.value(key).toString());
                if (!c.isValid()) throw ArgError{QStringLiteral("\"%1\" must be a colour like #ffcc00").arg(key)};
                r = c.redF(), g = c.greenF(), b = c.blueF();
                any = true;
            };
            auto number = [&](const char* key, double& v, double scale, double lo, double hi) {
                if (!a.contains(key)) return;
                const double x = a.value(key).toDouble(-1e9);
                if (x < lo || x > hi) throw ArgError{QStringLiteral("\"%1\" must be between %2 and %3").arg(key).arg(lo).arg(hi)};
                v = x * scale;
                any = true;
            };
            if (a.contains("font")) st.font = a.value("font").toString().toStdString(), any = true;
            if (a.contains("bold")) st.bold = a.value("bold").toBool(), any = true;
            if (a.contains("all_caps")) st.allCaps = a.value("all_caps").toBool(), any = true;
            number("size", st.size, 0.01, 1, 25);
            number("box_opacity", st.boxOpacity, 1, 0, 1);
            number("outline", st.outline, 0.01, 0, 30);
            number("shadow", st.shadow, 0.01, 0, 30);
            number("position", st.position, 0.01, 10, 100);
            colour("color", st.textR, st.textG, st.textB);
            colour("box_color", st.boxR, st.boxG, st.boxB);
            colour("outline_color", st.outlineR, st.outlineG, st.outlineB);
            colour("highlight_color", st.hiR, st.hiG, st.hiB);
            if (a.contains("animation")) {
                static const QStringList kinds = {"none", "word", "highlight", "pop", "one_word"};
                const int k = int(kinds.indexOf(a.value("animation").toString()));
                if (k < 0) throw ArgError{"animation is none, word, highlight, pop or one_word"};
                st.animation = k;
                any = true;
            }
            if (!any) return ok(QStringLiteral("Looks: %1").arg([&] {
                QStringList ids;
                for (const auto& v : looks) ids << v.toObject().value("id").toString();
                return ids.join(", ");
            }()), QJsonObject{{"looks", looks}});
            s.captionTracks[size_t(index)].style = st;
            save(l);
            return ok(QStringLiteral("Styled \"%1\"").arg(QString::fromStdString(s.captionTracks[size_t(index)].name)),
                      QJsonObject{{"style", QJsonDocument::fromJson(QByteArray::fromStdString(captionStyleToJsonString(st))).object()}});
        });

    add("montage_edit_captions", "Check and fix captions",
        "Check a caption track against reading limits (by default the Netflix Timed Text Style Guide's: 20 characters a "
        "second, 42 characters a line, two lines, 5/6 s to 7 s on screen, 2 frames between captions) or change it as a "
        "whole. `action`: check (what each caption breaks), fix_timing (short or fast captions stay up longer into the "
        "time after them, each ends the minimum gap before the next, short pauses close up), shift (by `by`: seconds or a "
        "timecode, negative for earlier), sync (the first and last of `captions`, or of the track, start at `first` and "
        "`last`, the rest stretched between: subtitles timed for another cut or frame rate), replace (`find` with "
        "`replace`, optionally `case_sensitive` and `whole_words`), place (`place`: bottom, top or middle, and left, centre or "
        "right, e.g. \"top left\": where the captions sit, kept in every caption format) or raise_over_titles (captions "
        "shown over lower thirds and other low titles move to the top). `captions` (indices) limits shift, replace and place. Limits "
        "can be changed: max_cps, max_line_chars, max_lines, min_seconds, max_seconds, min_gap_frames.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"track":{"type":"integer","default":0},
            "action":{"type":"string","enum":["check","fix_timing","shift","sync","replace","place","raise_over_titles"]},
            "place":{"type":"string"},
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
            } else if (action == "place") {
                int vertical = 0, align = 0;
                if (!parseCaptionPlace(need(a, "place").toStdString(), vertical, align))
                    throw ArgError{QStringLiteral("Unknown place \"%1\" (bottom, top or middle; left, centre or right)").arg(a.value("place").toString())};
                if (!placeCaptions(t.captions, chosen, vertical, align)) return fail("The captions are there already");
                Caption where;
                where.vertical = vertical;
                where.align = align;
                done = QStringLiteral("Placed %1 captions at the %2").arg(chosen.empty() ? t.captions.size() : chosen.size())
                           .arg(QString::fromStdString(captionPlaceName(where)));
            } else if (action == "raise_over_titles") {
                const int n = raiseCaptionsOverTitles(t.captions, s);
                if (!n) return ok(QStringLiteral("No caption is shown over a low title"), QJsonObject{{"changed", 0}});
                done = QStringLiteral("Moved %1 captions to the top").arg(n);
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

    add("montage_interpret_media", "Interpret footage",
        "Change how video files (or stills) are read, for every clip made from them, as Premiere's Interpret Footage, "
        "Resolve's Clip Attributes and Final Cut's Conform Speed do: frame_rate conforms the footage (each of its frames "
        "shown for 1/frame_rate of a second, so 120 fps footage at 24 plays five times slower as smooth slow motion, and "
        "its sound with it, at the new speed or with keep_pitch time-stretched); pixel_aspect replaces the file's (2 for "
        "a 2x anamorphic lens, 1.33, 1.5, 1.8, 0.9 for DV NTSC, 1 for square); alpha reads its transparency as straight, "
        "premultiplied, ignore (opaque) or invert; field_order forces progressive, upper (top field first) or lower "
        "(bottom field first) whatever the frames are flagged. Arguments left out keep their current setting; 0 or "
        "\"file\" goes back to the file's own; reset puts everything back. Clips keep starting on the same frame and keep "
        "their timeline length (shortened where the footage no longer reaches); subclips, clip markers and transcripts "
        "follow.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "media":{"type":["string","array"],"items":{"type":"string"},"description":"Media files or names in the project"},
            "frame_rate":{"type":["number","string"],"description":"Frames per second to play at (23.976, 25, 29.97...), or \"file\""},
            "pixel_aspect":{"type":["number","string"],"description":"Pixel aspect ratio, or \"file\""},
            "alpha":{"type":"string","enum":["file","straight","premultiplied","ignore","invert"]},
            "field_order":{"type":"string","enum":["file","progressive","upper","lower"]},
            "keep_pitch":{"type":"boolean","description":"A conformed sound keeps its pitch"},
            "raw_exposure":{"type":"number","description":"Camera RAW and CinemaDNG: exposure in stops (-5 to 5)"},
            "raw_temperature":{"type":["number","string"],"description":"Camera RAW: the light's colour temperature in kelvin (2000 to 25000), or \"as_shot\""},
            "raw_tint":{"type":"number","description":"Camera RAW: tint, + magenta, - green (-150 to 150)"},
            "raw_highlights":{"type":"string","enum":["clip","blend","rebuild"]},
            "raw_half":{"type":"boolean","description":"Camera RAW: decode at half size for speed"},
            "reset":{"type":"boolean","description":"Read the files as they are"}},"required":["project","media"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            std::vector<Id> ids;
            const QJsonValue mv = a.value("media");
            if (mv.isString()) ids.push_back(projectMedia(l.project, mv.toString()).id);
            for (const QJsonValue& v : mv.toArray()) ids.push_back(projectMedia(l.project, v.toString()).id);
            if (ids.empty()) throw ArgError{"\"media\" is required"};
            auto fileOr = [&](const char* key, double& value) {
                const QJsonValue v = a.value(key);
                if (v.isUndefined() || v.isNull()) return false;
                if (v.isString() && v.toString() == "file") value = 0;
                else if (v.isDouble() && v.toDouble() >= 0) value = v.toDouble();
                else throw ArgError{QStringLiteral("\"%1\" must be a number or \"file\"").arg(key)};
                return true;
            };
            QJsonArray out;
            int changed = 0;
            for (Id id : ids) {
                MediaItem* m = l.project.findMedia(id);
                if (m->subclipOf) m = l.project.findMedia(m->subclipOf);
                Interpretation i = a.value("reset").toBool() ? Interpretation{} : interpretationOf(*m);
                if (ImageSequence seq; parseImageSequencePath(m->path, seq)) i.fps = seq.fps;
                double fps = 0, par = 0;
                if (fileOr("frame_rate", fps)) i.fps = fps > 0 ? rateFor(fps) : Rational{0, 1};
                if (fileOr("pixel_aspect", par)) i.par = par;
                if (a.contains("alpha")) i.alpha = str(a, "alpha") == "file" ? "" : str(a, "alpha").toStdString();
                if (a.contains("field_order")) i.fields = str(a, "field_order") == "file" ? "" : str(a, "field_order").toStdString();
                if (a.contains("keep_pitch")) i.keepPitch = a.value("keep_pitch").toBool();
                if (a.contains("raw_exposure")) i.rawExposure = a.value("raw_exposure").toDouble();
                if (a.contains("raw_temperature")) {
                    const QJsonValue t = a.value("raw_temperature");
                    if (t.isString() && t.toString() != "as_shot") throw ArgError{"raw_temperature is kelvin (a number) or \"as_shot\""};
                    i.rawTemperature = t.isString() ? 0.0 : t.toDouble();
                }
                if (a.contains("raw_tint")) i.rawTint = a.value("raw_tint").toDouble();
                if (a.contains("raw_highlights")) i.rawHighlights = str(a, "raw_highlights") == "clip" ? "" : str(a, "raw_highlights").toStdString();
                if (a.contains("raw_half")) i.rawHalf = a.value("raw_half").toBool();
                const edit::Result r = edit::interpretFootage(l.project, m->id, i);
                if (!r.ok && !r.error.empty()) throw ArgError{QStringLiteral("%1: %2").arg(QString::fromStdString(m->name), QString::fromStdString(r.error))};
                changed += r.ok;
                m = l.project.findMedia(m->id);
                QJsonObject o{{"name", QString::fromStdString(m->name)}, {"frame_rate", m->fps.toDouble()}, {"duration_seconds", m->duration},
                              {"width", m->width}, {"height", m->height}};
                mediaPathJson(*m, o);
                out.append(o);
            }
            if (changed) save(l);
            return ok(changed ? QStringLiteral("Interpreted %1 media item(s)").arg(changed) : QStringLiteral("Nothing changed"),
                      QJsonObject{{"media", out}, {"changed", changed}});
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
                mediaPathJson(m, o);
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

    add("montage_remove_bleed", "Remove mic bleed from multitrack talk",
        "For talk recorded with a mic on each speaker (podcasts, interviews): each of the audio `tracks` dips by "
        "`reduction_db` wherever it is not its speaker's turn (more than `margin_db` below the loudest mic at that moment, "
        "or at its own noise floor), fading out after its speaker stops and back just before they start; two people "
        "talking at once both stay up. Written as volume keyframes on the clips (replacing theirs). `tracks` defaults to "
        "every unmuted audio track with clips.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"tracks":{"type":"array","items":{"type":"string"},"description":"e.g. [\"A1\",\"A2\"]"},
            "reduction_db":{"type":"number","default":-24},"margin_db":{"type":"number","default":10},
            "floor_db":{"type":"number","default":-50}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            std::vector<int> tracks;
            for (const QJsonValue& v : a.value("tracks").toArray()) {
                const TrackRef t = trackArg(v.toString(), s, false);
                if (t.kind != TrackKind::Audio) throw ArgError{"\"tracks\" are audio tracks"};
                tracks.push_back(t.index);
            }
            if (tracks.empty())
                for (size_t i = 0; i < s.audioTracks.size(); ++i)
                    if (!s.audioTracks[i].muted && !s.audioTracks[i].clips.empty()) tracks.push_back(int(i));
            BleedOptions o;
            if (a.value("reduction_db").isDouble()) o.reductionDb = std::clamp(a.value("reduction_db").toDouble(), -60.0, -3.0);
            if (a.value("margin_db").isDouble()) o.marginDb = std::clamp(a.value("margin_db").toDouble(), 1.0, 40.0);
            if (a.value("floor_db").isDouble()) o.floorDb = a.value("floor_db").toDouble();
            std::string err;
            const std::vector<Spans> dips = bleedSpans(l.project, s, tracks, o, &err);
            if (dips.empty()) return fail(QString::fromStdString(err));
            const int changed = removeMicBleed(s, tracks, dips, o);
            if (changed) save(l);
            QJsonArray list;
            for (size_t i = 0; i < tracks.size(); ++i) {
                QJsonArray spans;
                for (const auto& [from, to] : dips[i]) spans.append(QJsonObject{{"start_seconds", from}, {"end_seconds", to}});
                list.append(QJsonObject{{"track", QStringLiteral("A%1").arg(tracks[i] + 1)}, {"dips", spans}});
            }
            return ok(QStringLiteral("Mic bleed removed: %1 clip(s) dip while it is not their speaker's turn").arg(changed),
                      QJsonObject{{"tracks", list}, {"clips_changed", changed}});
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

    add("montage_export_versions", "Export versions in several shapes",
        "Render the active sequence in several shapes at once (16:9, 9:16, 4:5, 1:1): each shape that is not the "
        "sequence's own becomes a copy reframed to follow each shot's subject (captions raised clear of the platforms' "
        "buttons when tall), kept in the project as \"<name> 9x16\" and so on, then every version is rendered into "
        "`folder` with the export preset `preset` (default \"H.264 - High Quality\"), captions burned in when `captions` "
        "(default true) and the sound brought to `loudness_lufs` (default -14; 0 leaves it). Returns the files.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"shapes":{"type":"array","items":{"type":"string"},"description":"e.g. [\"16:9\", \"9:16\"]"},
            "folder":{"type":"string"},"preset":{"type":"string"},"captions":{"type":"boolean","default":true},
            "loudness_lufs":{"type":"number","default":-14}},"required":["project","shapes","folder"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            std::vector<VersionShape> shapes;
            for (const QJsonValue& v : a.value("shapes").toArray()) {
                VersionShape shape;
                if (!parseVersionShape(v.toString().toStdString(), shape)) throw ArgError{QStringLiteral("\"%1\" is not a shape like 16:9 or 9:16").arg(v.toString())};
                shapes.push_back(shape);
            }
            if (shapes.empty()) throw ArgError{"\"shapes\" lists at least one shape"};
            const ExportPreset* preset = findExportPreset(str(a, "preset", "H.264 - High Quality").toStdString());
            if (!preset) throw ArgError{"Unknown preset (see montage_list_presets)"};
            const QString folder = absolute(need(a, "folder"));
            QDir().mkpath(folder);
            std::vector<Id> ids;
            std::string err;
            const Id source = l.seq().id;
            if (!makeVersionSequences(l.project, source, shapes, ids, 1, [this](double f) { progress(f * 0.3, "Reframing"); }, nullptr, &err))
                return fail(QString::fromStdString(err));
            save(l);
            QJsonArray files;
            QString text;
            for (size_t i = 0; i < ids.size(); ++i) {
                const Sequence* v = l.project.findSequence(ids[i]);
                const ExportSettings st = versionSettings(*v, preset->settings, folder.toStdString(), a.value("captions").toBool(true),
                                                          a.value("loudness_lufs").toDouble(-14));
                const double base = 0.3 + 0.7 * double(i) / double(ids.size()), span = 0.7 / double(ids.size());
                if (!exportSequence(l.project, *v, st, [&](double f, FrameTime) { progress(base + span * f, "Rendering"); }, nullptr, &err))
                    return fail(QStringLiteral("%1: %2").arg(QString::fromStdString(v->name), QString::fromStdString(err)));
                files.append(QJsonObject{{"shape", QString::fromStdString(shapes[i].label)}, {"sequence", QString::fromStdString(v->name)},
                                         {"path", QString::fromStdString(st.path)}, {"width", v->width}, {"height", v->height}});
                text += QStringLiteral("Wrote %1 (%2 x %3)\n").arg(QString::fromStdString(st.path)).arg(v->width).arg(v->height);
            }
            return ok(text.trimmed(), QJsonObject{{"files", files}});
        });

    add("montage_export_review", "Export for review",
        "Send the cut out for notes without a review service (Frame.io's role): renders an H.264 review copy "
        "(\"<name> - Review.mp4\", at most `max_height` lines, default 1080; timecode burned in unless `timecode` is "
        "false; an optional `watermark` text; captions burned in) into `folder` with \"<name> - Review.html\", a "
        "self-contained page that plays it in any browser, offline, where reviewers step to a frame or mark a range and "
        "type notes, then save them as a notes file. montage_import_markers turns that file into markers coloured by "
        "reviewer. `note` is a message shown to the reviewers; the sequence's markers are shown unless `markers` is "
        "false; `in`/`out` (timecodes) limit it to a range. Returns both paths.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"folder":{"type":"string"},
            "max_height":{"type":"integer","default":1080},"timecode":{"type":"boolean","default":true},
            "watermark":{"type":"string"},"note":{"type":"string"},"markers":{"type":"boolean","default":true},
            "in":{"type":"string"},"out":{"type":"string"}},"required":["project","folder"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            const Sequence& s = l.seq();
            if (s.duration() == 0) return fail("The sequence is empty");
            ReviewExportOptions o;
            o.maxHeight = a.value("max_height").toInt(1080);
            o.timecode = a.value("timecode").toBool(true);
            o.watermark = str(a, "watermark").toStdString();
            o.note = str(a, "note").toStdString();
            o.markers = a.value("markers").toBool(true);
            if (a.contains("in")) o.in = timeArg(a.value("in"), s, "in");
            if (a.contains("out")) o.out = timeArg(a.value("out"), s, "out");
            if (o.in >= 0 && o.out >= 0 && o.out <= o.in) throw ArgError{"\"out\" comes after \"in\""};
            const QString folder = absolute(need(a, "folder"));
            QDir().mkpath(folder);
            const ReviewPackage pkg = reviewPackage(s, folder.toStdString(), o);
            std::string err;
            if (!exportSequence(l.project, s, pkg.settings, [this](double f, FrameTime) { progress(f, "Rendering"); }, nullptr, &err))
                return fail(QString::fromStdString(err));
            if (!writeReviewPage(pkg, &err)) return fail(QString::fromStdString(err));
            return ok(QStringLiteral("Wrote %1 and %2: send both; the notes saved from the page import as markers.")
                          .arg(QString::fromStdString(pkg.videoPath), QString::fromStdString(pkg.pagePath)),
                      QJsonObject{{"video", QString::fromStdString(pkg.videoPath)}, {"page", QString::fromStdString(pkg.pagePath)},
                                  {"width", pkg.page.width}, {"height", pkg.page.height}, {"frames", double(pkg.page.frames)}});
        });

    add("montage_export_dcp", "Export a DCP",
        "Make a Digital Cinema Package of the active sequence for cinemas and festivals: SMPTE (Bv2.1), 2K JPEG 2000 "
        "pictures in DCI X'Y'Z' in the Flat, Scope or full container (the frame fitted on black), 24-bit 48 kHz sound in "
        "5.1 (7.1 DS from a 7.1 sequence; a stereo mix on left and right), a composition playlist, packing list with "
        "hashes and asset map, in a folder under `folder` named by the Digital Cinema Naming Convention. 23.976 plays at "
        "24. The package is checked when done (as montage_verify_dcp); returns its name, folder and anything the check found.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"folder":{"type":"string","description":"Where to make the DCP folder"},
            "title":{"type":"string"},"kind":{"type":"string","enum":["feature","short","trailer","teaser","advertisement","test","rating","psa"],"default":"feature"},
            "container":{"type":"string","enum":["flat","scope","full"],"description":"Default: Scope for sequences 2:1 or wider, else Flat"},
            "fps":{"type":"integer","enum":[24,25,30,48],"description":"Default: the sequence's, to the nearest"},
            "language":{"type":"string","default":"en"},"territory":{"type":"string","default":"XX"},
            "issuer":{"type":"string"},"studio":{"type":"string"},"facility":{"type":"string"},
            "in_out":{"type":"boolean","default":false,"description":"Only In to Out"}},"required":["project","folder"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            const Sequence& s = l.seq();
            if (s.duration() == 0) return fail("The sequence is empty");
            DcpSettings st;
            st.title = str(a, "title", QFileInfo(need(a, "project")).completeBaseName()).toStdString();
            st.kind = str(a, "kind", "feature").toStdString();
            st.container = str(a, "container", QString::fromStdString(defaultDcpContainer(s))).toStdString();
            st.fps = a.value("fps").toInt(0);
            st.language = str(a, "language", "en").toStdString();
            st.territory = str(a, "territory", "XX").toStdString();
            st.issuer = str(a, "issuer", "Montage").toStdString();
            st.studio = str(a, "studio").toStdString();
            st.facility = str(a, "facility").toStdString();
            st.inOut = a.value("in_out").toBool();
            int cw = 0, ch = 0;
            if (!dcpContainer(st.container, cw, ch)) throw ArgError{"container is flat, scope or full"};
            const QString folder = absolute(need(a, "folder"));
            QDir().mkpath(folder);
            DcpResult r;
            std::string err;
            if (!exportDcp(l.project, s, st, folder.toStdString(), &r, [this](double f) {
                    progress(f, "Making the DCP");
                    return true;
                }, &err))
                return fail(QString::fromStdString(err));
            QJsonArray found;
            for (const std::string& i : verifyDcp(r.folder)) found.append(QString::fromStdString(i));
            const QJsonObject out{{"name", QString::fromStdString(r.name)}, {"folder", QString::fromStdString(r.folder)},
                                  {"cpl", QString::fromStdString(r.cpl)}, {"frames", double(r.frames)}, {"fps", r.fps},
                                  {"width", r.width}, {"height", r.height}, {"channels", r.channels},
                                  {"dci_profile", r.cinemaProfile}, {"problems", found}};
            return ok(found.isEmpty() ? QStringLiteral("Made %1 and checked it: no problems").arg(QString::fromStdString(r.name))
                                      : QStringLiteral("Made %1; the check found %2 problem(s)").arg(QString::fromStdString(r.name)).arg(found.size()),
                      out);
        });

    add("montage_verify_dcp", "Check a DCP",
        "Check a Digital Cinema Package folder as a server or festival would: asset map and volume index, every file "
        "present at its size, packing list hashes, each composition's assets and durations, picture track files readable "
        "as JPEG 2000 X'Y'Z' at a DCI size with the right number of frames and within 250 Mbit/s, sound 24-bit 48 kHz. "
        "Returns the problems found (none: it passed).",
        R"json({"type":"object","properties":{"folder":{"type":"string"}},"required":["folder"]})json", true, [](const QJsonObject& a) {
            const QString folder = absolute(need(a, "folder"));
            QJsonArray found;
            for (const std::string& i : verifyDcp(folder.toStdString())) found.append(QString::fromStdString(i));
            return ok(found.isEmpty() ? QStringLiteral("The DCP checks out") : QStringLiteral("%1 problem(s): %2").arg(found.size()).arg(found.at(0).toString()),
                      QJsonObject{{"problems", found}, {"ok", found.isEmpty()}});
        });

    add("montage_export_imf", "Export an IMF master",
        "Make an IMF (Interoperable Master Format) package of the active sequence, as Netflix, Amazon, Disney+ and "
        "broadcasters ask for: Application #2E (SMPTE ST 2067-21:2021), JPEG 2000 pictures in the IMF profiles (lossless "
        "by default) as full-range RGB 4:4:4 of 10 or 12 bits in Rec.709, P3-D65 PQ, Rec.2020 PQ or Rec.2020 HLG (HDR "
        "with mastering display metadata), 24-bit 48 kHz sound in stereo, 5.1 or 7.1 DS with multichannel labels, a "
        "composition playlist with the essence descriptors, packing list with hashes and asset map, in a new folder under "
        "`folder`. The package is checked when done (as montage_verify_imf).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"folder":{"type":"string","description":"Where to make the IMF folder"},
            "title":{"type":"string"},"kind":{"type":"string","enum":["feature","episode","short","trailer","teaser","advertisement","promotion","test"],"default":"feature"},
            "colour":{"type":"string","enum":["rec709","p3d65-pq","rec2020-pq","rec2020-hlg"],"description":"Default: as the sequence is graded"},
            "size":{"type":"string","enum":["sequence","hd","uhd","4k"],"default":"sequence"},
            "bits":{"type":"integer","enum":[10,12],"description":"Default: 10 for SDR, 12 for HDR"},
            "lossless":{"type":"boolean","default":true},"megabits_per_second":{"type":"number","description":"Lossy: the cap (default 400)"},
            "mastering_peak":{"type":"number","description":"HDR: the mastering display's peak, cd/m^2"},
            "language":{"type":"string","default":"en"},"issuer":{"type":"string"},
            "in_out":{"type":"boolean","default":false,"description":"Only In to Out"}},"required":["project","folder"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            const Sequence& s = l.seq();
            if (s.duration() == 0) return fail("The sequence is empty");
            ImfSettings st;
            st.title = str(a, "title", QFileInfo(need(a, "project")).completeBaseName()).toStdString();
            st.kind = str(a, "kind", "feature").toStdString();
            st.colour = str(a, "colour").toStdString();
            st.size = str(a, "size", "sequence").toStdString();
            st.bits = a.value("bits").toInt(0);
            st.lossless = a.value("lossless").toBool(true);
            st.megabitsPerSecond = a.value("megabits_per_second").toDouble(400);
            st.masteringPeak = a.value("mastering_peak").toDouble(0);
            st.language = str(a, "language", "en").toStdString();
            st.issuer = str(a, "issuer", "Montage").toStdString();
            st.inOut = a.value("in_out").toBool();
            const QString folder = absolute(need(a, "folder"));
            QDir().mkpath(folder);
            ImfResult r;
            std::string err;
            if (!exportImf(l.project, s, st, folder.toStdString(), &r, [this](double f) {
                    progress(f, "Making the IMF package");
                    return true;
                }, &err))
                return fail(QString::fromStdString(err));
            QJsonArray found;
            for (const std::string& i : verifyImf(r.folder)) found.append(QString::fromStdString(i));
            const QJsonObject out{{"folder", QString::fromStdString(r.folder)}, {"cpl", QString::fromStdString(r.cpl)},
                                  {"frames", double(r.frames)}, {"edit_rate", QStringLiteral("%1/%2").arg(r.rateNum).arg(r.rateDen)},
                                  {"width", r.width}, {"height", r.height}, {"bits", r.bits}, {"channels", r.channels},
                                  {"colour", QString::fromStdString(r.colour)}, {"rsiz", r.rsiz}, {"problems", found}};
            const QString name = QFileInfo(QString::fromStdString(r.folder)).fileName();
            return ok(found.isEmpty() ? QStringLiteral("Made %1 and checked it: no problems").arg(name)
                                      : QStringLiteral("Made %1; the check found %2 problem(s)").arg(name).arg(found.size()),
                      out);
        });

    add("montage_verify_imf", "Check an IMF package",
        "Check an IMF package folder: asset map, every file present at its size, packing list hashes, each composition's "
        "resources in the package with their essence descriptors and durations, the Application #2E identification, "
        "picture track files JPEG 2000 in an IMF profile with the right number of frames, sound 24-bit 48 kHz with the "
        "right number of samples. Returns the problems found (none: it passed).",
        R"json({"type":"object","properties":{"folder":{"type":"string"}},"required":["folder"]})json", true, [](const QJsonObject& a) {
            const QString folder = absolute(need(a, "folder"));
            QJsonArray found;
            for (const std::string& i : verifyImf(folder.toStdString())) found.append(QString::fromStdString(i));
            return ok(found.isEmpty() ? QStringLiteral("The IMF package checks out") : QStringLiteral("%1 problem(s): %2").arg(found.size()).arg(found.at(0).toString()),
                      QJsonObject{{"problems", found}, {"ok", found.isEmpty()}});
        });

    add("montage_render", "Render",
        "Render the active sequence (or its in-out range) to a file with an export preset (default \"H.264 - High Quality\"), "
        "optionally normalising the mix's loudness for where it is going. Broadcast deliveries: the XDCAM HD422, AVC-Intra 100 "
        "and DNxHR MXF presets write MXF OP1a with each sound channel a mono track; start_timecode sets the file's timecode.",
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
                "description":"Also write 24-bit WAV stems beside the output, one per audio track, per bus (plus Main) or per audio role"},
            "captions":{"type":"string","enum":["none","burn","embed","both"],"default":"none","description":"The visible caption track, burned into the picture and/or embedded as a subtitle stream"},
            "cea608":{"type":"boolean","default":false,"description":"Also carry the caption track as CEA-608 closed captions inside H.264/HEVC video (A/53, as US broadcast and streaming deliveries ask)"},
            "all_captions":{"type":"boolean","default":false,"description":"With embed: every caption track as its own subtitle stream, language tagged"},
            "audio_streams":{"description":"More audio streams after the mix (a master's M&E, dialogue, dubs): \"roles\" (one per role), \"tracks\" (one per track), or a list of {name, language, tracks:[\"A2\",...], role}",
                "anyOf":[{"type":"string","enum":["mix","roles","tracks"]},{"type":"array","items":{"type":"object","properties":{
                    "name":{"type":"string"},"language":{"type":"string"},"tracks":{"type":"array","items":{"type":"string"}},"role":{"type":"string"}}}}]},
            "audio_name":{"type":"string","description":"The mix stream's title"},"audio_language":{"type":"string","description":"The mix stream's language (ISO 639-1)"},
            "start_timecode":{"type":"string","description":"The file's starting timecode (MXF and MOV), e.g. 10:00:00:00 as broadcasters ask"},
            "mono_tracks":{"type":"integer","description":"Write the mix (then each audio stream) as mono tracks, padded with silence to this many, as broadcast MXF takes it; the MXF presets set 8, 4 or 2"}},
            "required":["project","output"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const ExportPreset* pr = findExportPreset(str(a, "preset", "H.264 - High Quality").toStdString());
            if (!pr) throw ArgError{"Unknown preset (see montage_list_presets)"};
            ExportSettings st = pr->settings;
            st.path = absolute(need(a, "output")).toStdString();
            if (a.contains("start_timecode")) {
                FrameTime t = 0;
                const std::string tc = str(a, "start_timecode").toStdString();
                if (!parseTimecode(tc, s.fps, t) || t < 0) throw ArgError{"\"start_timecode\" is a timecode like 10:00:00:00"};
                st.startTimecode = formatTimecode(t, s.fps);
            }
            if (a.contains("mono_tracks")) {
                st.monoAudioTracks = a.value("mono_tracks").toInt();
                if (st.monoAudioTracks < 0 || st.monoAudioTracks > 64) throw ArgError{"\"mono_tracks\" is 0 to 64"};
            }
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
            const QString cap = str(a, "captions", "none");
            if (cap != "none" && cap != "burn" && cap != "embed" && cap != "both") throw ArgError{"\"captions\" must be none, burn, embed or both"};
            st.burnInCaptions = cap == "burn" || cap == "both";
            st.cea608 = a.value("cea608").toBool();
            st.embedCaptions = cap == "embed" || cap == "both";
            if (st.embedCaptions && a.value("all_captions").toBool())
                for (const CaptionTrack& t : s.captionTracks) st.extraCaptions.push_back(t.id);
            st.audioName = a.value("audio_name").toString().toStdString();
            st.audioLanguage = a.value("audio_language").toString().toStdString();
            const QJsonValue streams = a.value("audio_streams");
            if (streams.isString() && streams.toString() != "mix") {
                if (streams.toString() != "roles" && streams.toString() != "tracks") throw ArgError{"\"audio_streams\" is mix, roles, tracks or a list"};
                st.extraAudio = stemStreams(s, streams.toString() == "roles" ? StemsByRole : StemsByTrack);
            } else if (streams.isArray()) {
                for (const QJsonValue& v : streams.toArray()) {
                    const QJsonObject o = v.toObject();
                    ExportSettings::AudioStream as;
                    as.name = o.value("name").toString().toStdString();
                    as.language = o.value("language").toString().toStdString();
                    as.role = o.value("role").toString().toStdString();
                    if (o.contains("tracks")) {
                        as.tracks.assign(s.audioTracks.size(), false);
                        for (const QJsonValue& t : o.value("tracks").toArray()) {
                            const QString name = t.toString().trimmed().toUpper();
                            const int i = name.startsWith('A') ? name.mid(1).toInt() - 1 : -1;
                            if (i < 0 || i >= int(s.audioTracks.size())) throw ArgError{QStringLiteral("No audio track %1").arg(t.toString())};
                            as.tracks[size_t(i)] = true;
                        }
                    }
                    st.extraAudio.push_back(as);
                }
            }
            if (!st.extraAudio.empty() && st.audioName.empty()) st.audioName = "Mix";
            const QString stems = str(a, "stems", "none");
            if (stems != "none" && stems != "tracks" && stems != "buses" && stems != "roles")
                throw ArgError{"\"stems\" must be none, tracks, buses or roles"};
            std::string err;
            LightLevels light;
            if (!exportSequence(l.project, s, st, [this](double f, FrameTime) { progress(f, "Rendering"); }, nullptr, &err, nullptr, nullptr, &light))
                return fail(QString::fromStdString(err));
            QJsonObject o{{"output", QString::fromStdString(st.path)}};
            QString text = QStringLiteral("Wrote %1").arg(QString::fromStdString(st.path));
            if (light.frames > 0) {  // HDR: what was rendered, measured
                unsigned cll = 0, fall = 0;
                hdr10LightLevels(light, cll, fall);
                o["max_cll"] = int(cll), o["max_fall"] = int(fall);
                text += QStringLiteral("\nMaxCLL %1 nits, MaxFALL %2 nits").arg(cll).arg(fall);
            }
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

    add("montage_animate_to_audio", "Animate to audio",
        "Make a clip's setting follow the sound (Resolve's Fairlight animator, After Effects' Convert Audio to "
        "Keyframes): keys through the clip on `param` of the effect `effect` (an effect id; omitted = the clip's "
        "Transform: scale, opacity, rotation, pos_x, pos_y...) from `low` when the audio track `track` (1-based, 0 = "
        "all) is silent to `high` at its loudest under the clip, by its level in a `band` (all, low, mid, high), "
        "thinned to the keys needed. For an audiogram add the generator audio_viz with montage_add_effect instead.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},"effect":{"type":"number"},
            "param":{"type":"string"},"track":{"type":"integer","default":1},"band":{"type":"string","enum":["all","low","mid","high"],"default":"all"},
            "low":{"type":"number"},"high":{"type":"number"}},"required":["project","clip","param","low","high"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Id id = clipArg(l, a).id;
            static const QStringList bands = {"all", "low", "mid", "high"};
            const int band = int(bands.indexOf(str(a, "band", "all")));
            if (band < 0) throw ArgError{"band is all, low, mid or high"};
            const std::string param = need(a, "param").toStdString();
            check(edit::animateToAudio(l.project, l.seq(), id, Id(a.value("effect").toDouble(0)), param, a.value("track").toInt(1),
                                       AudioBand(band), a.value("low").toDouble(), a.value("high").toDouble()));
            save(l);
            const Clip* c = edit::clipById(l.seq(), id);
            const Effect* e = nullptr;
            if (!a.contains("effect")) e = &c->motion;
            else
                for (const Effect& x : c->effects)
                    if (x.id == Id(a.value("effect").toDouble())) e = &x;
            if (!e && c->generator.id == Id(a.value("effect").toDouble())) e = &c->generator;
            const int keys = e ? int(e->params.at(param).keys.size()) : 0;
            return ok(QStringLiteral("%1 follows the sound with %2 keys").arg(QString::fromStdString(param)).arg(keys), QJsonObject{{"keys", keys}});
        });

    add("montage_audio_channels", "Audio channels",
        "Which of a file's audio channels a clip plays (Premiere's Modify > Audio Channels), for a lav and a boom on one "
        "camera, a field recorder's polyphonic WAV or an MXF with a stream per channel. `action`: list (the clip's "
        "source channels, with the recorder's names, and those it plays), set (`channels`, 1-based; one plays in the "
        "centre, two as left and right), mix (back to the stereo mix), split (a mono clip per channel on the tracks "
        "below, linked) or split_pairs (a stereo clip per pair). A picture clip acts on its linked sound. With "
        "`media` and `mode` (mix, mono, pairs) instead, sets how new clips of that media take its channels.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "action":{"type":"string","enum":["list","set","mix","split","split_pairs"]},
            "channels":{"type":"array","items":{"type":"integer"}},
            "media":{"type":"number"},"mode":{"type":"string","enum":["mix","mono","pairs"]}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            if (a.contains("media")) {
                MediaItem* m = l.project.findMedia(Id(a.value("media").toDouble()));
                if (!m) throw ArgError{"No such media"};
                const QString mode = str(a, "mode", "mix");
                if (mode != "mix" && mode != "mono" && mode != "pairs") throw ArgError{"mode is mix, mono or pairs"};
                if (sourceChannelCount(*m) < 2) return fail(QStringLiteral("%1 has one channel").arg(QString::fromStdString(m->name)));
                m->audioChannelMode = mode == "mix" ? std::string() : mode.toStdString();
                save(l);
                return ok(QStringLiteral("New clips of %1 take %2").arg(QString::fromStdString(m->name),
                                                                         mode == "mix" ? "the stereo mix" : mode == "mono" ? "a clip per channel" : "a clip per pair"),
                          QJsonObject{{"mode", mode}});
            }
            const Id id = clipArg(l, a).id;
            const QString action = str(a, "action", "list");
            edit::Result r;
            if (action == "set") {
                std::vector<int> channels;
                for (const QJsonValue& v : a.value("channels").toArray()) channels.push_back(v.toInt() - 1);
                if (channels.empty()) throw ArgError{"channels lists the channels to play, from 1"};
                r = edit::setClipChannels(l.project, l.seq(), id, channels);
            } else if (action == "mix") {
                r = edit::setClipChannels(l.project, l.seq(), id, {});
            } else if (action == "split" || action == "split_pairs") {
                r = edit::splitAudioChannels(l.project, l.seq(), id, action == "split_pairs");
            } else if (action != "list") {
                throw ArgError{QStringLiteral("Unknown action \"%1\"").arg(action)};
            }
            if (!r.ok) return fail(r.error.empty() ? QStringLiteral("Nothing to change") : QString::fromStdString(r.error));
            if (action != "list") save(l);
            // The clip's sound clips (with any made by a split), and what each plays.
            QJsonArray clips;
            QStringList lines;
            std::vector<Id> sounds;
            for (Id x : edit::linkedClips(l.seq(), id))
                if (const auto loc = edit::locate(l.seq(), x); loc && loc->track.kind == TrackKind::Audio) sounds.push_back(x);
            QJsonArray names;
            for (Id x : sounds) {
                const Clip* c = edit::clipById(l.seq(), x);
                const MediaItem* m = l.project.findMedia(c->mediaId);
                if (!m) continue;
                if (names.isEmpty())
                    for (const std::string& n : sourceChannelNames(*m)) names.append(QString::fromStdString(n));
                QJsonArray playing;
                for (int ch : c->channels) playing.append(ch + 1);
                const std::string label = channelsLabel(*m, c->channels);
                clips.append(QJsonObject{{"id", double(c->id)}, {"name", QString::fromStdString(c->name)},
                                         {"track", edit::locate(l.seq(), x)->track.index + 1}, {"channels", playing}});
                lines << QStringLiteral("A%1 %2: %3").arg(edit::locate(l.seq(), x)->track.index + 1).arg(QString::fromStdString(c->name),
                                                              label.empty() ? QStringLiteral("stereo mix") : QString::fromStdString(label));
            }
            if (clips.isEmpty()) return fail("The clip has no sound");
            QJsonArray created;
            for (Id x : r.created) created.append(double(x));
            return ok(QStringLiteral("Source channels: %1\n").arg(QJsonDocument(names).toJson(QJsonDocument::Compact).constData()) + lines.join('\n'),
                      QJsonObject{{"source_channels", names}, {"clips", clips}, {"created", created}});
        });

    add("montage_transform", "Position, scale and crop a clip",
        "Set a picture clip's transform, as the Program monitor's on-screen box does: `x` and `y` (pixels from the "
        "frame's centre), `scale`, `scale_x`, `scale_y` (percent), `rotation` (degrees), `opacity` (percent), "
        "`crop_left`/`crop_right`/`crop_top`/`crop_bottom` (percent) and `fit` (fit, fill, stretch, none). With `at` (a "
        "sequence frame inside the clip) the values become keys there, so several calls animate it; otherwise "
        "settings without keys take the value (animated ones are keyed at the clip's start). `align` then lines the "
        "picture up with the frame (center, top, bottom, left, right, top_left, top_right, bottom_left, bottom_right), "
        "`inset` (a share of the frame's height, default 0.05) in from the edges, for picture-in-picture, logos and "
        "lower thirds. Returns the picture's corners in the frame.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},"at":{"type":"integer"},
            "x":{"type":"number"},"y":{"type":"number"},"scale":{"type":"number"},"scale_x":{"type":"number"},"scale_y":{"type":"number"},
            "rotation":{"type":"number"},"opacity":{"type":"number"},"crop_left":{"type":"number"},"crop_right":{"type":"number"},
            "crop_top":{"type":"number"},"crop_bottom":{"type":"number"},"fit":{"type":"string","enum":["fit","fill","stretch","none"]},
            "align":{"type":"string","enum":["center","top","bottom","left","right","top_left","top_right","bottom_left","bottom_right"]},
            "inset":{"type":"number","default":0.05},
            "remove_letterbox":{"type":"boolean","default":false,"description":"Find black bars baked into the picture (letterbox, pillarbox), crop them off and scale what is left to fill the frame"}},"required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            Clip& c = clipArg(l, a);
            const Id id = c.id;
            const auto loc = edit::locate(s, id);
            if (!loc || loc->track.kind != TrackKind::Video) throw ArgError{"Only picture clips have a transform"};
            FrameTime t = c.start;
            const bool keyed = a.contains("at");
            if (keyed) {
                t = FrameTime(a.value("at").toInteger());
                if (t < c.start || t >= c.end()) throw ArgError{"at is a frame inside the clip"};
            }
            if (c.motion.empty()) c.motion = makeEffect(l.project, "transform");
            const FrameTime lt = t - c.start;
            static const std::pair<const char*, const char*> fields[] = {
                {"x", "pos_x"},           {"y", "pos_y"},           {"scale", "scale"},       {"scale_x", "scale_x"},
                {"scale_y", "scale_y"},   {"rotation", "rotation"}, {"opacity", "opacity"},   {"crop_left", "crop_left"},
                {"crop_right", "crop_right"}, {"crop_top", "crop_top"}, {"crop_bottom", "crop_bottom"}};
            int changed = 0;
            for (const auto& [arg, param] : fields) {
                if (!a.contains(arg)) continue;
                double v = a.value(arg).toDouble();
                if (std::string(param).rfind("crop", 0) == 0 || std::string(param) == "opacity") v = std::clamp(v, 0.0, 100.0);
                if (std::string(param).rfind("scale", 0) == 0) v = std::max(0.0, v);
                Param& prm = c.motion.params[param];
                if (keyed) prm.addKey(lt, v, Interp::Smooth);
                else prm.set(lt, v);
                ++changed;
            }
            if (a.contains("fit")) {
                static const QStringList fits = {"fit", "fill", "stretch", "none"};
                const int k = int(fits.indexOf(a.value("fit").toString()));
                if (k < 0) throw ArgError{"fit is fit, fill, stretch or none"};
                c.motion.params["fit"] = Param(double(k));
                ++changed;
            }
            if (a.contains("align")) {
                Align where;
                if (!parseAlign(a.value("align").toString().toStdString(), where)) throw ArgError{"Unknown align"};
                if (keyed) {
                    // A key at `at` first, so the move lands there only.
                    for (const char* p : {"pos_x", "pos_y"}) c.motion.params[p].addKey(lt, c.motion.p(p, lt), Interp::Smooth);
                }
                check(edit::alignClip(l.project, s, id, t, where, a.value("inset").toDouble(0.05)));
                ++changed;
            }
            QJsonObject found;
            if (a.value("remove_letterbox").toBool()) {
                const MediaItem* m = c.mediaId ? l.project.findMedia(c.mediaId) : nullptr;
                if (!m || m->kind != MediaKind::Video) throw ArgError{"remove_letterbox needs a video clip"};
                const double fps = s.fpsValue();
                const double from = std::min(c.sourceAt(0), c.sourceAt(double(c.duration))) / fps, to = std::max(c.sourceAt(0), c.sourceAt(double(c.duration))) / fps;
                Bars bars;
                std::string err;
                if (!detectBars(m->path, from, to, bars, &err)) return fail(QString::fromStdString(err));
                found = QJsonObject{{"left", bars.left}, {"right", bars.right}, {"top", bars.top}, {"bottom", bars.bottom}};
                if (bars.any()) {
                    check(edit::removeLetterbox(l.project, s, id, bars));
                    ++changed;
                } else if (!changed) {
                    return ok("No black bars found round the picture", QJsonObject{{"bars", found}});
                }
            }
            if (!changed) throw ArgError{"Give a setting to change, or align"};
            save(l);
            std::array<double, 4> xs, ys;
            QJsonArray corners;
            if (clipFrameQuad(l.project, s, *edit::clipById(s, id), t, xs, ys))
                for (int k = 0; k < 4; ++k) corners.append(QJsonArray{std::round(xs[size_t(k)] * 10) / 10, std::round(ys[size_t(k)] * 10) / 10});
            QJsonObject res{{"corners", corners}};
            if (!found.isEmpty()) res["bars"] = found;
            return ok(QStringLiteral("The picture lies at %1").arg(QString::fromUtf8(QJsonDocument(corners).toJson(QJsonDocument::Compact))), res);
        });

    add("montage_reframe_360", "Reframe 360° video",
        "Work with 360° (equirectangular) footage, as GoPro's Reframe and Insta360 Studio do. With `clip`: aim its "
        "Reframe 360° view (added if missing): `yaw` (degrees right), `pitch` (degrees up), `roll`, `fov` (degrees "
        "across; flat up to 170, little planet and tunnel up to 330) and `projection` (flat, little_planet, tunnel); "
        "with `at` (a sequence frame inside the clip) the angles become smooth keys there, so several calls make a "
        "camera move. With `media` and `is_360`: mark footage as 360° or flat (360° footage placed in a flat sequence "
        "gets the view at once). With `sequence_360`: make the active sequence a 360° one, whose exports carry "
        "spherical metadata for players and YouTube.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "yaw":{"type":"number"},"pitch":{"type":"number"},"roll":{"type":"number"},"fov":{"type":"number"},
            "projection":{"type":"string","enum":["flat","little_planet","tunnel"]},"at":{"type":"integer"},
            "media":{"type":"number"},"is_360":{"type":"boolean"},"sequence_360":{"type":"boolean"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            QStringList done;
            if (a.contains("media")) {
                MediaItem* m = l.project.findMedia(Id(a.value("media").toDouble()));
                if (!m || !m->hasVideo || m->kind == MediaKind::Sequence) throw ArgError{"No such picture media"};
                m->projection = a.value("is_360").toBool(true) ? "equirect" : "";
                done << QStringLiteral("%1 is %2").arg(QString::fromStdString(m->name), m->projection.empty() ? "flat" : "360° footage");
            }
            if (a.contains("sequence_360")) {
                l.seq().spherical = a.value("sequence_360").toBool();
                done << (l.seq().spherical ? QStringLiteral("The sequence is 360°") : QStringLiteral("The sequence is flat"));
            }
            QJsonObject view;
            if (a.contains("clip")) {
                const Clip& c = clipArg(l, a);
                const Id id = c.id;
                ReframeView v;
                if (a.contains("yaw")) v.yaw = a.value("yaw").toDouble();
                if (a.contains("pitch")) v.pitch = a.value("pitch").toDouble();
                if (a.contains("roll")) v.roll = a.value("roll").toDouble();
                if (a.contains("fov")) v.fov = a.value("fov").toDouble();
                if (a.contains("projection")) {
                    static const QStringList names = {"flat", "little_planet", "tunnel"};
                    const int k = int(names.indexOf(a.value("projection").toString()));
                    if (k < 0) throw ArgError{"projection is flat, little_planet or tunnel"};
                    v.projection = SphereView(k);
                }
                FrameTime key = -1;
                if (a.contains("at")) {
                    key = FrameTime(a.value("at").toInteger()) - c.start;
                    if (key < 0 || key >= c.duration) throw ArgError{"at is a frame inside the clip"};
                }
                check(edit::setReframe360(l.project, l.seq(), id, v, key));
                const Clip* after = edit::clipById(l.seq(), id);
                for (const Effect& e : after->effects)
                    if (e.type == "reframe_360") {
                        const FrameTime t = std::max<FrameTime>(0, key);
                        view = QJsonObject{{"yaw", e.p("yaw", t)}, {"pitch", e.p("pitch", t)}, {"roll", e.p("roll", t)},
                                           {"fov", e.p("fov", t, 100)}, {"keys", int(e.params.count("yaw") ? e.params.at("yaw").keys.size() : 0)}};
                        done << QStringLiteral("View: yaw %1°, pitch %2°, fov %3°%4").arg(e.p("yaw", t)).arg(e.p("pitch", t)).arg(e.p("fov", t, 100))
                                    .arg(key >= 0 ? QStringLiteral(" (key at frame %1)").arg(c.start + key) : QString());
                    }
            }
            if (done.isEmpty()) throw ArgError{"Give clip, media or sequence_360"};
            save(l);
            return ok(done.join('\n'), QJsonObject{{"view", view}});
        });

    add("montage_vfx_pull", "VFX pulls",
        "Pull shots for visual effects: each clip's source frames, untouched, at the footage's own size and rate, with "
        "`handles` frames either side (default 8, as far as the footage goes), as an image sequence in a folder per shot "
        "(format exr: half float in scene-linear light; dpx: 10-bit in the footage's own colour; tiff: 16-bit), numbered "
        "so the cut's first frame is `cut_in` (default 1001), with a pull_list.csv. `clips` are clip ids (default: every "
        "footage clip on the video tracks).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"folder":{"type":"string"},
            "clips":{"type":"array","items":{"type":"number"}},"format":{"type":"string","enum":["exr","dpx","tiff"],"default":"exr"},
            "handles":{"type":"integer","default":8},"cut_in":{"type":"integer","default":1001}},"required":["project","folder"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            VfxPullOptions o;
            o.folder = absolute(need(a, "folder")).toStdString();
            o.format = str(a, "format", "exr").toStdString();
            o.handles = std::clamp(a.value("handles").toInt(8), 0, 1000);
            o.cutIn = a.value("cut_in").toInt(1001);
            std::vector<Id> clips;
            for (const QJsonValue& v : a.value("clips").toArray()) clips.push_back(Id(v.toDouble()));
            if (clips.empty())
                for (const Track& t : l.seq().videoTracks)
                    for (const Clip& c : t.clips) clips.push_back(c.id);
            std::vector<VfxShot> shots;
            std::string err;
            if (!exportVfxPulls(l.project, l.seq(), clips, o, &shots, [this](double f, FrameTime) { progress(f, "Pulling"); }, nullptr, &err))
                return fail(QString::fromStdString(err));
            QJsonArray list;
            QStringList lines;
            for (const VfxShot& s : shots) {
                list.append(QJsonObject{{"name", QString::fromStdString(s.name)}, {"folder", QString::fromStdString(s.folder)},
                                        {"first_frame", s.firstFrame}, {"cut_in", s.cutIn}, {"cut_out", s.cutOut}, {"last_frame", s.lastFrame},
                                        {"head_handle", s.headHandle}, {"tail_handle", s.tailHandle}});
                lines << QStringLiteral("%1: frames %2-%3 (cut %4-%5)").arg(QString::fromStdString(s.name)).arg(s.firstFrame).arg(s.lastFrame).arg(s.cutIn).arg(s.cutOut);
            }
            const QString csv = QDir(QString::fromStdString(o.folder)).filePath("pull_list.csv");
            return ok(QStringLiteral("Pulled %1 shots:\n%2\nPull list: %3").arg(shots.size()).arg(lines.join('\n'), csv),
                      QJsonObject{{"shots", list}, {"pull_list", csv}});
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
