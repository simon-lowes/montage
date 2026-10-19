#include "McpServer.h"

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
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
#include "core/GradeGraph.h"
#include "core/AudioChannels.h"
#include "core/TranscriptCorrect.h"
#include "core/SpellCheck.h"
#include "core/ColorGroups.h"
#include "core/GraphicsTemplate.h"
#include "core/Storyboard.h"
#include "render/ExtendClip.h"
#include "render/ColorChart.h"
#include "render/ReviewExport.h"
#include "render/RoomTone.h"
#include "render/Versions.h"
#include "render/Dcp.h"
#include "render/Adm.h"
#include "render/Iamf.h"
#include "render/Imf.h"
#include "render/BroadcastMxf.h"
#include "render/LightLevel.h"
#include "render/Hdr10Plus.h"
#include "render/Spherical.h"
#include "render/ClipPlacement.h"
#include "audio/DialogueAlign.h"
#include "audio/SpeechCleanup.h"
#include "core/AafImport.h"
#include "core/Provenance.h"
#include "media/ContentCredentials.h"
#include "media/Offload.h"
#include "media/Recover.h"
#include "media/Proxy.h"
#include "media/SpeechSearch.h"
#include "media/TextReader.h"
#include "media/ImageSequence.h"
#include "media/Interpret.h"
#include "media/Pulldown.h"
#include "render/RateConversion.h"
#include "media/MediaPool.h"
#include "render/Ofx.h"
#include "media/Psd.h"
#include "media/Exr.h"
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
#include "core/Ale.h"
#include "core/Cdl.h"
#include "core/EditOps.h"
#include "core/TimelineCompare.h"
#include "core/Reconform.h"
#include "core/Adr.h"
#include "core/AudioDescription.h"
#include "core/Effects.h"
#include "core/Expression.h"
#include "core/History.h"
#include "core/Interchange.h"
#include "core/MediaLog.h"
#include "core/ProjectIO.h"
#include "core/ScriptCut.h"
#include "render/LinedScript.h"
#include "core/Ambisonics.h"
#include "media/Sofa.h"
#include "core/Surround.h"
#include "core/TranscriptEdit.h"
#include "media/DualSystem.h"
#include "media/Relink.h"
#include "media/SpeechEnhance.h"
#include "media/LipSync.h"
#include "media/MotionDeblur.h"
#include "media/SuperScale.h"
#include "media/Translator.h"
#include "render/AudioReactive.h"
#include "render/Expressions.h"
#include "render/VfxPull.h"
#include "render/Amf.h"
#include "render/AafExport.h"
#include "render/AutoMix.h"
#include "render/AutoBroll.h"
#include "render/Highlights.h"
#include "render/MusicEdit.h"
#include "render/DialogueMatch.h"
#include "render/VoiceMatch.h"
#include "media/Analysis.h"
#include "media/AutoDuck.h"
#include "media/MicBleed.h"
#include "core/Slate.h"
#include "render/PaperEdit.h"
#include "render/LutExport.h"
#include "render/ProjectManager.h"
#include "render/QualityCheck.h"
#include "render/ConformCheck.h"
#include "media/Decoder.h"
#include "media/Faces.h"
#include "media/FaceMesh.h"
#include "media/FaceTracks.h"
#include "media/DepthMap.h"
#include "media/VideoDepth.h"
#include "render/FollowFocus.h"
#include "media/Matting.h"
#include "media/Inpaint.h"
#include "media/Rife.h"
#include "media/TextToSpeech.h"
#include "media/VoiceClone.h"
#include "media/SoundSearch.h"
#include "media/LtcTimecode.h"
#include "media/StemSeparation.h"
#include "media/Transcriber.h"
#include "media/VisualSearch.h"
#include "render/ClipAnalysis.h"
#include "render/OGraf.h"
#include "render/Gallery.h"
#include "render/DialogueLoudness.h"
#include "render/AutoTrims.h"
#include "render/TrimPasses.h"
#include "media/Segmenter.h"
#include "render/Compositor.h"
#include "render/Exporter.h"
#include "render/RenderFarm.h"
#include "render/Shorts.h"
#include "render/Letterbox.h"
#include "render/Processing.h"
#include "render/Restoration.h"
#include "media/LensCalibration.h"
#include "media/LensDatabase.h"
#include "core/Production.h"
#include "core/ProjectLock.h"

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

// An audio track by reference ("A2") or name, as its id (a sidechain key); "" or "none" for none.
std::string audioTrackId(const QString& ref, const Sequence& s) {
    if (ref.trimmed().isEmpty() || ref.trimmed().compare("none", Qt::CaseInsensitive) == 0) return {};
    for (const Track& t : s.audioTracks)
        if (QString::fromStdString(t.name).compare(ref.trimmed(), Qt::CaseInsensitive) == 0) return std::to_string(t.id);
    const TrackRef r = trackArg(ref, s, false);
    if (r.kind != TrackKind::Audio) throw ArgError{QStringLiteral("That setting names an audio track (A1, A2... or its name)")};
    return std::to_string(s.audioTracks[size_t(r.index)].id);
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
    checkStereoMedia(l.project);
    if (!l.project.active()) throw ArgError{QStringLiteral("%1 has no sequence").arg(l.path)};
    return l;
}

// Saves, keeping the previous version as <project>.bak for montage_undo. A project someone has open to edit in
// Montage (its lock held by another process: core/ProjectLock.h) is not written behind their back.
void save(Loaded& l) {
    if (const LockStatus st = projectLockStatus(l.path.toStdString()); st.state == LockState::Theirs)
        throw ArgError{QStringLiteral("%1 is editing %2 in Montage: make the change through Montage's live agent link, or once they have "
                                      "closed it")
                           .arg(QString::fromStdString(st.owner.describe()), QFileInfo(l.path).fileName())};
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

// A video clip by its id or its name.
const Clip& videoClipArg(Loaded& l, const QJsonObject& a) {
    Sequence& s = l.seq();
    if (a.value("clip").isString()) {
        const std::string name = a.value("clip").toString().toStdString();
        const Clip* found = nullptr;
        for (const Track& t : s.videoTracks)
            for (const Clip& c : t.clips)
                if (c.name == name) {
                    if (found) throw ArgError{QStringLiteral("Several clips are named \"%1\": give its id").arg(QString::fromStdString(name))};
                    found = &c;
                }
        if (!found) throw ArgError{QStringLiteral("No video clip named \"%1\" in the active sequence").arg(QString::fromStdString(name))};
        return *found;
    }
    const Clip& c = clipArg(l, a);
    const auto loc = edit::locate(s, c.id);
    if (!loc || loc->track.kind != TrackKind::Video) throw ArgError{"\"clip\" must be a video clip (its picture, not its sound)"};
    return c;
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
        if (c.generator.type.rfind("title", 0) == 0) o["text"] = QString::fromStdString(c.generator.s("text"));
    } else if (const MediaItem* m = p.findMedia(c.mediaId)) {
        o["media"] = QString::fromStdString(m->path.empty() ? m->name : m->path);
        o["source_in_seconds"] = c.sourceIn / s.fpsValue();
    }
    if (c.speed != 1.0 || c.reverse) o["speed"] = (c.reverse ? -1 : 1) * c.speed;
    if (!c.enabled) o["enabled"] = false;
    if (c.linkGroup) o["linked_group"] = double(c.linkGroup);
    if (!c.role.empty()) o["role"] = QString::fromStdString(c.role);
    if (!c.layer.empty()) o["layer"] = QString::fromStdString(c.layer);
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
    if (std::vector<PulldownStretch> st; parsePulldown(i.pulldown, st) && !st.empty()) {
        // Pulldown removed: the cadence's stretches (a break where an edit after the pulldown changed it).
        QJsonArray stretches;
        for (const PulldownStretch& x : st) {
            static const char* letters[5] = {"A", "B", "C", "D", "D2"};
            QJsonObject so{{"from_frame", double(x.start)}, {"field_order", x.topFirst ? "upper" : "lower"}};
            if (x.position < 0) so["video"] = true;
            else so["starts_on"] = letters[x.position];
            stretches.append(so);
        }
        o["pulldown_removed"] = stretches;
    }
    if (i.rawExposure != 0) o["raw_exposure"] = i.rawExposure;
    if (i.rawTemperature > 0) o["raw_temperature"] = i.rawTemperature;
    if (i.rawTint != 0) o["raw_tint"] = i.rawTint;
    if (!i.rawHighlights.empty()) o["raw_highlights"] = QString::fromStdString(i.rawHighlights);
    if (i.rawHalf) o["raw_half"] = true;
    if (!i.stereo.empty()) o["stereo"] = QString::fromStdString(i.stereo);
    if (i.swapEyes) o["swap_eyes"] = true;
    if (!i.layer.empty()) o["layer"] = QString::fromStdString(i.layer);
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
    for (const Marker& m : s.markers) {
        QJsonObject mo{{"at", tc(m.t, s)}, {"name", QString::fromStdString(m.name)}, {"comment", QString::fromStdString(m.comment)}};
        if (!m.drawing.empty()) mo["drawing_strokes"] = int(parseReviewDrawing(m.drawing).size());  // a reviewer drew on the frame
        markers.append(mo);
    }
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
        o["color_space"] = QString::fromStdString(m.colorSpace.empty() ? std::string("rec709") : m.colorSpace);
        if (m.maxCll > 0) {
            o["max_cll"] = m.maxCll;
            o["max_fall"] = m.maxFall;
        }
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
    // With `clone`, in a voice cloned from a recording (Chatterbox), time-stretched to fit, each file and its media noting
    // that it is generated and from whose voice; `exact` makes each line fill its room.
    struct CloneVoice {
        std::vector<float> sample;  // mono at kCloneSampleRate
        CloneOptions options;
        std::string from, note;
        bool exact = false;
    };
    QString speakLines(Project& p, Sequence& s, const std::vector<SpeechLine>& lines, const std::string& voice, double speed, TrackRef track,
                       const QString& folder, QJsonArray& placed, double& seconds, const CloneVoice* clone = nullptr);
    // The voice for montage_clone_speech and montage_dub: from voice_clip, voice_media (a file or the project's media, from
    // voice_from to voice_to seconds), or else the clip heard at `fallback`; consent must be given.
    CloneVoice cloneVoiceArg(const QJsonObject& a, const Project& p, const Sequence& s, FrameTime fallback);
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
        if (!indexVideo(analysisSource(m), m.duration, v, 0, [&](double f) { progress(f, QStringLiteral("Indexing %1").arg(QString::fromStdString(m.name))); }, nullptr, &err))
            return QString::fromStdString(m.name + ": " + err);
        m.visual = std::make_shared<const VisualIndex>(std::move(v));
        changed = true;
    }
    return {};
}

QString McpServer::Impl::speakLines(Project& p, Sequence& s, const std::vector<SpeechLine>& lines, const std::string& voice, double speed,
                                    TrackRef track, const QString& folder, QJsonArray& placed, double& seconds, const CloneVoice* clone) {
    static_assert(kCloneSampleRate == kTtsSampleRate);
    QDir().mkpath(folder);
    int n = 1;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::vector<float> audio;
        std::string err;
        progress(double(i) / lines.size(), QStringLiteral("Speaking"));
        const double room = lines[i].fit > 0 ? lines[i].fit / s.fpsValue() : 0;
        if (clone) {
            if (!cloneSpeechToFit(lines[i].text, clone->sample, kCloneSampleRate, clone->options, speed, room, clone->exact, audio, &err))
                return QString::fromStdString(err);
        } else {
            if (!synthesizeSpeech(lines[i].text, voice, speed, audio, &err)) return QString::fromStdString(err);
            const double said = double(audio.size()) / kTtsSampleRate;
            if (room > 0 && said > room * 1.02) {
                const double faster = std::min(1.6, speed * said / room);
                if (faster > speed * 1.02 && !synthesizeSpeech(lines[i].text, voice, faster, audio, &err)) return QString::fromStdString(err);
            }
        }
        QString path;
        do path = folder + '/' + QString::fromStdString(s.name) + (clone ? QStringLiteral(" Cloned Speech ") : QStringLiteral(" Speech ")) +
                  QString::number(n++) + QStringLiteral(".wav");
        while (QFileInfo::exists(path));
        if (!(clone ? writeClonedSpeechWav(path.toStdString(), audio, clone->note, &err) : writeSpeechWav(path.toStdString(), audio, &err)))
            return QString::fromStdString(err);
        const Id media = mediaFor(p, path);
        if (MediaItem* m = clone ? p.findMedia(media) : nullptr) {
            m->metadata["comment"] = clone->note;
            if (std::find(m->keywords.begin(), m->keywords.end(), "AI voice") == m->keywords.end()) m->keywords.push_back("AI voice");
        }
        if (clone && clone->exact && lines[i].fit > 0) {
            // In place of what was there.
            const edit::Result lifted = edit::liftRange(p, s, lines[i].at, lines[i].at + lines[i].fit, {track});
            if (!lifted.ok && !lifted.error.empty()) return QString::fromStdString(lifted.error);
        }
        const edit::Result r = edit::placeMedia(p, s, media, lines[i].at, 0, -1, {TrackKind::Video, 0}, track, false);
        if (!r.ok) return QString::fromStdString(r.error);
        if (clone && clone->exact && lines[i].fit > 0)  // (never past its room, whatever rounding gave its length)
            for (const Id id : r.created)
                if (Clip* made = edit::clipById(s, id); made && made->duration > lines[i].fit) made->duration = lines[i].fit;
        seconds += double(audio.size()) / kTtsSampleRate;
        placed.append(QJsonObject{{"file", path}, {"at_seconds", lines[i].at / s.fpsValue()}, {"seconds", double(audio.size()) / kTtsSampleRate},
                                  {"clip", r.created.empty() ? 0.0 : double(r.created.front())}});
    }
    return {};
}

McpServer::Impl::CloneVoice McpServer::Impl::cloneVoiceArg(const QJsonObject& a, const Project& p, const Sequence& s, FrameTime fallback) {
    if (!a.value("consent").toBool())
        throw ArgError{"Cloning a voice needs its owner's consent: pass consent: true to confirm you have it (or that it is your own)"};
    if (!voiceCloneAvailable()) throw ArgError{"This build of Montage cannot clone voices (no ONNX Runtime)"};
    if (!voiceCloneModel().installed())
        throw ArgError{QStringLiteral("The voice cloning model is not downloaded (%1 MB): clone a voice once in the app, or fetch it into %2")
                           .arg(voiceCloneModel().bytes() / 1000000)
                           .arg(QString::fromStdString(voiceCloneModel().directory()))};
    CloneVoice v;
    const CloneLanguage* lang = findCloneLanguage(str(a, "language", "en").toStdString());
    if (!lang) throw ArgError{QStringLiteral("Voice cloning does not speak \"%1\"").arg(str(a, "language"))};
    v.options.language = lang->code;
    v.options.exaggeration = std::clamp(a.value("exaggeration").toDouble(0.5), 0.25, 2.0);
    const double fps = s.fpsValue();
    std::string path, err;
    std::vector<int> channels;
    double from = 0, to = 0;
    const Clip* c = nullptr;
    if (a.contains("voice_clip")) {
        c = edit::clipById(const_cast<Sequence&>(s), Id(a.value("voice_clip").toDouble()));
        if (!c) throw ArgError{"No such voice_clip"};
    } else if (a.contains("voice_media")) {
        const QString name = a.value("voice_media").toString();
        for (const MediaItem& m : p.media)
            if (QString::fromStdString(m.name) == name || QString::fromStdString(m.path) == absolute(name)) path = m.path;
        if (path.empty()) path = absolute(name).toStdString();
        if (!QFileInfo::exists(QString::fromStdString(path))) throw ArgError{QStringLiteral("No voice_media \"%1\"").arg(name)};
        from = std::max(0.0, a.value("voice_from").toDouble(0));
        to = a.contains("voice_to") ? a.value("voice_to").toDouble() : from + 30;
        v.from = QFileInfo(QString::fromStdString(path)).fileName().toStdString();
    } else {
        // The voice heard where it speaks: a heard clip (unmuted; soloed if any are), one with a transcript first
        // (someone speaking, not the music).
        const bool anySolo = std::any_of(s.audioTracks.begin(), s.audioTracks.end(), [](const Track& t) { return t.solo; });
        for (const bool spoken : {true, false}) {
            for (const Track& t : s.audioTracks) {
                if (t.muted || (anySolo && !t.solo)) continue;
                for (const Clip& k : t.clips)
                    if (k.contains(fallback) && k.mediaId && k.enabled)
                        if (const MediaItem* m = p.findMedia(k.mediaId); m && m->hasAudio && (!spoken || m->transcript)) {
                            c = &k;
                            break;
                        }
                if (c) break;
            }
            if (c) break;
        }
        if (!c) throw ArgError{"Give voice_clip or voice_media: no clip with sound is heard there to take a voice from"};
    }
    if (c) {
        const MediaItem* m = c->mediaId ? p.findMedia(c->mediaId) : nullptr;
        if (!m || !m->hasAudio) throw ArgError{"That clip has no sound to take a voice from"};
        path = m->path;
        channels = c->channels;
        const double x = c->sourceFrameAt(c->start) / fps, y = c->sourceFrameAt(c->end() - 1) / fps;
        from = std::min(x, y);
        to = std::min(std::max(x, y) + 1 / fps, from + 30);
        v.from = (c->name.empty() ? m->name : c->name) + " (" + QFileInfo(QString::fromStdString(m->path)).fileName().toStdString() + ")";
    }
    if (!voiceSampleFrom(path, from, to, v.sample, &err, channels, s.sampleRate > 0 ? s.sampleRate : 48000)) throw ArgError{QString::fromStdString(err)};
    v.note = QStringLiteral("Generated speech (%1) in a voice cloned from %2 with Chatterbox Multilingual in Montage, %3; the speaker's consent "
                            "was confirmed by the caller.")
                 .arg(QString::fromUtf8(lang->name), QString::fromStdString(v.from), QDateTime::currentDateTime().toString(Qt::ISODate))
                 .toStdString();
    return v;
}

void McpServer::Impl::addTools() {
    add("montage_probe_media", "Probe media",
        "Describe a video, audio or image file: duration, size, frame rate, codecs, colour space and any HDR light levels "
        "(max_cll, max_fall in nits); for an OpenEXR render its layers "
        "(AOVs, \"\" the main picture) and Cryptomattes with their object names.",
        R"json({"type":"object","properties":{"path":{"type":"string","description":"Media file"}},"required":["path"]})json", true,
        [](const QJsonObject& a) {
            MediaItem m;
            std::string err;
            if (!probeMedia(absolute(need(a, "path")).toStdString(), m, &err)) return fail(QString::fromStdString(err));
            QJsonObject o = mediaJson(m);
            if (const auto exr = exrAvailable() ? cachedExrInfo(exrFrameFile(m.path, 0)) : nullptr) {
                QJsonArray layers, mattes;
                for (const ExrLayer& l : exr->layers) {
                    QJsonArray ch;
                    for (const std::string& c : l.channels) ch.append(QString::fromStdString(c));
                    layers.append(QJsonObject{{"name", QString::fromStdString(l.name)}, {"channels", ch}});
                }
                for (const Cryptomatte& cm : exr->cryptomattes) {
                    QJsonArray objects;
                    for (const auto& [name, id] : cm.objects)
                        if (objects.size() < 500) objects.append(QJsonObject{{"name", QString::fromStdString(name)}, {"id", QString::fromStdString(cryptomatteHex(id))}});
                    mattes.append(QJsonObject{{"name", QString::fromStdString(cm.name)}, {"objects", objects}, {"object_count", int(cm.objects.size())}});
                }
                o["exr_compression"] = QString::fromStdString(exr->compression);
                o["exr_layers"] = layers;
                if (!mattes.isEmpty()) o["cryptomattes"] = mattes;
            }
            return ok(json(o), o);
        });

    add("montage_read_credentials", "Read Content Credentials",
        "Read and check a file's Content Credentials (C2PA): whether it has any, whether they are intact and who signed "
        "them (state Trusted, Valid or Invalid, with the problems), the app or camera that made it, its actions, and whether "
        "it says AI generated or changed it. `full` adds the whole manifest store as JSON.",
        R"json({"type":"object","properties":{"path":{"type":"string","description":"Media file"},
            "full":{"type":"boolean","default":false,"description":"Add the whole manifest store (manifest_store)"}},"required":["path"]})json", true,
        [](const QJsonObject& a) {
            const std::string path = absolute(need(a, "path")).toStdString();
            if (!contentCredentialsAvailable()) return fail("This build of Montage cannot read Content Credentials (no C2PA library)");
            if (!contentCredentialsFormat(path)) return fail("Content Credentials cannot be in this kind of file");
            CredentialSummary c;
            std::string err;
            if (!readContentCredentials(path, c, &err)) return fail(QString::fromStdString(err));
            auto list = [](const std::vector<std::string>& v) {
                QJsonArray out;
                for (const std::string& x : v) out.append(QString::fromStdString(x));
                return out;
            };
            QJsonObject o{{"present", c.present}};
            if (c.present) {
                o["state"] = QString::fromStdString(c.state);
                o["issuer"] = QString::fromStdString(c.issuer);
                o["signed_at"] = QString::fromStdString(c.signedAt);
                o["generator"] = QString::fromStdString(c.generator);
                o["title"] = QString::fromStdString(c.title);
                o["actions"] = list(c.actions);
                o["ai_generated"] = c.aiGenerated;
                o["problems"] = list(c.problems);
                o["label"] = QString::fromStdString(credentialsLabel(c));
                if (a.value("full").toBool()) o["manifest_store"] = QJsonDocument::fromJson(QByteArray::fromStdString(c.json)).object();
            }
            return ok(c.present ? QString::fromStdString(credentialsLabel(c)) : QStringLiteral("No Content Credentials"), o);
        });

    add("montage_offload", "Offload a camera card",
        "Copy a camera card (or any folder) to one or more destinations as <destination>/<card name>, as Resolve's Clone "
        "tool and Silverstack do: each file read once and written to all of them while hashed (XXH64), each copy read back "
        "and compared (`verify`), and an ASC MHL generation written into each copy (`mhl`). A file already at a destination "
        "is kept when identical (an interrupted offload resumes), reported when not. A card with an ASC MHL history is "
        "checked against it and the history carried over. With `project`, the first copy's media is imported into a bin "
        "named after the card. Lists every problem.",
        R"json({"type":"object","properties":{"source":{"type":"string"},"destinations":{"type":"array","items":{"type":"string"}},
            "verify":{"type":"boolean","default":true},"mhl":{"type":"boolean","default":true},
            "author":{"type":"string"},"location":{"type":"string"},"comment":{"type":"string"},
            "project":{"type":"string","description":"Import the copied media into this project"}},"required":["source","destinations"]})json",
        false, [this](const QJsonObject& a) {
            QStringList dests;
            for (const QJsonValue& v : a.value("destinations").toArray()) dests << absolute(v.toString());
            if (dests.isEmpty()) throw ArgError{"\"destinations\" lists where to copy the card"};
            OffloadSettings os;
            os.verify = a.value("verify").toBool(true);
            os.mhl = a.value("mhl").toBool(true);
            os.author = str(a, "author");
            os.location = str(a, "location");
            os.comment = str(a, "comment");
            const QString source = absolute(need(a, "source"));
            std::optional<Loaded> l;
            if (a.contains("project")) l = open(a);  // checked before a long offload, not after
            const OffloadResult r = offloadCard(source, dests, os, [this](double f, const QString&) {
                progress(f, "Offloading");
                return true;
            });
            if (!r.error.isEmpty()) return fail(r.error);
            QJsonArray issues;
            for (const OffloadIssue& i : r.issues) issues.append(QJsonObject{{"path", i.path}, {"problem", i.problem}});
            QJsonObject out{{"ok", r.ok}, {"files", r.files}, {"bytes", double(r.bytes)}, {"already_there", r.alreadyThere},
                            {"copies", QJsonArray::fromStringList(r.copies)}, {"issues", issues}};
            QString text = QStringLiteral("%1 %2 file(s) (%3 bytes) to %4 destination(s)%5.")
                               .arg(r.ok ? "Offloaded and verified" : "Offloaded with problems:")
                               .arg(r.files)
                               .arg(r.bytes)
                               .arg(r.copies.size())
                               .arg(r.alreadyThere ? QStringLiteral(", %1 already there").arg(r.alreadyThere) : QString());
            for (const OffloadIssue& i : r.issues) text += QStringLiteral("\n  %1: %2").arg(i.path, i.problem);
            for (const QString& note : r.notes) text += QStringLiteral("\n  ") + note;
            if (!r.notes.isEmpty()) out["notes"] = QJsonArray::fromStringList(r.notes);
            if (!r.damaged.isEmpty()) out["damaged"] = QJsonArray::fromStringList(r.damaged);  // for montage_repair_media
            if (l && !r.copies.isEmpty()) {
                const std::string bin = QFileInfo(r.copies.front()).fileName().toStdString();
                int imported = 0;
                // Not the hash lists, half-written parts, or what the project already has (an offload resumed); runs
                // of numbered frames (CinemaDNG, EXR...) as one image sequence each.
                std::set<std::string> have;
                for (const MediaItem& m : l->project.media) have.insert(m.path);
                QDirIterator it(r.copies.front(), QDir::Files, QDirIterator::Subdirectories);
                QStringList files;
                while (it.hasNext()) {
                    const QString f = it.next();
                    if (!f.contains("/ascmhl/") && !f.endsWith(".montage-part")) files << f;
                }
                files.sort();
                std::map<std::string, std::pair<ImageSequence, int>> runs;
                for (const QString& f : files)
                    if (ImageSequence seq; isFrameFormat(f.toStdString()) && detectImageSequence(f.toStdString(), seq)) {
                        runs[seq.pattern].first = seq;
                        ++runs[seq.pattern].second;
                    }
                std::set<std::string> made;
                for (const QString& f : files) {
                    std::string path = f.toStdString();
                    if (ImageSequence seq; isFrameFormat(path) && detectImageSequence(path, seq) && runs[seq.pattern].second >= 2) {
                        if (!made.insert(seq.pattern).second) continue;
                        double rate = 0;
                        seq.fps = cinemaDng(path, &rate) && rate > 0 ? rateFor(rate) : l->seq().fps;
                        path = imageSequencePath(seq);
                    }
                    if (have.count(path)) continue;
                    MediaItem m;
                    if (!probeMedia(path, m)) continue;
                    m.id = l->project.newId();
                    m.bin = bin;
                    l->project.media.push_back(m);
                    have.insert(path);
                    ++imported;
                }
                save(*l);
                out["imported"] = imported;
                text += QStringLiteral("\nImported %1 clip(s) into the bin \"%2\".").arg(imported).arg(QString::fromStdString(bin));
            }
            return ok(text, out);
        });

    add("montage_verify_mhl", "Verify a folder against its ASC MHL hash list",
        "Check a folder (a card or an offloaded copy) against its ASC MHL history, whichever tool wrote it: each file's "
        "latest record compared with the file now (missing, changed) and files the history lacks listed (added). With "
        "`record`, a new generation records each file as verified, failed or original (a folder without a history gets its "
        "first that way).",
        R"json({"type":"object","properties":{"folder":{"type":"string"},"record":{"type":"boolean","default":false},
            "author":{"type":"string"}},"required":["folder"]})json",
        false, [this](const QJsonObject& a) {
            OffloadSettings os;
            os.author = str(a, "author");
            const MhlVerifyResult v = verifyMhl(absolute(need(a, "folder")), a.value("record").toBool(), os, [this](double f, const QString&) {
                progress(f, "Verifying");
                return true;
            });
            if (!v.error.isEmpty()) return fail(v.error);
            const QJsonObject out{{"ok", v.ok},
                                  {"verified", v.verified},
                                  {"missing", QJsonArray::fromStringList(v.missing)},
                                  {"changed", QJsonArray::fromStringList(v.changed)},
                                  {"added", QJsonArray::fromStringList(v.added)},
                                  {"unchecked", QJsonArray::fromStringList(v.unchecked)},
                                  {"generation", v.generation}};
            QString text = v.ok ? QStringLiteral("All %1 recorded file(s) match.").arg(v.verified)
                                : QStringLiteral("%1 match; %2 missing, %3 changed.").arg(v.verified).arg(v.missing.size()).arg(v.changed.size());
            if (!v.added.isEmpty()) text += QStringLiteral(" %1 not in the hash list.").arg(v.added.size());
            if (!v.generation.isEmpty()) text += QStringLiteral(" Recorded as %1.").arg(v.generation);
            return ok(text, out);
        });

    add("montage_repair_media", "Repair an interrupted recording",
        "Rebuild a recording the camera or recorder never finished (a flat battery, a pulled card) into a new file: an MP4 "
        "or MOV without its index (moov) from a good clip from the same camera and settings (`reference`; H.264, HEVC and "
        "ProRes pictures, AAC and PCM sound), an MXF without its footer (closed header with the duration, footer with an "
        "index table), or a WAV, BWF or RF64 whose sizes were never written (bext and iXML kept). The damaged file is not "
        "changed; `output` (a relative one is beside it) defaults to <name>_repaired.<ext>, and an existing one is only "
        "replaced with `overwrite`. With `project`, the repaired file is imported into "
        "it with the keyword Recovered (and the start timecode a camera sidecar gives).",
        R"json({"type":"object","properties":{"file":{"type":"string"},"reference":{"type":"string"},"output":{"type":"string"},
            "overwrite":{"type":"boolean","default":false},"project":{"type":"string","description":"Import the repaired file into this project"},
            "bin":{"type":"string"}},"required":["file"]})json",
        false, [this](const QJsonObject& a) {
            const QString file = absolute(need(a, "file"));
            const RepairCheck check = checkRecording(file);
            if (check.kind == RepairKind::None) {
                if (!check.opens) return fail(check.problem.isEmpty() ? QStringLiteral("It cannot be read") : check.problem);
                return ok(QStringLiteral("%1 opens: nothing to repair.").arg(QFileInfo(file).fileName()), QJsonObject{{"repaired", false}});
            }
            if (check.needsReference && !a.contains("reference"))
                throw ArgError{"\"reference\" is a good clip from the same camera, needed to rebuild an MP4 or MOV"};
            QString output = repairedPath(file);
            if (a.contains("output")) {
                output = str(a, "output");
                if (QFileInfo(output).isRelative()) output = QFileInfo(file).dir().filePath(output);
                output = QFileInfo(output).absoluteFilePath();
            }
            if (sameFile(output, file) || (check.needsReference && sameFile(output, absolute(str(a, "reference")))))
                throw ArgError{"\"output\" must be a new file, not the damaged file or the reference"};
            if (QFileInfo::exists(output) && !a.value("overwrite").toBool())
                throw ArgError{"\"output\" exists: pass overwrite to replace it"};
            std::optional<Loaded> l;
            if (a.contains("project")) l = open(a);
            RepairReport r;
            QString err;
            if (!repairRecording(file, check.needsReference ? absolute(str(a, "reference")) : QString(), output, &r, &err, [this](double f) {
                    progress(f, "Repairing");
                    return true;
                }))
                return fail(err);
            static const char* kinds[] = {"none", "movie", "mxf", "wav"};
            QJsonObject out{{"repaired", true}, {"kind", kinds[int(r.kind)]}, {"output", output}, {"seconds", r.seconds},
                            {"frames", r.videoFrames}, {"audio_samples", double(r.audioSamples)}, {"bytes_used", double(r.bytesUsed)},
                            {"bytes_left", double(r.bytesLeft)}, {"problem", check.problem}};
            if (r.timecode >= 0) out["timecode"] = r.timecode;
            if (!r.notes.isEmpty()) out["notes"] = QJsonArray::fromStringList(r.notes);
            QString text = QStringLiteral("%1. Repaired into %2: %3 s").arg(check.problem, QFileInfo(output).fileName()).arg(r.seconds, 0, 'f', 2);
            if (r.videoFrames) text += QStringLiteral(", %1 frames").arg(r.videoFrames);
            text += QStringLiteral("; %1 bytes at the end were not whole.").arg(r.bytesLeft);
            for (const QString& n : r.notes) text += QStringLiteral("\n  ") + n;
            if (l) {
                MediaItem m;
                std::string perr;
                if (!probeMedia(output.toStdString(), m, &perr)) return fail(QStringLiteral("Repaired, but it does not open: %1").arg(QString::fromStdString(perr)));
                m.id = l->project.newId();
                m.bin = str(a, "bin").toStdString();
                m.keywords.push_back("Recovered");
                m.metadata["comment"] = QStringLiteral("Recovered from %1 by Repair Media").arg(QFileInfo(file).fileName()).toStdString();
                if (r.timecode >= 0) m.timecode = r.timecode;
                l->project.media.push_back(m);
                save(*l);
                out["media"] = double(m.id);
                text += QStringLiteral("\nImported as %1.").arg(QString::fromStdString(m.name));
            }
            return ok(text, out);
        });

    add("montage_production", "List or make a production",
        "A production is a folder of a team's projects on a shared drive (Premiere's Productions, Avid's shared projects): "
        "lists its projects with who is editing each (a project being edited cannot be changed by these tools), or makes "
        "the folder a production with `create`.",
        R"json({"type":"object","properties":{"folder":{"type":"string"},"create":{"type":"boolean","default":false},
            "name":{"type":"string","description":"The production's name when making one"}},"required":["folder"]})json",
        true, [](const QJsonObject& a) {
            const QString folder = absolute(need(a, "folder"));
            if (!isProduction(folder.toStdString())) {
                if (!a.value("create").toBool()) return fail(QStringLiteral("%1 is not a production (pass create to make it one)").arg(folder));
                std::string err;
                if (!createProduction(folder.toStdString(), str(a, "name").toStdString(), &err)) return fail(QString::fromStdString(err));
            }
            QJsonArray projects;
            QStringList lines;
            for (const ProductionProject& pp : listProduction(folder.toStdString())) {
                QString status = QStringLiteral("free");
                if (pp.lock.state == LockState::Theirs || pp.lock.state == LockState::Mine)
                    status = QStringLiteral("editing: ") + QString::fromStdString(pp.lock.owner.describe());
                else if (pp.lock.state == LockState::Stale)
                    status = QStringLiteral("free (left open by %1)").arg(QString::fromStdString(pp.lock.owner.describe()));
                projects.append(QJsonObject{{"project", QString::fromStdString(pp.path)},
                                            {"name", QString::fromStdString(pp.relative)},
                                            {"status", status},
                                            {"editor", pp.lock.state == LockState::Theirs || pp.lock.state == LockState::Mine
                                                           ? QString::fromStdString(pp.lock.owner.describe())
                                                           : QString()},
                                            {"saved", pp.modified.toUTC().toString(Qt::ISODate)}});
                lines << QStringLiteral("%1 — %2").arg(QString::fromStdString(pp.relative), status);
            }
            const QString name = QString::fromStdString(productionName(folder.toStdString()));
            return ok(QStringLiteral("Production \"%1\": %2 project(s)%3").arg(name).arg(projects.size()).arg(lines.isEmpty() ? QString() : "\n" + lines.join('\n')),
                      QJsonObject{{"production", name}, {"folder", folder}, {"projects", projects}});
        });

    add("montage_import_from_project", "Bring sequences from another project",
        "Copy sequences (by name; all when none are named) from another .montage project into this one, with the "
        "sequences they nest and the media their clips play (linked where it is; files the project has are not added "
        "twice), each as a sequence of its own. The first becomes active.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"from":{"type":"string","description":"The .montage project to copy from"},
            "sequences":{"type":"array","items":{"type":"string"}}},"required":["project","from"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Project from;
            std::string err;
            const QString src = absolute(need(a, "from"));
            if (!loadProject(src.toStdString(), from, &err)) return fail(QStringLiteral("Cannot open %1: %2").arg(src, QString::fromStdString(err)));
            checkStereoMedia(from);
            QStringList names;
            for (const QJsonValue& v : a.value("sequences").toArray()) names << v.toString();
            std::vector<Id> wanted;
            for (const Sequence& s : from.sequences)
                if (names.isEmpty() || names.contains(QString::fromStdString(s.name))) wanted.push_back(s.id);
            for (const QString& n : names)
                if (std::none_of(from.sequences.begin(), from.sequences.end(), [&](const Sequence& s) { return QString::fromStdString(s.name) == n; }))
                    return fail(QStringLiteral("%1 has no sequence \"%2\"").arg(QFileInfo(src).fileName(), n));
            const std::vector<Id> made = importFromProject(l.project, from, wanted);
            if (made.empty()) return fail("Nothing to import");
            l.project.activeSequence = made.front();
            save(l);
            QJsonArray seqs;
            for (Id id : made) {
                const Sequence* s = l.project.findSequence(id);
                seqs.append(QJsonObject{{"id", double(id)}, {"name", s ? QString::fromStdString(s->name) : QString()}});
            }
            return ok(QStringLiteral("Imported %1 sequence(s) from %2").arg(made.size()).arg(QFileInfo(src).fileName()),
                      QJsonObject{{"sequences", seqs}, {"project", l.path}});
        });

    add("montage_create_project", "Create a project",
        "Create a .montage project with the given media laid end to end on V1/A1. The sequence takes the first video's "
        "size and frame rate unless width, height or fps are given.",
        R"json({"type":"object","properties":{
            "project":{"type":"string","description":"Path of the .montage file to write"},
            "media":{"type":"array","items":{"type":"string"},"description":"Media files, in order"},
            "width":{"type":"integer"},"height":{"type":"integer"},"fps":{"type":"number"},
            "audio_layout":{"type":"string","enum":["stereo","5.1","7.1","5.1.2","5.1.4","7.1.2","7.1.4","ambix","ambix2","ambix3"],"default":"stereo"}},
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
                    throw ArgError{"\"audio_layout\" must be stereo, 5.1, 7.1, 5.1.2, 5.1.4, 7.1.2, 7.1.4, ambix, ambix2 or ambix3"};
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
            "layer":{"type":["number","string"],"description":"With psd_mode layer: the layer's name or index (0 = the bottom). For an OpenEXR render: the layer (AOV) the clip shows, as montage_probe_media lists them"},
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
            // An OpenEXR render's layer (AOV).
            if (!psdMedia && a.value("layer").isString()) {
                const MediaItem* m = l.project.findMedia(media);
                const auto exr = m && exrAvailable() ? cachedExrInfo(exrFrameFile(m->path, 0)) : nullptr;
                const std::string layer = a.value("layer").toString().toStdString();
                if (!exr) throw ArgError{"\"layer\" is for OpenEXR renders and Photoshop files (with psd_mode layer)"};
                if (!exr->hasLayer(layer)) throw ArgError{QStringLiteral("%1 has no layer \"%2\"").arg(need(a, "media"), QString::fromStdString(layer))};
                for (Id id : r.created)
                    if (auto loc = edit::locate(s, id); loc && loc->track.kind == TrackKind::Video)
                        if (Clip* c = edit::clipById(s, id)) c->layer = layer;
            }
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

    add("montage_sync", "Find and fix clips out of sync",
        "Linked clips playing the same media as their picture but out of step with it (Premiere's red out-of-sync "
        "numbers, Avid's sync breaks), each with how many frames late (negative: early). `action` move puts a clip back "
        "in sync by moving it, slip by slipping its source where it is (refused past the media's ends); with `clip` "
        "only that clip, else every one listed.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "action":{"type":"string","enum":["check","move","slip"],"default":"check"},"clip":{"type":"number"}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const QString action = str(a, "action", "check");
            if (action != "check" && action != "move" && action != "slip") throw ArgError{"\"action\" is check, move or slip"};
            auto listed = [&] {
                QJsonArray out;
                for (const edit::SyncOffset& o : edit::syncOffsets(s)) {
                    const Clip* c = edit::clipById(s, o.clip);
                    const auto loc = edit::locate(s, o.clip);
                    out.append(QJsonObject{{"clip", double(o.clip)}, {"name", QString::fromStdString(c->name)},
                                           {"track", QString::fromStdString(trackAt(s, loc->track)->name)}, {"anchor", double(o.anchor)},
                                           {"frames", o.frames}, {"seconds", o.frames / s.fps.toDouble()}});
                }
                return out;
            };
            if (action == "check") {
                const QJsonArray out = listed();
                QString text = out.isEmpty() ? QStringLiteral("Every linked clip is in sync.") : QStringLiteral("%1 clip(s) out of sync:").arg(out.size());
                for (const auto& v : out) {
                    const QJsonObject o = v.toObject();
                    text += QStringLiteral("\n  %1 (%2, clip %3): %4 frame(s) %5").arg(o["name"].toString(), o["track"].toString())
                                .arg(qint64(o["clip"].toDouble())).arg(std::fabs(o["frames"].toDouble())).arg(o["frames"].toDouble() > 0 ? "late" : "early");
                }
                return ok(text, QJsonObject{{"clips", out}});
            }
            std::vector<Id> ids;
            if (a.contains("clip")) {
                ids.push_back(clipArg(l, a).id);
            } else {
                // Moved one at a time, a clip going later must never land on the head of the next one still to move:
                // those going later go last first, those going earlier first first.
                std::vector<edit::SyncOffset> all = edit::syncOffsets(s);
                std::stable_sort(all.begin(), all.end(), [&](const edit::SyncOffset& x, const edit::SyncOffset& y) {
                    const FrameTime xs = edit::clipById(s, x.clip)->start, ys = edit::clipById(s, y.clip)->start;
                    const bool xl = x.frames < 0, yl = y.frames < 0;  // going later
                    if (xl != yl) return xl;
                    return xl ? xs > ys : xs < ys;
                });
                for (const edit::SyncOffset& o : all) ids.push_back(o.clip);
            }
            if (ids.empty()) return ok(QStringLiteral("Every linked clip is in sync."), QJsonObject{{"clips", QJsonArray{}}});
            int fixed = 0;
            QStringList errors;
            for (Id id : ids) {
                const auto r = action == "move" ? edit::moveIntoSync(l.project, s, id) : edit::slipIntoSync(l.project, s, id);
                if (r.ok) ++fixed;
                else errors << QStringLiteral("clip %1: %2").arg(qint64(id)).arg(QString::fromStdString(r.error));
            }
            if (!fixed) return fail(errors.join("; "));
            save(l);
            QString text = QStringLiteral("%1 clip(s) %2 into sync.").arg(fixed).arg(action == "move" ? "moved" : "slipped");
            if (!errors.isEmpty()) text += QStringLiteral(" Not fixed: ") + errors.join("; ");
            return ok(text, QJsonObject{{"fixed", fixed}, {"clips", listed()}});
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

    add("montage_grade_nodes", "Colour nodes",
        "A picture clip's grade as a node graph (Resolve's Color page nodes): correctors in series, side by side through "
        "a parallel mixer (each branch from the same picture, blended by their keys or averaged) or a layer mixer (each "
        "input over those before it with its blend mode, where its key is), and keys passed between nodes (an outside "
        "node grades the rest of the picture). `action`: list; convert (the clip's colour effects, one node each, in "
        "series); add (a corrector after node `anchor`, by default the last: `how` serial, parallel, layer or outside); "
        "key_mixer (the keys of `keys` joined into corrector `node`'s key input); remove (`node`); connect (`from` 0 the "
        "clip's picture, `to` 0 the output, `input` 0 the picture, 1 a corrector's key or a mixer's input number, `key` "
        "true for a key); disconnect (`to`, `input`); set (`node`: `label`, `enabled`, a layer mixer's `blend` modes, "
        "`x`/`y`, and `settings`: mix, key_gain, key_invert, key_mode 0 multiply 1 add 2 subtract 3 replace, and mask.* "
        "as montage_add_effect's, which limit the node to a window, qualifier or depth); move_effect (effect `effect` into "
        "corrector `node`, 0 out of the graph); share (corrector `node` as a shared node `name`, one change seen by every "
        "clip using it); use_shared (shared node `shared` added after the clip's last node); unshare (`node` keeps a copy); "
        "rename_shared, remove_shared (`shared`: one no clip uses); flatten (back to an effect stack). Add effects to a "
        "node with montage_add_effect's `node` or `shared`.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "action":{"type":"string","enum":["list","convert","add","key_mixer","remove","connect","disconnect","set","move_effect","share","use_shared","unshare","rename_shared","remove_shared","flatten"]},
            "node":{"type":"integer"},"anchor":{"type":"integer"},"how":{"type":"string","enum":["serial","parallel","layer","outside"]},
            "keys":{"type":"array","items":{"type":"integer"}},"from":{"type":"integer"},"to":{"type":"integer"},
            "input":{"type":"integer"},"key":{"type":"boolean"},"label":{"type":"string"},"enabled":{"type":"boolean"},
            "blend":{"type":"array","items":{"type":"string"}},"x":{"type":"number"},"y":{"type":"number"},
            "settings":{"type":"object","additionalProperties":{"type":"number"}},"effect":{"type":"number"},
            "shared":{"type":"number"},"name":{"type":"string"}},"required":["project","action"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const QString action = need(a, "action");
            auto nodeArg = [&](const char* key = "node") {
                if (!a.value(key).isDouble()) throw ArgError{QStringLiteral("\"%1\" must be a node's key (see action list)").arg(key)};
                return a.value(key).toInt();
            };
            auto sharedArg = [&] {
                if (!a.value("shared").isDouble()) throw ArgError{"\"shared\" must be a shared node's id (see action list)"};
                return Id(a.value("shared").toDouble());
            };
            Id id = 0;
            const bool needsClip = action != "rename_shared" && action != "remove_shared" && !(action == "list" && !a.contains("clip"));
            if (needsClip) {
                const Clip& c = clipArg(l, a);
                const auto loc = edit::locate(s, c.id);
                if (!loc || loc->track.kind != TrackKind::Video) throw ArgError{"Colour nodes are a picture clip's"};
                id = c.id;
            }
            edit::Result r;
            int made = 0;
            Id madeShared = 0;
            if (action == "convert") {
                r = edit::convertToNodes(s, id);
            } else if (action == "add") {
                const QString how = str(a, "how", "serial");
                const edit::NodeHow h = how == "parallel" ? edit::NodeHow::Parallel : how == "layer" ? edit::NodeHow::Layer
                                      : how == "outside" ? edit::NodeHow::Outside : edit::NodeHow::Serial;
                if (how != "serial" && h == edit::NodeHow::Serial) throw ArgError{"\"how\" is serial, parallel, layer or outside"};
                r = edit::addGradeNode(l.project, s, id, h, a.value("anchor").toInt(), &made);
            } else if (action == "key_mixer") {
                std::vector<int> keys;
                for (const QJsonValue& v : a.value("keys").toArray()) keys.push_back(v.toInt());
                r = edit::addKeyMixer(s, id, keys, nodeArg(), &made);
            } else if (action == "remove") {
                r = edit::removeGradeNode(l.project, s, id, nodeArg());
            } else if (action == "connect") {
                r = edit::connectGradeNodes(s, id, {a.value("from").toInt(), a.value("to").toInt(), a.value("input").toInt(), a.value("key").toBool()});
            } else if (action == "disconnect") {
                r = edit::disconnectGradeNodes(s, id, nodeArg("to"), a.value("input").toInt());
            } else if (action == "set") {
                const int key = nodeArg();
                const GradeNode* n = findGradeNode(edit::clipById(s, id)->grade, key);
                if (!n) throw ArgError{"No such node (see action list)"};
                GradeNode changed = *n;
                if (a.contains("label")) changed.label = str(a, "label").toStdString();
                if (a.contains("enabled")) changed.enabled = a.value("enabled").toBool();
                if (a.contains("x")) changed.x = a.value("x").toDouble();
                if (a.contains("y")) changed.y = a.value("y").toDouble();
                if (a.contains("blend")) {
                    if (n->kind != "layer") throw ArgError{"Blend modes are a layer mixer's"};
                    changed.blend.clear();
                    for (const QJsonValue& v : a.value("blend").toArray()) changed.blend.push_back(v.toString().toStdString());
                }
                r = edit::setGradeNode(s, id, changed);
                if (!r.ok && !r.error.empty()) throw ArgError{QString::fromStdString(r.error)};
                const QJsonObject settings = a.value("settings").toObject();
                if (!settings.isEmpty()) {
                    std::vector<std::pair<std::string, double>> values;
                    for (auto it = settings.begin(); it != settings.end(); ++it) values.push_back({it.key().toStdString(), it.value().toDouble()});
                    check(edit::setGradeNodeParams(l.project, s, id, key, values));
                    r = {};
                }
            } else if (action == "move_effect") {
                if (!a.value("effect").isDouble()) throw ArgError{"\"effect\" must be an effect id on the clip"};
                r = edit::moveEffectToNode(l.project, s, id, Id(a.value("effect").toDouble()), a.value("node").toInt());
            } else if (action == "share") {
                r = edit::shareGradeNode(l.project, s, id, nodeArg(), str(a, "name").toStdString(), &madeShared);
            } else if (action == "use_shared") {
                r = edit::useSharedNode(l.project, s, id, sharedArg(), &made);
            } else if (action == "unshare") {
                r = edit::unshareGradeNode(l.project, s, id, nodeArg());
            } else if (action == "rename_shared") {
                r = edit::renameSharedNode(s, sharedArg(), need(a, "name").toStdString());
            } else if (action == "remove_shared") {
                r = edit::removeSharedNode(s, sharedArg());
            } else if (action == "flatten") {
                r = edit::flattenGrade(l.project, s, id);
            } else if (action != "list") {
                throw ArgError{QStringLiteral("Unknown action \"%1\"").arg(action)};
            }
            if (!r.ok) return fail(r.error.empty() ? QStringLiteral("Nothing to change") : QString::fromStdString(r.error));
            if (action != "list") save(l);
            auto effectsJson = [](const std::vector<const Effect*>& fx) {
                QJsonArray list;
                for (const Effect* e : fx) {
                    const EffectInfo* info = findEffectInfo(e->type);
                    list.append(QJsonObject{{"id", double(e->id)}, {"type", QString::fromStdString(e->type)},
                                            {"name", QString::fromStdString(info ? info->displayName : e->type)}, {"enabled", e->enabled}});
                }
                return list;
            };
            QJsonObject out;
            QStringList lines;
            if (id) {
                const Clip& c = *edit::clipById(s, id);
                QJsonArray nodes, links, outside;
                for (const GradeNode& n : c.grade.nodes) {
                    QJsonObject o{{"key", n.key}, {"kind", QString::fromStdString(n.kind)}, {"name", QString::fromStdString(gradeNodeName(n))},
                                  {"enabled", n.enabled}, {"x", n.x}, {"y", n.y}};
                    if (n.shared) o["shared"] = double(n.shared);
                    if (n.kind == "layer") {
                        QJsonArray blend;
                        for (const std::string& b : n.blend) blend.append(QString::fromStdString(b));
                        o["blend"] = blend;
                    }
                    if (n.kind == "corrector") {
                        const std::vector<const Effect*> fx = gradeNodeEffects(s, c, n.key);
                        o["effects"] = effectsJson(fx);
                        if (const Effect* st = gradeNodeSettings(c, n.key)) {
                            QJsonObject settings;
                            for (const auto& [name, prm] : st->params) settings[QString::fromStdString(name)] = prm.value;
                            o["settings"] = settings;
                        }
                        QStringList names;
                        for (const Effect* e : fx) names << QString::fromStdString(findEffectInfo(e->type) ? findEffectInfo(e->type)->displayName : e->type);
                        lines << QStringLiteral("%1 (%2)%3: %4").arg(QString::fromStdString(gradeNodeName(n))).arg(n.key)
                                     .arg(n.enabled ? "" : " off").arg(names.isEmpty() ? QStringLiteral("empty") : names.join(", "));
                    } else {
                        lines << QStringLiteral("%1 (%2)").arg(QString::fromStdString(gradeNodeName(n))).arg(n.key);
                    }
                    nodes.append(o);
                }
                for (const GradeLink& k : c.grade.links)
                    links.append(QJsonObject{{"from", k.from}, {"to", k.to}, {"input", k.input}, {"key", k.key}});
                std::vector<const Effect*> rest;
                for (const Effect& e : c.effects)
                    if (!e.node || c.grade.empty()) rest.push_back(&e);
                out = QJsonObject{{"nodes", nodes}, {"links", links}, {"outside", effectsJson(rest)}};
                QJsonArray order;
                for (int k : gradeOrder(c.grade)) order.append(k);
                out["order"] = order;
                if (c.grade.empty()) lines << QStringLiteral("No nodes: the clip's effects run as a stack (action convert)");
            }
            QJsonArray shared;
            for (const SharedNode& n : s.sharedNodes) {
                std::vector<const Effect*> fx;
                for (const Effect& e : n.effects) fx.push_back(&e);
                shared.append(QJsonObject{{"id", double(n.id)}, {"name", QString::fromStdString(n.name)}, {"effects", effectsJson(fx)},
                                          {"clips", sharedNodeUsers(s, n.id)}});
                lines << QStringLiteral("Shared node %1 \"%2\": %3 effect(s), %4 clip(s)").arg(qint64(n.id)).arg(QString::fromStdString(n.name))
                             .arg(n.effects.size()).arg(sharedNodeUsers(s, n.id));
            }
            out["shared_nodes"] = shared;
            if (made) out["created"] = made;
            if (madeShared) out["shared"] = double(madeShared);
            return ok(lines.join('\n'), out);
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
        "motion makes any title roll or crawl, starting and ending off screen. The 3d template is extruded text turned in "
        "space and lit (Final Cut's 3D titles), `depth` deep, spinning in with `spin_in`.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"text":{"type":"string"},
            "at":{"type":["number","string"]},"duration":{"type":["number","string"],"default":3},
            "track":{"type":"string"},"size":{"type":"number","description":"Font size in pixels"},
            "template":{"type":"string","enum":["plain","lower_third","lower_third_box","centred","chapter","callout","typewriter","cascade","pop_words","drop","wave","decode","end_card","credits","crawl","3d"],"default":"plain"},
            "depth":{"type":"number","description":"3d: how far the letters are extruded (pixels)"},
            "spin_in":{"type":"boolean","description":"3d: spin in at the start"},
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
            const std::string type = tpl == "plain" ? std::string("title") : tpl == "3d" ? std::string("title3d") : "title_" + tpl.toStdString();
            if (type != "title" && type != "title3d" && !findTitleTemplate(type)) throw ArgError{QStringLiteral("Unknown template \"%1\"").arg(tpl)};
            if (type == "title3d" && (a.contains("text_animation") || a.contains("motion")))
                throw ArgError{"A 3D title has its own animation (spin_in); text_animation and motion are for flat titles"};
            if (type != "title3d" && (a.contains("depth") || a.contains("spin_in"))) throw ArgError{"depth and spin_in are for the 3d template"};
            Clip c = makeGeneratorClip(l.project, type, std::max<FrameTime>(1, len));
            c.generator.strings["text"] = need(a, "text").toStdString();
            for (const char* key : {"size", "depth"})
                if (a.contains(key) && !a.value(key).isDouble()) throw ArgError{QStringLiteral("\"%1\" is a number").arg(key)};
            if (a.contains("spin_in") && !a.value("spin_in").isBool()) throw ArgError{"\"spin_in\" is true or false"};
            if (a.contains("size")) c.generator.params["size"] = Param(std::clamp(a.value("size").toDouble(), 4.0, 1000.0));
            if (a.contains("depth")) c.generator.params["depth"] = Param(std::clamp(a.value("depth").toDouble(), 0.0, 1000.0));
            if (a.value("spin_in").toBool()) c.generator.params["anim_in"] = Param(2.0);
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
        "the picture's depth (on video clips analysed with montage_steady_depth, depth that holds steady through the shot). depth_blur focuses on its focus depth (follow 0), the depth at point_x / point_y (follow 1; "
        "keys on the point rack between the keyed points' depths) or the nearest face (follow 2); montage_follow_focus keys its focus to follow a point, object or face through a clip; fstop scales its blur, "
        "blades / blade_rotation / squeeze / cat_eye shape the aperture, highlights lifts bokeh highlights and breathing "
        "scales the picture with the focus. "
        "remove_background cuts people out (keep 1 keeps the background instead), and mask.shape 4 limits any effect to "
        "the people in the picture. mask.shape 7 limits it to objects of an OpenEXR render's Cryptomatte: strings "
        "mask.crypto_layer (which Cryptomatte, default the file's first) and mask.crypto_ids (the objects' ids as 8 hex "
        "digits, comma separated, as montage_probe_media lists them). object_removal paints out whatever its mask covers and fills it in. stabilize and "
        "rolling_shutter (straightening the skew a rolling shutter gives pans; readout as a percentage of a frame) measure "
        "the camera's movement when added; when the clip has camera motion data (a GoPro GPMF track, a CAMM track, a .gcsv log "
        "beside it) stabilize lines it up and uses it (method 3, Camera Gyro, unless params give another method; horizon "
        "0-100 levels the horizon from gravity). "
        "sky_replace finds the sky (bright, smooth, open to the top of the frame, far away when the depth model is "
        "installed) and puts the picture on another track (source 0; track 0 is the one above, hidden unless hide 0) or a "
        "gradient (source 1, colours zenith and horizon) in its place; sensitivity, lowest (the lowest the sky reaches, % "
        "of the height), refine, shift and soften shape its key, view 1 shows it; steady (on by default) carries the sky from the frames around each so its edge holds through the shot; the sky follows the camera (follow 1, "
        "measured when added; follow 2 uses the clip's camera_track solve) by follow_amount; sky_exposure, sky_blur, "
        "match (the foreground's colour moved toward the new sky's) and wrap / wrap_width (its light round the "
        "foreground's edges) tie it in; a mask limits where the sky may be. "
        "face_shape reshapes each face by its mesh (percent, -100..100): face_width, jaw, chin, forehead, nose_width, "
        "nose_length, eye_size, eye_distance, mouth_width, mouth_height; show draws the mesh. look_at_lens turns each open "
        "eye's iris to the lens (amount; limit, the largest gaze off the lens in degrees it corrects, fading over the next "
        "10; show marks the irises). "
        "Restoration: dirt_removal takes out specks that are on one frame only (compared with the frames either side, "
        "following the motion); dust_spots replaces hand-marked spots (strings.spots, lines of \"frame x y radius\": the "
        "frame counted from the clip's start in sequence frames, the centre as fractions of the frame's width and height, "
        "the radius as a fraction of its width); scratch_removal fills thin vertical scratches; patch_fill covers its mask "
        "with the picture source_x / source_y away (fractions of the frame), blended seamlessly; paint draws clone or heal "
        "strokes (strings.strokes, lines of \"clone|heal frame hold track dx dy radius softness x y x y ...\": frame as for "
        "spots, hold 1 for every frame, track 1 to follow the mask's track, dx dy to where it copies from and the points "
        "as fractions of the frame's width and height, radius a fraction of its width, softness 0-1). "
        "lip_sync redraws the largest face's mouth and jaw to speak an audio track's sound (strings.track, e.g. \"A2\"; "
        "every track when empty), as a dub needs (MuseTalk; amount mixes it); it needs consent: true (the person shown has "
        "agreed) and its models (scripts/fetch-models.sh --lipsync), and runs in exports and renders (about 4 s a frame on four cores).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},"effect":{"type":"string"},
            "consent":{"type":"boolean","description":"lip_sync: the person shown has agreed to it"},
            "params":{"type":"object","additionalProperties":{"type":"number"}},
            "strings":{"type":"object","additionalProperties":{"type":"string"},"description":"Text settings: curves (\"x,y x,y\"), hue curves, a LUT file, the Colour Warper's mesh (\"spoke,ring,hue,sat,luma;...\": spokes 0-11 every 30 degrees from red, rings 1-4 for saturation 25-100 %, hue moved in degrees, saturation in 0-1 units, brightness in stops)..."},
            "mask_path":{"type":"array","items":{"type":["array","object"]},"description":"A closed Bezier mask: three or more points, fractions of the clip's frame"},
            "mask_smooth":{"type":"boolean","default":false},
            "group_stage":{"type":"string","enum":["pre","post"],"description":"Add it to the clip's colour group instead: its pre-clip grade (before each member's own effects) or post-clip grade (after them)"},
            "node":{"type":"integer","description":"Into the clip's corrector node with this key (montage_grade_nodes); a shared node's goes into the shared node"},
            "shared":{"type":"number","description":"Into this shared node (montage_grade_nodes), seen by every clip using it"}},
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
            if (type == "motion_deblur" && (!deblurAvailable() || !deblurModel().installed()))
                return fail("Motion Deblur needs its model: run `scripts/fetch-models.sh` or add the effect once in the app");
            if (type == "lip_sync") {
                if (!a.value("consent").toBool())
                    throw ArgError{"Lip Sync redraws a person's mouth to say other words: pass consent: true once the person shown has agreed"};
                if (!lipSyncAvailable() || !lipSyncModel().installed() || !faceModel().installed() || !faceMeshModel().installed())
                    return fail("Lip Sync needs its models: run `scripts/fetch-models.sh --lipsync` or add the effect once in the app");
            }
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
                if ((type == "dust_spots" && name == "spots") || (type == "paint" && name == "strokes")) {
                    // Spots and strokes are drawn on the Program monitor in the app; here they are given as text.
                    const std::string value = it.value().toString().toStdString();
                    size_t lines = 0;
                    for (const QString& line : it.value().toString().split('\n'))
                        if (!line.trimmed().isEmpty()) ++lines;
                    const size_t parsed = type == "paint" ? paintStrokesFromString(value).size() : dustSpotsFromString(value).size();
                    if (parsed != lines)
                        throw ArgError{type == "paint" ? QStringLiteral("strokes are lines of \"clone|heal frame hold track dx dy radius softness x y x y ...\"")
                                                       : QStringLiteral("spots are lines of \"frame x y radius\"")};
                    e.strings[name] = value;
                    continue;
                }
                if (name == "mask.crypto_layer" || name == "mask.crypto_ids") {
                    // The mask's Cryptomatte (mask.shape 7), as the mask's own params are taken.
                    if (!supportsMask(type)) throw ArgError{QStringLiteral("\"%1\" cannot have a mask").arg(QString::fromStdString(type))};
                    const std::string value = it.value().toString().toStdString();
                    if (name == "mask.crypto_ids") {
                        float id;
                        for (const QString& hex : it.value().toString().split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts))
                            if (!parseCryptomatteHex(hex.toStdString(), id))
                                throw ArgError{QStringLiteral("mask.crypto_ids are ids of 8 hex digits, separated by commas")};
                    }
                    e.strings[name] = value;
                    continue;
                }
                const auto si = std::find_if(info->strings.begin(), info->strings.end(), [&](const StringParamInfo& x) { return x.name == name; });
                if (si == info->strings.end())
                    throw ArgError{QStringLiteral("\"%1\" has no text setting \"%2\"").arg(QString::fromStdString(type), it.key())};
                std::string value = it.value().toString().toStdString();
                if (si->kind == StringKind::Track) value = audioTrackId(it.value().toString(), l.seq());
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
                return fail("Face Refinement, Blemish Remover, Redact Faces, Face Shape, Look at Lens and Lens Blur (Depth) following the nearest face need the face models: run `scripts/fetch-models.sh` or add them once in the app");
            if ((type == "face_shape" || type == "look_at_lens") && (!faceMeshAvailable() || !faceMeshModel().installed()))
                return fail("Face Shape and Look at Lens need the face mesh model: run `scripts/fetch-models.sh` or add one once in the app");
            if ((needsPersonMatte(e, 0) || type == "behind_people") && (!mattingAvailable() || !mattingModel().installed()))
                return fail("Remove Background, Behind People and People masks need their model: run `scripts/fetch-models.sh` or add one once in the app");
            if (needsDepth(e, 0) && (!depthAvailable() || !depthModel().installed()))
                return fail("Depth effects need the depth model: run `scripts/fetch-models.sh` or add one once in the app");
            const auto loc = edit::locate(l.seq(), c.id);
            const bool audioClip = loc && loc->track.kind == TrackKind::Audio;
            if (audioClip != (info->category == EffectCategory::AudioFilter))
                throw ArgError{audioClip ? QStringLiteral("That is an audio clip: choose an audio effect")
                                         : QStringLiteral("That is a video clip: choose a video effect")};
            if (type == "camera_track") throw ArgError{"Solve a clip's camera with montage_camera_track"};
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
            if (a.contains("node") || a.contains("shared")) {
                if (audioClip || runsBeforeGrade(type))
                    throw ArgError{"A node takes picture effects that work on the picture it is given (that one works on the picture as it comes: add it to the clip)"};
                Id shared = Id(a.value("shared").toDouble());
                if (a.contains("node")) {
                    const GradeNode* n = findGradeNode(c.grade, a.value("node").toInt());
                    if (!n || n->kind != "corrector") throw ArgError{"No corrector node with that key on the clip (see montage_grade_nodes)"};
                    if (n->shared) shared = n->shared;
                    else e.node = n->key;
                }
                if (shared) {
                    SharedNode* sn = nullptr;
                    for (SharedNode& x : l.seq().sharedNodes)
                        if (x.id == shared) sn = &x;
                    if (!sn) throw ArgError{"No such shared node (see montage_grade_nodes)"};
                    sn->effects.push_back(e);
                    save(l);
                    return ok(QStringLiteral("Added %1 to shared node \"%2\"").arg(QString::fromStdString(displayName), QString::fromStdString(sn->name)),
                              QJsonObject{{"effect_id", double(e.id)}});
                }
                // (after the node's own effects: before the next node's, where the graph runs)
                auto at = c.effects.end();
                for (auto it = c.effects.begin(); it != c.effects.end(); ++it)
                    if (it->node == e.node) at = it + 1;
                c.effects.insert(at, e);
                save(l);
                return ok(QStringLiteral("Added %1 to %2's node %3").arg(QString::fromStdString(displayName), QString::fromStdString(c.name)).arg(e.node),
                          QJsonObject{{"effect_id", double(e.id)}});
            }
            if (type == "stabilize" || type == "rolling_shutter") {
                // They work from the camera's movement, measured now, and move the whole frame (so they go first).
                std::string motion, err, gyro, gyroNote;
                const bool steady = type == "stabilize";
                if (!analyzeClipStabilization(l.project, l.seq(), c, motion, {}, nullptr, &err, steady ? &gyro : nullptr, &gyroNote))
                    return fail(QStringLiteral("Could not measure the camera's movement: %1").arg(QString::fromStdString(err)));
                e.strings["motion"] = motion;
                QString note;
                bool usingGyro = false;
                if (steady && !gyro.empty()) {
                    e.strings["gyro"] = gyro;
                    if (!a.value("params").toObject().contains("method")) e.params["method"] = Param(3);
                    usingGyro = std::lround(e.p("method", 0, 2)) == 3;
                    note = usingGyro ? QStringLiteral(" (with the camera's motion data)")
                                     : QStringLiteral(" (camera motion data found: method 3 uses it)");
                } else if (steady && !gyroNote.empty() && gyroNote != "The clip has no camera motion data") {
                    note = QStringLiteral(" (camera motion data not used: %1)").arg(QString::fromStdString(gyroNote));
                }
                c.effects.insert(c.effects.begin(), e);
                save(l);
                return ok(QStringLiteral("Added %1 to %2%3").arg(QString::fromStdString(displayName), QString::fromStdString(c.name), note),
                          QJsonObject{{"effect_id", double(e.id)}, {"camera_gyro", usingGyro}});
            }
            QString note;
            if (type == "sky_replace" && std::lround(e.p("follow", 0, 1)) == 1) {
                // The new sky moves with the camera, measured now (a still, or a clip it fails on, keeps the sky still).
                std::string motion, err;
                if (analyzeClipStabilization(l.project, l.seq(), c, motion, {}, nullptr, &err)) e.strings["motion"] = motion;
                else note = QStringLiteral(" (the sky stays still: %1)").arg(QString::fromStdString(err));
            }
            c.effects.push_back(e);
            save(l);
            return ok(QStringLiteral("Added %1 to %2%3").arg(QString::fromStdString(displayName), QString::fromStdString(c.name), note),
                      QJsonObject{{"effect_id", double(e.id)}});
        });

    add("montage_track_effect", "Add an effect to an audio track",
        "Add an audio effect to an audio track's inserts (after its clips are mixed, before its fader), or with `remove` "
        "take one off by its id. A compressor or noise gate can listen to another track instead of its own signal: "
        "strings {\"sidechain\": \"A1\"} (a track's reference or name) ducks music under dialogue or opens a gate on a "
        "kick; key_hpf_hz high-passes what it listens to.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"track":{"type":"string","description":"A1, A2..."},
            "effect":{"type":"string"},"params":{"type":"object","additionalProperties":{"type":"number"}},
            "strings":{"type":"object","additionalProperties":{"type":"string"}},
            "remove":{"type":"number","description":"An effect id to take off instead"}},"required":["project","track"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const TrackRef r = trackArg(need(a, "track"), s, false);
            if (r.kind != TrackKind::Audio) throw ArgError{"Effects go on audio tracks (A1, A2...)"};
            Track& t = s.audioTracks[size_t(r.index)];
            if (a.contains("remove")) {
                const Id id = Id(a.value("remove").toDouble());
                const auto before = t.effects.size();
                std::erase_if(t.effects, [&](const Effect& e) { return e.id == id; });
                if (t.effects.size() == before) return fail(QStringLiteral("No effect %1 on %2").arg(qint64(id)).arg(need(a, "track")));
                save(l);
                return ok(QStringLiteral("Removed it"), QJsonObject{{"effects", int(t.effects.size())}});
            }
            const std::string type = need(a, "effect").toStdString();
            const EffectInfo* info = findEffectInfo(type);
            if (!info || info->hidden || info->category != EffectCategory::AudioFilter)
                throw ArgError{QStringLiteral("Unknown audio effect \"%1\" (see montage_list_effects)").arg(QString::fromStdString(type))};
            if (isSourceAudioEffect(type)) throw ArgError{QStringLiteral("%1 works on a clip's own audio: add it to the clip").arg(QString::fromStdString(info->displayName))};
            Effect e = makeEffect(l.project, type);
            const QJsonObject params = a.value("params").toObject();
            for (auto it = params.begin(); it != params.end(); ++it) {
                const std::string name = it.key().toStdString();
                if (std::none_of(info->params.begin(), info->params.end(), [&](const ParamInfo& p) { return p.name == name; }))
                    throw ArgError{QStringLiteral("\"%1\" has no parameter \"%2\"").arg(QString::fromStdString(type), it.key())};
                e.params[name] = Param(it.value().toDouble());
            }
            const QJsonObject strings = a.value("strings").toObject();
            for (auto it = strings.begin(); it != strings.end(); ++it) {
                const std::string name = it.key().toStdString();
                const auto si = std::find_if(info->strings.begin(), info->strings.end(), [&](const StringParamInfo& x) { return x.name == name; });
                if (si == info->strings.end())
                    throw ArgError{QStringLiteral("\"%1\" has no text setting \"%2\"").arg(QString::fromStdString(type), it.key())};
                e.strings[name] = si->kind == StringKind::Track ? audioTrackId(it.value().toString(), s) : it.value().toString().toStdString();
            }
            if (e.s("sidechain") == std::to_string(t.id)) throw ArgError{"A track cannot be its own sidechain"};
            t.effects.push_back(e);
            save(l);
            return ok(QStringLiteral("Added %1 to %2").arg(QString::fromStdString(info->displayName), QString::fromStdString(t.name)),
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
        "measure what they render anyway. Warns when MaxCLL is above the sequence's mastering peak. A PQ sequence's "
        "scenes for dynamic HDR metadata are measured in the same pass (each scene's brightest red, green and blue, average and spread, "
        "as ST 2094-40 sends them) and kept too, for exports with `hdr10plus`.",
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
            const bool pq = sequenceColorSpace(s).transfer == Transfer::Pq;
            std::vector<Hdr10PlusScene> scenes;
            bool measuredOk = false;
            if (!pq) {
                measuredOk = measureLightLevels(l.project, s, from, to, light, &err);
            } else {
                // HDR10+ over whole scenes (grown to the stored scenes the stretch cuts into), light levels over the stretch.
                const FrameTime lo = std::max<FrameTime>(0, from), hi = to > lo ? to : s.duration();
                FrameTime a = lo, b = hi;
                widenHdr10PlusRange(s.hdr10Plus, a, b);
                const LightMeter meter(sequenceColorSpace(s));
                const auto frame = [&](FrameTime f) {
                    RenderOptions o;
                    o.scale = 1.0;
                    o.highQuality = true;
                    Image img = renderProgramFrame(l.project, s, f, o);
                    if (f >= lo && f < hi) meter.add(img, f, light);
                    return img;
                };
                measuredOk = analyseHdr10Plus(l.project, s, a, b, sequenceColorSpace(s), std::clamp(s.hdrPeakNits, 100.0, 10000.0), scenes,
                                              &err, {}, nullptr, nullptr, frame);
            }
            if (!measuredOk) return fail(QString::fromStdString(err));
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
            if (pq) {
                QJsonArray list;
                for (const Hdr10PlusScene& sc : scenes)
                    list.append(QJsonObject{{"start", tc(sc.start, s)}, {"end", tc(sc.end, s)},
                                            {"max_rgb", QJsonArray{sc.maxScl[0], sc.maxScl[1], sc.maxScl[2]}},
                                            {"average", sc.average}, {"percentile_99_98", sc.percentiles[6]}});
                out["hdr10plus_scenes"] = list;
                text += QStringLiteral("; dynamic HDR metadata: %1 scene(s)").arg(scenes.size());
            }
            if (a.value("save").toBool(true)) {
                s.hdrMaxCll = cll, s.hdrMaxFall = fall;
                if (pq) mergeHdr10PlusScenes(s.hdr10Plus, scenes);  // replacing those they overlap; others stay
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

    add("montage_automate_track", "Automate a track's or bus's mix",
        "Set an audio track's mix automation, as written from the mixer: its mode (off, read, write, latch, touch) and "
        "volume (dB), pan (-1 left to 1 right) and mute (1 muted, 0 not; a switch held to the next point) points at times; "
        "points replace the lane's points between the first and last given. sends sets aux sends to buses by name (a bus "
        "not there yet is made, as an effects return): level_db, pre_fader, muted, level points, or remove. inserts sets "
        "points on the track's insert effects' parameters (an effect's id, a parameter's name as montage_describe lists it, "
        "or param.<id> for a plugin's; on/off and choice parameters hold to the next point). With bus, the named bus's "
        "mode, volume, pan, mute and inserts are set instead. clear removes the lanes. Lists the automation afterwards.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"track":{"type":"string","default":"A1"},
            "bus":{"type":"string","description":"Automate this bus (by name) instead of a track"},
            "mode":{"type":"string","enum":["off","read","write","latch","touch"]},
            "volume":{"type":"array","items":{"type":"array","items":{"type":["number","string"]},"minItems":2,"maxItems":2},
                      "description":"[[time, dB], ...]"},
            "pan":{"type":"array","items":{"type":"array","items":{"type":["number","string"]},"minItems":2,"maxItems":2},
                   "description":"[[time, pan], ...]"},
            "mute":{"type":"array","items":{"type":"array","items":{"type":["number","string"]},"minItems":2,"maxItems":2},
                    "description":"[[time, 1 or 0], ...]"},
            "sends":{"type":"array","items":{"type":"object","properties":{
                "bus":{"type":"string"},"level_db":{"type":"number"},"pre_fader":{"type":"boolean"},"muted":{"type":"boolean"},
                "remove":{"type":"boolean"},
                "level":{"type":"array","items":{"type":"array","items":{"type":["number","string"]},"minItems":2,"maxItems":2},
                         "description":"[[time, dB], ...]"}},"required":["bus"]}},
            "inserts":{"type":"array","items":{"type":"object","properties":{
                "effect":{"type":"number","description":"The insert's effect id"},"param":{"type":"string"},
                "points":{"type":"array","items":{"type":"array","items":{"type":["number","string"]},"minItems":2,"maxItems":2},
                          "description":"[[time, value], ...]"},
                "clear":{"type":"boolean","description":"Remove this parameter's points first"}},"required":["effect","param"]}},
            "clear":{"type":"boolean","default":false}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const QStringList names{"off", "read", "write", "latch", "touch"};
            auto mode = [&]() {
                const QString m = str(a, "mode").toLower();
                if (!names.contains(m)) throw ArgError{QStringLiteral("\"mode\" must be off, read, write, latch or touch")};
                return int(names.indexOf(m));
            };
            // (hold: a switch's keys hold to the next; whole: its values are whole numbers, a mute's 0 or 1, a choice's index)
            auto points = [&](const QJsonArray& pts, const QString& key, Param& lane, double lo, double hi, bool hold, bool whole = true) {
                if (pts.isEmpty()) return;
                std::vector<Keyframe> keys;
                for (const auto& v : pts) {
                    const QJsonArray pt = v.toArray();
                    if (pt.size() != 2) throw ArgError{QStringLiteral("\"%1\" points are [time, value]").arg(key)};
                    Keyframe k{timeArg(pt.at(0), s, key.toLatin1().constData()), std::clamp(pt.at(1).toDouble(), lo, hi)};
                    if (hold) k.interp = Interp::Hold;
                    if (hold && whole) k.v = std::round(k.v);
                    keys.push_back(k);
                }
                std::sort(keys.begin(), keys.end(), [](const Keyframe& x, const Keyframe& y) { return x.t < y.t; });
                // A switch stays as it was before its first point (a lane's first key otherwise reaches back to the start).
                const FrameTime first = keys.front().t;
                if (hold && first > 0 && std::none_of(lane.keys.begin(), lane.keys.end(), [&](const Keyframe& k) { return k.t < first; })) {
                    const double was = lane.animated() ? lane.at(first - 1) : lane.value;
                    Keyframe before{first - 1, whole ? std::round(was) : was};
                    before.interp = Interp::Hold;
                    keys.insert(keys.begin(), before);
                }
                std::erase_if(lane.keys, [&](const Keyframe& k) { return k.t >= keys.front().t && k.t <= keys.back().t; });
                for (const Keyframe& k : keys) lane.keys.push_back(k);
                std::sort(lane.keys.begin(), lane.keys.end(), [](const Keyframe& x, const Keyframe& y) { return x.t < y.t; });
            };
            auto describe = [&](const Param& lane, const char* unit) {
                QStringList parts;
                for (const Keyframe& k : lane.keys) parts << QStringLiteral("%1 %2%3").arg(tc(k.t, s)).arg(k.v, 0, 'f', 2).arg(QString::fromLatin1(unit));
                return parts.isEmpty() ? QStringLiteral("none") : parts.join(QStringLiteral(", "));
            };
            auto findBus = [&](const QString& name) -> Bus* {
                for (Bus& b : s.buses)
                    if (QString::fromStdString(b.name).compare(name, Qt::CaseInsensitive) == 0) return &b;
                return nullptr;
            };
            // Points on a chain's insert parameters, and the animated ones listed.
            auto insertPoints = [&](std::vector<Effect>& chain) {
                for (const auto& v : a.value("inserts").toArray()) {
                    const QJsonObject io = v.toObject();
                    const Id id = Id(io.value("effect").toDouble());
                    const auto e = std::find_if(chain.begin(), chain.end(), [id](const Effect& x) { return x.id == id; });
                    if (e == chain.end()) throw ArgError{QStringLiteral("No insert effect %1 here").arg(id)};
                    const std::string param = io.value("param").toString().trimmed().toStdString();
                    // The parameter as the Inspector lists it (a plugin's as recorded when it was added): its range, and
                    // whether it is a switch (on/off, a choice, a plugin's stepped parameter at its own values).
                    double lo = 0, hi = 0, def = 0;
                    bool hold = false, whole = false, known = false;
                    for (const ParamInfo& pi : effectParams(*e))
                        if (pi.name == param && pi.kind != ParamKind::Color) {
                            lo = pi.min, hi = pi.max, def = pi.def, known = true;
                            whole = pi.kind == ParamKind::Bool || pi.kind == ParamKind::Choice;
                            hold = whole;
                        }
                    // A plugin's stepped parameter, as recorded when it was added ("meta.<id>": label, min, max, default,
                    // stepped...); a continuous one with a wide range (20 to 20000 Hz) ramps.
                    if (known && e->type == "plugin" && param.rfind("param.", 0) == 0) {
                        const QStringList meta = QString::fromStdString(e->s("meta." + param.substr(6))).split('\t');
                        hold = hold || (meta.size() >= 5 && meta[4] == QStringLiteral("1"));
                    }
                    if (!known) throw ArgError{QStringLiteral("%1 has no parameter %2").arg(QString::fromStdString(e->type), QString::fromStdString(param))};
                    const QJsonArray pts = io.value("points").toArray();
                    if (!e->params.count(param)) e->params[param] = Param(def);  // not set yet: its default outside the points
                    Param& lane = e->params[param];
                    if (io.value("clear").toBool()) lane.keys.clear();
                    points(pts, QStringLiteral("points"), lane, lo, hi, hold, whole);
                }
            };
            auto describeInserts = [&](const std::vector<Effect>& chain) {
                QStringList parts;
                for (const Effect& e : chain)
                    for (const auto& [name, lane] : e.params)
                        if (lane.animated())
                            parts << QStringLiteral("%1 (%2) %3: %4").arg(QString::fromStdString(e.type)).arg(e.id)
                                         .arg(QString::fromStdString(name), describe(lane, ""));
                return parts.isEmpty() ? QStringLiteral("none") : parts.join(QStringLiteral("; "));
            };
            if (a.contains("bus")) {
                Bus* b = findBus(str(a, "bus"));
                if (!b) throw ArgError{QStringLiteral("No bus called %1").arg(str(a, "bus"))};
                if (a.value("clear").toBool()) b->volumeAuto = Param(), b->panAuto = Param(), b->muteAuto = Param();
                if (a.contains("mode")) b->automation = mode();
                points(a.value("volume").toArray(), QStringLiteral("volume"), b->volumeAuto, -96, 12, false);
                points(a.value("pan").toArray(), QStringLiteral("pan"), b->panAuto, -1, 1, false);
                points(a.value("mute").toArray(), QStringLiteral("mute"), b->muteAuto, 0, 1, true);
                insertPoints(b->effects);
                save(l);
                return ok(QStringLiteral("Bus %1: %2 automation\nVolume: %3\nPan: %4\nMute: %5\nInserts: %6")
                              .arg(QString::fromStdString(b->name), QString::fromLatin1(automationModeName(busAutomation(*b))),
                                   describe(b->volumeAuto, " dB"), describe(b->panAuto, ""), describe(b->muteAuto, ""),
                                   describeInserts(b->effects)));
            }
            const TrackRef ref = trackArg(str(a, "track", "A1"), s, false);
            if (ref.kind != TrackKind::Audio) throw ArgError{QStringLiteral("Automation is for audio tracks (A1, A2...)")};
            const int ti = ref.index;
            if (a.value("clear").toBool()) {
                Track& t = s.audioTracks.at(size_t(ti));
                t.volumeAuto = Param(), t.panAuto = Param(), t.muteAuto = Param();
                for (Send& sd : t.sends) sd.levelAuto = Param();
            }
            if (a.contains("mode")) s.audioTracks.at(size_t(ti)).automation = mode();
            for (const auto& v : a.value("sends").toArray()) {
                const QJsonObject so = v.toObject();
                const QString name = so.value("bus").toString().trimmed();
                if (name.isEmpty()) throw ArgError{QStringLiteral("Each send names its bus")};
                Bus* b = findBus(name);
                if (!b) {
                    if (so.value("remove").toBool()) throw ArgError{QStringLiteral("No bus called %1").arg(name)};
                    Bus nb;  // a new effects return
                    nb.id = l.project.newId();
                    nb.name = name.toStdString();
                    s.buses.push_back(nb);
                    b = &s.buses.back();
                }
                const Id bus = b->id;
                if (so.value("remove").toBool()) {
                    if (auto r = edit::removeSend(s, ti, bus); !r.ok) throw ArgError{QString::fromStdString(r.error)};
                    continue;
                }
                const auto& sends = s.audioTracks.at(size_t(ti)).sends;
                auto was = std::find_if(sends.begin(), sends.end(), [bus](const Send& x) { return x.bus == bus; });
                const double level = so.contains("level_db") ? so.value("level_db").toDouble() : (was != sends.end() ? was->levelDb : 0.0);
                const bool pre = so.contains("pre_fader") ? so.value("pre_fader").toBool() : (was != sends.end() && was->preFader);
                if (auto r = edit::setSend(s, ti, bus, level, pre); !r.ok) throw ArgError{QString::fromStdString(r.error)};
                for (Send& sd : s.audioTracks.at(size_t(ti)).sends)
                    if (sd.bus == bus) {
                        if (so.contains("muted")) sd.muted = so.value("muted").toBool();
                        points(so.value("level").toArray(), QStringLiteral("level"), sd.levelAuto, -96, 12, false);
                    }
            }
            Track& t = s.audioTracks.at(size_t(ti));
            points(a.value("volume").toArray(), QStringLiteral("volume"), t.volumeAuto, -60, 12, false);
            points(a.value("pan").toArray(), QStringLiteral("pan"), t.panAuto, -1, 1, false);
            points(a.value("mute").toArray(), QStringLiteral("mute"), t.muteAuto, 0, 1, true);
            insertPoints(t.effects);
            save(l);
            QStringList sendText;
            for (const Send& sd : t.sends) {
                QString busName = QStringLiteral("?");
                for (const Bus& b : s.buses)
                    if (b.id == sd.bus) busName = QString::fromStdString(b.name);
                sendText << QStringLiteral("%1 %2 dB%3%4%5").arg(busName).arg(sd.levelDb, 0, 'f', 1)
                                .arg(sd.preFader ? QStringLiteral(" pre-fader") : QString(), sd.muted ? QStringLiteral(" (muted)") : QString(),
                                     sd.levelAuto.animated() ? QStringLiteral(", level: ") + describe(sd.levelAuto, " dB") : QString());
            }
            return ok(QStringLiteral("%1: %2 automation\nVolume: %3\nPan: %4\nMute: %5\nSends: %6\nInserts: %7")
                          .arg(str(a, "track", "A1"), QString::fromLatin1(automationModeName(trackAutomation(t))), describe(t.volumeAuto, " dB"),
                               describe(t.panAuto, ""), describe(t.muteAuto, ""),
                               sendText.isEmpty() ? QStringLiteral("none") : sendText.join(QStringLiteral("; ")), describeInserts(t.effects)));
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
            const WorkingSpaceScope working(&sequenceColorSpace(s));
            const Lut3D lut = bakeClipLut(s, c, t, a.value("size").toInt(33), &skipped);
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
        "captions (each track's language) and titles (`title_language`, default en-US), the project's vocabulary allowed, "
        "and linked clips out of sync with their picture (`sync`). Lists each problem with its timecodes; with markers, puts a red \"QC:\" marker on each (replacing earlier ones).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"from":{"type":["number","string"]},
            "to":{"type":["number","string"]},"flashing":{"type":"boolean","default":true},"levels":{"type":"boolean","default":true},
            "black_seconds":{"type":"number","default":1,"description":"0 = not checked"},
            "freeze_seconds":{"type":"number","default":5,"description":"0 = not checked"},
            "silence_seconds":{"type":"number","default":2,"description":"0 = not checked"},
            "clipping":{"type":"boolean","default":true},
            "loudness_target":{"type":"number","description":"LUFS, e.g. -14 (streaming) or -23 (EBU R128); omitted = not checked"},
            "peak_ceiling":{"type":"number","default":-1,"description":"dBTP, checked with the loudness"},
            "loudness_dialogue":{"type":"boolean","default":false,"description":"Check the dialogue's loudness (montage_dialogue_loudness), as Netflix does: -27 with peak_ceiling -2"},
            "loudness_tolerance":{"type":"number","description":"LU either side of the target (default 1, 2 with loudness_dialogue)"},
            "spelling":{"type":"boolean","default":true},"title_language":{"type":"string","default":"en-US"},
            "sync":{"type":"boolean","default":true},"markers":{"type":"boolean","default":false}},"required":["project"]})json",
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
            q.dialogueGated = a.value("loudness_dialogue").toBool(false);
            q.loudnessTolerance = std::max(0.1, a.value("loudness_tolerance").toDouble(q.dialogueGated ? 2 : 1));
            q.spelling = a.value("spelling").toBool(true);
            q.titleLanguage = str(a, "title_language", "en-US").toStdString();
            q.sync = a.value("sync").toBool(true);
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

    add("montage_dialogue_loudness", "Measure dialogue loudness",
        "Measure the sequence's (or from..to's) loudness over its speech only, the \"Dialog LKFS\" Netflix delivers at "
        "(-27 LKFS ±2, true peaks under -2 dBTP), beside the programme's BS.1770 loudness, its true peak and the share of "
        "the sound that is speech (under 15 %, the programme figure is the one that counts: `value`). The speech is found "
        "from the Dialogue clips (`gate` roles), transcribed words (transcripts) or by listening (classifier); auto takes "
        "the first the sequence has. Speech-gated loudness from Montage's own speech detection: an estimate of the dialogue "
        "measurement Netflix's QC makes.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"from":{"type":["number","string"]},
            "to":{"type":["number","string"]},"gate":{"type":"string","enum":["auto","roles","transcripts","classifier"],"default":"auto"},
            "surround":{"type":"boolean","default":false,"description":"Measure a surround sequence's own channels, not its stereo fold-down"}},
            "required":["project"]})json",
        true, [this](const QJsonObject& a) {
            Loaded l = open(a);
            const Sequence& s = l.seq();
            DialogueLoudnessOptions o;
            o.in = a.contains("from") ? timeArg(a.value("from"), s, "from") : 0;
            o.out = a.contains("to") ? timeArg(a.value("to"), s, "to") : -1;
            o.layout = a.value("surround").toBool(false);
            if (!parseSpeechGate(str(a, "gate", "auto").toStdString(), o.gate))
                throw ArgError{"\"gate\" is auto, roles, transcripts or classifier"};
            std::string why;
            const DialogueLoudness d = measureDialogueLoudness(l.project, s, o, [this](double f) { progress(f, "Measuring"); }, nullptr, &why);
            if (!why.empty()) return fail(QString::fromStdString(why));
            QJsonArray speech;
            for (const auto& [from, to] : d.speech) speech.append(QJsonArray{from, to});
            const QJsonObject r{{"valid", d.valid}, {"dialogue_lkfs", d.dialogue}, {"programme_lkfs", d.programme},
                                {"true_peak_dbtp", d.truePeak}, {"speech_share", d.speechShare}, {"uses_programme", d.usesProgramme},
                                {"value", d.value()}, {"gate", QString::fromLatin1(speechGateName(d.gate))}, {"estimate", true},
                                {"speech_seconds", speech}, {"text", QString::fromStdString(describeDialogueLoudness(d))}};
            return ok(json(r), r);
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
        "page (montage_export_review; one marker a note, named and coloured by reviewer, with its replies, status and "
        "drawing; notes imported before bring their markers up to date, and a note never replaces another marker). Give "
        "the file's `path` or its `text`.",
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
            const MarkerImport r = addImportedMarkers(s, markers);
            save(l);
            return ok(r.updated ? QStringLiteral("Added %1 marker(s), brought %2 up to date").arg(r.added).arg(r.updated)
                                : QStringLiteral("Added %1 marker(s)").arg(r.added),
                      QJsonObject{{"added", r.added}, {"updated", r.updated}});
        });

    add("montage_ale", "Avid Log Exchange (ALE) in and out",
        "import: read an ALE (as dailies arrive from Media Composer, Resolve, Silverstack or the lab) and fill in each "
        "matching media item's scene, shot, take, tape, camera, description and comment, and its ASC CDL; rows find their "
        "media by source file, by name (with or without extension) or by tape and start timecode. With cdl_to_clips "
        "(default) every clip of a media item that got a CDL is graded with it (its cdl effect). export: the media (all, "
        "or those named in `media`) as an ALE, timecodes at the sequence's rate, with CDL columns when any has one. Give a "
        "`path`, or `text` to import from.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"action":{"type":"string","enum":["import","export"]},
            "path":{"type":"string"},"text":{"type":"string"},"cdl_to_clips":{"type":"boolean","default":true},
            "media":{"type":"array","items":{"type":"string"},"description":"export: media names (default all)"}},
            "required":["project","action"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const QString action = need(a, "action");
            if (action == "import") {
                std::string text = str(a, "text").toStdString();
                if (a.contains("path")) {
                    QFile f(absolute(need(a, "path")));
                    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Cannot read %1").arg(f.fileName()));
                    text = f.readAll().toStdString();
                }
                AleTable table;
                std::string err;
                if (!parseAle(text, table, &err)) return fail(QString::fromStdString(err));
                const AleImport r = applyAle(l.project, table, a.value("cdl_to_clips").toBool(true));
                if (!r.matched.empty()) save(l);
                QJsonArray unmatched;
                for (const std::string& u : r.unmatched) unmatched.append(QString::fromStdString(u));
                return ok(QStringLiteral("%1 of %2 row(s) matched media; %3 CDL(s); %4 clip(s) graded")
                              .arg(r.matched.size()).arg(r.rows).arg(r.cdls).arg(r.clips),
                          QJsonObject{{"rows", r.rows}, {"matched", int(r.matched.size())}, {"cdls", r.cdls}, {"clips", r.clips}, {"unmatched", unmatched}});
            }
            if (action != "export") throw ArgError{QStringLiteral("\"action\" must be import or export")};
            std::vector<Id> ids;
            if (a.contains("media")) {
                for (const QJsonValue& v : a.value("media").toArray()) {
                    const MediaItem* found = nullptr;
                    for (const MediaItem& m : l.project.media)
                        if (!found && QString::fromStdString(m.name).compare(v.toString(), Qt::CaseInsensitive) == 0) found = &m;
                    if (!found) throw ArgError{QStringLiteral("No media named %1").arg(v.toString())};
                    ids.push_back(found->id);
                }
            } else {
                for (const MediaItem& m : l.project.media) ids.push_back(m.id);
            }
            const AleTable table = aleFromMedia(l.project, ids, l.seq().fps);
            if (table.rows.empty()) return fail(QStringLiteral("No media to log (stills and sequences are left out)"));
            const std::string text = writeAle(table);
            if (a.contains("path")) {
                QFile f(absolute(need(a, "path")));
                if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(text.data(), qint64(text.size())) != qint64(text.size()))
                    return fail(QStringLiteral("Cannot write %1").arg(f.fileName()));
                return ok(QStringLiteral("Wrote %1 media item(s) to %2").arg(table.rows.size()).arg(f.fileName()));
            }
            return ok(QString::fromStdString(text));
        });

    add("montage_cdl", "ASC CDL on clips",
        "get: a clip's ASC CDL (slope, offset, power per channel, saturation, the space it is applied in). set: give a "
        "clip slope, offset and power (three numbers each, or one for all) and saturation, as its cdl effect; `space` is "
        "\"Working space\" or a colour space it was made in (a camera log, ACEScct). import: a .cc, .ccc or .cdl (path or "
        "text): onto `clip` (the correction whose id matches it, else the first), or without a clip onto every video clip "
        "whose name, file or tape matches a correction's id. export: the clip's CDL (or every clip's that has one) as cdl, "
        "cc or ccc XML, to `path` or returned.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"action":{"type":"string","enum":["get","set","import","export"]},
            "clip":{"type":"integer"},"slope":{"type":["array","number"]},"offset":{"type":["array","number"]},"power":{"type":["array","number"]},
            "saturation":{"type":"number"},"space":{"type":"string"},"id":{"type":"string"},"path":{"type":"string"},"text":{"type":"string"},
            "format":{"type":"string","enum":["cdl","cc","ccc"]}},"required":["project","action"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const QString action = need(a, "action");
            auto describe = [&](const Clip& c, const Cdl& g) {
                auto arr = [](const double v[3]) { return QJsonArray{v[0], v[1], v[2]}; };
                QString space = "Working space";
                for (const Effect& e : c.effects)
                    if (e.type == "cdl" && e.enabled) {
                        space = QString::fromStdString(e.s("space", "Working space"));
                        break;
                    }
                return QJsonObject{{"clip", double(c.id)}, {"id", QString::fromStdString(g.id)}, {"slope", arr(g.slope)}, {"offset", arr(g.offset)},
                                   {"power", arr(g.power)}, {"saturation", g.saturation}, {"space", space},
                                   {"asc_sop", QString::fromStdString(cdlSopText(g))}};
            };
            if (action == "get") {
                const Clip& c = clipArg(l, a);
                Cdl g;
                if (!clipCdl(c, 0, g)) return ok(QStringLiteral("Clip %1 has no CDL").arg(qulonglong(c.id)), QJsonObject{{"clip", double(c.id)}});
                const QJsonObject o = describe(c, g);
                return ok(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)), o);
            }
            if (action == "set") {
                Clip& c = clipArg(l, a);
                if (c.isGenerator()) throw ArgError{QStringLiteral("A CDL grades footage, not a generator")};
                if (auto loc = edit::locate(s, c.id); !loc || loc->track.kind != TrackKind::Video) throw ArgError{QStringLiteral("A CDL goes on a video clip")};
                Cdl g;
                clipCdl(c, 0, g);
                auto three = [&](const char* key, double out[3], double lo) {
                    if (!a.contains(key)) return;
                    const QJsonValue v = a.value(key);
                    if (v.isDouble()) {
                        out[0] = out[1] = out[2] = v.toDouble();
                    } else {
                        const QJsonArray arr = v.toArray();
                        if (arr.size() != 3) throw ArgError{QStringLiteral("\"%1\" must be one number or three").arg(key)};
                        for (int i = 0; i < 3; ++i) {
                            if (!arr[i].isDouble()) throw ArgError{QStringLiteral("\"%1\" must be numbers").arg(key)};
                            out[i] = arr[i].toDouble();
                        }
                    }
                    for (int i = 0; i < 3; ++i)
                        if (!std::isfinite(out[i]) || out[i] < lo) throw ArgError{QStringLiteral("\"%1\" is out of range").arg(key)};
                };
                three("slope", g.slope, 0);
                three("offset", g.offset, -1e9);
                three("power", g.power, 1e-6);
                if (a.contains("saturation")) {
                    if (!a.value("saturation").isDouble() || a.value("saturation").toDouble() < 0) throw ArgError{QStringLiteral("\"saturation\" must be a number of 0 or more")};
                    g.saturation = a.value("saturation").toDouble();
                }
                if (a.contains("id")) g.id = str(a, "id").toStdString();
                QString space;
                if (a.contains("space")) {
                    space = str(a, "space");
                    bool known = space == "Working space";
                    for (const auto& cs : colorSpaces()) known |= QString::fromStdString(cs.label) == space;
                    if (!known) throw ArgError{QStringLiteral("Unknown colour space \"%1\" (see montage_list_effects for cdl's choices)").arg(space)};
                }
                setClipCdl(l.project, c, g);
                if (!space.isEmpty())
                    for (Effect& e : c.effects)
                        if (e.type == "cdl") {
                            e.strings["space"] = space.toStdString();
                            break;
                        }
                save(l);
                const QJsonObject o = describe(c, g);
                return ok(QStringLiteral("Clip %1 graded: %2, saturation %3").arg(qulonglong(c.id)).arg(QString::fromStdString(cdlSopText(g))).arg(g.saturation), o);
            }
            if (action == "import") {
                std::string text = str(a, "text").toStdString();
                if (a.contains("path")) {
                    QFile f(absolute(need(a, "path")));
                    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Cannot read %1").arg(f.fileName()));
                    text = f.readAll().toStdString();
                }
                std::string err;
                const std::vector<Cdl> cdls = parseCdlXml(text, &err);
                if (cdls.empty()) return fail(QString::fromStdString(err));
                auto matching = [&](const Clip& c) -> const Cdl* {
                    std::vector<std::string> names{c.name};
                    if (const MediaItem* m = l.project.findMedia(c.mediaId)) {
                        names.push_back(m->name);
                        if (!m->path.empty()) names.push_back(QFileInfo(QString::fromStdString(uninterpretedPath(m->path))).fileName().toStdString());
                        if (auto t = m->metadata.find("tape"); t != m->metadata.end()) names.push_back(t->second);
                    }
                    return matchCdl(cdls, names);
                };
                int graded = 0;
                if (a.contains("clip")) {
                    Clip& c = clipArg(l, a);
                    if (c.isGenerator()) throw ArgError{QStringLiteral("A CDL grades footage, not a generator")};
                    if (auto loc = edit::locate(s, c.id); !loc || loc->track.kind != TrackKind::Video) throw ArgError{QStringLiteral("A CDL goes on a video clip")};
                    const Cdl* g = matching(c);
                    setClipCdl(l.project, c, g ? *g : cdls.front());
                    graded = 1;
                } else {
                    if (cdls.size() == 1 && cdls.front().id.empty()) throw ArgError{QStringLiteral("The correction has no id to match: give \"clip\"")};
                    for (Track& t : s.videoTracks)
                        for (Clip& c : t.clips)
                            if (!c.isGenerator())
                                if (const Cdl* g = matching(c)) {
                                    setClipCdl(l.project, c, *g);
                                    ++graded;
                                }
                }
                if (graded) save(l);
                QString msg = QStringLiteral("%1 correction(s) read; %2 clip(s) graded").arg(cdls.size()).arg(graded);
                if (!err.empty()) msg += QStringLiteral(" (%1)").arg(QString::fromStdString(err));
                return ok(msg, QJsonObject{{"corrections", int(cdls.size())}, {"clips", graded}});
            }
            if (action != "export") throw ArgError{QStringLiteral("\"action\" must be get, set, import or export")};
            std::vector<Cdl> cdls;
            auto take = [&](const Clip& c) {
                Cdl g;
                if (!clipCdl(c, 0, g)) return;
                if (g.id.empty()) g.id = c.name;
                cdls.push_back(g);
            };
            if (a.contains("clip")) take(clipArg(l, a));
            else
                for (const Track& t : s.videoTracks)
                    for (const Clip& c : t.clips) take(c);
            if (cdls.empty()) return fail(QStringLiteral("No CDL to export"));
            const QString fmt = str(a, "format", a.contains("path") ? QString::fromStdString(std::string(cdlFormatFor(need(a, "path").toStdString()) == CdlFormat::Cc ? "cc" : cdlFormatFor(need(a, "path").toStdString()) == CdlFormat::Ccc ? "ccc" : "cdl")) : QStringLiteral("cdl"));
            const CdlFormat format = fmt == "cc" ? CdlFormat::Cc : fmt == "ccc" ? CdlFormat::Ccc : CdlFormat::Cdl;
            if (format == CdlFormat::Cc && cdls.size() > 1) return fail(QStringLiteral("A .cc holds one correction; %1 clips have CDLs (give \"clip\", or use ccc or cdl)").arg(cdls.size()));
            const std::string xml = writeCdlXml(cdls, format);
            if (a.contains("path")) {
                QFile f(absolute(need(a, "path")));
                if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(xml.data(), qint64(xml.size())) != qint64(xml.size()))
                    return fail(QStringLiteral("Cannot write %1").arg(f.fileName()));
                return ok(QStringLiteral("Wrote %1 correction(s) to %2").arg(cdls.size()).arg(f.fileName()));
            }
            return ok(QString::fromStdString(xml));
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
                    const bool found = m->kind == MediaKind::Video && readSlateFromPicture(analysisSource(*m), slate, &why);
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
            const QString file = a.value("script_file").toString();
            if (!file.isEmpty()) {
                QFile f(absolute(file));
                if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Cannot open %1").arg(file));
                text = f.readAll().toStdString();
            }
            ProjectScript given;
            given.format = scriptFormat(file.toStdString(), text);
            given.text = text;
            const auto lines = readScreenplay(given).lines;
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

    add("montage_script", "The project's script and its lined coverage",
        "Keep a screenplay in the project and see which takes read each of its lines (the lined script): action \"set\" "
        "reads a script (text or a file: Fountain, Final Draft .fdx or plain text) and finds each line in the transcribed "
        "takes; \"match\" finds them again after more takes are transcribed; \"status\" lists the lines with their takes "
        "(best first, circled ones marked) and which one the active sequence uses; \"pdf\" writes the lined script; "
        "\"remove\" takes the script away. Transcribe the takes first.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "action":{"type":"string","enum":["set","match","status","pdf","remove"],"default":"status"},
            "script":{"type":"string","description":"set: the script's text"},
            "script_file":{"type":"string","description":"set: or a file holding it"},
            "min_coverage":{"type":"number","default":0.6,"description":"set, match: share of a line a reading must say (0.2-1)"},
            "path":{"type":"string","description":"pdf: where to write it"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const QString action = a.value("action").toString(QStringLiteral("status"));
            ScriptCutOptions o;
            o.minCoverage = std::clamp(a.value("min_coverage").toDouble(0.6), 0.2, 1.0);
            auto status = [&](const QString& message) {
                const Screenplay sp = readScreenplay(l.project.script);
                const std::vector<int> used = l.project.active() ? scriptLinesInCut(l.project, *l.project.active()) : std::vector<int>();
                QJsonArray lines;
                int read = 0, inCut = 0;
                for (size_t i = 0; i < sp.lines.size(); ++i) {
                    QJsonObject line{{"line", int(i)}, {"text", QString::fromStdString(sp.lines[i].text)}};
                    if (!sp.lines[i].speaker.empty()) line["speaker"] = QString::fromStdString(sp.lines[i].speaker);
                    QJsonArray takes;
                    const auto* readings = i < l.project.script.readings.size() ? &l.project.script.readings[i] : nullptr;
                    for (size_t k = 0; readings && k < readings->size(); ++k) {
                        const ScriptReading& r = (*readings)[k];
                        const MediaItem* m = l.project.findMedia(r.media);
                        if (!m) continue;
                        QJsonObject t{{"media", QString::fromStdString(m->name)}, {"start", r.start}, {"end", r.end},
                                      {"coverage", r.coverage}, {"extra_words", r.extraWords}};
                        if (isCircledTake(*m)) t["circled"] = true;
                        if (i < used.size() && used[i] == int(k)) t["in_cut"] = true;
                        takes.append(t);
                    }
                    read += takes.isEmpty() ? 0 : 1;
                    inCut += i < used.size() && used[i] >= 0 ? 1 : 0;
                    line["takes"] = takes;
                    lines.append(line);
                }
                return ok(message.isEmpty() ? QStringLiteral("%1 of %2 line(s) read by a take, %3 in the cut").arg(read).arg(sp.lines.size()).arg(inCut)
                                            : message,
                          QJsonObject{{"name", QString::fromStdString(l.project.script.name)},
                                      {"format", QString::fromStdString(l.project.script.format)},
                                      {"title", QString::fromStdString(sp.title)}, {"read", read}, {"in_cut", inCut}, {"lines", lines}});
            };
            if (action == QLatin1String("set")) {
                std::string text = a.value("script").toString().toStdString();
                QString name = QStringLiteral("Script");
                if (const QString file = a.value("script_file").toString(); !file.isEmpty()) {
                    QFile f(absolute(file));
                    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Cannot open %1").arg(file));
                    text = f.readAll().toStdString();
                    name = absolute(file);
                }
                if (QString::fromStdString(text).trimmed().isEmpty()) return fail("Pass script or script_file");
                const int found = setProjectScript(l.project, name.toStdString(), "", text, o);
                save(l);
                return status(QStringLiteral("Found %1 of %2 line(s) in the takes").arg(found).arg(readScreenplay(l.project.script).lines.size()));
            }
            if (l.project.script.text.empty()) return fail("The project has no script (use action \"set\")");
            if (action == QLatin1String("match")) {
                matchProjectScript(l.project, o);
                save(l);
                return status({});
            }
            if (action == QLatin1String("remove")) {
                l.project.script = {};
                save(l);
                return ok(QStringLiteral("Script removed"), {});
            }
            if (action == QLatin1String("pdf")) {
                const QString path = a.value("path").toString();
                if (path.isEmpty()) return fail("Pass path");
                const LinedScriptLayout layout = layoutLinedScript(l.project, l.project.active());
                QString error;
                if (!exportLinedScriptPdf(layout, absolute(path), &error)) return fail(error);
                return ok(QStringLiteral("Wrote %1 page(s)").arg(layout.pages), QJsonObject{{"path", absolute(path)}, {"pages", layout.pages}});
            }
            if (action != QLatin1String("status")) return fail(QStringLiteral("Unknown action %1").arg(action));
            return status({});
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

    add("montage_conform_check", "Conform check against the offline reference",
        "Each shot of the sequence (each stretch where one clip from media is the picture on top) compared with the "
        "offline cut's reference movie, as an editor checks a conform from an XML, AAF or EDL: matches; reframed (the "
        "reposition the list lost, solved as scale, rotation and position, keyed where it moves); slipped (the source "
        "frames off by `slip`); other_take (the reference shows another take: `media`, `source_in`); no_match; or missing "
        "(a gap here where the reference has a picture: `clip` 0). "
        "Grades are not counted and the reference's burnt-in timecode and overlays are masked. apply puts right every "
        "shot that can be (one save); markers adds a coloured marker on each shot that differs.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "reference":{"type":"string","description":"The reference movie: a media item's name or path, or a file (added to the project)"},
            "reference_start":{"type":["number","string"],"description":"The sequence time the reference's first frame belongs at (default 0)"},
            "apply":{"type":"boolean","default":false},"markers":{"type":"boolean","default":false}},
            "required":["project","reference"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Id ref = 0;
            bool added = false;
            try {
                ref = projectMedia(l.project, need(a, "reference")).id;
            } catch (const ArgError&) {
                if (!QFileInfo(absolute(need(a, "reference"))).isFile()) throw;  // (not in the project, and no such file)
                const size_t before = l.project.media.size();
                ref = mediaFor(l.project, need(a, "reference"));
                added = l.project.media.size() > before;
            }
            Sequence& s = l.seq();
            ConformOptions o;
            if (a.contains("reference_start")) o.referenceStart = timeArg(a.value("reference_start"), s, "reference_start");
            std::string err;
            const std::vector<ConformShot> shots = conformCheck(l.project, s, ref, o, {}, nullptr, &err);
            if (shots.empty() && !err.empty()) return fail(QString::fromStdString(err));
            QJsonArray list;
            std::map<ConformVerdict, int> n;
            for (const ConformShot& c : shots) {
                ++n[c.verdict];
                const Clip* clip = edit::clipById(s, c.clip);
                QJsonObject row{{"clip", double(c.clip)},
                              {"name", QString::fromStdString(clip ? clip->name : std::string())},
                              {"at", tc(c.start, s)},
                              {"at_seconds", secs(c.start, s)},
                              {"length", double(c.length)},
                              {"verdict", QString::fromLatin1(conformVerdictName(c.verdict)).toLower().replace(' ', '_')},
                              {"likeness", std::round(c.score * 1000) / 1000},
                              {"details", QString::fromStdString(c.details)}};
                if (c.verdict == ConformVerdict::Slipped) row["slip"] = double(c.slip);
                if (c.verdict == ConformVerdict::OtherTake) {
                    const MediaItem* m = l.project.findMedia(c.media);
                    row["media"] = QString::fromStdString(m ? m->name : std::string());
                    row["source_in"] = c.sourceIn / s.fpsValue();
                }
                if (!c.keys.empty()) {
                    QJsonArray keys;
                    for (const ConformKey& k : c.keys)
                        keys.append(QJsonObject{{"at", double(k.at)}, {"pos_x", k.posX}, {"pos_y", k.posY}, {"scale", k.scale}, {"rotation", k.rotation}});
                    row["transform"] = keys;
                }
                list.append(row);
            }
            int fixed = 0, marked = 0;
            if (a.value("markers").toBool()) {
                static const std::map<ConformVerdict, const char*> colours{{ConformVerdict::Reframed, "Cerulean"}, {ConformVerdict::Slipped, "Yellow"},
                                                                            {ConformVerdict::OtherTake, "Violet"}, {ConformVerdict::NoMatch, "Red"},
                                                                            {ConformVerdict::Missing, "Mango"}};
                for (const ConformShot& c : shots)
                    if (c.verdict != ConformVerdict::Matches) {
                        edit::addMarker(s, Marker{c.start, c.length, std::string("Conform: ") + conformVerdictName(c.verdict), c.details,
                                                  labelFromName(colours.at(c.verdict))});
                        ++marked;
                    }
            }
            if (a.value("apply").toBool())
                for (const ConformShot& c : shots) fixed += applyConformFix(l.project, s, c) ? 1 : 0;
            if (fixed || marked || added) save(l);
            QStringList parts;
            for (ConformVerdict v : {ConformVerdict::Matches, ConformVerdict::Reframed, ConformVerdict::Slipped, ConformVerdict::OtherTake,
                                     ConformVerdict::NoMatch, ConformVerdict::Missing})
                if (n[v]) parts << QStringLiteral("%1 %2").arg(n[v]).arg(QString::fromLatin1(conformVerdictName(v)).toLower());
            QString text = shots.empty() ? QStringLiteral("No shots from media to check") : QStringLiteral("%1 shot(s): %2").arg(shots.size()).arg(parts.join(", "));
            if (fixed) text += QStringLiteral("; fixed %1").arg(fixed);
            if (marked) text += QStringLiteral("; %1 marker(s)").arg(marked);
            return ok(text, QJsonObject{{"shots", list}, {"fixed", fixed}, {"markers", marked}});
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

    add("montage_adr", "ADR cue list and takes",
        "The lines to re-record to picture (ADR, looping): the sequence's cue list (cue number, character, the line, a "
        "note on why, start and end, status: to_record, recorded, approved or omitted) and each cue's takes. list shows "
        "it; add adds cues (numbered from the characters' initials when no name is given; a name already there updates "
        "that cue); from_captions makes a cue of each caption between from and to (a speaker label before the text is "
        "the character), from_markers one of each range marker; update changes a cue; remove takes it off; "
        "import_sheet and export_sheet read and write a cue sheet (CSV); add_take adds a recorded file as the cue's "
        "next take (its first sample at recorded_from, by default the line's start): the first becomes a clip over the "
        "line on the ADR track (or `track`), later ones join it as takes of its audition, the newest the pick; "
        "pick_take plays another take.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "action":{"type":"string","enum":["list","add","from_captions","from_markers","update","remove","import_sheet","export_sheet","add_take","pick_take"],"default":"list"},
            "cues":{"type":"array","items":{"type":"object","properties":{"name":{"type":"string"},"character":{"type":"string"},
                "line":{"type":"string"},"note":{"type":"string"},"start":{"type":["number","string"]},"end":{"type":["number","string"]},
                "status":{"type":"string"}},"required":["start","end"]}},
            "cue":{"type":"string","description":"A cue number (update, remove, add_take, pick_take)"},
            "rename":{"type":"string"},"character":{"type":"string"},"line":{"type":"string"},"note":{"type":"string"},
            "start":{"type":["number","string"]},"end":{"type":["number","string"]},
            "status":{"type":"string","enum":["to_record","recorded","approved","omitted"]},
            "from":{"type":["number","string"]},"to":{"type":["number","string"]},
            "path":{"type":"string","description":"The cue sheet (import_sheet, export_sheet)"},
            "media":{"type":"string","description":"The recorded take's file (add_take)"},
            "recorded_from":{"type":["number","string"],"description":"Where the take's first sample belongs (add_take)"},
            "track":{"type":"integer","minimum":1,"description":"Audio track for a cue's first take (default: the ADR track)"},
            "take":{"type":"integer","minimum":1,"description":"pick_take: which take"}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const QString action = a.value("action").toString("list");
            auto statusArg = [](const QString& v) {
                const QString n = v.trimmed().toLower().replace('_', ' ');
                if (n != "to record" && n != "recorded" && n != "approved" && n != "omitted")
                    throw ArgError{"\"status\" must be to_record, recorded, approved or omitted"};
                return adrStatusFromName(n.toStdString());
            };
            auto cueNamed = [&](const QString& name) -> AdrCue& {
                for (AdrCue& q : s.adrCues)
                    if (QString::fromStdString(q.name) == name) return q;
                throw ArgError{QStringLiteral("No ADR cue \"%1\"").arg(name)};
            };
            auto listing = [&] {
                QJsonArray cues;
                for (const AdrCue& q : s.adrCues) {
                    QJsonObject o{{"name", QString::fromStdString(q.name)}, {"start", tc(q.start, s)}, {"end", tc(q.end, s)},
                                  {"status", QString::fromStdString(adrStatusName(q.status)).toLower().replace(' ', '_')},
                                  {"takes", adrTakeCount(s, q)}};
                    if (!q.character.empty()) o["character"] = QString::fromStdString(q.character);
                    if (!q.line.empty()) o["line"] = QString::fromStdString(q.line);
                    if (!q.note.empty()) o["note"] = QString::fromStdString(q.note);
                    if (const Clip* c = q.clip ? edit::clipById(s, q.clip) : nullptr) {
                        o["clip"] = double(c->id);
                        if (!c->takes.empty()) o["take"] = c->take + 1;
                    }
                    cues.append(o);
                }
                return QJsonObject{{"cues", cues}};
            };
            auto added = [&](std::vector<AdrCue> cues, const QString& what) {
                const std::vector<Id> ids = addAdrCues(l.project, s, std::move(cues));
                save(l);
                return ok(QStringLiteral("%1 %2 ADR cue(s)").arg(what).arg(ids.size()), listing());
            };
            const FrameTime from = a.contains("from") ? timeArg(a.value("from"), s, "from") : 0;
            const FrameTime to = a.contains("to") ? timeArg(a.value("to"), s, "to") : -1;
            if (action == "list") return ok(QStringLiteral("%1 ADR cue(s)").arg(s.adrCues.size()), listing());
            if (action == "add") {
                std::vector<AdrCue> cues;
                for (const QJsonValue& v : a.value("cues").toArray()) {
                    const QJsonObject o = v.toObject();
                    AdrCue q;
                    q.name = o.value("name").toString().toStdString();
                    q.character = o.value("character").toString().toStdString();
                    q.line = o.value("line").toString().toStdString();
                    q.note = o.value("note").toString().toStdString();
                    q.start = timeArg(o.value("start"), s, "start");
                    q.end = timeArg(o.value("end"), s, "end");
                    if (q.end <= q.start) throw ArgError{"A cue's end must come after its start"};
                    if (o.contains("status")) q.status = statusArg(o.value("status").toString());
                    cues.push_back(std::move(q));
                }
                if (cues.empty()) throw ArgError{"\"cues\" lists the cues to add"};
                return added(std::move(cues), "Added or updated");
            }
            if (action == "from_captions") {
                std::vector<AdrCue> cues = adrCuesFromCaptions(s, -1, from, to);
                if (cues.empty()) return fail("No captions there");
                return added(std::move(cues), "Made");
            }
            if (action == "from_markers") {
                std::vector<AdrCue> cues = adrCuesFromMarkers(s, from, to);
                if (cues.empty()) return fail("No range markers (markers with a duration) there");
                return added(std::move(cues), "Made");
            }
            if (action == "update") {
                AdrCue& q = cueNamed(need(a, "cue"));
                if (a.contains("rename")) {
                    const std::string n = str(a, "rename").trimmed().toStdString();
                    for (const AdrCue& o : s.adrCues)
                        if (&o != &q && o.name == n) throw ArgError{"Another cue has that number"};
                    if (n.empty()) throw ArgError{"A cue needs a number"};
                    q.name = n;
                }
                if (a.contains("character")) q.character = str(a, "character").toStdString();
                if (a.contains("line")) q.line = str(a, "line").toStdString();
                if (a.contains("note")) q.note = str(a, "note").toStdString();
                if (a.contains("status")) q.status = statusArg(str(a, "status"));
                const FrameTime start = a.contains("start") ? timeArg(a.value("start"), s, "start") : q.start;
                const FrameTime end = a.contains("end") ? timeArg(a.value("end"), s, "end") : q.end;
                if (end <= start) throw ArgError{"A cue's end must come after its start"};
                q.start = start, q.end = end;
                std::stable_sort(s.adrCues.begin(), s.adrCues.end(), [](const AdrCue& x, const AdrCue& y) { return x.start < y.start; });
                save(l);
                return ok("Updated the cue", listing());
            }
            if (action == "remove") {
                removeAdrCue(s, cueNamed(need(a, "cue")).id);
                save(l);
                return ok("Removed the cue", listing());
            }
            if (action == "import_sheet") {
                QFile f(absolute(need(a, "path")));
                if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("Cannot read %1").arg(f.fileName()));
                std::vector<AdrCue> cues;
                std::string err;
                if (!parseAdrCueSheet(f.readAll().toStdString(), s, cues, &err)) return fail(QString::fromStdString(err));
                return added(std::move(cues), "Read");
            }
            if (action == "export_sheet") {
                QFile f(absolute(need(a, "path")));
                const std::string csv = adrCueSheetCsv(s);
                if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(csv.data(), qint64(csv.size())) != qint64(csv.size()))
                    return fail(QStringLiteral("Cannot write %1").arg(f.fileName()));
                QJsonObject r = listing();
                r["path"] = f.fileName();
                return ok(QStringLiteral("Wrote %1 cue(s)").arg(s.adrCues.size()), r);
            }
            if (action == "add_take") {
                const AdrCue& q = cueNamed(need(a, "cue"));
                const Id cue = q.id;
                const FrameTime at = a.contains("recorded_from") ? timeArg(a.value("recorded_from"), s, "recorded_from") : q.start;
                const Id media = mediaFor(l.project, need(a, "media"));
                const int track = a.contains("track") ? a.value("track").toInt() - 1 : -1;
                if (a.contains("track") && (track < 0 || track > int(s.audioTracks.size())))
                    throw ArgError{QStringLiteral("\"track\" must be 1 to %1").arg(s.audioTracks.size() + 1)};
                const edit::Result r = edit::addAdrTake(l.project, s, cue, media, at, track);
                if (!r.ok) return fail(QString::fromStdString(r.error));
                save(l);
                return ok(QStringLiteral("Take %1 of %2").arg(adrTakeCount(s, *findAdrCue(s, cue))).arg(QString::fromStdString(findAdrCue(s, cue)->name)),
                          listing());
            }
            if (action == "pick_take") {
                const AdrCue& q = cueNamed(need(a, "cue"));
                if (!q.clip || !edit::clipById(s, q.clip)) return fail("That cue has no takes");
                const edit::Result r = edit::pickTake(l.project, s, q.clip, a.value("take").toInt(0) - 1);
                if (!r.ok) return fail(QString::fromStdString(r.error));
                save(l);
                return ok("Picked the take", listing());
            }
            throw ArgError{"Unknown action"};
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

    add("montage_match_dialogue", "Match dialogue to a reference",
        "Make ADR or a pickup sound like the production take it sits with (the reference clip): the reference's room is "
        "measured from its speech (decay time per octave) and added as a Convolution Reverb at the level that leaves as "
        "much late reverberation as the reference has; a Parametric EQ is fitted to the result (with tone_over_time, "
        "refitted every half second and keyed); the clip volume brings the speech to the reference's loudness. Matching "
        "again replaces the earlier match (and a Match Voice EQ).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"reference":{"type":"number","description":"The production take"},
            "clips":{"type":"array","items":{"type":"number"}},"room":{"type":"boolean","default":true},"tone":{"type":"boolean","default":true},
            "tone_over_time":{"type":"boolean","default":false},"level":{"type":"boolean","default":true}},
            "required":["project","reference","clips"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Clip& ref = clipArg(l, a, "reference");
            DialogueMatchOptions o;
            o.room = a.value("room").toBool(true);
            o.tone = a.value("tone").toBool(true);
            o.toneOverTime = a.value("tone_over_time").toBool(false);
            o.level = a.value("level").toBool(true);
            std::vector<Id> ids;
            for (const QJsonValue& v : a.value("clips").toArray()) {
                const Clip* c = edit::clipById(s, Id(v.toDouble()));
                auto loc = c ? edit::locate(s, c->id) : std::nullopt;
                if (!c || !loc || loc->track.kind != TrackKind::Audio || c->id == ref.id)
                    throw ArgError{QStringLiteral("%1 is not an audio clip other than the reference").arg(qulonglong(v.toDouble()))};
                if (std::find(ids.begin(), ids.end(), c->id) == ids.end()) ids.push_back(c->id);
            }
            std::vector<std::pair<Id, DialogueMatchPlan>> plans;
            std::string err;
            const QString refName = QString::fromStdString(ref.name);
            const int n = matchDialogue(l.project, s, ref.id, ids, o, &plans, &err);
            if (n == 0) return fail(err.empty() ? QStringLiteral("No clips to match") : QString::fromStdString(err));
            save(l);
            QJsonArray out;
            for (const auto& [id, plan] : plans) {
                QJsonArray rt;
                for (double t : plan.room.rt60) rt.append(t);
                out.append(QJsonObject{{"clip", double(id)}, {"room_added", plan.roomAdded}, {"rt60", rt},
                                       {"room_level_db", plan.wetDb}, {"late_reverb_db", plan.matchedLateDb},
                                       {"reference_late_reverb_db", plan.referenceLateDb}, {"eq_keys", int(plan.eqOverTime.size())},
                                       {"low_db", plan.eq.lowDb}, {"b300_db", plan.eq.b1Db}, {"b1200_db", plan.eq.b2Db},
                                       {"b4000_db", plan.eq.b3Db}, {"high_db", plan.eq.highDb}, {"gain_db", plan.gainDb},
                                       {"loudness_lufs", plan.matchedLufs}, {"reference_loudness_lufs", plan.referenceLufs}});
            }
            // (a clip whose sound could not be read is left as it was)
            QJsonArray skipped;
            for (Id id : ids)
                if (std::none_of(plans.begin(), plans.end(), [&](const auto& pl) { return pl.first == id; })) skipped.append(double(id));
            return ok(QStringLiteral("Matched %1 clip(s) to %2").arg(n).arg(refName), QJsonObject{{"clips", out}, {"not_matched", skipped}});
        });

    add("montage_align_dialogue", "Align dialogue to a guide",
        "Fit an ADR take, a pickup or a converted voice (clip, an audio clip; on an audition, the take it plays) to the "
        "timing of the guide clip's speech (the production sound), its pitch kept: both are compared as speech, the take "
        "stretched and squeezed (pauses most) to land on the guide's syllables, as a Dialogue Alignment effect on the clip "
        "(aligning again replaces it; remove takes it off). mode shift lines up another microphone of the same take with "
        "the boom by moving it alone, to a fraction of a sample, flipping its polarity when that was reversed. from and to "
        "(seconds or timecode) narrow the guide's range (default: where the two clips overlap); precision thins the "
        "anchors (loose, normal, tight). The clip stays where it is, lengthened to the end of the range if it stops "
        "short and its track has room.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number","description":"The take"},
            "guide":{"type":"number","description":"The guide (production sound)"},"mode":{"type":"string","enum":["warp","shift"],"default":"warp"},
            "precision":{"type":"string","enum":["loose","normal","tight"],"default":"normal"},
            "from":{"type":["number","string"]},"to":{"type":["number","string"]},"handles":{"type":"number","default":0.5},
            "remove":{"type":"boolean","default":false}},"required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Clip& take = clipArg(l, a, "clip");
            const Id takeId = take.id;
            if (a.value("remove").toBool(false)) {
                if (const edit::Result r = edit::removeDialogueAlignment(l.project, s, takeId); !r.ok) return fail(QString::fromStdString(r.error));
                save(l);
                return ok(QStringLiteral("Took the alignment off %1").arg(QString::fromStdString(take.name)), QJsonObject{{"clip", double(takeId)}});
            }
            if (!a.contains("guide")) throw ArgError{"\"guide\" is needed (the clip to align to)"};
            const Clip& guide = clipArg(l, a, "guide");
            DialogueAlignOptions o;
            const QString mode = a.value("mode").toString("warp");
            if (mode != "warp" && mode != "shift") throw ArgError{"\"mode\" must be warp or shift"};
            o.shift = mode == "shift";
            if (a.contains("precision") && !parseAlignPrecision(a.value("precision").toString().toStdString(), o.precision))
                throw ArgError{"\"precision\" must be loose, normal or tight"};
            if (a.contains("from")) o.from = timeArg(a.value("from"), s, "from");
            if (a.contains("to")) o.to = timeArg(a.value("to"), s, "to");
            o.handles = std::clamp(a.value("handles").toDouble(0.5), 0.0, 10.0);
            DialogueAlignReport rep;
            const edit::Result r = edit::alignDialogue(l.project, s, takeId, guide.id, o, &rep);
            if (!r.ok) return fail(QString::fromStdString(r.error));
            save(l);
            QJsonObject out{{"clip", double(takeId)}, {"mode", mode}, {"from", rep.from}, {"to", rep.to}, {"confidence", rep.confidence}};
            if (o.shift) {
                out["delay_ms"] = rep.delay * 1000;
                out["inverted"] = rep.inverted;
            } else {
                out["anchors"] = rep.anchors;
                out["min_rate"] = rep.minRate;
                out["max_rate"] = rep.maxRate;
            }
            const QString name = QString::fromStdString(edit::clipById(s, takeId)->name);
            return ok(o.shift ? QStringLiteral("Shifted %1 by %2 ms%3").arg(name).arg(rep.delay * 1000, 0, 'f', 2).arg(rep.inverted ? " (polarity flipped)" : "")
                              : QStringLiteral("Aligned %1 to the guide (%2 anchors)").arg(name).arg(rep.anchors),
                      out);
        });

    add("montage_set_surround", "Set up a surround mix",
        "Mix the active sequence in stereo, 5.1 (L R C LFE Ls Rs), 7.1 (L R C LFE Lb Rb Ls Rs), an immersive layout with "
        "overhead speakers (5.1.2, 5.1.4, 7.1.2 with its overhead pair at the sides, 7.1.4) or an ambisonic field (ambix, ambix2, ambix3: "
        "first to third order), and place audio tracks among the speakers or in the field. A "
        "position is an angle (0 straight ahead, 90 right, -90 left, 180 behind) and a distance (1 at the speakers, 0 spread "
        "over all of them); height (0 at the ear to 1 overhead) lifts it in immersive layouts; width narrows a stereo track "
        "to a point (0, e.g. dialogue in the centre speaker); lfe_db sends it to the subwoofer (-100 off); object makes the "
        "track an audio object of its own in ADM masters (montage_export_adm) instead of part of the bed. path moves it: "
        "points of time, angle, distance and height (the rest from the track's own), keyed on its position lanes and "
        "played as its automation reads (Read, the default, Latch or Touch); an object's movement goes into the ADM "
        "master; an empty path stops it moving. A track routed to a bus is placed by its bus. Export with montage_render "
        "(downmix_stereo for a stereo copy).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "layout":{"type":"string","enum":["stereo","5.1","7.1","5.1.2","5.1.4","7.1.2","7.1.4","ambix","ambix2","ambix3"]},
            "tracks":{"type":"array","items":{"type":"object","properties":{
                "track":{"type":"string","description":"Audio track, e.g. A1"},"angle":{"type":"number","default":0},
                "distance":{"type":"number","default":1},"height":{"type":"number","default":0},"width":{"type":"number","default":1},
                "lfe_db":{"type":"number","default":-100},"object":{"type":"boolean","default":false},
                "path":{"type":"array","items":{"type":"object","properties":{"at":{"type":["number","string"]},
                    "angle":{"type":"number"},"distance":{"type":"number"},"height":{"type":"number"}},"required":["at"]}}},
                "required":["track"]}}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            if (a.contains("layout")) {
                const std::string layout = a.value("layout").toString().toStdString();
                if (std::find(audioLayouts().begin(), audioLayouts().end(), layout) == audioLayouts().end())
                    throw ArgError{"\"layout\" must be stereo, 5.1, 7.1, 5.1.2, 5.1.4, 7.1.2, 7.1.4, ambix, ambix2 or ambix3"};
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
                p.z = std::clamp(t.value("height").toDouble(0), 0.0, 1.0);
                p.object = t.value("object").toBool(false);
                if (t.contains("path")) {
                    // Keys on the position lanes; what a point leaves out is the track's own.
                    tr->surroundXAuto = Param(), tr->surroundYAuto = Param(), tr->surroundZAuto = Param();
                    for (const QJsonValue& kv : t.value("path").toArray()) {
                        const QJsonObject k = kv.toObject();
                        const FrameTime at = timeArg(k.value("at"), s, "at");
                        const double ka = k.contains("angle") ? k.value("angle").toDouble() * M_PI / 180 : angle;
                        const double kd = std::clamp(k.contains("distance") ? k.value("distance").toDouble() : dist, 0.0, 1.0);
                        tr->surroundXAuto.addKey(at, kd * std::sin(ka));
                        tr->surroundYAuto.addKey(at, kd * std::cos(ka));
                        tr->surroundZAuto.addKey(at, std::clamp(k.contains("height") ? k.value("height").toDouble() : p.z, 0.0, 1.0));
                    }
                }
                QJsonObject placed{{"track", QString::fromStdString(tr->name)}, {"x", p.x}, {"y", p.y}, {"z", p.z}, {"width", p.width},
                                   {"lfe_db", p.lfeDb}, {"object", p.object}};
                if (surroundAnimated(*tr)) placed["path_points"] = int(tr->surroundXAuto.keys.size());
                out.append(placed);
            }
            save(l);
            return ok(QStringLiteral("%1 mix, %2 track(s) placed").arg(QString::fromStdString(s.audioLayout)).arg(out.size()),
                      QJsonObject{{"layout", QString::fromStdString(s.audioLayout)},
                                  {"channels", layoutChannels(s.audioLayout)}, {"tracks", out}});
        });

    add("montage_export_adm", "Export an immersive master (ADM BWF)",
        "Write the active sequence as an ADM BWF file (ITU-R BS.2076 metadata in a BS.2088 BW64 wave), the interchange "
        "format of immersive and object-based audio masters and of the EBU ADM renderer: a bed of every track that is not an audio object, in the "
        "sequence's layout (named by the BS.2094 common definitions), and one channel for each track marked as an object "
        "(montage_set_surround object), with its position. 48 kHz, 24 bits. Returns what the file holds.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"path":{"type":"string","description":"The .wav to write"},
            "title":{"type":"string"},"in_out":{"type":"boolean","default":false,"description":"Only In to Out"}},
            "required":["project","path"]})json",
        true, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Sequence& s = l.seq();
            AdmSettings st;
            st.title = str(a, "title", QString::fromStdString(s.name)).toStdString();
            st.inOut = a.value("in_out").toBool();
            const QString path = absolute(need(a, "path"));
            AdmResult r;
            std::string err;
            if (!exportAdmBwf(l.project, s, st, path.toStdString(), &r, {}, &err)) return fail(QString::fromStdString(err));
            BwfInfo info;
            if (!readBwfInfo(r.path, info, &err)) return fail(QString::fromStdString(err));
            QJsonArray objects;
            for (int t : admObjectTracks(s)) objects.append(QString::fromStdString(s.audioTracks[size_t(t)].name));
            return ok(QStringLiteral("Wrote a %1 bed and %2 object(s)").arg(QString::fromStdString(s.audioLayout)).arg(r.objects),
                      QJsonObject{{"path", path}, {"layout", QString::fromStdString(s.audioLayout)}, {"bed_channels", r.bedChannels},
                                  {"bed_pack", QString::fromStdString(r.bedPack)}, {"objects", objects}, {"channels", info.channels},
                                  {"seconds", double(r.samples) / 48000.0}, {"chna_tracks", int(info.chna.size())}});
        });

    add("montage_export_iamf", "Export immersive audio as IAMF",
        "Write the active sequence's sound as IAMF (the Alliance for Open Media's Immersive Audio Model and Formats), "
        "which YouTube and players take immersive and ambisonic sound in: an .mp4 (or .m4a) whose track is IAMF, or the "
        "bare stream (.iamf). A surround or immersive layout (5.1 to 7.1.4, montage_set_surround) is a bed in that layout "
        "with any audio objects rendered into it; an ambisonic one (first to third order) a scene, a substream per "
        "component. The mix presentation carries the loudness measured in the layout and in stereo (BS.1770 integrated, "
        "sample and true peak) and plays binaurally on headphones unless `binaural` is false. FLAC (lossless) or 24-bit "
        "PCM, 48 kHz. Returns what the file holds, read back.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"path":{"type":"string","description":"The .mp4, .m4a or .iamf to write"},
            "codec":{"type":"string","enum":["flac","lpcm"],"default":"flac"},
            "binaural":{"type":"boolean","default":true,"description":"On headphones: rendered binaurally (false: as stereo)"},
            "title":{"type":"string"},"in_out":{"type":"boolean","default":false,"description":"Only In to Out"}},
            "required":["project","path"]})json",
        true, [this](const QJsonObject& a) {
            Loaded l = open(a);
            const Sequence& s = l.seq();
            IamfSettings st;
            st.codec = str(a, "codec", "flac").toStdString();
            if (st.codec != "flac" && st.codec != "lpcm") throw ArgError{"\"codec\" must be \"flac\" or \"lpcm\""};
            st.binaural = a.value("binaural").toBool(true);
            st.title = str(a, "title", QString::fromStdString(s.name)).toStdString();
            st.inOut = a.value("in_out").toBool();
            const QString path = absolute(need(a, "path"));
            IamfResult r;
            std::string err;
            if (!exportIamf(l.project, s, st, path.toStdString(), &r, [this](double f) { progress(f, "IAMF"); return true; }, &err))
                return fail(QString::fromStdString(err));
            IamfInfo info;
            if (!readIamfInfo(r.path, info, &err)) return fail(QString::fromStdString(err));
            QJsonArray loudness;
            for (const IamfLoudness& m : info.loudness)
                loudness.append(QJsonObject{{"layout", QString::fromStdString(m.layout)}, {"integrated_lufs", m.integrated},
                                            {"sample_peak_dbfs", m.digitalPeak}, {"true_peak_dbtp", m.truePeak}});
            return ok(QStringLiteral("Wrote %1 IAMF (%2, %3 substreams)").arg(QString::fromStdString(r.layout), QString::fromStdString(r.codec)).arg(r.substreams),
                      QJsonObject{{"path", path}, {"layout", QString::fromStdString(r.layout)}, {"codec", QString::fromStdString(r.codec)},
                                  {"scene_based", info.sceneBased}, {"channels", r.channels}, {"substreams", info.substreams},
                                  {"coupled", info.coupled}, {"seconds", double(r.samples) / 48000.0}, {"frames", info.frames},
                                  {"binaural", info.headphonesMode == 1}, {"loudness", loudness}});
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
        "(Scenarist, CEA-608), .mcc (MCC captions: CEA-608 and CTA-708, with the track's places and colours), .ttml / .xml / .dfxp (TTML, IMSC 1.1 Text profile, with the track's colours), .stl "
        "(EBU Tech 3264, 25 or 30 fps) or .ass / .ssa (SubStation Alpha, with the track's style); or read any of them "
        "as a new caption track (`import`), or the closed captions inside a video clip's file (`import_embedded`: CTA-708 service 1, else CEA-608 channel 1). "
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
                throw ArgError{QStringLiteral("Unknown caption format \"%1\": use .srt, .vtt, .scc, .mcc, .ttml, .stl or .ass")
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

    add("montage_find_sounds", "Find sounds by description",
        "Search the project's recordings by what is heard (\"applause\", \"a door slamming\", \"rain on a window\", \"a crowd "
        "cheering\", \"a car passing\"), with CLAP running on this computer. Recordings not indexed yet are indexed first "
        "(10-second windows every 5 s, once; the index is saved in the project). Returns the best moments: media file and media "
        "times, best first.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"query":{"type":"string"},
            "max":{"type":"integer","default":10}},"required":["project","query"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            if (!soundSearchAvailable()) return fail("This build of Montage cannot search sound (no ONNX Runtime)");
            if (!soundModel().installed())
                return fail("The sound search model is not downloaded: run `scripts/fetch-models.sh` or search What's Heard in the app once");
            const std::string query = need(a, "query").toStdString();
            std::string err;
            bool changed = false;
            QJsonArray skipped;  // offline or unreadable: reported, and the rest still searched
            for (MediaItem& m : l.project.media) {
                if (!m.hasAudio || m.path.empty() || m.subclipOf || (m.sounds && !m.sounds->samples.empty())) continue;
                VisualIndex v;
                if (!indexSound(m.path, m.duration, v, 0,
                                [&](double f) { progress(f, QStringLiteral("Listening to %1").arg(QString::fromStdString(m.name))); }, nullptr, &err)) {
                    skipped.append(QJsonObject{{"media", QString::fromStdString(m.path)}, {"error", QString::fromStdString(err)}});
                    continue;
                }
                m.sounds = std::make_shared<const VisualIndex>(std::move(v));
                changed = true;
            }
            if (changed) save(l);
            auto model = SoundModel::load(&err);
            const std::vector<float> q = model ? model->text(query, &err) : std::vector<float>{};
            if (q.empty()) return fail(QString::fromStdString(err));
            const std::vector<ShotMatch> hits = findSounds(l.project, q, size_t(std::clamp(a.value("max").toInt(10), 1, 100)));
            QJsonArray list;
            QString text;
            for (const ShotMatch& h : hits) {
                const MediaItem* m = l.project.findMedia(h.media);
                if (!m) continue;
                list.append(QJsonObject{{"media", QString::fromStdString(m->path)}, {"start_seconds", h.start}, {"end_seconds", h.end},
                                        {"best_seconds", h.best}, {"score", double(h.score)}});
                text += QStringLiteral("%1  %2-%3 s (score %4)\n")
                            .arg(QString::fromStdString(m->name))
                            .arg(h.start, 0, 'f', 1)
                            .arg(h.end, 0, 'f', 1)
                            .arg(double(h.score), 0, 'f', 3);
            }
            for (const QJsonValue& v : skipped)
                text += QStringLiteral("(not searched: %1: %2)\n").arg(v.toObject().value("media").toString(), v.toObject().value("error").toString());
            return ok(text.isEmpty() ? QStringLiteral("No recording with sound") : text, QJsonObject{{"moments", list}, {"skipped", skipped}});
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

    add("montage_graphics", "Graphics templates",
        "Editable graphics (Premiere's motion graphics templates, Final Cut's published parameters): a sequence of titles, "
        "shapes and images publishes some of its clips' settings as fields (publish: clip, param such as text, color, size, "
        "bold, align, and effect for one of the clip's effects, or picture: true to swap the clip's image), and every clip "
        "that plays that sequence sets them for itself (set: clip, values by field id; reset: ids). fields lists a "
        "sequence's fields or a graphic clip's values; unpublish removes one. save_template saves the sequence (with the "
        "files it plays) to the template library as name; templates lists the library; insert places a template (by name "
        "or path) at a time on a new top track; fill fills a graphic and copies of it after it from CSV text (a header row "
        "naming fields by id or label, one graphic a row). sequence (a name) picks the sequence for fields, publish, "
        "unpublish and save_template; else the active one.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "action":{"type":"string","enum":["fields","publish","unpublish","set","reset","save_template","templates","insert","fill"]},
            "sequence":{"type":"string"},"clip":{"type":"integer"},"effect":{"type":"integer"},"param":{"type":"string"},
            "picture":{"type":"boolean"},"label":{"type":"string"},"field":{"type":"string"},
            "values":{"type":"object"},"ids":{"type":"array","items":{"type":"string"}},
            "name":{"type":"string"},"template":{"type":"string"},"at":{"type":["number","string"]},"csv":{"type":"string"},
            "folder":{"type":"string","description":"The template library (default Montage's own)"}},"required":["project","action"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            const QString action = a.value("action").toString();
            const std::string folder = a.contains("folder") ? absolute(a.value("folder").toString()).toStdString() : graphicsTemplatesDirectory();
            if (action == "templates") {
                QJsonArray list;
                for (const GraphicsTemplateInfo& t : listGraphicsTemplates(folder)) {
                    QJsonArray fields;
                    for (const std::string& f : t.fields) fields.append(QString::fromStdString(f));
                    list.append(QJsonObject{{"name", QString::fromStdString(t.name)}, {"path", QString::fromStdString(t.path)},
                                            {"fields", fields}, {"seconds", t.seconds}, {"width", t.width}, {"height", t.height}});
                }
                return ok(QStringLiteral("%1 template(s)").arg(list.size()), QJsonObject{{"templates", list}});
            }
            Sequence* sq = l.project.active();
            if (a.contains("sequence")) {
                sq = nullptr;
                for (Sequence& q : l.project.sequences)
                    if (q.name == a.value("sequence").toString().toStdString()) sq = &q;
                if (!sq) throw ArgError{QStringLiteral("No sequence \"%1\"").arg(a.value("sequence").toString())};
            }
            auto fieldsJson = [&](const Sequence& t, const Clip* graphic) {
                QJsonArray out;
                for (const TemplateField& f : liveFields(t)) {
                    QJsonObject o{{"id", QString::fromStdString(f.id)}, {"label", QString::fromStdString(f.label)},
                                  {"kind", QString::fromStdString(f.kind)}, {"clip", double(f.clip)},
                                  {"default", QString::fromStdString(fieldValue(t, f))}};
                    if (graphic) o["value"] = QString::fromStdString(fieldValue(t, *graphic, f));
                    double lo, hi;
                    std::vector<std::string> choices;
                    if (fieldRange(t, f, lo, hi, &choices)) {
                        if (f.kind == "number") o["min"] = lo, o["max"] = hi;
                        if (f.kind == "choice") {
                            QJsonArray c;
                            for (const std::string& x : choices) c.append(QString::fromStdString(x));
                            o["choices"] = c;
                        }
                    }
                    out.append(o);
                }
                return out;
            };
            if (action == "fields") {
                if (a.contains("clip")) {
                    const Clip& c = clipArg(l, a);
                    const Sequence* t = graphicOf(l.project, c);
                    if (!t) throw ArgError{"That clip is not a graphic with fields"};
                    return ok(QStringLiteral("%1 field(s)").arg(liveFields(*t).size()),
                              QJsonObject{{"fields", fieldsJson(*t, &c)}, {"template", QString::fromStdString(t->name)}});
                }
                return ok(QStringLiteral("%1 field(s)").arg(liveFields(*sq).size()), QJsonObject{{"fields", fieldsJson(*sq, nullptr)}});
            }
            if (action == "publish") {
                if (!a.value("clip").isDouble()) throw ArgError{"\"clip\" is the clip (in the sequence) whose setting to publish"};
                const Id clip = Id(a.value("clip").toDouble());
                const std::string label = a.value("label").toString().toStdString();
                const auto f = a.value("picture").toBool() ? mediaFieldFor(l.project, *sq, clip, label)
                                                           : fieldFor(*sq, clip, Id(a.value("effect").toDouble()), need(a, "param").toStdString(), label);
                if (!f) throw ArgError{"No such clip or setting to publish (a picture needs a clip of an image or video)"};
                std::string id;
                check(publishField(*sq, *f, &id));
                save(l);
                return ok(QStringLiteral("Published \"%1\" as %2").arg(QString::fromStdString(f->label), QString::fromStdString(id)),
                          QJsonObject{{"id", QString::fromStdString(id)}, {"fields", fieldsJson(*sq, nullptr)}});
            }
            if (action == "unpublish") {
                check(unpublishField(*sq, need(a, "field").toStdString()));
                save(l);
                return ok(QStringLiteral("Unpublished"), QJsonObject{{"fields", fieldsJson(*sq, nullptr)}});
            }
            if (action == "set" || action == "reset") {
                Clip& c = clipArg(l, a);
                const Sequence* t = graphicOf(l.project, c);
                if (!t) throw ArgError{"That clip is not a graphic with fields"};
                const std::vector<TemplateField> fields = liveFields(*t);
                auto find = [&](const std::string& key) -> const TemplateField* {
                    for (const TemplateField& f : fields)
                        if (f.id == key) return &f;
                    for (const TemplateField& f : fields)
                        if (f.label == key) return &f;
                    return nullptr;
                };
                if (action == "set") {
                    const QJsonObject values = a.value("values").toObject();
                    if (values.isEmpty()) throw ArgError{"\"values\" maps field ids to values"};
                    std::map<std::string, std::string> next = c.fields;
                    for (auto it = values.begin(); it != values.end(); ++it) {
                        const TemplateField* f = find(it.key().toStdString());
                        if (!f) throw ArgError{QStringLiteral("No field \"%1\"").arg(it.key())};
                        const QString text = it.value().isDouble() ? QString::number(it.value().toDouble(), 'g', 10)
                                             : it.value().isBool() ? QString(it.value().toBool() ? "1" : "0")
                                                                   : it.value().toString();
                        std::string out, err;
                        if (!normalizeFieldValue(l.project, *t, *f, text.toStdString(), out, &err)) throw ArgError{QString::fromStdString(err)};
                        next[f->id] = out;
                    }
                    c.fields = next;
                } else {
                    const QJsonArray ids = a.value("ids").toArray();
                    if (ids.isEmpty()) c.fields.clear();
                    for (const QJsonValue& v : ids)
                        if (const TemplateField* f = find(v.toString().toStdString())) c.fields.erase(f->id);
                }
                save(l);
                return ok(action == "set" ? QStringLiteral("Set") : QStringLiteral("Reset"), QJsonObject{{"fields", fieldsJson(*t, &c)}});
            }
            if (action == "save_template") {
                const QString name = a.contains("name") ? a.value("name").toString() : QString::fromStdString(sq->name);
                std::string path, err;
                if (!saveGraphicsTemplate(l.project, sq->id, name.toStdString(), folder, &path, &err)) return fail(QString::fromStdString(err));
                return ok(QStringLiteral("Saved %1 with %2 field(s)").arg(name).arg(liveFields(*sq).size()),
                          QJsonObject{{"path", QString::fromStdString(path)}});
            }
            if (action == "insert") {
                QString path = need(a, "template");
                if (!QFileInfo(absolute(path)).isFile())
                    for (const GraphicsTemplateInfo& t : listGraphicsTemplates(folder))
                        if (QString::fromStdString(t.name).compare(path, Qt::CaseInsensitive) == 0) path = QString::fromStdString(t.path);
                if (!QFileInfo(absolute(path)).isFile()) throw ArgError{QStringLiteral("No template \"%1\" (see action templates)").arg(path)};
                Sequence& s = l.seq();
                const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : 0;
                const edit::Result r = insertGraphicsTemplate(l.project, s, absolute(path).toStdString(), at, -1);
                check(r);
                save(l);
                const Clip* c = edit::clipById(l.seq(), r.created.at(0));
                const Sequence* t = c ? graphicOf(l.project, *c) : nullptr;
                return ok(QStringLiteral("Inserted at %1").arg(tc(at, l.seq())),
                          QJsonObject{{"clip", double(r.created.at(0))}, {"fields", t ? fieldsJson(*t, c) : QJsonArray{}}});
            }
            if (action == "fill") {
                const Id clip = clipArg(l, a).id;
                std::vector<Id> made;
                std::vector<std::string> unmatched;
                check(fillGraphicsFromTable(l.project, l.seq(), clip, need(a, "csv").toStdString(), &made, &unmatched));
                save(l);
                QJsonArray clips, cols;
                for (const Id id : made) clips.append(double(id));
                for (const std::string& u : unmatched) cols.append(QString::fromStdString(u));
                return ok(QStringLiteral("Filled %1 graphic(s)").arg(made.size()), QJsonObject{{"clips", clips}, {"unmatched", cols}});
            }
            throw ArgError{QStringLiteral("Unknown action \"%1\"").arg(action)};
        });

    add("montage_separate_stems", "Separate stems",
        "Split the sound of audio clips into stems, on this computer. Music with Spleeter (Deezer Research): 4 (vocals, "
        "drums, bass, other), 5 (vocals, piano, drums, bass, other) or 2 (vocals, accompaniment), as 44.1 kHz WAVs. A "
        "film's finished mix with Bandit v2 (Watcharasupat, Wu and Lerch; CC BY-SA 4.0): 3 (dialogue, music, effects), as "
        "48 kHz WAVs whose clips get the Dialogue, Music and Effects roles, so montage_export audio_streams dme puts the "
        "dialogue and an M&E in a master; dialogue is the cleanest, music and effects less exact on real film mixes. Each stem is in "
        "the Stems folder beside the project, separated from the source the clip plays plus handles seconds either side, "
        "and played by a copy of the clip (timing, speed, volume, pan) on an audio track named for the stem (Vocals, "
        "Dialogue...; reused where free, else added), linked with the clip. The clip itself is disabled unless "
        "mute_original is false. Needs the model (montage-cli models lists it; the app downloads it on first use).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clips":{"type":"array","items":{"type":"number"}},
            "stems":{"type":"integer","enum":[2,3,4,5],"default":4},"mute_original":{"type":"boolean","default":true},
            "handles":{"type":"number","default":2}},"required":["project","clips"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const int stems = a.value("stems").toInt(4);
            const std::vector<std::string>& names = stemNames(stems);
            if (names.empty()) throw ArgError{"\"stems\" is 2, 3, 4 or 5"};
            if (!stemSeparationAvailable()) return fail("This build of Montage has no ONNX Runtime");
            if (!stemModel(stems).installed()) return fail(QString::fromStdString("The " + stemModel(stems).title + " is not downloaded"));
            std::vector<Id> ids;
            for (const QJsonValue& v : a.value("clips").toArray()) ids.push_back(Id(v.toDouble()));
            if (ids.empty()) throw ArgError{"\"clips\" must list clip ids"};
            const double handles = std::clamp(a.value("handles").toDouble(2), 0.0, 60.0);
            const QString folder = QFileInfo(absolute(need(a, "project"))).absolutePath() + QStringLiteral("/Stems");
            QDir().mkpath(folder);
            std::vector<std::string> titles;
            for (const std::string& n : names) titles.push_back(stemTitle(n));
            // Every clip checked before any is separated; files written are removed if anything fails.
            std::vector<std::pair<double, double>> ranges;
            for (const Id id : ids) {
                double from = 0, to = 0;
                std::string err;
                if (!stemSource(l.project, s, id, handles, from, to, &err))
                    throw ArgError{QStringLiteral("Clip %1: %2").arg(qulonglong(id)).arg(QString::fromStdString(err))};
                ranges.emplace_back(from, to);
            }
            struct Written {
                std::vector<std::string> files;
                bool keep = false;
                ~Written() {
                    if (!keep)
                        for (const std::string& f : files) QFile::remove(QString::fromStdString(f));
                }
            } written;
            QJsonArray placed;
            for (size_t c = 0; c < ids.size(); ++c) {
                const Id id = ids[c];
                const auto [from, to] = ranges[c];
                std::string err;
                const Clip* clip = edit::clipById(s, id);
                const MediaItem* m = l.project.findMedia(clip->mediaId);
                QString base = QString::fromStdString(clip->name.empty() ? m->name : clip->name);
                for (const QChar bad : QStringLiteral("/\\:*?\"<>|")) base.replace(bad, '_');
                std::vector<std::string> outs;
                for (const std::string& t : titles) {
                    QString path = folder + '/' + base + ' ' + QString::fromStdString(t) + QStringLiteral(".wav");
                    for (int k = 2; QFileInfo::exists(path); ++k)
                        path = folder + '/' + base + ' ' + QString::fromStdString(t) + ' ' + QString::number(k) + QStringLiteral(".wav");
                    outs.push_back(path.toStdString());
                }
                const std::string source = m->path, clipName = clip->name.empty() ? m->name : clip->name;
                if (!separateStemsToFiles(source, from, to, clip->channels, stems, outs, {}, nullptr, &err))
                    return fail(QStringLiteral("Clip %1: %2").arg(qulonglong(id)).arg(QString::fromStdString(err)));
                written.files.insert(written.files.end(), outs.begin(), outs.end());
                std::vector<Id> media;
                for (size_t k = 0; k < outs.size(); ++k) {
                    media.push_back(mediaFor(l.project, QString::fromStdString(outs[k])));
                    if (MediaItem* mi = l.project.findMedia(media.back())) {
                        mi->metadata["comment"] = titles[k] + " separated from " + clipName + " (" +
                                                  QFileInfo(QString::fromStdString(uninterpretedPath(source))).fileName().toStdString() +
                                                  ") with " + (stems == 3 ? "Bandit v2" : "Spleeter") + " in Montage";
                        if (std::find(mi->keywords.begin(), mi->keywords.end(), "Stem") == mi->keywords.end()) mi->keywords.push_back("Stem");
                    }
                }
                const edit::Result r = placeStems(l.project, s, id, media, titles, from, a.value("mute_original").toBool(true));
                if (!r.ok) return fail(QStringLiteral("Clip %1: %2").arg(qulonglong(id)).arg(QString::fromStdString(r.error)));
                for (size_t k = 0; k < r.created.size() && k < titles.size(); ++k)
                    placed.append(QJsonObject{{"clip", double(r.created[k])}, {"stem", QString::fromStdString(titles[k])},
                                              {"from_clip", double(id)}, {"file", QString::fromStdString(outs[k])}});
            }
            written.keep = true;
            save(l);
            return ok(QStringLiteral("Separated %1 clip(s) into %2 stems each").arg(ids.size()).arg(stems), QJsonObject{{"stems", placed}});
        });

    add("montage_clone_speech", "Speak in a cloned voice",
        "Speak text in a voice cloned from a few seconds of someone speaking (Chatterbox Multilingual, on this computer; "
        "5 to 10 clean seconds is best), in any of 23 languages (ar da de el en es fi fr he hi it ja ko ms nl no pl pt ru "
        "sv sw tr zh), and place it on an audio track: text at a time, a caption track spoken cue by cue, or text in place "
        "of what is said from replace.start to replace.end (fitted to that time and put over it on the track, as Descript's "
        "Overdub does; the voice heard there is cloned unless one is given). The voice is voice_clip (a clip id, what it "
        "plays), voice_media (a file or the project's media, voice_from to voice_to seconds), or the clip heard at the "
        "time. Needs consent: true, confirming you have the speaker's consent (or it is your own voice). Each WAV (in the "
        "Voiceover folder) and its media note that it is generated, and from whose voice; its media gets the keyword AI voice.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"text":{"type":"string"},
            "captions":{"type":"integer","description":"Caption track index to speak instead of text"},
            "consent":{"type":"boolean","description":"You have the speaker's consent to clone their voice"},
            "voice_clip":{"type":"integer"},"voice_media":{"type":"string"},"voice_from":{"type":"number"},"voice_to":{"type":"number"},
            "language":{"type":"string","default":"en"},"exaggeration":{"type":"number","default":0.5,"description":"0.25 flat to 2 very expressive"},
            "speed":{"type":"number","default":1},"at":{"type":["number","string"]},
            "replace":{"type":"object","properties":{"start":{"type":["number","string"]},"end":{"type":["number","string"]}},"required":["start","end"]},
            "track":{"type":"string","description":"Audio track, default A1"}},"required":["project","consent"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            std::vector<SpeechLine> lines;
            FrameTime where = a.contains("at") ? timeArg(a.value("at"), s, "at") : 0;
            const bool replace = a.contains("replace");
            if (replace) {
                const QJsonObject r = a.value("replace").toObject();
                const FrameTime start = timeArg(r.value("start"), s, "replace.start"), end = timeArg(r.value("end"), s, "replace.end");
                if (end <= start) throw ArgError{"replace.end must be after replace.start"};
                lines.push_back({need(a, "text").toStdString(), start, end - start});
                where = start;
            } else if (a.contains("captions")) {
                const int index = a.value("captions").toInt();
                if (index < 0 || index >= int(s.captionTracks.size())) throw ArgError{"No such caption track"};
                lines = captionSpeech(s.captionTracks[size_t(index)]);
                if (!lines.empty()) where = lines.front().at;
            } else {
                lines.push_back({need(a, "text").toStdString(), where, -1});
            }
            if (lines.empty()) return fail("Nothing to say");
            CloneVoice voice = cloneVoiceArg(a, l.project, s, where);
            voice.exact = replace;
            const TrackRef au = trackArg(str(a, "track", "A1"), s, true, &l.project, &s);
            QJsonArray placed;
            double total = 0;
            const QString err = speakLines(l.project, s, lines, {}, std::clamp(a.value("speed").toDouble(1), 0.5, 2.0), au,
                                           QFileInfo(absolute(need(a, "project"))).absolutePath() + QStringLiteral("/Voiceover"), placed, total,
                                           &voice);
            if (!err.isEmpty()) return fail(err);
            save(l);
            return ok(QStringLiteral("Placed %1 clip(s) in the voice from %2, %3 s in all")
                          .arg(lines.size())
                          .arg(QString::fromStdString(voice.from))
                          .arg(total, 0, 'f', 1),
                      QJsonObject{{"clips", placed}, {"voice_from", QString::fromStdString(voice.from)}, {"language", QString::fromStdString(voice.options.language)}});
        });

    add("montage_convert_voice", "Convert a voice",
        "Say a clip's speech again in another person's voice (speech to speech, as Resolve's Voice Convert: Chatterbox, on "
        "this computer), its words, timing and delivery kept, for ADR read by someone else or a temp line made to sound "
        "like the actor. clip is an audio clip; its sound (with handles seconds either side, default 1) is converted and "
        "added to it as a take, picked, so the original stays one take away. The voice is voice_clip (a clip id, what it "
        "plays) or voice_media (a file or the project's media, voice_from to voice_to seconds); 5 to 10 clean seconds of "
        "them speaking is best. Needs consent: true, confirming you have the speaker's consent (or it is your own voice). "
        "The WAV (in the Voiceover folder) and its media note what it was converted from and whose voice it has; its media "
        "gets the keyword AI voice.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"integer","description":"The audio clip to convert"},
            "consent":{"type":"boolean","description":"You have the speaker's consent to use their voice"},
            "voice_clip":{"type":"integer"},"voice_media":{"type":"string"},"voice_from":{"type":"number"},"voice_to":{"type":"number"},
            "handles":{"type":"number","default":1,"description":"Seconds converted either side of what the clip plays"}},
            "required":["project","clip","consent"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Id clip = clipArg(l, a).id;
            const auto at = edit::locate(s, clip);
            double from = 0, to = 0;
            if (!at || at->track.kind != TrackKind::Audio ||
                !edit::voiceConversionRange(l.project, s, clip, std::clamp(a.value("handles").toDouble(1), 0.0, 10.0), from, to))
                throw ArgError{"clip must be a clip with sound on an audio track"};
            if (!a.contains("voice_clip") && !a.contains("voice_media")) throw ArgError{"Give voice_clip or voice_media: the voice to convert to"};
            if (a.contains("voice_clip") && Id(a.value("voice_clip").toDouble()) == clip) throw ArgError{"voice_clip is the clip being converted"};
            const CloneVoice voice = cloneVoiceArg(a, l.project, s, 0);
            const Clip* c = edit::clipById(s, clip);
            const MediaItem* m = l.project.findMedia(c->mediaId);
            const std::string name = c->name.empty() ? m->name : c->name;
            std::vector<float> speech, converted;
            std::string err;
            progress(0, QStringLiteral("Converting the voice"));
            if (!voiceSoundFrom(m->path, from, to, speech, &err, c->channels, s.sampleRate > 0 ? s.sampleRate : 48000) ||
                !convertVoice(speech, kCloneSampleRate, voice.sample, kCloneSampleRate, converted, &err,
                              [this](double f) { progress(f, QStringLiteral("Converting the voice")); }))
                return fail(QString::fromStdString(err));
            const std::string note = "Speech from " + name + " converted to a voice cloned from " + voice.from + " with Chatterbox in Montage, " +
                                     QDateTime::currentDateTime().toString(Qt::ISODate).toStdString() +
                                     "; the speaker's consent was confirmed by the caller.";
            const QString folder = QFileInfo(absolute(need(a, "project"))).absolutePath() + QStringLiteral("/Voiceover");
            QDir().mkpath(folder);
            QString path;
            int n = 1;
            do path = folder + '/' + QString::fromStdString(edit::convertedVoiceStem(name)) + QString::number(n++) + QStringLiteral(".wav");
            while (QFileInfo::exists(path));
            if (!writeClonedSpeechWav(path.toStdString(), converted, note, &err)) return fail(QString::fromStdString(err));
            const Id media = mediaFor(l.project, path);
            if (MediaItem* made = l.project.findMedia(media)) {
                made->metadata["comment"] = note;
                made->bin = "Voiceover";  // (as the app files it)
                if (std::find(made->keywords.begin(), made->keywords.end(), "AI voice") == made->keywords.end()) made->keywords.push_back("AI voice");
            }
            if (const edit::Result r = edit::addConvertedTake(l.project, s, clip, media, from); !r.ok) {
                QFile::remove(path);  // (the project is not saved)
                return fail(QString::fromStdString(r.error));
            }
            save(l);
            const Clip* now = edit::clipById(s, clip);
            return ok(QStringLiteral("Converted %1 (%2 s) to the voice from %3: take %4 of %5, picked")
                          .arg(QString::fromStdString(name))
                          .arg(double(converted.size()) / kCloneSampleRate, 0, 'f', 2)
                          .arg(QString::fromStdString(voice.from))
                          .arg(now->take + 1)
                          .arg(now->takes.size()),
                      QJsonObject{{"file", path}, {"media", double(media)}, {"seconds", double(converted.size()) / kCloneSampleRate},
                                  {"take", now->take}, {"takes", int(now->takes.size())}, {"voice_from", QString::fromStdString(voice.from)}});
        });

    add("montage_audio_description", "Audio description",
        "Described video for broadcasters and streaming services: descriptions of what is seen, spoken in the gaps between "
        "the dialogue. gaps finds them (speech on the audio tracks other than the AD track; at least min_gap seconds, kept "
        "clear of the lines); write puts descriptions (start, optional end: by default the gap it starts in, or three "
        "seconds; empty text removes one) on the hidden \"Audio Description\" caption track; list shows them with how they "
        "fit at pace words a minute; voice speaks them with an AI voice (Kokoro) onto the AD track as clips of the role "
        "Description, replacing what was voiced before; duck lowers every other clip under them by duck_db; hear sets "
        "whether they play while working. montage_render's described option (or the export dialog's) writes the mix "
        "without them and a stream of the programme with them.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "action":{"type":"string","enum":["gaps","write","list","voice","duck","hear"],"default":"list"},
            "min_gap":{"type":"number","default":2},"pace":{"type":"number","default":160},
            "descriptions":{"type":"array","items":{"type":"object","properties":{"start":{"type":["number","string"]},
                "end":{"type":["number","string"]},"text":{"type":"string"}},"required":["start","text"]}},
            "voice":{"type":"string","default":"bf_emma"},"duck_db":{"type":"number","default":-9},"on":{"type":"boolean"}},
            "required":["project"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const QString action = a.value("action").toString("list");
            const double pace = std::clamp(a.value("pace").toDouble(160), 60.0, 300.0);
            auto adTrack = [&]() {
                for (int i = 0; i < int(s.audioTracks.size()); ++i)
                    if (s.audioTracks[size_t(i)].name == "AD") return i;
                return -1;
            };
            auto findGaps = [&](std::vector<DescriptionGap>& out, double minGap) -> QString {
                std::string err;
                const Spans speech = descriptionSpeech(l.project, s, adTrack(), &err);
                if (!err.empty()) return QString::fromStdString(err);
                out = descriptionGaps(speech, s.fpsValue(), 0, std::max<FrameTime>(1, s.duration()), minGap);
                return {};
            };
            auto listing = [&] {
                QJsonArray list;
                const int t = findDescriptionTrack(s);
                if (t >= 0)
                    for (const Caption& c : s.captionTracks[size_t(t)].captions) {
                        const DescriptionFit f = descriptionFit(c.text, double(c.end - c.start) / s.fpsValue(), pace);
                        list.append(QJsonObject{{"start", tc(c.start, s)}, {"end", tc(c.end, s)}, {"text", QString::fromStdString(c.text)},
                                                {"room", f.room}, {"needed", f.needed}, {"fits", f.fits}, {"speed", f.speed},
                                                {"over_words", f.overWords}});
                    }
                return QJsonObject{{"descriptions", list}, {"heard", !edit::roleMuted(s, kDescriptionRole)},
                                   {"voiced", hasDescriptionClips(s)}};
            };
            if (action == "list") return ok(QStringLiteral("%1 description(s)").arg(listing().value("descriptions").toArray().size()), listing());
            if (action == "gaps") {
                std::vector<DescriptionGap> gaps;
                if (const QString err = findGaps(gaps, std::max(0.5, a.value("min_gap").toDouble(2))); !err.isEmpty()) return fail(err);
                QJsonArray out;
                for (const DescriptionGap& g : gaps)
                    out.append(QJsonObject{{"start", tc(g.start, s)}, {"end", tc(g.end, s)}, {"seconds", double(g.length()) / s.fpsValue()},
                                           {"words", int(std::floor(double(g.length()) / s.fpsValue() * pace / 60))}});
                return ok(QStringLiteral("%1 gap(s) between the dialogue").arg(gaps.size()), QJsonObject{{"gaps", out}});
            }
            if (action == "write") {
                const QJsonArray items = a.value("descriptions").toArray();
                if (items.isEmpty()) throw ArgError{"\"descriptions\" lists what to write"};
                std::vector<DescriptionGap> gaps;
                bool haveGaps = false;
                for (const QJsonValue& v : items) {
                    const QJsonObject o = v.toObject();
                    const FrameTime start = timeArg(o.value("start"), s, "start");
                    FrameTime end = -1;
                    if (o.contains("end")) {
                        end = timeArg(o.value("end"), s, "end");
                    } else {
                        if (!haveGaps) {
                            if (const QString err = findGaps(gaps, 0.1); !err.isEmpty()) return fail(err);  // whatever its length
                            haveGaps = true;
                        }
                        for (const DescriptionGap& g : gaps)
                            if (start >= g.start && start < g.end) end = g.end;
                        if (end < 0) end = start + FrameTime(std::llround(3 * s.fpsValue()));
                        // Up to the next description, written now or already there, so one never erases another.
                        for (const QJsonValue& w : items)
                            if (const FrameTime next = timeArg(w.toObject().value("start"), s, "start"); next > start) end = std::min(end, next);
                        if (const int t = findDescriptionTrack(s); t >= 0)
                            for (const Caption& c : s.captionTracks[size_t(t)].captions)
                                if (c.start > start) end = std::min(end, c.start);
                    }
                    const std::string text = o.value("text").toString().toStdString();
                    if (!setDescription(l.project, s, start, end, text) && !text.empty())
                        throw ArgError{"A description's end must come after its start"};
                }
                save(l);
                return ok("Wrote the descriptions", listing());
            }
            if (action == "voice") {
                const int t = findDescriptionTrack(s);
                std::vector<SpeechLine> lines;
                if (t >= 0)
                    for (const Caption& c : s.captionTracks[size_t(t)].captions) {
                        std::string text = c.text;
                        std::replace(text.begin(), text.end(), '\n', ' ');
                        if (!text.empty()) lines.push_back({text, c.start, c.end - c.start});
                    }
                if (lines.empty()) return fail("There are no descriptions to voice");
                if (!ttsAvailable()) return fail("This build of Montage cannot speak (no ONNX Runtime)");
                if (!ttsModel().installed())
                    return fail("The speech model is not downloaded: run `scripts/fetch-models.sh` or generate a voiceover once in the app");
                const std::string voice = str(a, "voice", "bf_emma").toStdString();
                if (!findTtsVoice(voice)) throw ArgError{QStringLiteral("Unknown voice \"%1\"").arg(QString::fromStdString(voice))};
                std::vector<Id> old;
                for (const Track& tr : s.audioTracks)
                    for (const Clip& c : tr.clips)
                        if (c.role == kDescriptionRole) old.push_back(c.id);
                if (!old.empty()) edit::removeClips(l.project, s, old, false);
                int ad = adTrack();
                if (ad < 0) {
                    ad = edit::addTrack(l.project, s, TrackKind::Audio).index;
                    s.audioTracks[size_t(ad)].name = "AD";
                }
                QJsonArray placed;
                double total = 0;
                const QString err = speakLines(l.project, s, lines, voice, 1.0, {TrackKind::Audio, ad},
                                               QFileInfo(absolute(need(a, "project"))).absolutePath() + QStringLiteral("/Audio Description"),
                                               placed, total);
                if (!err.isEmpty()) return fail(err);
                for (const QJsonValue& v : placed)
                    if (Clip* c = edit::clipById(s, Id(v.toObject().value("clip").toDouble()))) c->role = kDescriptionRole;
                save(l);
                QJsonObject r = listing();
                r["clips"] = placed;
                return ok(QStringLiteral("Voiced %1 description(s), %2 s in all").arg(placed.size()).arg(total, 0, 'f', 1), r);
            }
            if (action == "duck") {
                if (!hasDescriptionClips(s)) return fail("Voice the descriptions first");
                // On a lane of its own, heard only with the descriptions: the mix without them is not dipped.
                const int changed = duckUnderDescriptions(s, std::clamp(a.value("duck_db").toDouble(-9), -40.0, -1.0));
                save(l);
                return ok(QStringLiteral("Ducked %1 clip(s) under the descriptions").arg(changed), QJsonObject{{"ducked", changed}});
            }
            if (action == "hear") {
                if (!a.contains("on")) throw ArgError{"\"on\" says whether the descriptions are heard"};
                edit::setRoleMuted(s, kDescriptionRole, !a.value("on").toBool());
                save(l);
                return ok(a.value("on").toBool() ? "Descriptions are heard" : "Descriptions are muted (exports still describe)", listing());
            }
            throw ArgError{"Unknown action"};
        });

    add("montage_dub", "Dub a caption track",
        "Dub a caption track on this computer: translated from its language (Opus-MT; skipped if it is in the dub's language "
        "already), each cue spoken at its time on a new audio track \"Dub (<language>)\", and every other audio clip lowered "
        "by duck_db while the dub speaks (volume keyframes, replacing theirs; 0 leaves them), as in a voice-over translation. "
        "The translated captions are added as a hidden track. The voice is an English AI voice (Kokoro; a little faster where "
        "a cue is short), or with voice: \"clone\" the speaker's own voice cloned (Chatterbox; from voice_clip, voice_media, "
        "or the clip heard under the first caption) into `to`, any of ar da de el en es fi fr he hi it ko ms nl no pl pt ru "
        "sv sw tr zh (not Japanese: translations come in kanji, which the voice model cannot read), which needs consent: true. "
        "lip_sync: true also gives the picture clips under the dub Lip Sync to it (their speaker's mouth redrawn to speak "
        "the dub; needs consent: true and scripts/fetch-models.sh --lipsync). Needs the translation and speech models (the app asks the first time; or "
        "scripts/fetch-models.sh).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "track":{"type":"integer","default":0,"description":"Caption track index"},
            "from":{"type":"string","description":"The track's language, if its setting is wrong"},
            "voice":{"type":"string","default":"af_heart","description":"A Kokoro voice, or \"clone\""},"speed":{"type":"number","default":1},
            "to":{"type":"string","default":"en","description":"With voice clone: the dub's language"},
            "consent":{"type":"boolean"},"voice_clip":{"type":"integer"},"voice_media":{"type":"string"},
            "voice_from":{"type":"number"},"voice_to":{"type":"number"},
            "duck_db":{"type":"number","default":-18},"lip_sync":{"type":"boolean","default":false}},"required":["project"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const int index = a.value("track").toInt(0);
            if (index < 0 || index >= int(s.captionTracks.size())) return fail("No such caption track");
            if (s.captionTracks[size_t(index)].captions.empty()) return fail("That caption track is empty");
            if (!ttsAvailable() || !translatorAvailable()) return fail("This build of Montage cannot dub (no ONNX Runtime)");
            const std::string voice = str(a, "voice", "af_heart").toStdString();
            const bool cloning = voice == "clone";
            const bool lipSync = a.value("lip_sync").toBool();
            if (lipSync) {
                if (!lipSyncAvailable() || !lipSyncModel().installed() || !faceModel().installed() || !faceMeshModel().installed())
                    return fail("Lip Sync needs its models: run `scripts/fetch-models.sh --lipsync` or add the effect once in the app");
                if (!a.value("consent").toBool())
                    throw ArgError{"lip_sync redraws the speaker's mouth to say the dub: pass consent: true once they have agreed"};
            }
            std::optional<CloneVoice> clone;
            std::string to = "en";
            if (cloning) {
                QJsonObject args = a;
                args["language"] = str(a, "to", "en");
                // (as the app: translations come in kanji, and the voice model reads Japanese in kana only)
                if (const CloneLanguage* want = findCloneLanguage(args["language"].toString().toStdString()); want && std::string(want->code) == "ja")
                    throw ArgError{"Dubbing into Japanese in a cloned voice is not offered: translations come in kanji, which the voice model cannot read (montage_clone_speech reads kana)"};
                const FrameTime first = s.captionTracks[size_t(index)].captions.front().start;
                clone = cloneVoiceArg(args, l.project, s, first);
                to = clone->options.language;
            } else {
                if (!ttsModel().installed())
                    return fail("The speech model is not downloaded: run `scripts/fetch-models.sh` or generate a voiceover once in the app");
                if (!findTtsVoice(voice)) throw ArgError{QStringLiteral("Unknown voice \"%1\"").arg(QString::fromStdString(voice))};
            }
            const CloneLanguage* toLang = findCloneLanguage(to);
            const QString toName = toLang ? QString::fromUtf8(toLang->name) : QStringLiteral("English");
            const double speed = std::clamp(a.value("speed").toDouble(1), 0.5, 2.0);
            const double duckDb = std::clamp(a.value("duck_db").toDouble(-18), -60.0, 0.0);
            const CaptionTrack source = s.captionTracks[size_t(index)];
            const std::string from = a.contains("from") ? a.value("from").toString().toLower().toStdString()
                                                        : (source.language.empty() ? "en" : source.language);
            int english = index;  // the captions spoken (in `to`)
            if (from != to) {
                const auto route = translationRoute(from, to);
                if (route.empty())
                    return fail(QStringLiteral("There is no translation from %1 to %2").arg(QString::fromStdString(from), toName));
                for (const ModelPack* pack : route)
                    if (!pack->installed())
                        return fail(QStringLiteral("The %1 is not downloaded (%2 MB): translate once in the app, or fetch it into %3")
                                        .arg(QString::fromStdString(pack->title))
                                        .arg(pack->bytes() / 1000000)
                                        .arg(QString::fromStdString(pack->directory())));
                std::vector<std::string> out;
                std::string err;
                progress(0, QStringLiteral("Translating"));
                if (!translateTexts(captionTexts(source), from, to, out, {}, nullptr, &err)) return fail(QString::fromStdString(err));
                CaptionTrack t = translatedTrack(source, out, l.project.newId(), to, translationLanguageName(to));
                t.visible = false;
                s.captionTracks.push_back(std::move(t));
                english = int(s.captionTracks.size()) - 1;
            }
            const std::vector<SpeechLine> lines = captionSpeech(s.captionTracks[size_t(english)]);
            if (lines.empty()) return fail("Nothing to say");
            const TrackRef dub = edit::addTrack(l.project, s, TrackKind::Audio);
            s.audioTracks[size_t(dub.index)].name = QStringLiteral("Dub (%1)").arg(toName).toStdString();
            QJsonArray placed;
            double total = 0;
            const QString err = speakLines(l.project, s, lines, voice, speed, dub,
                                           QFileInfo(absolute(need(a, "project"))).absolutePath() + QStringLiteral("/Voiceover"), placed, total,
                                           clone ? &*clone : nullptr);
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
            const int synced = lipSync ? edit::lipSyncToTrack(l.project, s, dub.index) : 0;
            save(l);
            QJsonArray sample;
            for (size_t i = 0; i < std::min<size_t>(3, lines.size()); ++i) sample.append(QString::fromStdString(lines[i].text));
            return ok(QStringLiteral("Dubbed %1 caption(s) onto A%2 (%3 s of speech); lowered %4 other clip(s) under it")
                          .arg(lines.size())
                          .arg(dub.index + 1)
                          .arg(total, 0, 'f', 1)
                              .arg(ducked) +
                          (synced ? QStringLiteral("; Lip Sync on %1 picture clip(s)").arg(synced) : QString()),
                      QJsonObject{{"captions_track", english}, {"audio_track", QStringLiteral("A%1").arg(dub.index + 1)}, {"clips", placed},
                                  {"ducked", ducked}, {"lip_synced", synced}, {"first", sample}});
        });

    add("montage_refresh_media", "Refresh media from disk",
        "Re-read media files whose length or streams changed on disk: a file still being recorded (a growing file from a "
        "recorder or capture card; the app follows those by itself), a render that finished, a file replaced. Each named "
        "media, or all; lists what changed with its old and new length. Clips keep their edits.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "media":{"type":"array","items":{"type":["string","number"]},"description":"Media names, files or ids (default: all)"}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            std::vector<MediaItem*> items;
            for (const QJsonValue& v : a.value("media").toArray()) {
                MediaItem* m = v.isDouble() ? l.project.findMedia(Id(v.toDouble())) : &projectMedia(l.project, v.toString());
                if (!m) throw ArgError{QStringLiteral("No media with id %1").arg(v.toDouble(), 0, 'f', 0)};
                items.push_back(m);
            }
            if (!a.contains("media"))
                for (MediaItem& m : l.project.media) items.push_back(&m);
            QJsonArray changed, unreadable;
            for (MediaItem* m : items) {
                if (m->path.empty() || m->kind == MediaKind::Sequence || m->subclipOf) continue;
                MediaItem fresh;
                if (!probeMedia(m->path, fresh)) {
                    unreadable.append(QString::fromStdString(m->name));
                    continue;
                }
                if (std::fabs(fresh.duration - m->duration) < 1e-6 && fresh.hasVideo == m->hasVideo && fresh.hasAudio == m->hasAudio) continue;
                changed.append(QJsonObject{{"media", double(m->id)}, {"name", QString::fromStdString(m->name)},
                                           {"was", m->duration}, {"now", fresh.duration}});
                m->duration = fresh.duration;
                m->hasVideo = fresh.hasVideo, m->hasAudio = fresh.hasAudio;
                if (fresh.width > 0) m->width = fresh.width, m->height = fresh.height;
                if (fresh.fps.valid()) m->fps = fresh.fps;
                m->sampleRate = fresh.sampleRate, m->channels = fresh.channels;
            }
            if (!changed.isEmpty()) save(l);
            QString text = changed.isEmpty() ? QStringLiteral("Nothing changed") : QStringLiteral("%1 changed").arg(changed.size());
            if (!unreadable.isEmpty()) text += QStringLiteral("; could not read %1").arg(unreadable.size());
            return ok(text, QJsonObject{{"changed", changed}, {"unreadable", unreadable}});
        });

    add("montage_storyboard", "Storyboard a bin",
        "The storyboard (Premiere's Freeform view, Avid's Frame view): a bin's media as thumbnails placed freely on a board, "
        "positions in thumbnail cells (x across, y down), read in rows top to bottom and left to right. board lists a bin's "
        "media with their spots (those not placed yet are laid out after the rest) and that reading order; move puts media "
        "at spots; arrange lays a bin's media (or those named) in a grid `columns` wide in their current order; assemble puts "
        "media (default: the bin's, in storyboard order; named media keep their storyboard order unless keep_order) into "
        "the active sequence one after another from `at`, inserted or over what is there.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "action":{"type":"string","enum":["board","move","arrange","assemble"]},
            "bin":{"type":"string","description":"Bin path (\"\" or left out: the top level)"},
            "media":{"type":"array","items":{"type":["string","number"]},"description":"Media names, files or ids"},
            "spots":{"type":"array","items":{"type":"object","properties":{"media":{"type":["string","number"]},"x":{"type":"number"},"y":{"type":"number"}},"required":["media","x","y"]}},
            "columns":{"type":"integer","minimum":1,"default":6},
            "at":{"type":["number","string"],"description":"Where assembling starts (default 0)"},
            "insert":{"type":"boolean","default":false},
            "keep_order":{"type":"boolean","default":false,"description":"Assemble the named media in the order given"},
            "video_track":{"type":"string","default":"V1"},"audio_track":{"type":"string","default":"A1"}},
            "required":["project","action"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const QString action = need(a, "action");
            auto mediaRef = [&](const QJsonValue& v) -> MediaItem& {
                if (v.isDouble()) {
                    if (MediaItem* m = l.project.findMedia(Id(v.toDouble()))) return *m;
                    throw ArgError{QStringLiteral("No media with id %1").arg(v.toDouble(), 0, 'f', 0)};
                }
                return projectMedia(l.project, v.toString());
            };
            const std::string bin = a.value("bin").toString().toStdString();
            if (!bin.empty()) {
                const std::vector<std::string> bins = projectBins(l.project);
                if (std::find(bins.begin(), bins.end(), bin) == bins.end())
                    throw ArgError{QStringLiteral("No bin \"%1\" (bins are paths such as \"Interviews/Day 1\")").arg(QString::fromStdString(bin))};
            }
            std::vector<Id> binMedia;  // (as the app's board shows it; assembling leaves out the sequence itself)
            for (const MediaItem& m : l.project.media)
                if (m.bin == bin) binMedia.push_back(m.id);
            std::vector<Id> named;
            for (const QJsonValue& v : a.value("media").toArray()) named.push_back(mediaRef(v).id);
            const int columns = std::max(1, a.value("columns").toInt(6));
            auto boardJson = [&](const std::vector<Id>& ids) {
                const std::map<Id, BoardSpot> spots = boardLayout(l.project, ids, columns);
                QJsonArray items, order;
                for (Id id : storyboardOrder(l.project, ids)) {
                    const MediaItem* m = l.project.findMedia(id);
                    const BoardSpot& b = spots.at(id);
                    items.append(QJsonObject{{"media", double(id)}, {"name", QString::fromStdString(m->name)}, {"x", b.x}, {"y", b.y}, {"placed", b.placed}});
                    order.append(double(id));
                }
                return QJsonObject{{"items", items}, {"order", order}};
            };
            if (action == "board") {
                const QJsonObject o = boardJson(named.empty() ? binMedia : named);
                return ok(QStringLiteral("%1 item(s) on the board").arg(o.value("items").toArray().size()), o);
            }
            if (action == "move") {
                const QJsonArray spots = a.value("spots").toArray();
                if (spots.isEmpty()) throw ArgError{"\"spots\" is required: [{media, x, y}]"};
                std::vector<Id> moved;
                for (const QJsonValue& v : spots) {
                    const QJsonObject o = v.toObject();
                    if (!o.value("x").isDouble() || !o.value("y").isDouble()) throw ArgError{"each spot needs numbers x and y"};
                    MediaItem& m = mediaRef(o.value("media"));
                    m.board = {std::max(0.0, o.value("x").toDouble()), std::max(0.0, o.value("y").toDouble()), true};
                    moved.push_back(m.id);
                }
                save(l);
                return ok(QStringLiteral("Moved %1").arg(moved.size()), boardJson(moved));
            }
            if (action == "arrange") {
                const std::vector<Id> ids = storyboardOrder(l.project, named.empty() ? binMedia : named);
                if (ids.empty()) throw ArgError{"Nothing to arrange in that bin"};
                arrangeStoryboard(l.project, ids, columns);
                save(l);
                return ok(QStringLiteral("Arranged %1 in rows of %2").arg(ids.size()).arg(columns), boardJson(ids));
            }
            if (action == "assemble") {
                Sequence& s = l.seq();
                std::vector<Id> ids = named.empty() ? binMedia : named;
                if (!(a.value("keep_order").toBool() && !named.empty())) ids = storyboardOrder(l.project, ids);
                if (ids.empty()) throw ArgError{"Nothing to assemble in that bin"};
                const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : 0;
                const TrackRef v = trackArg(a.value("video_track").toString("V1"), s, true, &l.project, &s);
                const TrackRef au = trackArg(a.value("audio_track").toString("A1"), s, true, &l.project, &s);
                if (v.kind != TrackKind::Video || au.kind != TrackKind::Audio) throw ArgError{"video_track must be a video track and audio_track an audio track"};
                const edit::Result r = assembleStoryboard(l.project, s, ids, at, v, au, a.value("insert").toBool());
                check(r);
                save(l);
                QJsonArray clips;
                for (Id c : r.created) clips.append(double(c));
                return ok(QStringLiteral("Assembled %1 item(s) from %2").arg(ids.size()).arg(tc(at, l.seq())), QJsonObject{{"clips", clips}});
            }
            throw ArgError{"\"action\" must be board, move, arrange or assemble"};
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
        "(bottom field first) whatever the frames are flagged; remove_pulldown undoes 2:3 pulldown (29.97 to 23.976). Arguments left out keep their current setting; 0 or "
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
            "remove_pulldown":{"type":"string","enum":["off","auto","A","B","C","D"],"description":"29.97 or 30 frame video carrying 23.976 or 24 frame pictures with 2:3 pulldown: woven back into those frames (it then plays at 23.976 or 24, with no combed or repeated frames). auto finds the cadence from the fields, stretch by stretch (an edit made after the pulldown breaks it); A to D say which frame of the cadence the file starts on (A A, B B, B C, C D), the fields in field_order (or as the file says)"},
            "raw_exposure":{"type":"number","description":"Camera RAW, CinemaDNG, ProRes RAW and Magic Lantern Video: exposure in stops (-5 to 5)"},
            "raw_temperature":{"type":["number","string"],"description":"Camera RAW: the light's colour temperature in kelvin (2000 to 25000), or \"as_shot\""},
            "raw_tint":{"type":"number","description":"Camera RAW: tint, + magenta, - green (-150 to 150)"},
            "raw_highlights":{"type":"string","enum":["clip","blend","rebuild"],"description":"Camera RAW stills and CinemaDNG (ProRes RAW clips at the sensor's white)"},
            "raw_half":{"type":"boolean","description":"Camera RAW: decode at half size for speed"},
            "stereo":{"type":"string","enum":["file","none","sbs","sbs_half","tb","tb_half","mv"],"description":"Stereoscopic 3D: how the eyes are packed (side by side or top and bottom, full size or squeezed, or mv for MV-HEVC's two views), none for a flat picture"},
            "swap_eyes":{"type":"boolean","description":"Stereoscopic 3D: the eyes are the other way round"},
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
                if (a.contains("remove_pulldown")) {
                    const QString pd = str(a, "remove_pulldown");
                    if (pd == "off") {
                        i.pulldown.clear();
                    } else if (pd == "auto") {
                        std::vector<PulldownStretch> st;
                        std::string why;
                        if (!detectPulldown(uninterpretedPath(m->path), st, {}, nullptr, &why))
                            throw ArgError{QStringLiteral("%1: %2").arg(QString::fromStdString(m->name), QString::fromStdString(why))};
                        i.pulldown = pulldownValue(st);
                    } else if (pd.size() == 1 && pd[0] >= 'A' && pd[0] <= 'D') {
                        i.pulldown = pulldownStartingAt(pd[0].toLatin1() - 'A', i.fields.empty() ? fileTopFieldFirst(uninterpretedPath(m->path)) : i.fields != "lower");
                    } else {
                        throw ArgError{"\"remove_pulldown\" is off, auto or A to D"};
                    }
                }
                if (a.contains("raw_exposure")) i.rawExposure = a.value("raw_exposure").toDouble();
                if (a.contains("raw_temperature")) {
                    const QJsonValue t = a.value("raw_temperature");
                    if (t.isString() && t.toString() != "as_shot") throw ArgError{"raw_temperature is kelvin (a number) or \"as_shot\""};
                    i.rawTemperature = t.isString() ? 0.0 : t.toDouble();
                }
                if (a.contains("raw_tint")) i.rawTint = a.value("raw_tint").toDouble();
                if (a.contains("raw_highlights")) i.rawHighlights = str(a, "raw_highlights") == "clip" ? "" : str(a, "raw_highlights").toStdString();
                if (a.contains("raw_half")) i.rawHalf = a.value("raw_half").toBool();
                if (a.contains("stereo")) i.stereo = str(a, "stereo") == "file" ? "" : str(a, "stereo").toStdString();
                if (a.contains("swap_eyes")) i.swapEyes = a.value("swap_eyes").toBool();
                const edit::Result r = edit::interpretFootage(l.project, m->id, i);
                if (!r.ok && !r.error.empty()) throw ArgError{QStringLiteral("%1: %2").arg(QString::fromStdString(m->name), QString::fromStdString(r.error))};
                changed += r.ok;
                m = l.project.findMedia(m->id);
                QJsonObject o{{"name", QString::fromStdString(m->name)}, {"frame_rate", m->fps.toDouble()}, {"duration_seconds", m->duration},
                              {"width", m->width}, {"height", m->height}};
                if (!m->stereo.empty()) o["stereo"] = QString::fromStdString(m->stereo);
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

    add("montage_gallery", "Colour gallery stills",
        "The colour gallery (Resolve's Gallery): stills of the cut, each with the grade of the clip it shows, kept in "
        "albums beside the project (Gallery/<project>/<album>, a PNG and the grade as JSON). list gives the albums and "
        "an album's stills; grab takes a still of the picture at `at` with the top clip's grade; apply puts a still's "
        "grade (its colour effects and nodes) in place of the grade of `clips`, their other effects kept; rename, remove "
        "and add_album tidy up. A still is named by its id or its label (1, 2...).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "action":{"type":"string","enum":["list","grab","apply","rename","remove","add_album"],"default":"list"},
            "album":{"type":"string","default":"Stills"},
            "still":{"type":"string","description":"apply, rename, remove: the still's id or label"},
            "at":{"type":["number","string"],"description":"grab: timeline time (default the sequence's playhead)"},
            "clips":{"type":"array","items":{"type":["number","string"]},"description":"apply: video clips' ids or names"},
            "label":{"type":"string","description":"rename: its new label"},
            "name":{"type":"string","description":"add_album: the album's name"}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const std::string folder = galleryFolder(l.path.toStdString());
            const std::string album = str(a, "album", kDefaultAlbum).toStdString();
            const QString action = str(a, "action", "list");
            std::string err;
            auto stillJson = [](const GalleryStill& g) {
                return QJsonObject{{"id", QString::fromStdString(g.id)},           {"label", QString::fromStdString(g.label)},
                                   {"picture", QString::fromStdString(g.picture)}, {"clip", QString::fromStdString(g.clip)},
                                   {"sequence", QString::fromStdString(g.sequence)}, {"timecode", QString::fromStdString(g.timecode)},
                                   {"created", QString::fromStdString(g.created)}, {"grade_effects", int(g.effects.size())},
                                   {"grade_nodes", int(g.graph.nodes.size())}};
            };
            auto still = [&]() {
                GalleryStill g;
                if (!findStill(folder, album, need(a, "still").toStdString(), g))
                    throw ArgError{QStringLiteral("No still \"%1\" in album \"%2\" (or several have that label: give its id)").arg(str(a, "still"), QString::fromStdString(album))};
                return g;
            };
            if (action == "list") {
                QJsonArray albums, stills;
                for (const std::string& name : galleryAlbums(folder)) albums.append(QString::fromStdString(name));
                for (const GalleryStill& g : galleryStills(folder, album)) stills.append(stillJson(g));
                return ok(QStringLiteral("%1 still(s) in %2").arg(stills.size()).arg(QString::fromStdString(album)),
                          QJsonObject{{"folder", QString::fromStdString(folder)}, {"albums", albums}, {"stills", stills}});
            }
            if (action == "add_album") {
                if (!addAlbum(folder, need(a, "name").toStdString(), &err)) return fail(QString::fromStdString(err));
                return ok(QStringLiteral("Added album %1").arg(str(a, "name")));
            }
            if (action == "grab") {
                const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : s.playhead;
                GalleryStill g;
                if (!grabStill(l.project, s, at, folder, album, g, &err)) return fail(QString::fromStdString(err));
                return ok(QStringLiteral("Grabbed still %1 of %2").arg(QString::fromStdString(g.label), QString::fromStdString(g.clip)), stillJson(g));
            }
            if (action == "rename") {
                GalleryStill g = still();
                if (!renameStill(folder, g, need(a, "label").toStdString(), &err))
                    return fail(err.empty() ? QStringLiteral("Cannot rename the still") : QString::fromStdString(err));
                return ok(QStringLiteral("Renamed"), stillJson(g));
            }
            if (action == "remove") {
                if (!removeStill(folder, still())) return fail("Cannot remove the still");
                return ok(QStringLiteral("Removed"));
            }
            if (action != "apply") throw ArgError{"\"action\" is list, grab, apply, rename, remove or add_album"};
            const GalleryStill g = still();
            std::vector<Id> clips;
            for (const QJsonValue& v : a.value("clips").toArray()) clips.push_back(videoClipArg(l, QJsonObject{{"clip", v}}).id);
            if (clips.empty()) throw ArgError{"\"clips\" is required"};
            const auto r = edit::applyStillGrade(l.project, s, g, clips);
            if (!r.ok) return fail(QString::fromStdString(r.error));
            save(l);
            return ok(QStringLiteral("Applied still %1's grade to %2 clip(s)").arg(QString::fromStdString(g.label)).arg(r.applied),
                      QJsonObject{{"applied", double(r.applied)}});
        });

    add("montage_trim", "HDR and SDR trim passes",
        "Trim passes: the sequence graded once (its master, usually HDR) delivered to other "
        "displays, each a trim target: a preset (sdr: Rec.709 100 nits; hdr10-1000, hdr10-600, hdr10-300: PQ rolled off "
        "to that peak; hlg) or a space and peak of its own. Each shot (the top picture clip on screen) can be trimmed for "
        "a target on its code values: lift (-0.5..0.5), gamma (-1..1), gain (0..2), saturation (0..2), highlights (a "
        "roll-off, 0..1) and mid_detail (-1..1), keyframed with `keyframe` (or where a control has keys), and peak (the "
        "shot's brightest light in nits, which the target's roll-off starts from; 0 the master's peak). list gives the "
        "targets and the clips' trims; add_target, remove_target; set a clip's controls (at a timeline time, default the "
        "playhead); reset clips' trims; copy one clip's trim to others; analyse measures each shot's light in an HDR "
        "master (between `from` and `to`, default all) and gives every shot with no trim of its own a starting trim for "
        "`target` (default every target), shots of one colour group pulled towards their median, returning each shot's "
        "light and trims (whether each was filled, kept the shot's own or is on a locked track; `measure_only` changes "
        "nothing). montage_render's trim_target delivers a target.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "action":{"type":"string","enum":["list","add_target","remove_target","set","reset","copy","analyse"],"default":"list"},
            "target":{"type":"string","description":"A target's id; add_target: a preset (sdr, hdr10-1000, hdr10-600, hdr10-300, hlg) or omitted with color_space"},
            "from":{"type":["number","string"],"description":"analyse: timeline time to start at (default the start)"},
            "to":{"type":["number","string"],"description":"analyse: timeline time to end at (exclusive; default the end)"},
            "measure_only":{"type":"boolean","default":false,"description":"analyse: return the light and trims without filling them"},
            "color_space":{"type":"string","description":"add_target: a display space (rec709, rec2100pq, rec2100hlg...)"},
            "peak_nits":{"type":"number","description":"add_target: the display's peak (PQ)"},
            "name":{"type":"string","description":"add_target: its name"},
            "clip":{"type":["number","string"],"description":"set, copy: the clip (id or name)"},
            "clips":{"type":"array","items":{"type":["number","string"]},"description":"reset, copy: clips (ids or names)"},
            "values":{"type":"object","description":"set: controls and their values, e.g. {\"gain\":0.9,\"highlights\":0.4}"},
            "at":{"type":["number","string"],"description":"set: timeline time a keyframed control is set at (default the playhead)"},
            "keyframe":{"type":"boolean","default":false,"description":"set: add keyframes at `at` (the first also keeping the control's value at the clip's start)"}},
            "required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const QString action = str(a, "action", "list");
            auto done = [&](edit::Result r, const QString& said) {
                if (!r.ok) return fail(QString::fromStdString(r.error));
                save(l);
                return ok(said);
            };
            auto clipsArg = [&] {
                std::vector<Id> ids;
                for (const QJsonValue& v : a.value("clips").toArray()) ids.push_back(videoClipArg(l, QJsonObject{{"clip", v}}).id);
                if (ids.empty()) throw ArgError{"\"clips\" is required"};
                return ids;
            };
            if (action == "list") {
                QJsonArray targets, presets, trims;
                for (const TrimTarget& tt : s.trimTargets)
                    targets.append(QJsonObject{{"id", QString::fromStdString(tt.id)}, {"name", QString::fromStdString(tt.name)},
                                               {"color_space", QString::fromStdString(tt.colorSpace)}, {"peak_nits", trimTargetPeak(s, tt.id)}});
                for (const TrimTarget& tt : trimTargetPresets()) presets.append(QString::fromStdString(tt.id));
                for (const Track& tr : s.videoTracks)
                    for (const Clip& c : tr.clips)
                        for (const auto& [target, e] : c.trims) {
                            QJsonObject values;
                            for (const TrimControl& k : trimControls()) {
                                const auto it = e.params.find(k.name);
                                if (it == e.params.end()) continue;
                                if (it->second.animated()) values[k.name] = QStringLiteral("keyframed");
                                else if (std::fabs(it->second.value - k.def) > 1e-9) values[k.name] = it->second.value;
                            }
                            trims.append(QJsonObject{{"clip", double(c.id)}, {"name", QString::fromStdString(c.name)},
                                                     {"target", QString::fromStdString(target)}, {"values", values}});
                        }
                return ok(QStringLiteral("%1 trim target(s), %2 trim(s)").arg(targets.size()).arg(trims.size()),
                          QJsonObject{{"master", QString::fromStdString(s.colorSpace)}, {"master_peak_nits", s.hdrPeakNits},
                                      {"targets", targets}, {"presets", presets}, {"trims", trims}});
            }
            if (action == "add_target") {
                TrimTarget tt;
                const QString preset = str(a, "target");
                if (!preset.isEmpty() && !a.contains("color_space")) {
                    const auto& all = trimTargetPresets();
                    const auto it = std::find_if(all.begin(), all.end(), [&](const TrimTarget& x) { return x.id == preset.toStdString(); });
                    if (it == all.end()) throw ArgError{"\"target\" is a preset: sdr, hdr10-1000, hdr10-600, hdr10-300 or hlg"};
                    tt = *it;
                } else {
                    tt.id = preset.toStdString();
                    tt.colorSpace = need(a, "color_space").toStdString();
                    tt.peakNits = a.value("peak_nits").toDouble(1000);
                }
                if (a.contains("name")) tt.name = str(a, "name").toStdString();
                const size_t before = s.trimTargets.size();
                const auto r = edit::addTrimTarget(s, tt);
                if (!r.ok) return fail(QString::fromStdString(r.error));
                save(l);
                const TrimTarget& added = s.trimTargets.at(before);
                return ok(QStringLiteral("Added trim target %1").arg(QString::fromStdString(added.name)),
                          QJsonObject{{"id", QString::fromStdString(added.id)}});
            }
            if (action == "remove_target") return done(edit::removeTrimTarget(s, need(a, "target").toStdString()), QStringLiteral("Removed"));
            if (action == "set") {
                const Clip& c = videoClipArg(l, a);
                std::map<std::string, double> values;
                const QJsonObject v = a.value("values").toObject();
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!it.value().isDouble()) throw ArgError{QStringLiteral("\"%1\" must be a number").arg(it.key())};
                    values[it.key().toStdString()] = it.value().toDouble();
                }
                if (values.empty()) throw ArgError{"\"values\" is required"};
                const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : s.playhead;
                return done(edit::setTrim(s, c.id, need(a, "target").toStdString(), values, at, a.value("keyframe").toBool()),
                            QStringLiteral("Trimmed %1").arg(QString::fromStdString(c.name)));
            }
            if (action == "reset") return done(edit::resetTrims(s, clipsArg(), str(a, "target").toStdString()), QStringLiteral("Reset"));
            if (action == "copy") {
                const Clip& c = videoClipArg(l, a);
                return done(edit::copyTrim(s, c.id, clipsArg(), need(a, "target").toStdString()), QStringLiteral("Copied"));
            }
            if (action == "analyse") {
                std::vector<std::string> targets;
                if (a.contains("target")) {
                    if (!findTrimTarget(s, need(a, "target").toStdString())) return fail("No trim target " + str(a, "target"));
                    targets.push_back(str(a, "target").toStdString());
                }
                for (const TrimTarget& tt : s.trimTargets)
                    if (!a.contains("target")) targets.push_back(tt.id);
                if (targets.empty()) return fail("Add a trim target first (add_target)");
                const FrameTime from = a.contains("from") ? timeArg(a.value("from"), s, "from") : 0;
                const FrameTime to = a.contains("to") ? timeArg(a.value("to"), s, "to") : 0;
                if (a.contains("to") && to <= from) throw ArgError{"\"to\" must be after \"from\""};
                std::vector<ShotLight> lights;
                std::string err;
                if (!measureShotLight(l.project, s, from, to, lights, &err)) return fail(QString::fromStdString(err));
                std::vector<AutoTrim> trims;
                for (const std::string& t : targets) {
                    const std::vector<AutoTrim> some = solveAutoTrims(s, t, lights);
                    trims.insert(trims.end(), some.begin(), some.end());
                }
                // (which shots have their own, and which are on locked tracks, before filling)
                std::map<std::pair<Id, std::string>, bool> own, locked;
                for (const AutoTrim& t : trims)
                    if (const Clip* c = edit::clipById(s, t.clip)) {
                        own[{t.clip, t.target}] = hasOwnTrim(*c, t.target);
                        const auto loc = edit::locate(s, t.clip);
                        const Track* tr = loc ? trackAt(s, loc->track) : nullptr;
                        locked[{t.clip, t.target}] = !tr || tr->locked;
                    }
                const bool measureOnly = a.value("measure_only").toBool();
                FrameTime filled = 0;
                if (!measureOnly) {
                    const auto r = edit::fillAutoTrims(s, trims);
                    if (r.ok) save(l), filled = r.applied;
                }
                QJsonArray shots;
                for (const ShotLight& sl : lights) {
                    const Clip* c = edit::clipById(s, sl.clip);
                    QJsonArray its;
                    for (const AutoTrim& t : trims) {
                        if (t.clip != sl.clip) continue;
                        QJsonObject values;
                        for (const auto& [k, v] : t.values) values[QString::fromStdString(k)] = v;
                        its.append(QJsonObject{{"target", QString::fromStdString(t.target)}, {"values", values},
                                               {"aimed_grey_nits", t.aimedGrey}, {"kept_own", own[{t.clip, t.target}]},
                                               {"locked", locked[{t.clip, t.target}]},
                                               {"filled", !measureOnly && !own[{t.clip, t.target}] && !locked[{t.clip, t.target}]}});
                    }
                    shots.append(QJsonObject{{"clip", double(sl.clip)}, {"name", c ? QString::fromStdString(c->name) : QString()},
                                             {"from", double(sl.from)}, {"to", double(sl.to)}, {"log_average_nits", sl.logAverage},
                                             {"low_nits", sl.low}, {"high_nits", sl.high}, {"peak_nits", sl.peak}, {"trims", its}});
                }
                return ok(QStringLiteral("%1 shot(s) measured, %2 trim(s) filled").arg(lights.size()).arg(filled),
                          QJsonObject{{"shots", shots}, {"filled", double(filled)}});
            }
            throw ArgError{"\"action\" is list, add_target, remove_target, set, reset, copy or analyse"};
        });

    add("montage_match_chart", "Match a colour chart",
        "Colour-chart calibration (Resolve's Color Match): finds an X-Rite ColorChecker Classic (24 patches) in a clip's "
        "picture at `at` (or its middle frame), upright, upside down or on its side, or uses `corners` (the patch area's "
        "four corners in fractions of the frame, the chart's top left first, by Dark Skin, then clockwise), measures its "
        "patches and puts a Chart Match first in the clip's effects (a colour matrix in linear light) that brings them to "
        "the chart's published values. `same_media` grades every clip of the same media too. Returns the corners and "
        "the patches' mean CIE76 delta E before and after (under 2 is a close match). `measure_only` changes nothing.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":["number","string"],"description":"A video clip's id or name"},
            "at":{"type":["number","string"],"description":"Timeline time of the frame showing the chart"},
            "corners":{"type":"array","items":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2},"minItems":4,"maxItems":4},
            "same_media":{"type":"boolean","default":false},"measure_only":{"type":"boolean","default":false}},
            "required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Clip clip = videoClipArg(l, a);
            const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : clip.start + clip.duration / 2;
            const Image frame = chartFrame(l.project, s, clip, at);
            if (frame.empty()) return fail("The clip has no picture to read a chart from");
            TrackQuad q;
            double residual = 0;
            if (a.contains("corners")) {
                const QJsonArray cs = a.value("corners").toArray();
                if (cs.size() != 4) throw ArgError{"\"corners\" is four [x, y] pairs"};
                for (int i = 0; i < 4; ++i) {
                    const QJsonArray pt = cs[i].toArray();
                    if (pt.size() != 2 || !pt[0].isDouble() || !pt[1].isDouble()) throw ArgError{"\"corners\" is four [x, y] pairs"};
                    const double x = pt[0].toDouble(), y = pt[1].toDouble();
                    if (!(x >= 0 && x <= 1 && y >= 0 && y <= 1)) throw ArgError{"\"corners\" are fractions of the frame, 0 to 1"};
                    q.p[i] = {x, y};
                }
            } else if (!detectChart(chartView(frame, s), q, &residual)) {
                return fail("No ColorChecker chart found in the picture: give its corners");
            }
            bool complete = false;
            const ChartColors shot = sampleChart(frame, q, sequenceColorSpace(s), &complete);
            if (!complete) throw ArgError{"\"corners\" must outline the chart: four corners apart, in order round it, not crossed"};
            const ChartFit fit = fitChart(shot);
            if (!fit.ok) return fail("The patches could not be matched (are the corners on the chart?)");
            QJsonArray corners;
            for (const Point2& p : q.p) corners.append(QJsonArray{std::round(p.x * 1e4) / 1e4, std::round(p.y * 1e4) / 1e4});
            QJsonObject out{{"corners", corners}, {"delta_e_before", std::round(fit.deltaEBefore * 100) / 100},
                            {"delta_e_after", std::round(fit.deltaEAfter * 100) / 100}, {"delta_e_worst", std::round(fit.deltaEMax * 100) / 100}};
            if (a.value("measure_only").toBool()) return ok(QStringLiteral("Chart measured: delta E %1 now, %2 once matched").arg(fit.deltaEBefore, 0, 'f', 1).arg(fit.deltaEAfter, 0, 'f', 1), out);
            const std::vector<Id> clips = a.value("same_media").toBool() ? clipsOfSameMedia(s, clip.id) : std::vector<Id>{clip.id};
            const int n = applyChartMatchToClips(l.project, s, clips, fit);
            save(l);
            out["clips"] = n;
            return ok(QStringLiteral("Matched the chart on %1 clip(s): delta E %2 before, %3 after").arg(n).arg(fit.deltaEBefore, 0, 'f', 1).arg(fit.deltaEAfter, 0, 'f', 1), out);
        });

    add("montage_correct_lens", "Correct a lens",
        "Lens-profile correction (Premiere's and Resolve's lens correction, Lightroom's profiles): puts a Lens Correction "
        "first in a video clip's effects from lensfun's profile of the lens it was shot with, undoing its distortion, "
        "colour fringes (lateral chromatic aberration) and vignetting and, for a fisheye (GoPro, DJI...), making the "
        "picture rectilinear, scaled to fill the frame. `camera` defaults to the one the clip's media names (its Camera "
        "Model); `lens` may be left out when that camera has a fixed lens. Names match by their words (\"HERO11 Black\", "
        "\"FE 24-70mm f/2.8 GM\"). `focal` (mm) and `aperture` (f-number) default to the lens's shortest and widest; "
        "vignetting depends on the aperture (0: not known, no vignetting correction). `same_media` corrects every clip of the media. `list` changes nothing and "
        "returns the camera found and the lenses that fit it (`query` narrows them by words). The profile is kept in the "
        "project. `params` sets its other settings: distortion, tca, vignetting and rectilinear (0 off, 1 on), fit (0 fill "
        "the frame, 1 show the whole picture, 2 neither), scale (%), recording_crop (how much narrower than the sensor's "
        "width the recording is), distance (focus, m). Needs the lens database: downloaded by the app (Clip > Correct "
        "Lens...), scripts/fetch-models.sh, MONTAGE_LENS_DB, or lensfun's installed one.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":["number","string"],"description":"A video clip's id or name"},
            "camera":{"type":"string"},"lens":{"type":"string"},"focal":{"type":"number"},"aperture":{"type":"number"},
            "params":{"type":"object","additionalProperties":{"type":"number"}},
            "same_media":{"type":"boolean","default":false},"list":{"type":"boolean","default":false},"query":{"type":"string"}},
            "required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Clip clip = videoClipArg(l, a);
            const MediaItem* media = clip.mediaId ? l.project.findMedia(clip.mediaId) : nullptr;
            if (!media) throw ArgError{"\"clip\" must show media (not a generator or a nested sequence)"};
            const std::shared_ptr<const lens::Database> db = lensDatabase();
            if (!db) return fail("No lens database: correct a lens once in the app (Clip > Correct Lens...), run scripts/fetch-models.sh, or set MONTAGE_LENS_DB to lensfun's database folder");
            for (const char* k : {"camera", "lens", "query"})
                if (a.contains(k) && !a.value(k).isString()) throw ArgError{QStringLiteral("\"%1\" must be a string").arg(k)};
            for (const char* k : {"focal", "aperture"})
                if (a.contains(k) && !a.value(k).isDouble()) throw ArgError{QStringLiteral("\"%1\" must be a number").arg(k)};
            LensChoice c = suggestLens(*db, *media);
            if (a.contains("camera")) {
                const std::string want = a.value("camera").toString().toStdString();
                c.camera = want.empty() ? nullptr : db->camera(want);
                if (!want.empty() && !c.camera) throw ArgError{QStringLiteral("No camera \"%1\" in the lens database").arg(QString::fromStdString(want))};
                // Its own lens when it has a fixed one.
                c.lens = db->fixedLens(c.camera);
                c.focal = c.lens ? c.lens->minFocal : 0;
                c.aperture = -1;
            }
            auto cameraJson = [&]() -> QJsonValue {
                if (!c.camera) return QJsonValue::Null;
                return QJsonObject{{"name", QString::fromStdString(lens::cameraName(*c.camera))}, {"mount", QString::fromStdString(c.camera->mount)},
                                   {"crop_factor", c.camera->cropFactor}};
            };
            if (a.value("list").toBool()) {
                const QStringList words = a.value("query").toString().split(' ', Qt::SkipEmptyParts);
                QJsonArray lenses;
                for (const lens::Lens* x : db->lensesFor(c.camera)) {
                    const QString name = QString::fromStdString(lens::lensName(*x));
                    if (!std::all_of(words.begin(), words.end(), [&](const QString& w) { return name.contains(w, Qt::CaseInsensitive); })) continue;
                    if (lenses.size() >= 200) break;
                    lenses.append(QJsonObject{{"name", name}, {"type", QString::fromStdString(lens::projectionName(x->type))},
                                              {"focal_min", x->minFocal}, {"focal_max", x->maxFocal}, {"crop_factor", x->cropFactor}});
                }
                return ok(QStringLiteral("%1 lens(es) fit %2").arg(lenses.size()).arg(c.camera ? QString::fromStdString(lens::cameraName(*c.camera)) : QStringLiteral("any camera")),
                          QJsonObject{{"camera", cameraJson()}, {"lenses", lenses}});
            }
            if (a.contains("lens")) {
                const std::string want = a.value("lens").toString().toStdString();
                c.lens = db->lens(want, c.camera);
                if (!c.lens) throw ArgError{QStringLiteral("No lens \"%1\"%2 in the lens database (list them with \"list\": true)")
                                                .arg(QString::fromStdString(want), c.camera ? " for the " + QString::fromStdString(lens::cameraName(*c.camera)) : QString())};
                c.focal = 0;
                c.aperture = -1;
            }
            if (!c.lens) return fail(c.camera ? QStringLiteral("Which lens was on the %1? Give \"lens\" (\"list\": true lists them)").arg(QString::fromStdString(lens::cameraName(*c.camera)))
                                              : QStringLiteral("The clip's camera is not known: give \"camera\" and \"lens\""));
            if (a.contains("focal")) c.focal = a.value("focal").toDouble();
            if (a.contains("aperture")) c.aperture = a.value("aperture").toDouble();
            if (c.focal < 0 || c.focal > 2000) throw ArgError{"\"focal\" must be from 0 to 2000 (mm)"};
            if (a.contains("aperture") && (c.aperture < 0 || c.aperture > 64))
                throw ArgError{"\"aperture\" must be from 0 (not known: no vignetting correction) to 64"};
            const QJsonObject params = a.value("params").toObject();
            const EffectInfo* info = findEffectInfo("lens_correction");
            for (auto it = params.begin(); it != params.end(); ++it) {
                const std::string name = it.key().toStdString();
                const auto pi = std::find_if(info->params.begin(), info->params.end(), [&](const ParamInfo& x) { return x.name == name; });
                if (pi == info->params.end() || name == "focal" || name == "aperture" || name == "camera_crop")
                    throw ArgError{QStringLiteral("\"params\" has no setting \"%1\" (focal and aperture are their own arguments)").arg(it.key())};
                if (!it.value().isDouble() || it.value().toDouble() < pi->min || it.value().toDouble() > pi->max)
                    throw ArgError{QStringLiteral("\"%1\" must be a number from %2 to %3").arg(it.key()).arg(pi->min).arg(pi->max)};
                if ((pi->kind == ParamKind::Bool || pi->kind == ParamKind::Choice) && it.value().toDouble() != std::floor(it.value().toDouble()))
                    throw ArgError{QStringLiteral("\"%1\" must be a whole number").arg(it.key())};
            }
            const std::vector<Id> clips = a.value("same_media").toBool() ? clipsOfSameMedia(s, clip.id) : std::vector<Id>{clip.id};
            const int n = applyLensCorrection(l.project, s, clips, c);
            for (Id id : clips)
                if (Clip* k = edit::clipById(s, id))
                    for (Effect& x : k->effects)
                        if (x.type == "lens_correction")
                            for (auto it = params.begin(); it != params.end(); ++it) x.params[it.key().toStdString()] = Param(it.value().toDouble());
            save(l);
            const Effect* e = nullptr;
            if (const Clip* now = edit::clipById(s, clip.id))
                for (const Effect& x : now->effects)
                    if (x.type == "lens_correction") e = &x;
            QJsonObject out{{"camera", cameraJson()}, {"lens", QString::fromStdString(lens::lensName(*c.lens))},
                            {"type", QString::fromStdString(lens::projectionName(c.lens->type))}, {"clips", n}};
            if (e) {
                out["effect"] = double(e->id);
                out["focal"] = e->p("focal", 0);
                out["aperture"] = e->p("aperture", 0);
            }
            return ok(QStringLiteral("Corrected the %1 on %2 clip(s)").arg(QString::fromStdString(lens::lensName(*c.lens))).arg(n), out);
        });

    add("montage_calibrate_lens", "Calibrate a lens",
        "Lens calibration: makes a lens profile for the lens a video clip was shot with and puts it in a Lens Correction "
        "first in the clip's effects. From a checkerboard: `board` is its inner corners across and down ([9, 6] for a "
        "board of 10 x 7 squares); the whole board is looked for in up to 40 frames of the clip (`from` and `to`, "
        "timeline times, narrow them), best seen at many angles and reaching the frame's corners, and the focal length, "
        "the lens's centre, its distortion and (`anamorphic`) a squeeze are fitted so the corners land where they were "
        "seen (`squeeze` holds one known, or starts the fit). Or from `lines` that should be straight (an edge of a building, a horizon: each three points or more "
        "along it, fractions of the frame at `at`), snapped to the picture's edges there unless `snap` is false, and "
        "straightened (the focal length is not known from lines; `focal` names it). `model` is lensfun's poly3, poly5 "
        "(default) or ptlens. `maker`, `name` and `focal` (mm) name the lens; it is saved to the user's lens folder, so "
        "Lens Correction offers it for clips from the same camera. `same_media` corrects every clip of the media; "
        "`fit_only` changes nothing. Returns the fit: rms and worst (pixels: the corners' reprojection, or how far the "
        "lines are from straight), the terms (in halves of the frame's shorter side), the centre (lensfun's units), the squeeze, the focal length found and the "
        "profile's file (or why it was not saved: the clip is corrected anyway).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":["number","string"],"description":"A video clip's id or name"},
            "board":{"type":"array","items":{"type":"integer"},"minItems":2,"maxItems":2},
            "from":{"type":["number","string"]},"to":{"type":["number","string"]},
            "lines":{"type":"array","items":{"type":"array","items":{"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2}}},
            "at":{"type":["number","string"],"description":"Timeline time of the frame the lines are on"},"snap":{"type":"boolean","default":true},
            "model":{"type":"string","enum":["poly3","poly5","ptlens"],"default":"poly5"},
            "anamorphic":{"type":"boolean","default":false},"squeeze":{"type":"number","default":1},"fit_center":{"type":"boolean","description":"Default: true for a board, false for lines"},
            "maker":{"type":"string"},"name":{"type":"string"},"focal":{"type":"number"},
            "same_media":{"type":"boolean","default":false},"fit_only":{"type":"boolean","default":false}},
            "required":["project","clip"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Clip clip = videoClipArg(l, a);
            const MediaItem* media = clip.mediaId ? l.project.findMedia(clip.mediaId) : nullptr;
            if (!media || !media->hasVideo) throw ArgError{"\"clip\" must show footage (not a generator or a nested sequence)"};
            const bool board = a.contains("board"), lines = a.contains("lines");
            if (board == lines) throw ArgError{"Give \"board\" (a checkerboard's inner corners) or \"lines\" (that should be straight)"};
            LensFitOptions o;
            const QString model = str(a, "model", "poly5");
            if (model != "poly3" && model != "poly5" && model != "ptlens") throw ArgError{"\"model\" is poly3, poly5 or ptlens"};
            o.model = model == "poly3" ? lens::Distortion::Poly3 : model == "ptlens" ? lens::Distortion::PtLens : lens::Distortion::Poly5;
            o.fitSqueeze = a.value("anamorphic").toBool();
            o.squeeze = a.value("squeeze").toDouble(1);
            if (!(o.squeeze >= 0.25 && o.squeeze <= 4)) throw ArgError{"\"squeeze\" must be from 0.25 to 4"};
            o.fitCenter = a.value("fit_center").toBool(board);
            if (a.contains("focal") && !(a.value("focal").toDouble() > 0 && a.value("focal").toDouble() <= 2000))
                throw ArgError{"\"focal\" must be from 0 to 2000 (mm)"};
            const double seqFps = s.fpsValue();
            auto mediaSeconds = [&](FrameTime t) { return clip.sourceAt(double(std::clamp<FrameTime>(t, clip.start, clip.start + clip.duration - 1) - clip.start)) / seqFps; };
            LensFit fit;
            int width = 0, height = 0;
            std::string err;
            QJsonObject out;
            if (board) {
                const QJsonArray b = a.value("board").toArray();
                const int cols = b.size() == 2 ? b[0].toInt() : 0, rows = b.size() == 2 ? b[1].toInt() : 0;
                if (cols < 2 || rows < 2 || cols > 60 || rows > 60) throw ArgError{"\"board\" is the inner corners across and down, 2 to 60 each"};
                const FrameTime from = a.contains("from") ? timeArg(a.value("from"), s, "from") : clip.start;
                const FrameTime to = a.contains("to") ? timeArg(a.value("to"), s, "to") : clip.start + clip.duration;
                if (to <= from) throw ArgError{"\"to\" must be after \"from\""};
                ClipCalibration cc;
                const double t0 = mediaSeconds(from), t1 = mediaSeconds(to - 1) + 1 / seqFps;
                if (!calibrateClip(media->path, std::min(t0, t1), std::max(t0, t1), cols, rows, o, cc, &err,
                                   [this](double f) { progress(f, "Calibrating"); }))
                    return fail(QString::fromStdString(err));
                fit = cc.fit;
                width = cc.width, height = cc.height;
                out["frames_read"] = cc.framesRead;
                out["boards_found"] = cc.boardsFound;
            } else {
                const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : clip.start + clip.duration / 2;
                VideoDecoder dec;
                if (!dec.open(media->path, &err)) return fail(QString::fromStdString(err));
                width = dec.displayWidth(), height = dec.displayHeight();
                const Frame16Ptr frame = dec.frameAt(mediaSeconds(at), width, height, true);
                if (!frame) return fail("The clip's frame could not be read");
                const GrayImage gray = toGray(*frame);
                const bool snap = a.value("snap").toBool(true);
                std::vector<std::vector<Point2>> drawn;
                for (const QJsonValue& lv : a.value("lines").toArray()) {
                    std::vector<Point2> pts;
                    for (const QJsonValue& pv : lv.toArray()) {
                        const QJsonArray q = pv.toArray();
                        const double x = q.size() == 2 ? q[0].toDouble(-1) : -1, y = q.size() == 2 ? q[1].toDouble(-1) : -1;
                        if (!(x >= 0 && x <= 1 && y >= 0 && y <= 1)) throw ArgError{"\"lines\" are points [x, y], fractions of the frame"};
                        pts.push_back({x * width, y * height});
                    }
                    if (pts.size() < (snap ? 2u : 3u)) throw ArgError{snap ? "Each line needs two points or more" : "Each line needs three points or more"};
                    drawn.push_back(snap ? snapLineToEdge(gray, pts) : pts);
                }
                if (!calibrateFromLines(drawn, width, height, o, fit, &err)) return fail(QString::fromStdString(err));
            }
            // Named, saved and put on the clip(s).
            const std::shared_ptr<const lens::Database> db = lensDatabase();
            const lens::Camera* camera = db ? suggestLens(*db, *media).camera : nullptr;
            LensIdentity id;
            const auto device = media->metadata.find("device");
            id.maker = a.contains("maker") ? str(a, "maker").toStdString()
                     : camera                ? camera->maker
                                             : std::string("Calibrated");
            id.focalMm = a.value("focal").toDouble(0);
            const double real = fitFocalMm(fit, width, height, camera ? camera->cropFactor : 0);
            const double named = id.focalMm > 0 ? id.focalMm : real;
            id.model = a.contains("name") ? str(a, "name").toStdString()
                                          : (device != media->metadata.end() ? device->second + " " : std::string()) +
                                                (named > 0 ? QString::number(named, 'f', 0).toStdString() + "mm " : std::string()) + "(calibrated)";
            if (id.model == "(calibrated)") id.model = "Lens (calibrated)";
            id.cropFactor = camera ? camera->cropFactor : 0;
            id.mount = camera ? camera->mount : std::string();
            const lens::Lens made = calibratedLens(fit, width, height, id);
            QJsonArray terms;
            for (int k = 0; k < (fit.model == lens::Distortion::Poly3 ? 1 : fit.model == lens::Distortion::Poly5 ? 2 : 3); ++k) terms.append(fit.terms[k]);
            out["lens"] = QString::fromStdString(lens::lensName(made));
            out["model"] = model;
            out["terms"] = terms;
            out["center"] = QJsonArray{fit.centerX, fit.centerY};
            out["squeeze"] = fit.squeeze;
            out["rms"] = fit.rms;
            out["worst"] = fit.worst;
            out["views"] = fit.views;
            out["points"] = fit.points;
            out["width"] = width;
            out["height"] = height;
            if (real > 0) out["focal_found"] = std::round(real * 100) / 100;
            const QString summary = board ? QStringLiteral("Fitted from %1 frames of the board: corners within %2 px (rms %3)")
                                                .arg(fit.views).arg(fit.worst, 0, 'f', 3).arg(fit.rms, 0, 'f', 3)
                                          : QStringLiteral("Fitted from %1 lines: straight within %2 px (rms %3)").arg(fit.views).arg(fit.worst, 0, 'f', 3).arg(fit.rms, 0, 'f', 3);
            if (a.value("fit_only").toBool()) return ok(summary, out);
            std::string file;
            // (Put on the clip even when it cannot be kept: the effect holds the profile itself.)
            const bool saved = saveUserLens(made, &file, &err);
            if (saved) out["file"] = QString::fromStdString(file);
            else out["not_saved"] = QString::fromStdString(err);
            LensChoice c;
            c.lens = &made;
            c.camera = camera;
            c.focal = made.minFocal;
            c.aperture = 0;
            const std::vector<Id> clips = a.value("same_media").toBool() ? clipsOfSameMedia(s, clip.id) : std::vector<Id>{clip.id};
            const int n = applyLensCorrection(l.project, s, clips, c);
            save(l);
            out["clips"] = n;
            if (const Clip* now = edit::clipById(s, clip.id))
                for (const Effect& x : now->effects)
                    if (x.type == "lens_correction") out["effect"] = double(x.id);
            return ok(summary + (saved ? QStringLiteral("\nSaved %1 and corrected %2 clip(s)")
                                       : QStringLiteral("\nCorrected %2 clip(s) with %1, not saved: ") + QString::fromStdString(err))
                                      .arg(QString::fromStdString(lens::lensName(made))).arg(n), out);
        });

    add("montage_object_mask", "Pick objects for a mask",
        "AI object masks (Premiere's Object Mask, Resolve's Magic Mask, Final Cut's Magnetic Mask), with several objects in "
        "one mask: limits a clip's video effect (`effect`, its id) to objects picked by clicks. `object` is the object's "
        "index (0, the first, by default), \"new\" for another object joining the mask or \"new_cut\" for one cut out of "
        "it (once the mask has a first). `points` are clicks on the frame at `at` (fractions of the clip's frame, [x, y, label]: label 1 the object, 0 not the object, 2 and 3 a "
        "box's top left and bottom right); that frame is segmented at once. `track` follows the object forward, backward "
        "or both from there to the clip's ends (without `at`, from the first frame it was clicked on). `subtract` cuts an object (not the first) out of the mask or adds it back; "
        "`name` names it; `remove` drops it (on its own). Edges: mask.refine (0-100) snaps the mask's edge to the picture's, "
        "mask.consolidate (px) closes holes and gaps (montage_add_effect params). Returns each object's clicked and "
        "segmented frames.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "effect":{"type":"number","description":"The effect's id on the clip"},
            "object":{"type":["integer","string"],"description":"An index (0 the first), \"new\" or \"new_cut\""},
            "at":{"type":["number","string"],"description":"Timeline time of the frame clicked (the clip's start by default)"},
            "points":{"type":"array","items":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3}},
            "track":{"type":"string","enum":["forward","backward","both"]},
            "subtract":{"type":"boolean"},"name":{"type":"string"},"remove":{"type":"boolean","default":false}},
            "required":["project","clip","effect"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Id clipId = clipArg(l, a).id;
            const auto loc = edit::locate(s, clipId);
            if (!loc || loc->track.kind != TrackKind::Video) throw ArgError{"\"clip\" must be a video clip"};
            const Id effectId = Id(a.value("effect").toDouble());
            Effect* e = edit::ownedEffect(s, clipId, effectId);
            if (!e || !supportsMask(e->type)) throw ArgError{QStringLiteral("No video effect %1 on that clip (see montage_project_info)").arg(qint64(effectId))};
            Clip& c = *edit::clipById(s, clipId);
            const MediaItem* m = c.mediaId ? l.project.findMedia(c.mediaId) : nullptr;
            if (!m || m->kind != MediaKind::Video) throw ArgError{"Object masks follow objects in video clips"};
            e->params["mask.shape"] = Param(3.0);
            // Which object (the editor's choice of one is not part of the project: the first unless named).
            int which = 0;
            const QJsonValue ov = a.value("object");
            if (ov.isString()) {
                const QString kind = ov.toString();
                if (kind != "new" && kind != "new_cut") throw ArgError{"\"object\" is an index, \"new\" or \"new_cut\""};
                e->object = withNewObject(l.project, s, c, *e, kind == "new_cut", &which);
                if (!e->object) throw ArgError{"The mask has no object to cut out of yet: pick its first (\"object\": 0)"};
            } else if (ov.isDouble()) {
                which = int(ov.toDouble());
                const int n = e->object ? e->object->count() : 1;
                if (which < 0 || which >= n) throw ArgError{QStringLiteral("The mask has %1 object(s): \"object\" is 0 to %2").arg(n).arg(n - 1)};
            }
            if (a.value("remove").toBool()) {
                if (!e->object) throw ArgError{"The mask has no objects yet"};
                for (const char* k : {"points", "track", "name", "subtract", "at"})
                    if (a.contains(k)) throw ArgError{QStringLiteral("\"remove\" drops the object: give it without \"%1\"").arg(k)};
                e->object = withoutObject(*e, which);
                which = std::max(0, which - 1);
            } else {
                if (a.contains("subtract")) {
                    const bool cut = a.value("subtract").toBool();
                    if (cut && which == 0) throw ArgError{"The first object always adds: cut out another (\"object\": \"new_cut\")"};
                    if (e->object) e->object = withObjectSubtracted(*e, which, cut);
                }
                if (a.contains("name")) {
                    auto o = std::make_shared<ObjectMask>(e->object ? *e->object : ObjectMask{});
                    o->object(which).name = a.value("name").toString().toStdString();
                    e->object = o;
                }
                FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : c.start;
                if (!c.contains(at)) throw ArgError{"\"at\" must be inside the clip"};
                if (!a.contains("at") && !a.contains("points") && a.contains("track") && e->object) {
                    // Tracking without `at`: from the first frame of the clip the object was clicked on.
                    const auto& clicked = e->object->object(which).prompts;
                    for (FrameTime f = 0; f < c.duration; ++f)
                        if (auto it = clicked.find(clipObjectFrame(l.project, s, c, f)); it != clicked.end() && !it->second.empty()) {
                            at = c.start + f;
                            break;
                        }
                }
                const FrameTime local = at - c.start;
                const bool needModel = a.contains("points") || a.contains("track");
                if (needModel && (!segmenterAvailable() || !objectModel().installed()))
                    return fail("Object masks need their model: run `scripts/fetch-models.sh` or pick an object once in the app");
                std::string err;
                if (a.contains("points")) {
                    std::vector<ObjectPoint> pts;
                    for (const QJsonValue& v : a.value("points").toArray()) {
                        const QJsonArray pt = v.toArray();
                        const double x = pt.at(0).toDouble(-1), y = pt.at(1).toDouble(-1);
                        const int label = pt.at(2).toInt(-1);
                        if (pt.size() != 3 || !(x >= 0 && x <= 1 && y >= 0 && y <= 1) || label < 0 || label > 3)
                            throw ArgError{"Each point is [x, y, label]: x and y fractions of the frame, label 0 to 3"};
                        pts.push_back({x, y, label});
                    }
                    e->object = withObjectPrompts(l.project, s, c, *e, local, pts, which);
                    if (!e->object) return fail("The clip has no media to pick from");
                    auto seg = segmentClipObjectFrame(l.project, s, c, *e->object, local, &err, which);
                    if (!seg) return fail(QString::fromStdString(err));
                    e->object = seg;
                }
                const QString dir = str(a, "track");
                if (!dir.isEmpty()) {
                    if (!e->object || e->object->object(which).prompts.empty()) throw ArgError{"Click the object first (\"points\")"};
                    for (bool forward : {true, false}) {
                        if ((forward && dir == "backward") || (!forward && dir == "forward")) continue;
                        auto tracked = trackClipObject(l.project, s, c, *e->object, local, forward,
                                                       [this, forward](double f) { progress(f, forward ? QStringLiteral("Tracking forward") : QStringLiteral("Tracking backward")); },
                                                       nullptr, &err, which);
                        if (!tracked) return fail(QString::fromStdString(err));
                        e->object = tracked;
                    }
                }
            }
            save(l);
            QJsonArray objects;
            if (e->object)
                for (int i = 0; i < e->object->count(); ++i) {
                    const ObjectMask& o = e->object->object(i);
                    objects.append(QJsonObject{{"index", i}, {"name", QString::fromStdString(o.name)}, {"subtract", i > 0 && o.subtract},
                                               {"clicked_frames", int(o.prompts.size())}, {"segmented_frames", int(o.frames.size())}});
                }
            return ok(QStringLiteral("The mask has %1 object(s)").arg(objects.size()), QJsonObject{{"objects", objects}, {"object", which}});
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
            "output":{"type":"string","description":"Also save the full-size frame here (PNG, JPEG, TIFF or AVIF by extension)"},
            "rendition":{"type":"string","description":"An HDR sequence's frame: \"\" (default) as SDR screens show it (its SDR trim target, else tone mapped to Rec.709), \"master\" (saved as a 16-bit PNG with cICP, mDCV and cLLI or a 10-bit AVIF), or a trim target's id"},
            "quality":{"type":"integer","minimum":0,"maximum":100,"default":90,"description":"JPEG and AVIF quality"},
            "stereo_view":{"type":"string","enum":["left","right","anaglyph","sbs","sbs_half","tb","tb_half","difference"],"default":"left","description":"A stereoscopic 3D sequence: which eye, both, or where they differ"}},"required":["project","at"]})json", true,
        [](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const FrameTime at = timeArg(a.value("at"), s, "at");
            const int w = std::clamp(a.value("width").toInt(640), 64, 1920);
            RenderOptions ro;
            const ColorSpace* space = nullptr;
            double peak = 0;
            std::string err;
            // The image returned is for looking at: as SDR screens show it, unless a target is asked for.
            const std::string rendition = str(a, "rendition").toStdString();
            if (!stillRendition(s, rendition == "master" ? "" : rendition, ro, space, peak, &err)) throw ArgError{QString::fromStdString(err)};
            if (space->hdr()) ro.displaySpace = "rec709";  // (an HDR target, as the viewer shows it)
            ro.scale = std::min(1.0, double(w) / std::max(1, s.width));
            ro.captions = true;
            if (a.contains("stereo_view") && !stereoViewFromName(str(a, "stereo_view").toStdString(), ro.stereoView))
                throw ArgError{"Unknown stereo_view"};
            Image img = renderProgramFrame(l.project, s, at, ro);
            flattenOver(img, 0, 0, 0);
            QImage q(img.width, img.height, QImage::Format_RGBA8888);
            toRgba8(img, q.bits(), size_t(q.bytesPerLine()));
            QByteArray png;
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            q.save(&buf, "PNG");
            QString saved;
            if (a.contains("output")) {
                if (s.stereo3d && ro.stereoView != StereoView::Left) {
                    // The view asked for, at full size.
                    RenderOptions full;
                    full.captions = true;
                    full.stereoView = ro.stereoView;
                    Image big = renderProgramFrame(l.project, s, at, full);
                    flattenOver(big, 0, 0, 0);
                    QImage q8(big.width, big.height, QImage::Format_RGBA8888);
                    toRgba8(big, q8.bits(), size_t(q8.bytesPerLine()));
                    if (!q8.save(absolute(str(a, "output")), "PNG")) return fail(QStringLiteral("Cannot write %1").arg(str(a, "output")));
                } else {
                    StillOptions so;
                    so.rendition = rendition;
                    so.quality = std::clamp(a.value("quality").toInt(90), 0, 100);
                    StillResult res;
                    if (!exportStill(l.project, s, at, absolute(str(a, "output")).toStdString(), &err, so, &res))
                        return fail(QString::fromStdString(err));
                    saved = QStringLiteral("; saved %1-bit %2").arg(res.bits).arg(QString::fromStdString(res.colorSpace));
                    if (res.maxCll > 0) saved += QStringLiteral(" (MaxCLL %1, MaxFALL %2 nits)").arg(res.maxCll).arg(res.maxFall);
                }
            }
            ToolResult r = ok(QStringLiteral("Frame at %1 (%2x%3)%4").arg(tc(at, s)).arg(img.width).arg(img.height).arg(saved));
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
        "self-contained page that plays it in any browser, offline, where reviewers step to a frame or mark a range, "
        "draw on the picture (pen, arrow, box, ellipse), type notes, reply and set a status, then save them as a notes "
        "file. montage_import_markers turns that file into markers coloured by reviewer, with their drawings (importing "
        "it again brings those markers up to date). `note` is a message shown to the reviewers; the sequence's markers "
        "are shown unless `markers` is false; `in`/`out` (timecodes) limit it to a range. A copy of the same cut sent "
        "to the same folder again is the next version (\"<name> - Review v2\"; `version` sets the number) and its page "
        "compares it with the version before, side by side or with a wipe, unless `compare` is false. Returns both "
        "paths and the version.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"folder":{"type":"string"},
            "max_height":{"type":"integer","default":1080},"timecode":{"type":"boolean","default":true},
            "watermark":{"type":"string"},"note":{"type":"string"},"markers":{"type":"boolean","default":true},
            "in":{"type":"string"},"out":{"type":"string"},"version":{"type":"integer","minimum":1,"description":"Default: the next after those in the folder"},
            "compare":{"type":"boolean","default":true}},"required":["project","folder"]})json",
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
            if (a.contains("version")) {
                o.version = a.value("version").toInt(0);
                if (o.version < 1 || o.version > 999) throw ArgError{"\"version\" is 1 to 999"};
            }
            o.compare = a.value("compare").toBool(true);
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
                                  {"width", pkg.page.width}, {"height", pkg.page.height}, {"frames", double(pkg.page.frames)},
                                  {"version", pkg.page.version},
                                  {"compares_with", pkg.page.previous ? QJsonValue(pkg.page.previous->version) : QJsonValue()}});
        });

    add("montage_export_dcp", "Export a DCP",
        "Make a Digital Cinema Package of the active sequence for cinemas and festivals: SMPTE (Bv2.1), 2K or 4K JPEG 2000 "
        "pictures in DCI X'Y'Z' in the Flat, Scope or full container (the frame fitted on black), both eyes of a "
        "stereoscopic sequence with `stereo` (2K), 24-bit 48 kHz sound in "
        "5.1 (7.1 DS from a 7.1 sequence; a stereo mix on left and right), a composition playlist, packing list with "
        "hashes and asset map, in a folder under `folder` named by the Digital Cinema Naming Convention. 23.976 plays at "
        "24. `subtitles` names a caption track to send as SMPTE subtitles (or closed captions, or burned in). `encrypt` "
        "encrypts the track files and keeps their keys in a DKDM, from which montage_make_kdm makes KDMs for cinemas. "
        "The package is checked when done (as montage_verify_dcp); returns its name, folder, anything the check found and "
        "what Bv2.1 advises.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"folder":{"type":"string","description":"Where to make the DCP folder"},
            "title":{"type":"string"},"kind":{"type":"string","enum":["feature","short","trailer","teaser","advertisement","test","rating","psa"],"default":"feature"},
            "container":{"type":"string","enum":["flat","scope","full"],"description":"Default: Scope for sequences 2:1 or wider, else Flat"},
            "fps":{"type":"integer","enum":[24,25,30,48],"description":"Default: the sequence's, to the nearest"},
            "language":{"type":"string","default":"en"},"territory":{"type":"string","default":"XX"},
            "issuer":{"type":"string"},"studio":{"type":"string"},"facility":{"type":"string"},
            "in_out":{"type":"boolean","default":false,"description":"Only In to Out"},
            "resolution":{"type":"string","enum":["2k","4k"],"default":"2k"},
            "stereo":{"type":"boolean","default":false,"description":"Stereoscopic 3D: both eyes (the sequence must be stereoscopic; 2K)"},
            "subtitles":{"type":"string","description":"A caption track, by name or language, to go with it"},
            "subtitles_as":{"type":"string","enum":["subtitles","closed_captions","burn_in"],"default":"subtitles"},
            "subtitle_language":{"type":"string","description":"RFC 5646; default: the caption track's"},
            "encrypt":{"type":"boolean","default":false,"description":"Encrypted track files; cinemas need KDMs (montage_make_kdm)"}},"required":["project","folder"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            const Sequence& s = l.seq();
            if (s.duration() == 0) return fail("The sequence is empty");
            DcpSettings st;
            st.resolution = str(a, "resolution", "2k").toStdString();
            st.stereo = a.value("stereo").toBool();
            st.encrypt = a.value("encrypt").toBool();
            const QString as = str(a, "subtitles_as", "subtitles");
            if (as != QLatin1String("subtitles") && as != QLatin1String("closed_captions") && as != QLatin1String("burn_in"))
                throw ArgError{"subtitles_as is subtitles, closed_captions or burn_in"};
            if (str(a, "subtitles").isEmpty() && (a.contains("subtitles_as") || a.contains("subtitle_language")))
                throw ArgError{"subtitles_as and subtitle_language need subtitles (a caption track)"};
            if (const QString want = str(a, "subtitles"); !want.isEmpty()) {
                for (const CaptionTrack& t : s.captionTracks)
                    if (QString::fromStdString(t.name).compare(want, Qt::CaseInsensitive) == 0) st.subtitles = t.id;
                for (const CaptionTrack& t : s.captionTracks)
                    if (!st.subtitles && QString::fromStdString(t.language).compare(want, Qt::CaseInsensitive) == 0) st.subtitles = t.id;
                if (!st.subtitles) throw ArgError{"subtitles names no caption track of the sequence"};
                st.closedCaptions = as == QLatin1String("closed_captions");
                st.burnIn = as == QLatin1String("burn_in");
                st.subtitleLanguage = str(a, "subtitle_language").toStdString();
            }
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
            QJsonArray found, advised;
            std::vector<std::string> advice;
            for (const std::string& i : verifyDcp(r.folder, {}, &advice)) found.append(QString::fromStdString(i));
            for (const std::string& i : advice) advised.append(QString::fromStdString(i));
            const QJsonObject out{{"name", QString::fromStdString(r.name)}, {"folder", QString::fromStdString(r.folder)},
                                  {"cpl", QString::fromStdString(r.cpl)}, {"cpl_id", QString::fromStdString(r.cplId)},
                                  {"frames", double(r.frames)}, {"fps", r.fps},
                                  {"width", r.width}, {"height", r.height}, {"channels", r.channels},
                                  {"dci_profile", r.cinemaProfile}, {"stereo", r.stereo}, {"encrypted", r.encrypted},
                                  {"signed", r.signedPackage}, {"subtitles", r.subtitleCount},
                                  {"dkdm", r.dkdm.empty() ? QJsonValue() : QJsonValue(QString::fromStdString(r.dkdm))},
                                  {"problems", found}, {"advice", advised}};
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

    add("montage_make_kdm", "Make KDMs for an encrypted DCP",
        "Make Key Delivery Messages (SMPTE ST 430-1) so cinema servers can play an encrypted DCP: one for each server "
        "certificate (the files the cinema sends; a chain file's leaf is used), its keys valid from `from` to `until`, made "
        "from the DKDM montage_export_dcp kept, signed by Montage's certificate chain. The window must sit inside the "
        "DKDM's and the server certificate's. Returns the KDM files.",
        R"json({"type":"object","properties":{"dkdm":{"type":"string","description":"The DCP's DKDM (montage_export_dcp returns it)"},
            "servers":{"type":"array","items":{"type":"string"},"description":"The servers' certificate files (PEM)"},
            "from":{"type":"string","description":"ISO 8601 date and time; default now"},
            "until":{"type":"string","description":"ISO 8601 date and time; default a week after from"},
            "folder":{"type":"string","description":"Where to write the KDMs"},
            "annotation":{"type":"string"}},"required":["dkdm","servers","folder"]})json",
        false, [](const QJsonObject& a) {
            KdmRequest r;
            r.dkdm = absolute(need(a, "dkdm")).toStdString();
            for (const QJsonValue& v : a.value("servers").toArray()) r.recipients.push_back(absolute(v.toString()).toStdString());
            if (r.recipients.empty()) throw ArgError{"servers names no certificate"};
            auto when = [&](const char* key, const QDateTime& fallback) {
                const QString text = str(a, key);
                if (text.isEmpty()) return fallback;
                const QDateTime t = QDateTime::fromString(text, Qt::ISODate);
                if (!t.isValid()) throw ArgError{QStringLiteral("%1 is not an ISO 8601 date and time").arg(key)};
                return t;
            };
            const QDateTime from = when("from", QDateTime::currentDateTime());
            const QDateTime until = when("until", from.addDays(7));
            r.notBefore = from.toSecsSinceEpoch();
            r.notAfter = until.toSecsSinceEpoch();
            r.offsetMinutes = from.offsetFromUtc() / 60;
            r.annotation = str(a, "annotation").toStdString();
            const QString folder = absolute(need(a, "folder"));
            QDir().mkpath(folder);
            r.folder = folder.toStdString();
            std::vector<std::string> files;
            std::string err;
            if (!makeKdms(r, &files, &err)) return fail(QString::fromStdString(err));
            QJsonArray made;
            for (const std::string& f : files) made.append(QString::fromStdString(f));
            return ok(QStringLiteral("Made %1 KDM(s) in %2").arg(files.size()).arg(folder), QJsonObject{{"kdms", made}});
        });

    add("montage_export_imf", "Export an IMF master",
        "Make an IMF (Interoperable Master Format) package of the active sequence, as Netflix, Amazon, Disney+ and "
        "broadcasters ask for: Application #2E (SMPTE ST 2067-21:2021), JPEG 2000 pictures in the IMF profiles (lossless "
        "by default) as full-range RGB 4:4:4 of 10 or 12 bits in Rec.709, P3-D65 PQ, Rec.2020 PQ or Rec.2020 HLG (HDR "
        "with mastering display metadata), 24-bit 48 kHz sound in stereo, 5.1 or 7.1 DS with multichannel labels, a "
        "composition playlist with the essence descriptors, packing list with hashes and asset map, in a new folder under "
        "`folder`. The package is checked when done (as montage_verify_imf). `htj2k` codes the pictures as High-Throughput "
        "JPEG 2000 instead (ST 2067-21:2023, lossless), far faster to code and decode. Versions: `sounds` makes a sound "
        "track per language and kind (each mixed from the roles it hears), the caption tracks become IMSC 1.1 subtitles, "
        "captions or forced narratives (`texts`), and `supplement_of` (an earlier package's folder) makes a supplemental "
        "package carrying only what changed since it: the composition plays the earlier package's track files where the "
        "pictures and sound are the same. Give `original_sequence`, the sequence that package was mastered from (kept as "
        "it was), when the cut has changed.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"folder":{"type":"string","description":"Where to make the IMF folder"},
            "title":{"type":"string"},"kind":{"type":"string","enum":["feature","episode","short","trailer","teaser","advertisement","promotion","test"],"default":"feature"},
            "colour":{"type":"string","enum":["rec709","p3d65-pq","rec2020-pq","rec2020-hlg"],"description":"Default: as the sequence is graded"},
            "size":{"type":"string","enum":["sequence","hd","uhd","4k"],"default":"sequence"},
            "bits":{"type":"integer","enum":[10,12],"description":"Default: 10 for SDR, 12 for HDR"},
            "lossless":{"type":"boolean","default":true},"htj2k":{"type":"boolean","default":false},"megabits_per_second":{"type":"number","minimum":20,"maximum":6400,"description":"Lossy: the cap (default 400)"},
            "mastering_peak":{"type":"number","minimum":100,"maximum":10000,"description":"HDR: the mastering display's peak, cd/m^2"},
            "language":{"type":"string","default":"en"},"issuer":{"type":"string"},
            "in_out":{"type":"boolean","default":false,"description":"Only In to Out"},
            "sounds":{"type":"array","description":"Sound tracks (default: the mix as the programme in `language`)","items":{"type":"object","properties":{
                "language":{"type":"string","description":"RFC 5646, e.g. de"},"kind":{"type":"string","enum":["PRM","ME","DV"],"default":"PRM","description":"Programme, music and effects, or described"},
                "roles":{"type":"array","items":{"type":"string"},"description":"The roles heard (default: the mix as it plays)"}},"required":["language"]}},
            "captions":{"type":"boolean","default":true,"description":"Caption tracks as IMSC 1.1 timed text"},
            "texts":{"type":"array","description":"Which caption tracks, and as what (default: all as subtitles)","items":{"type":"object","properties":{
                "track":{"type":"string","description":"The caption track's name"},"kind":{"type":"string","enum":["subtitles","captions","forced"],"default":"subtitles"}},"required":["track"]}},
            "supplement_of":{"type":"string","description":"An earlier IMF package's folder: make a supplemental package of it (beside it: `folder` must be the folder holding it)"},
            "original_sequence":{"type":"string","description":"Supplemental: the sequence the earlier package was mastered from (default: the picture has not changed)"}},
            "required":["project","folder"]})json",
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
            st.htj2k = a.value("htj2k").toBool(false);
            st.megabitsPerSecond = a.value("megabits_per_second").toDouble(400);
            st.masteringPeak = a.value("mastering_peak").toDouble(0);
            st.language = str(a, "language", "en").toStdString();
            st.issuer = str(a, "issuer", "Montage").toStdString();
            st.inOut = a.value("in_out").toBool();
            for (const QJsonValue& v : a.value("sounds").toArray()) {
                const QJsonObject o = v.toObject();
                ImfSound snd;
                snd.language = str(o, "language", "en").toStdString();
                snd.kind = str(o, "kind", "PRM").toStdString();
                snd.roles = stringList(o, "roles");
                st.sounds.push_back(snd);
            }
            st.captions = a.value("captions").toBool(true);
            for (const QJsonValue& v : a.value("texts").toArray()) {
                const QJsonObject o = v.toObject();
                const QString name = need(o, "track");
                ImfText t;
                for (const CaptionTrack& c : s.captionTracks)
                    if (QString::fromStdString(c.name) == name) t.track = c.id;
                if (!t.track) throw ArgError{QStringLiteral("No caption track named \"%1\"").arg(name)};
                t.kind = str(o, "kind", "subtitles").toStdString();
                st.texts.push_back(t);
            }
            if (a.contains("supplement_of")) st.supplementOf = absolute(need(a, "supplement_of")).toStdString();
            if (a.contains("original_sequence")) {
                const QString name = need(a, "original_sequence");
                for (const Sequence& sq : l.project.sequences)
                    if (QString::fromStdString(sq.name) == name) st.originalSequence = sq.id;
                if (!st.originalSequence) throw ArgError{QStringLiteral("No sequence named \"%1\"").arg(name)};
            }
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
                                  {"colour", QString::fromStdString(r.colour)}, {"rsiz", r.rsiz}, {"sounds", r.sounds}, {"texts", r.texts},
                                  {"supplemental", r.supplemental}, {"coded_frames", double(r.codedFrames)},
                                  {"new_samples", double(r.newSamples)}, {"problems", found}};
            const QString name = QFileInfo(QString::fromStdString(r.folder)).fileName();
            return ok(found.isEmpty() ? QStringLiteral("Made %1 and checked it: no problems").arg(name)
                                      : QStringLiteral("Made %1; the check found %2 problem(s)").arg(name).arg(found.size()),
                      out);
        });

    add("montage_verify_imf", "Check an IMF package",
        "Check an IMF package folder: asset map, every file present at its size, packing list hashes, each composition's "
        "resources in the package with their essence descriptors and durations, the Application #2E identification, "
        "picture track files JPEG 2000 in an IMF profile with the right number of frames, sound 24-bit 48 kHz with the "
        "right number of samples, timed text that reads. A supplemental package's track files from earlier packages are "
        "looked for in the folders beside it. Returns the problems found (none: it passed).",
        R"json({"type":"object","properties":{"folder":{"type":"string"}},"required":["folder"]})json", true, [](const QJsonObject& a) {
            const QString folder = absolute(need(a, "folder"));
            QJsonArray found;
            for (const std::string& i : verifyImf(folder.toStdString())) found.append(QString::fromStdString(i));
            return ok(found.isEmpty() ? QStringLiteral("The IMF package checks out") : QStringLiteral("%1 problem(s): %2").arg(found.size()).arg(found.at(0).toString()),
                      QJsonObject{{"problems", found}, {"ok", found.isEmpty()}});
        });

    add("montage_read_broadcast_mxf", "Read a broadcast MXF file",
        "Read a broadcast MXF file's delivery metadata (with the EBU's bmx): the picture essence and whether it is "
        "interlaced, sound tracks, duration and start timecode, the AS-11 specification, core and UK DPP metadata and "
        "parts, and the captions in its ST 436 ancillary data (CEA-708 or 608, and OP-47 teletext subtitles on a page).",
        R"json({"type":"object","properties":{"file":{"type":"string"},"teletext_page":{"type":"integer","default":888}},"required":["file"]})json",
        true, [](const QJsonObject& a) {
            const QString file = absolute(need(a, "file"));
            BroadcastMxfInfo info;
            std::string why;
            if (!readBroadcastMxf(file.toStdString(), info, &why)) throw ArgError{QString::fromStdString(why)};
            auto map = [](const std::map<std::string, std::string>& m) {
                QJsonObject o;
                for (const auto& [k, v] : m) o.insert(QString::fromStdString(k), QString::fromStdString(v));
                return o;
            };
            QJsonArray parts;
            for (const MxfPart& p : info.parts)
                parts.append(QJsonObject{{"number", p.number}, {"total", p.total}, {"start", double(p.start)}, {"duration", double(p.duration)}});
            std::vector<Caption> cea, teletext;
            ancCaptions(info, a.value("teletext_page").toInt(888), cea, teletext);
            auto captions = [](const std::vector<Caption>& v) {
                QJsonArray out;
                for (const Caption& c : v)
                    out.append(QJsonObject{{"start", double(c.start)}, {"end", double(c.end)}, {"text", QString::fromStdString(c.text)}});
                return out;
            };
            QJsonObject out{{"picture", QString::fromStdString(info.pictureEssence)},
                            {"interlaced", info.separateFields},
                            {"sound_tracks", info.soundTracks},
                            {"duration", double(info.duration)},
                            {"edit_rate", QStringLiteral("%1/%2").arg(info.editRate.num).arg(info.editRate.den)},
                            {"start_timecode", QString::fromStdString(info.startTimecode)},
                            {"as11_spec", QString::fromStdString(info.as11Spec)},
                            {"core", map(info.core)},
                            {"dpp", map(info.dpp)},
                            {"parts", parts},
                            {"anc_frames", int(info.anc.size())},
                            {"cea708_captions", captions(cea)},
                            {"teletext_subtitles", captions(teletext)}};
            const QString shim = info.core.count("ShimName") ? QString::fromStdString(info.core.at("ShimName")) : QStringLiteral("no AS-11 metadata");
            return ok(QStringLiteral("%1, %2; %3 part(s), %4 caption(s), %5 subtitle(s)")
                          .arg(QString::fromStdString(info.pictureEssence), shim)
                          .arg(info.parts.size())
                          .arg(cea.size())
                          .arg(teletext.size()),
                      out);
        });

    add("montage_render", "Render",
        "Render the active sequence (or its in-out range) to a file with an export preset (default \"H.264 - High Quality\"), "
        "optionally normalising the mix's loudness for where it is going. Broadcast deliveries: the XDCAM HD422, AVC-Intra 100 "
        "and DNxHR MXF presets write MXF OP1a with each sound channel a mono track; start_timecode sets the file's timecode. "
        "rate_change delivers it at another frame rate: 2:3 pulldown, a PAL speed change or a standards conversion.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"output":{"type":"string"},
            "preset":{"type":"string"},"in":{"type":["number","string"]},"out":{"type":["number","string"]},
            "loudness_lufs":{"type":"number","description":"Normalise the mix to this loudness, e.g. -14 (streaming) or -23 (EBU R128)"},
            "peak_ceiling":{"type":"number","default":-1,"description":"True peak ceiling (dBTP) when normalising"},
            "loudness_dialogue":{"type":"boolean","default":false,"description":"loudness_lufs is the dialogue's (Netflix: -27, peak_ceiling -2), measured as montage_dialogue_loudness does"},
            "burn_in":{"type":"object","description":"Overlays for a review copy","properties":{
                "timecode":{"type":"boolean"},"clip_name":{"type":"boolean"},"text":{"type":"string"},
                "corner":{"type":"string","enum":["top_left","top_centre","top_right","bottom_left","bottom_centre","bottom_right"],"default":"top_left"},
                "watermark":{"type":"string","description":"Image file, e.g. a logo"},
                "watermark_corner":{"type":"string","enum":["top_left","top_centre","top_right","bottom_left","bottom_centre","bottom_right"],"default":"bottom_right"},
                "watermark_opacity":{"type":"number","default":0.6}}},
            "downmix_stereo":{"type":"boolean","default":false,"description":"A 5.1/7.1 sequence: fold the mix down to stereo"},
            "described":{"type":"boolean","default":false,"description":"Audio description: the mix without the descriptions, then a stream of the programme with them (montage_audio_description)"},
            "stems":{"type":"string","enum":["none","tracks","buses","roles"],"default":"none",
                "description":"Also write 24-bit WAV stems beside the output, one per audio track, per bus (plus Main) or per audio role"},
            "captions":{"type":"string","enum":["none","burn","embed","both"],"default":"none","description":"The visible caption track, burned into the picture and/or embedded as a subtitle stream"},
            "trim_target":{"type":"string","description":"A trim pass (montage_trim): deliver that target of the sequence (its space and peak, each shot's trim) instead of the master"},
            "hdr10plus":{"type":"boolean","default":false,"description":"PQ exports: dynamic HDR metadata (SMPTE ST 2094-40), scene by scene, in HEVC and AV1 video and as name.hdr10plus.json beside the file (measured first unless montage_measure_hdr's analysis still matches the cut)"},
            "cea608":{"type":"boolean","default":false,"description":"Also carry the caption track as closed captions inside H.264/HEVC video (A/53, as US broadcast and streaming deliveries ask): CEA-608 and CTA-708 service 1"},
            "cea708":{"type":"boolean","default":true,"description":"With cea608: the CTA-708 service beside the 608 bytes"},
            "all_captions":{"type":"boolean","default":false,"description":"With embed: every caption track as its own subtitle stream, language tagged"},
            "audio_streams":{"description":"More audio streams after the mix (a master's M&E, dialogue, dubs): \"roles\" (one per role), \"tracks\" (one per track), \"dme\" (the Dialogue role, then an M&E of Music and Effects; each left out when none of its roles is heard), or a list of {name, language, tracks:[\"A2\",...], role (several joined by +, as Music+Effects)}",
                "anyOf":[{"type":"string","enum":["mix","roles","tracks","dme"]},{"type":"array","items":{"type":"object","properties":{
                    "name":{"type":"string"},"language":{"type":"string"},"tracks":{"type":"array","items":{"type":"string"}},"role":{"type":"string"}}}}]},
            "audio_name":{"type":"string","description":"The mix stream's title"},"audio_language":{"type":"string","description":"The mix stream's language (ISO 639-1)"},
            "start_timecode":{"type":"string","description":"The file's starting timecode (MXF and MOV), e.g. 10:00:00:00 as broadcasters ask"},
            "mono_tracks":{"type":"integer","description":"Write the mix (then each audio stream) as mono tracks, padded with silence to this many, as broadcast MXF takes it; the MXF presets set 8, 4 or 2"},
            "interlaced":{"type":"boolean","default":false,"description":"Code the video as interlaced fields, top first (each picture as both fields: PsF), as 1080i deliveries ask: MPEG-2, AVC-Intra, ProRes"},
            "rate_change":{"type":"string","enum":["none","pulldown","speed","convert"],"default":"none","description":"Deliver at another frame rate: pulldown (a 23.976 or 24 sequence with 2:3 or 2:2:2:4 pulldown, into fields at 29.97 or 30, coded interlaced: MPEG-2, AVC-Intra or H.264, ProRes, FFV1; or as repeated frames at 59.94 or 60); speed (each frame shown once at frame_rate, the programme faster or slower, as PAL's 25 from 24 is 4 % shorter, its sound sped with it, captions and markers with it); convert (a standards conversion to frame_rate, keeping its length: each new frame made from the two nearest by optical flow, or RIFE where installed, never blended across a cut)"},
            "frame_rate":{"type":["number","string"],"description":"With rate_change: the rate delivered (23.976, 24, 25, 29.97, 30, 50, 59.94, 60...); pulldown's default is 29.97 (30 from 24)"},
            "pulldown_cadence":{"type":"string","enum":["2:3","2:2:2:4"],"default":"2:3","description":"With pulldown: 2:3 (A A, B B, B C, C D, D D) or 2:2:2:4 (no frame of mixed fields)"},
            "keep_pitch":{"type":"boolean","default":true,"description":"With speed: the sound keeps its pitch (time-stretched); false plays it faster and higher (varispeed), as a telecine's PAL speed-up"},
            "mxf_delivery":{"type":"string","enum":["op1a","rdd9","as11-dpp","as11-x2","as11-x8"],"default":"op1a","description":"Broadcast MXF by the book (rewrapped with the EBU's bmx): rdd9 = RDD 9 (the MPEG-2 HD 4:2:2 MXF preset); AS-11 with its descriptive metadata and parts: as11-dpp (UK DPP HD: the AVC-Intra 100 preset at 25 fps, coded 1080i25), as11-x2 (AVC-Intra HD), as11-x8 (MPEG-2 HD 4:2:2 in RDD 9, the NABA's)"},
            "as11":{"type":"object","additionalProperties":{"type":["string","number","boolean"]},"description":"AS-11 metadata by the names AS-11 gives them: core (SeriesTitle, ProgrammeTitle, EpisodeTitleNumber, ShimName, ShimVersion, AudioTrackLayout, PrimaryAudioLanguage, ClosedCaptionsPresent/Type/Language) and UK DPP (ProductionNumber, Synopsis, Originator, CopyrightYear, Distributor, PSEPass, SecondaryAudioLanguage, LineUpStart, IdentClockStart, CompletionDate, ContactEmail, ContactTelephoneNumber...). Unset ones come from the sequence: its name, languages, captions; line-up and clock from markers named Line-up and Clock, parts from markers with a duration named Part ..."},
            "anc_captions":{"type":"string","enum":["none","cea708","teletext","both"],"default":"none","description":"In a broadcast MXF delivery: the caption track as ancillary data (ST 436): CEA-708 caption packets (with 608) and/or OP-47 teletext subtitles"},
            "teletext_page":{"type":"integer","default":888,"description":"The teletext subtitle page (100 to 899)"},
            "credentials":{"type":"object","description":"Sign it with Content Credentials (C2PA, as Premiere does): a signed record in the file that Montage made it, each AI model that generated or changed what is in the range, and the credentials its sources carried. MP4, MOV, WAV, MP3, JPEG, PNG and TIFF",
                "properties":{"certificate":{"type":"string","description":"The signing certificate chain (PEM, the signing certificate first; issued by a certificate authority, for digital signatures with e-mail protection or document signing use)"},
                    "key":{"type":"string","description":"Its private key (PEM)"},
                    "allow_ai_training":{"type":"boolean","default":false,"description":"false: the credentials ask that it not be used to train AI models"}},"required":["certificate","key"]},
            "stereo_view":{"type":"string","enum":["sbs","sbs_half","tb","tb_half","left","right","anaglyph"],"default":"sbs","description":"A stereoscopic 3D sequence: the eyes side by side or top and bottom (full size, or squeezed into one frame), or one picture"},
            "exr_compression":{"type":"string","enum":["none","rle","zips","zip","piz","pxr24","b44","b44a","dwaa","dwab","zstd","htj2k256","htj2k32"],"description":"OpenEXR presets: the frames' compression (default zip); HTJ2K is High-Throughput JPEG 2000, lossless"},
            "farm":{"type":"string","description":"A render farm folder: send the render there for its render nodes (montage-cli node) instead of rendering now; returns the job's id. The output is where the nodes write (each maps paths to its own)"}},
            "required":["project","output"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const ExportPreset* pr = findExportPreset(str(a, "preset", "H.264 - High Quality").toStdString());
            if (!pr) throw ArgError{"Unknown preset (see montage_list_presets)"};
            ExportSettings st = pr->settings;
            st.path = absolute(need(a, "output")).toStdString();
            if (a.contains("rate_change") && str(a, "rate_change") != "none") {
                st.rateChange = str(a, "rate_change").toStdString();
                if (st.rateChange != "pulldown" && st.rateChange != "speed" && st.rateChange != "convert")
                    throw ArgError{"\"rate_change\" is none, pulldown, speed or convert"};
                const QJsonValue r = a.value("frame_rate");
                const double fr = r.isString() ? r.toString().toDouble() : r.toDouble();
                if (!r.isUndefined() && !(fr > 0)) throw ArgError{"\"frame_rate\" is frames a second, as 25 or 29.97"};
                if (fr > 0) st.rate = rateFor(fr);
                st.cadence = str(a, "pulldown_cadence", "2:3").toStdString();
                st.rateKeepPitch = a.value("keep_pitch").toBool(true);
            }
            if (a.contains("start_timecode")) {
                // (in the delivered rate's frames)
                RateConversion conv;
                std::string why;
                if (!planRateConversion(s, st, conv, &why)) throw ArgError{QString::fromStdString(why)};
                const Rational rate = conv.active() ? conv.to : s.fps;
                FrameTime t = 0;
                const std::string tc = str(a, "start_timecode").toStdString();
                if (!parseTimecode(tc, rate, t) || t < 0) throw ArgError{"\"start_timecode\" is a timecode like 10:00:00:00"};
                st.startTimecode = formatTimecode(t, rate);
            }
            if (a.contains("mono_tracks")) {
                st.monoAudioTracks = a.value("mono_tracks").toInt();
                if (st.monoAudioTracks < 0 || st.monoAudioTracks > 64) throw ArgError{"\"mono_tracks\" is 0 to 64"};
            }
            if (a.contains("in")) st.in = timeArg(a.value("in"), s, "in");
            if (a.contains("out")) st.out = timeArg(a.value("out"), s, "out");
            st.interlaced = a.value("interlaced").toBool(st.interlaced);
            if (a.contains("mxf_delivery")) {
                const std::string d = str(a, "mxf_delivery").toStdString();
                if (d != "op1a" && d != "rdd9" && d != "as11-dpp" && d != "as11-x2" && d != "as11-x8")
                    throw ArgError{"\"mxf_delivery\" is op1a, rdd9, as11-dpp, as11-x2 or as11-x8"};
                st.mxfDelivery = d == "op1a" ? std::string() : d;
            }
            if (a.value("as11").isObject()) {
                const QJsonObject m = a.value("as11").toObject();
                for (auto it = m.begin(); it != m.end(); ++it) {
                    const QJsonValue v = it.value();
                    const QString text = v.isBool() ? (v.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
                                         : v.isDouble() ? (v.toDouble() == std::floor(v.toDouble()) && std::fabs(v.toDouble()) < 9e15
                                                               ? QString::number(qint64(v.toDouble()))
                                                               : QString::number(v.toDouble(), 'g', 15))
                                                        : v.toString();
                    st.as11Metadata.push_back((it.key() + "=" + text).toStdString());
                }
            }
            if (a.contains("anc_captions")) {
                const QString c = str(a, "anc_captions");
                if (c != "none" && c != "cea708" && c != "teletext" && c != "both") throw ArgError{"\"anc_captions\" is none, cea708, teletext or both"};
                st.ancCaptions = c == "cea708" || c == "both";
                st.ancTeletext = c == "teletext" || c == "both";
            }
            if (a.contains("teletext_page")) {
                st.teletextPage = a.value("teletext_page").toInt();
                if (st.teletextPage < 100 || st.teletextPage > 899) throw ArgError{"\"teletext_page\" is 100 to 899"};
            }
            if ((st.ancCaptions || st.ancTeletext || !st.as11Metadata.empty()) && st.mxfDelivery.empty())
                throw ArgError{"\"anc_captions\" and \"as11\" go with an \"mxf_delivery\""};
            if (!st.as11Metadata.empty() && st.mxfDelivery.rfind("as11", 0) != 0)
                throw ArgError{"\"as11\" metadata goes with an AS-11 \"mxf_delivery\" (as11-dpp, as11-x2 or as11-x8)"};
            if (a.contains("exr_compression")) {
                if (st.videoCodec != "exr") throw ArgError{"\"exr_compression\" is for the OpenEXR preset"};
                st.exrCompression = str(a, "exr_compression").toStdString();
            }
            if (a.contains("stereo_view")) {
                StereoView v;
                st.stereo = str(a, "stereo_view").toStdString();
                if (!stereoViewFromName(st.stereo, v) || v == StereoView::Difference)
                    throw ArgError{"\"stereo_view\" is sbs, sbs_half, tb, tb_half, left, right or anaglyph"};
            }
            if (a.value("loudness_lufs").isDouble()) {
                st.loudnessTarget = a.value("loudness_lufs").toDouble();
                if (st.loudnessTarget >= 0 || st.loudnessTarget < -70) throw ArgError{"\"loudness_lufs\" must be between -70 and 0"};
                st.peakCeiling = std::min(0.0, a.value("peak_ceiling").toDouble(-1));
                st.dialogueGated = a.value("loudness_dialogue").toBool(false);
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
            st.describedStream = a.value("described").toBool();
            if (st.describedStream && !containerCarriesStreams(st.path))
                throw ArgError{"A described export needs a container with several audio streams (mp4, mov, mkv, mxf)"};
            const QString cap = str(a, "captions", "none");
            if (cap != "none" && cap != "burn" && cap != "embed" && cap != "both") throw ArgError{"\"captions\" must be none, burn, embed or both"};
            st.burnInCaptions = cap == "burn" || cap == "both";
            st.cea608 = a.value("cea608").toBool();
            st.cea708 = a.value("cea708").toBool(true);
            st.hdr10Plus = a.value("hdr10plus").toBool();
            st.trimTarget = str(a, "trim_target").toStdString();
            if (!st.trimTarget.empty() && !findTrimTarget(s, st.trimTarget))
                throw ArgError{QStringLiteral("The sequence has no trim target \"%1\" (montage_trim lists them)").arg(str(a, "trim_target"))};
            st.embedCaptions = cap == "embed" || cap == "both";
            if (st.embedCaptions && a.value("all_captions").toBool())
                for (const CaptionTrack& t : s.captionTracks)
                    if (t.name != kDescriptionTrackName) st.extraCaptions.push_back(t.id);  // descriptions are spoken, not subtitles
            st.audioName = a.value("audio_name").toString().toStdString();
            st.audioLanguage = a.value("audio_language").toString().toStdString();
            const QJsonValue streams = a.value("audio_streams");
            if (streams.isString() && streams.toString() != "mix") {
                const QString kind = streams.toString();
                if (kind != "roles" && kind != "tracks" && kind != "dme") throw ArgError{"\"audio_streams\" is mix, roles, tracks, dme or a list"};
                st.extraAudio = stemStreams(s, kind == "roles" ? StemsByRole : kind == "dme" ? StemsDialogueAndME : StemsByTrack);
                if (kind == "dme" && st.extraAudio.empty()) return fail("No clip heard has the Dialogue, Music or Effects role (montage_set_roles)");
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
            if (a.contains("credentials") && !a.value("credentials").isObject())
                throw ArgError{"\"credentials\" is an object: {certificate, key, allow_ai_training}"};
            if (a.value("credentials").isObject()) {
                const QJsonObject c = a.value("credentials").toObject();
                st.credentialCertificate = absolute(need(c, "certificate")).toStdString();
                st.credentialKey = absolute(need(c, "key")).toStdString();
                st.credentialAllowAiTraining = c.value("allow_ai_training").toBool();
                if (!contentCredentialsAvailable()) throw ArgError{"This build of Montage cannot sign with Content Credentials (no C2PA library)"};
                if (!exportCarriesCredentials(st)) throw ArgError{"Content Credentials cannot go in this kind of export (a single mp4, mov, wav, mp3, jpg, png or tif file can hold them)"};
                std::string why;
                if (!checkCredentialSigner({st.credentialCertificate, st.credentialKey, {}}, &why)) return fail(QString::fromStdString(why));
            }
            if (a.contains("farm")) {
                // Sent to a render farm's nodes instead (render/RenderFarm.h): the project as it is now.
                const std::string farm = absolute(need(a, "farm")).toStdString();
                const int groups = stems == "buses" ? StemsByBus : stems == "roles" ? StemsByRole : stems == "tracks" ? StemsByTrack : 0;
                std::string id, why;
                const std::string name = s.name + " - " + QFileInfo(QString::fromStdString(st.path)).fileName().toStdString();
                if (!submitFarmJob(farm, name, l.project, s.id, st, groups, &id, &why)) return fail(QString::fromStdString(why));
                return ok(QStringLiteral("Sent to the render farm as %1 (montage_render_farm follows it)").arg(QString::fromStdString(id)),
                          QJsonObject{{"job", QString::fromStdString(id)}, {"farm", QString::fromStdString(farm)}, {"output", QString::fromStdString(st.path)}});
            }
            std::string err;
            LightLevels light;
            if (!exportSequence(l.project, s, st, [this](double f, FrameTime) { progress(f, "Rendering"); }, nullptr, &err, nullptr, nullptr, &light))
                return fail(QString::fromStdString(err));
            QJsonObject o{{"output", QString::fromStdString(st.path)}};
            QString text = QStringLiteral("Wrote %1").arg(QString::fromStdString(st.path));
            if (st.hdr10Plus) {
                // Where HDR10+ went: in the video and beside it, beside it only, or nowhere (not PQ output).
                const std::string carriage = hdr10PlusCarriage(s, st);
                o["hdr10plus"] = carriage.empty() ? QStringLiteral("none") : QString::fromStdString(carriage);
                text += carriage == "video" ? QStringLiteral("\nDynamic HDR metadata (ST 2094-40) in the video, and beside it as .hdr10plus.json")
                        : carriage == "json" ? QStringLiteral("\nDynamic HDR metadata (ST 2094-40) beside it as .hdr10plus.json (this codec cannot carry it)")
                                             : QStringLiteral("\nNo dynamic HDR metadata: it is for PQ (HDR10) video");
            }
            if (!st.credentialCertificate.empty()) {
                // What the credentials say it was made with (core/Provenance.h).
                const Provenance made = sequenceProvenance(l.project, s, st.in, st.out);
                QJsonArray ai;
                for (const AiUse& u : made.ai)
                    ai.append(QJsonObject{{"feature", QString::fromStdString(u.feature)}, {"model", QString::fromStdString(u.model)},
                                          {"role", u.role == AiRole::Generated ? "generated" : u.role == AiRole::Enhanced ? "enhanced" : "assisted"}});
                o["credentials"] = QJsonObject{{"signed", true}, {"ai", ai}};
                text += made.ai.empty() ? QStringLiteral("\nSigned with Content Credentials (no AI recorded)")
                                        : QStringLiteral("\nSigned with Content Credentials, recording %1 AI use%2").arg(made.ai.size()).arg(made.ai.size() == 1 ? "" : "s");
            }
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

    add("montage_render_farm", "Render farm",
        "A render farm (a folder shared by computers running `montage-cli node <folder>`): its jobs (state, progress, node, "
        "output, error) or nodes, or a job cancelled (a rendering one stops within seconds), retried (a failed or cancelled "
        "one waits again) or removed. Jobs are sent with montage_render's farm argument.",
        R"json({"type":"object","properties":{"farm":{"type":"string","description":"The farm's folder"},
            "action":{"type":"string","enum":["jobs","nodes","cancel","retry","remove"],"default":"jobs"},
            "job":{"type":"string","description":"The job's id, for cancel, retry and remove"}},"required":["farm"]})json",
        false, [this](const QJsonObject& a) {
            const std::string farm = absolute(need(a, "farm")).toStdString();
            const QString action = str(a, "action", "jobs");
            std::string err;
            if (action == "jobs" || action == "nodes") {
                if (!openFarm(farm, &err)) return fail(QString::fromStdString(err));
                QJsonArray list;
                QStringList lines;
                if (action == "jobs") {
                    for (const FarmJob& j : farmJobs(farm)) {
                        list.append(QJsonObject{{"id", QString::fromStdString(j.id)}, {"name", QString::fromStdString(j.name)},
                                                {"state", farmStateName(j.state)}, {"progress", j.progress}, {"node", QString::fromStdString(j.node)},
                                                {"output", QString::fromStdString(j.output)}, {"error", QString::fromStdString(j.error)},
                                                {"cancel_requested", j.cancelRequested}, {"submitted_by", QString::fromStdString(j.submittedBy)}});
                        lines << QStringLiteral("%1 %2 %3").arg(QString::fromStdString(j.id), farmStateName(j.state), QString::fromStdString(j.name));
                    }
                    return ok(lines.isEmpty() ? QStringLiteral("No jobs") : lines.join('\n'), QJsonObject{{"jobs", list}});
                }
                for (const FarmNodeInfo& n : farmNodes(farm)) {
                    list.append(QJsonObject{{"name", QString::fromStdString(n.name)}, {"host", QString::fromStdString(n.host)},
                                            {"state", QString::fromStdString(n.state)}, {"job", QString::fromStdString(n.job)},
                                            {"updated", QString::fromStdString(n.updated)}});
                    lines << QStringLiteral("%1 %2").arg(QString::fromStdString(n.name), QString::fromStdString(n.state));
                }
                return ok(lines.isEmpty() ? QStringLiteral("No nodes have run on this farm") : lines.join('\n'), QJsonObject{{"nodes", list}});
            }
            const std::string id = need(a, "job").toStdString();
            bool done = false;
            if (action == "cancel") done = cancelFarmJob(farm, id, &err);
            else if (action == "retry") done = retryFarmJob(farm, id, &err);
            else if (action == "remove") done = removeFarmJob(farm, id, &err);
            else throw ArgError{"\"action\" is jobs, nodes, cancel, retry or remove"};
            if (!done) return fail(QString::fromStdString(err));
            return ok(QStringLiteral("%1: %2").arg(action, QString::fromStdString(id)), QJsonObject{{"job", QString::fromStdString(id)}});
        });

    add("montage_camera_track", "3D camera tracking",
        "The camera that filmed a video clip, solved from its footage (as Fusion's Camera Tracker and After Effects' 3D "
        "Camera Tracker do): features followed through the clip, the camera's path and focal length and the features' "
        "places in the scene, stood on the ground the most features lie on (y up, the first solved frame's camera 1.6 high). "
        "The camera needs to move (not only turn) over a scene with depth. solve keeps it on the clip in a 3D Camera "
        "Tracker effect (redoing it replaces it); info reports it; place puts a title in the scene: on the ground where "
        "the point (x, y) of the picture at `at` is (or standing there, `upright`), pinned to it in every frame; export "
        "writes the camera as a Nuke .chan file and the points as a PLY.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "action":{"type":"string","enum":["solve","info","place","export"],"default":"solve"},
            "field_of_view":{"type":"number","description":"solve: the lens's horizontal field of view in degrees, when known (else it is solved)"},
            "text":{"type":"string","default":"Title","description":"place: the title's text"},
            "at":{"type":["number","string"],"description":"place: when on the timeline to pick the point (default the clip's start)"},
            "x":{"type":"number","default":0.5,"description":"place: across the picture, 0 to 1"},
            "y":{"type":"number","default":0.7,"description":"place: down the picture, 0 to 1 (on the ground)"},
            "size":{"type":"number","default":0.4,"description":"place: the title's width as a share of its distance from the camera"},
            "upright":{"type":"boolean","default":false},"turn":{"type":"number","default":0,"description":"place: degrees about its face"},
            "chan":{"type":"string","description":"export: the .chan file to write"},"ply":{"type":"string","description":"export: the .ply file to write"},
            "first_frame":{"type":"integer","default":1,"description":"export: the .chan's first frame number"}},
            "required":["project","clip"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            Clip& c = clipArg(l, a);
            const QString action = str(a, "action", "solve");
            std::string err;
            auto summary = [](const CameraSolve& cs) {
                int solved = 0;
                for (const SolvedCamera& k : cs.cameras) solved += k.valid ? 1 : 0;
                return QJsonObject{{"focal_px", cs.focal}, {"field_of_view", cs.fieldOfView()}, {"width", cs.width}, {"height", cs.height},
                                   {"error_px", cs.error}, {"frames", int(cs.cameras.size())}, {"frames_solved", solved},
                                   {"keyframes", cs.keyframes}, {"points", int(cs.points.size())}, {"on_ground", cs.oriented}};
            };
            auto line = [](const CameraSolve& cs) {
                return QStringLiteral("Focal length %1 px (%2° across), error %3 px, %4 points%5")
                    .arg(cs.focal, 0, 'f', 1)
                    .arg(cs.fieldOfView(), 0, 'f', 1)
                    .arg(cs.error, 0, 'f', 2)
                    .arg(cs.points.size())
                    .arg(cs.oriented ? QString() : QStringLiteral(" (no ground found: titles cannot be placed)"));
            };
            if (action == "solve") {
                CameraSolveOptions o;
                if (a.contains("field_of_view")) {
                    const double fov = a.value("field_of_view").toDouble();
                    if (!(fov > 1 && fov < 179)) throw ArgError{"\"field_of_view\" is degrees, between 1 and 179"};
                    o.fieldOfView = fov;
                }
                CameraSolve cs;
                if (!solveClipCamera(l.project, s, c, o, cs, {}, nullptr, &err))
                    return fail(QStringLiteral("Could not solve the camera: %1").arg(QString::fromStdString(err)));
                Effect* e = nullptr;
                for (Effect& x : c.effects)
                    if (!e && x.type == "camera_track") e = &x;
                if (!e) {
                    c.effects.push_back(makeEffect(l.project, "camera_track"));
                    e = &c.effects.back();
                }
                e->strings["solve"] = cameraSolveToString(cs);
                QJsonObject r = summary(cs);
                r["effect_id"] = double(e->id);
                save(l);
                return ok(line(cs), r);
            }
            CameraSolve cs;
            if (!clipCameraSolve(c, cs)) return fail("The clip's camera is not solved yet (action solve)");
            if (action == "info") return ok(line(cs), summary(cs));
            if (action == "export") {
                if (!a.contains("chan") && !a.contains("ply")) throw ArgError{"Give \"chan\" and/or \"ply\": the files to write"};
                QJsonObject r;
                if (a.contains("chan")) {
                    const QString chan = absolute(need(a, "chan"));
                    if (!writeNukeChan(cs, chan.toStdString(), a.value("first_frame").toInt(1), &err)) return fail(QString::fromStdString(err));
                    r["chan"] = chan;
                }
                if (a.contains("ply")) {
                    const QString ply = absolute(need(a, "ply"));
                    if (!writePointsPly(cs, ply.toStdString(), &err)) return fail(QString::fromStdString(err));
                    r["ply"] = ply;
                }
                return ok(QStringLiteral("Wrote %1").arg(QStringList(r.keys()).join(" and ")), r);
            }
            if (action != "place") throw ArgError{"\"action\" is solve, info, place or export"};
            const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : c.start;
            if (!c.contains(at)) throw ArgError{"\"at\" is not within the clip"};
            for (const char* key : {"x", "y", "size", "turn"})
                if (a.contains(key) && !a.value(key).isDouble()) throw ArgError{QStringLiteral("\"%1\" is a number").arg(key)};
            const double size = a.value("size").toDouble(0.4);
            if (!(size > 0)) throw ArgError{"\"size\" is more than 0"};
            std::array<Vec3, 4> corners;
            if (!sceneRectangleAt(s, c, cs, at - c.start, a.value("x").toDouble(0.5), a.value("y").toDouble(0.7), size,
                                  double(s.height) / s.width, a.value("upright").toBool(), a.value("turn").toDouble(0), corners, &err))
                return fail(QString::fromStdString(err));
            const Id title = placeSceneTitle(l.project, s, c.id, str(a, "text", "Title").toStdString(), corners, &err);
            if (!title) return fail(QString::fromStdString(err));
            save(l);
            const Clip* made = edit::clipById(s, title);
            return ok(QStringLiteral("Placed a title in the scene"), made ? clipJson(l.project, s, *made) : QJsonObject{});
        });

    add("montage_steady_depth", "Steady depth for a video clip",
        "Analyses the depth of what a video clip shows with the video depth model (32 frames at a time, so a surface "
        "keeps its depth as the camera moves rather than shimmering frame to frame) and keeps it with the file's cache. "
        "The clip's depth effects (depth_blur, depth_fog, depth_map, relight, depth masks, sky_replace's depth) then use "
        "it. Analysing again replaces it. Returns whether it covers the clip.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "size":{"type":"integer","default":518,"description":"The analysis's short side in pixels (a multiple of 14, 140 to 770; less is faster)"},
            "check":{"type":"boolean","default":false,"description":"Only report whether the clip is analysed"}},
            "required":["project","clip"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Clip& c = clipArg(l, a);
            const bool covered = clipDepthAnalysed(l.project, s, c);
            if (a.value("check").toBool()) return ok(covered ? QStringLiteral("Analysed") : QStringLiteral("Not analysed"), QJsonObject{{"analysed", covered}});
            const double sizeArg = a.contains("size") ? a.value("size").toDouble(-1) : 518;
            const int size = int(sizeArg);
            if (sizeArg != size || size < 140 || size > 770 || size % 14) throw ArgError{"\"size\" is a multiple of 14 from 140 to 770"};
            if (!videoDepthAvailable() || !videoDepthModel().installed())
                return fail("Steady depth needs the video depth model: run `scripts/fetch-models.sh` or analyse once in the app");
            std::string err;
            if (!analyzeClipDepth(l.project, s, c, size, {}, &err)) return fail(QStringLiteral("Could not analyse the depth: %1").arg(QString::fromStdString(err)));
            return ok(QStringLiteral("Analysed the clip's depth"), QJsonObject{{"analysed", clipDepthAnalysed(l.project, s, c)}});
        });

    add("montage_follow_focus", "Follow Focus",
        "Keys a video clip's depth_blur (`effect`, its id) Focus Depth so the focus stays on what it follows, as a focus "
        "puller keeps it: a point (x, y on the frame at `at`, tracked to both ends of the clip), the effect's object "
        "mask (montage_object_mask, tracked), or the face nearest the point. Each frame's focus is the median steady "
        "depth inside the target (the clip is analysed first where montage_steady_depth has not), eased over `ease` "
        "seconds; where the target is lost the focus holds. Sets follow 0 and replaces the focus keys.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},
            "effect":{"type":"number","description":"The depth_blur effect's id on the clip"},
            "target":{"type":"string","enum":["point","object","face"],"default":"point"},
            "at":{"type":["number","string"],"description":"Timeline time of the frame the point is on (the clip's start by default)"},
            "x":{"type":"number","description":"Across the frame, 0 to 1 (default the effect's point_x)"},
            "y":{"type":"number","description":"Down the frame, 0 to 1 (default the effect's point_y)"},
            "ease":{"type":"number","default":0.3,"description":"Seconds a change of distance is eased over (0: none)"}},
            "required":["project","clip","effect"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            Sequence& s = l.seq();
            const Id clipId = clipArg(l, a).id;
            const Id effectId = Id(a.value("effect").toDouble());
            Effect* e = edit::ownedEffect(s, clipId, effectId);
            if (!e || e->type != "depth_blur") throw ArgError{QStringLiteral("No depth_blur effect %1 on that clip (see montage_project_info)").arg(qint64(effectId))};
            const Clip& c = *edit::clipById(s, clipId);
            FollowFocusSettings fs;
            const QString target = str(a, "target", "point");
            if (target == "object") fs.target = FocusTarget::Object;
            else if (target == "face") fs.target = FocusTarget::Face;
            else if (target != "point") throw ArgError{"\"target\" is point, object or face"};
            const FrameTime at = a.contains("at") ? timeArg(a.value("at"), s, "at") : c.start;
            if (!c.contains(at)) throw ArgError{"\"at\" must be inside the clip"};
            fs.at = at - c.start;
            fs.x = a.contains("x") ? a.value("x").toDouble(-1) : e->p("point_x", fs.at, 0.5);
            fs.y = a.contains("y") ? a.value("y").toDouble(-1) : e->p("point_y", fs.at, 0.5);
            if (!(fs.x >= 0 && fs.x <= 1 && fs.y >= 0 && fs.y <= 1)) throw ArgError{"\"x\" and \"y\" are fractions of the frame, 0 to 1"};
            fs.ease = a.contains("ease") ? a.value("ease").toDouble(-1) : 0.3;
            if (!(fs.ease >= 0 && fs.ease <= 10)) throw ArgError{"\"ease\" is seconds, 0 to 10"};
            if (fs.target == FocusTarget::Face && (!faceSearchAvailable() || !faceModel().installed()))
                return fail("Following a face needs the face model: run `scripts/fetch-models.sh`");
            Param focus;
            std::string err;
            if (!followFocus(l.project, s, c, *e, fs, focus, {}, &err)) return fail(QStringLiteral("Could not follow focus: %1").arg(QString::fromStdString(err)));
            e->params["focus"] = focus;
            e->params["follow"] = Param(0.0);
            save(l);
            QJsonArray keys;
            for (const auto& k : focus.keys) keys.append(QJsonObject{{"frame", double(c.start + k.t)}, {"focus", k.v}});
            return ok(QStringLiteral("Keyed Focus Depth at %1 frame(s)").arg(focus.keys.size()), QJsonObject{{"keys", keys}});
        });

    add("montage_ograf", "OGraf graphics",
        "OGraf graphics (the EBU's open HTML graphics format, as Resolve plays them: a folder with a *.ograf.json manifest "
        "and a JavaScript module) as clips. info reads a graphic: its name, whether it can go to any time, and the fields "
        "of its data with their defaults. add puts one on a video track at `at` for `duration` (default 5 s), its data the "
        "defaults with `data` over them; it plays in at its start, steps on every `advance` seconds when it has steps, and "
        "plays out `out` seconds before its end (default 1, 0 never), or follows `actions` instead ([{\"at\": seconds, "
        "\"type\": \"playAction\" | \"stopAction\" | \"updateAction\" | \"customAction\", \"params\": {...}}]). set changes a "
        "clip's data (merged), graphic or actions. render renders a clip's frames now (they are otherwise rendered the "
        "first time they are shown) and says why when the graphic fails. Graphics render offline in an installed Chrome, "
        "Chromium or Edge (or MONTAGE_CHROMIUM).",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "action":{"type":"string","enum":["info","add","set","render"],"default":"add"},
            "manifest":{"type":"string","description":"The graphic's .ograf.json (info, add; set to change it)"},
            "clip":{"type":"number","description":"set, render: the graphic's clip"},
            "at":{"type":["number","string"]},"duration":{"type":["number","string"],"default":5},"track":{"type":"string"},
            "data":{"type":"object","description":"Values for the graphic's fields (see info)"},
            "actions":{"type":"array","items":{"type":"object"}},
            "out":{"type":"number","default":1},"advance":{"type":"number","default":0}},
            "required":[]})json",
        false, [](const QJsonObject& a) {
            const QString action = str(a, "action", "add");
            std::string err;
            auto graphic = [&](const QString& manifest) {
                OGrafGraphic g;
                if (!readOGraf(manifest.toStdString(), g, &err)) throw ArgError{QString::fromStdString(err)};
                return g;
            };
            auto describe = [](const OGrafGraphic& g) {
                QJsonArray fields;
                for (const OGrafField& f : g.fields) {
                    QJsonObject o{{"key", QString::fromStdString(f.key)}, {"type", QString::fromStdString(f.type)}};
                    if (!f.title.empty()) o["title"] = QString::fromStdString(f.title);
                    if (!f.description.empty()) o["description"] = QString::fromStdString(f.description);
                    if (f.multiline) o["multiline"] = true;
                    if (!f.choices.empty()) {
                        QJsonArray c;
                        for (const std::string& v : f.choices) c.append(QString::fromStdString(v));
                        o["choices"] = c;
                    }
                    fields.append(o);
                }
                QJsonArray custom;
                for (const auto& [id, name] : g.customActions) custom.append(QJsonObject{{"id", QString::fromStdString(id)}, {"name", QString::fromStdString(name)}});
                return QJsonObject{{"name", QString::fromStdString(g.name)}, {"description", QString::fromStdString(g.description)},
                                   {"real_time", g.realTime}, {"any_time", g.nonRealTime}, {"steps", g.stepCount},
                                   {"fields", fields}, {"custom_actions", custom},
                                   {"defaults", QJsonDocument::fromJson(QByteArray::fromStdString(ografDefaultData(g))).object()}};
            };
            if (action == "info") {
                QJsonObject r = describe(graphic(need(a, "manifest")));
                r["browser"] = QString::fromStdString(ografBrowser());
                return ok(r.value("name").toString(), r);
            }
            if (a.contains("data") && !a.value("data").isObject()) throw ArgError{"\"data\" is an object"};
            if (a.contains("actions")) {
                if (!a.value("actions").isArray()) throw ArgError{"\"actions\" is an array"};
                for (const QJsonValue& v : a.value("actions").toArray()) {
                    const QJsonObject o = v.toObject();
                    static const QStringList types{"playAction", "stopAction", "updateAction", "customAction"};
                    if (!o.value("at").isDouble() || !types.contains(o.value("type").toString()))
                        throw ArgError{"Each action has \"at\" (seconds) and \"type\" (playAction, stopAction, updateAction or customAction)"};
                    if (o.contains("params") && !o.value("params").isObject()) throw ArgError{"An action's \"params\" is an object"};
                }
            }
            for (const char* key : {"out", "advance"})
                if (a.contains(key) && !a.value(key).isDouble()) throw ArgError{QStringLiteral("\"%1\" is a number").arg(key)};
            // The data, the actions and the timing onto a generator.
            auto apply = [&](Effect& e, const QJsonObject& base) {
                QJsonObject data = base;
                const QJsonObject given = a.value("data").toObject();
                for (auto it = given.begin(); it != given.end(); ++it) data[it.key()] = it.value();
                e.strings["data"] = QJsonDocument(data).toJson(QJsonDocument::Compact).toStdString();
                if (a.contains("actions")) {
                    if (a.value("actions").toArray().isEmpty()) e.strings.erase("actions");
                    else e.strings["actions"] = QJsonDocument(a.value("actions").toArray()).toJson(QJsonDocument::Compact).toStdString();
                }
                if (a.contains("out")) e.params["out"] = Param(std::clamp(a.value("out").toDouble(), 0.0, 30.0));
                if (a.contains("advance")) e.params["advance"] = Param(std::clamp(a.value("advance").toDouble(), 0.0, 60.0));
            };
            Loaded l = open(a);
            Sequence& s = l.seq();
            if (action == "add") {
                const QString manifest = QFileInfo(need(a, "manifest")).absoluteFilePath();
                const OGrafGraphic g = graphic(manifest);
                TrackRef t{TrackKind::Video, 0};
                if (a.contains("track")) t = trackArg(str(a, "track"), s, true, &l.project, &s);
                else {
                    int top = -1;
                    for (int i = 0; i < int(s.videoTracks.size()); ++i)
                        if (!s.videoTracks[size_t(i)].clips.empty()) top = i;
                    t = top + 1 < int(s.videoTracks.size()) ? TrackRef{TrackKind::Video, top + 1} : edit::addTrack(l.project, s, TrackKind::Video);
                }
                const FrameTime len = a.contains("duration") ? timeArg(a.value("duration"), s, "duration") : FrameTime(std::llround(5 * s.fpsValue()));
                Clip c = makeGeneratorClip(l.project, "ograf", std::max<FrameTime>(1, len));
                c.generator.strings["manifest"] = manifest.toStdString();
                apply(c.generator, QJsonDocument::fromJson(QByteArray::fromStdString(ografDefaultData(g))).object());
                c.start = timeArg(a.value("at"), s, "at");
                if (!g.name.empty()) c.name = g.name;
                const auto r = edit::overwrite(l.project, s, t, c);
                check(r);
                save(l);
                const Clip* made = r.created.empty() ? nullptr : edit::clipById(s, r.created[0]);
                QJsonObject out = made ? clipJson(l.project, s, *made) : QJsonObject{};
                out["graphic"] = describe(g);
                return ok(QStringLiteral("Added %1 on %2").arg(QString::fromStdString(g.name), QString::fromStdString(trackAt(s, t)->name)), out);
            }
            Clip& c = clipArg(l, a);
            if (c.generator.type != "ograf") throw ArgError{"That clip is not an OGraf graphic"};
            if (action == "set") {
                QJsonObject base = QJsonDocument::fromJson(QByteArray::fromStdString(c.generator.s("data"))).object();
                if (a.contains("manifest")) {
                    // Another graphic: its defaults beneath the data it had.
                    const QString manifest = QFileInfo(str(a, "manifest")).absoluteFilePath();
                    QJsonObject data = QJsonDocument::fromJson(QByteArray::fromStdString(ografDefaultData(graphic(manifest)))).object();
                    for (auto it = base.begin(); it != base.end(); ++it) data[it.key()] = it.value();
                    base = data;
                    c.generator.strings["manifest"] = manifest.toStdString();
                }
                apply(c.generator, base);
                save(l);
                return ok(QStringLiteral("Changed the graphic"), clipJson(l.project, s, c));
            }
            if (action != "render") throw ArgError{"\"action\" is info, add, set or render"};
            const OGrafJob job = ografClipJob(c.generator, s.width, s.height, s.fpsValue(), c.duration);
            if (job.manifest.empty()) throw ArgError{"That clip has no graphic"};
            QElapsedTimer clock;
            clock.start();
            const bool cached = ografCached(job);
            if (!prepareOGraf(job, {}, nullptr, &err)) return fail(QString::fromStdString(err));
            return ok(cached ? QStringLiteral("Already rendered") : QStringLiteral("Rendered %1 frames").arg(job.frames),
                      QJsonObject{{"frames", job.frames}, {"seconds", cached ? 0.0 : clock.elapsed() / 1000.0},
                                  {"folder", QString::fromStdString(ografCacheFolder(job))}});
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

    add("montage_expression", "Parameter expression",
        "Drive a clip's setting with a Lua expression evaluated at each frame, or bake one to keys. `param` of the effect "
        "`effect` (an effect id; omitted = the clip's Transform: pos_x, pos_y, scale, rotation, opacity...). set: puts "
        "`expression` on it (\"\" takes it off), e.g. \"value + wiggle(2, 10)\" or \"param('Logo', 'pos_x') + 50\"; "
        "it can use value (the keyed value), time and frame (from the clip's start), fps, duration, wiggle(freq, amount "
        "[, octaves, falloff, seed]), noise(x), random([lo, hi]), loopOut(cycle|pingpong|offset|continue), "
        "param([clip name or id,] name or effect.name), audio([track [, all|low|mid|high]]) (a track's level 0..1), "
        "linear/ease(t, t0, t1, v0, v1), clamp and Lua's math functions. Noise and random numbers are seeded, so renders "
        "repeat exactly; an expression that fails gives the keyed value. bake: replaces the expression with keys (thinned) "
        "for interchange. get: changes nothing. Returns the value at the clip's first frame (`at`: another clip frame) and any "
        "error.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},"effect":{"type":"number"},
            "param":{"type":"string"},"action":{"type":"string","enum":["set","bake","get"],"default":"set"},
            "expression":{"type":"string"},"at":{"type":"integer","default":0}},"required":["project","clip","param"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Id id = clipArg(l, a).id;
            const Id effect = Id(a.value("effect").toDouble(0));
            const std::string param = need(a, "param").toStdString();
            const QString action = str(a, "action", "set");
            if (action == "set") {
                if (!a.contains("expression")) throw ArgError{"expression is required to set one (\"\" takes it off)"};
                check(edit::setExpression(l.project, l.seq(), id, effect, param, a.value("expression").toString().toStdString()));
            } else if (action == "bake") {
                check(edit::bakeExpression(l.project, l.seq(), id, effect, param));
            } else if (action != "get") {
                throw ArgError{"action is set, bake or get"};
            }
            if (action != "get") save(l);
            const Clip* c = edit::clipById(l.seq(), id);
            const Effect* e = effect == 0 ? &c->motion : nullptr;
            for (const Effect* x : {&c->motion, &c->generator, &c->audio, &c->timing})
                if (effect && x->id == effect) e = x;
            for (const Effect& x : c->effects)
                if (effect && x.id == effect) e = &x;
            const Param* prm = e && e->params.count(param) ? &e->params.at(param) : nullptr;
            const FrameTime at = std::clamp<FrameTime>(a.value("at").toInt(0), 0, std::max<FrameTime>(0, c->duration - 1));
            ExpressionScope scope(&l.project, &l.seq(), c);
            std::string why;
            const double value = !prm ? 0.0 : prm->expr.empty() ? prm->at(at) : evaluateExpression(*prm, at, prm->keyedAt(at), &why);
            QJsonObject out{{"value", value}, {"expression", prm ? QString::fromStdString(prm->expr) : QString()},
                            {"keys", prm ? int(prm->keys.size()) : 0}};
            if (!why.empty()) out["error"] = QString::fromStdString(why);
            const QString text = action == "bake" ? QStringLiteral("Baked %1 to %2 keys").arg(QString::fromStdString(param)).arg(out["keys"].toInt())
                                 : prm && !prm->expr.empty()
                                     ? (why.empty() ? QStringLiteral("%1 = %2 at frame %3").arg(QString::fromStdString(param)).arg(value).arg(at)
                                                    : QStringLiteral("The expression fails at frame %1 (the keyed value is used): %2").arg(at).arg(QString::fromStdString(why)))
                                     : QStringLiteral("%1 has no expression").arg(QString::fromStdString(param));
            return ok(text, out);
        });

    add("montage_ltc", "Timecode from LTC",
        "Linear timecode (SMPTE or EBU LTC) recorded on a sound channel, as timecode generators feed cameras and recorders. "
        "read: finds the channel carrying it in each of `media` (the first `seconds` of every channel) and gives its "
        "rate, the timecode at the file's start and the first word read; with apply (the default) that becomes the "
        "media's timecode (the file's own kept), so merging clips, syncing dailies and multicam sync by timecode use it, "
        "and the LTC channel stops being played by its clips (one playing only it is disabled; new clips leave it out). "
        "file: back to the files' own timecode. generate: LTC of the active sequence's timecode (In to Out, else all of "
        "it) written as a WAV in a Timecode folder beside the project and placed on a new, muted audio track named LTC.",
        R"json({"type":"object","properties":{"project":{"type":"string"},
            "action":{"type":"string","enum":["read","file","generate"],"default":"read"},
            "media":{"type":"array","items":{"type":"number"}},"apply":{"type":"boolean","default":true},
            "seconds":{"type":"number","default":30,"maximum":120}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const QString action = str(a, "action", "read");
            if (action != "read" && action != "file" && action != "generate") throw ArgError{"action is read, file or generate"};
            if (action == "generate") {
                Sequence& s = l.seq();
                const FrameTime from = s.inPoint >= 0 && s.outPoint >= s.inPoint ? s.inPoint : 0;
                const FrameTime to = s.inPoint >= 0 && s.outPoint >= s.inPoint ? s.outPoint + 1 : s.duration();
                Rational rate;
                int per = 1;
                if (!ltcRateFor(s.fps, rate, per)) return fail("LTC runs at 23.976, 24, 25, 29.97 or 30 frames a second (or twice that)");
                const FrameTime at = from / per * per;
                const QString folder = QFileInfo(absolute(need(a, "project"))).absolutePath() + QStringLiteral("/Timecode");
                QDir().mkpath(folder);
                QString base = QString::fromStdString(s.name);
                for (const QChar bad : QStringLiteral("/\\:*?\"<>|")) base.replace(bad, '_');
                QString path = folder + '/' + base + QStringLiteral(" LTC.wav");
                for (int k = 2; QFileInfo::exists(path); ++k) path = folder + '/' + base + QStringLiteral(" LTC %1.wav").arg(k);
                std::string err;
                if (to <= at || !writeSequenceLtc(path.toStdString(), s.fps, at, to, 48000, &err))
                    return fail(err.empty() ? QStringLiteral("The sequence is empty") : QString::fromStdString(err));
                const Id media = mediaFor(l.project, path);
                if (MediaItem* m = l.project.findMedia(media)) {
                    m->timecode = double(at) / s.fpsValue();
                    m->metadata["comment"] = "LTC of " + s.name;
                }
                const Id clip = placeLtcTrack(l.project, s, media, at);
                if (!clip) return fail("The LTC could not be placed");
                save(l);
                return ok(QStringLiteral("LTC of %1 from %2 on a new muted track, LTC").arg(base, QString::fromStdString(formatTimecode(at, s.fps))),
                          QJsonObject{{"file", path}, {"media", double(media)}, {"clip", double(clip)},
                                      {"fps", QStringLiteral("%1/%2").arg(rate.num).arg(rate.den)}});
            }
            std::vector<Id> ids;
            for (const QJsonValue& v : a.value("media").toArray()) {
                const MediaItem* m = l.project.findMedia(Id(v.toDouble()));
                if (!m) throw ArgError{QStringLiteral("No media %1").arg(qulonglong(v.toDouble()))};
                ids.push_back(m->id);
            }
            if (ids.empty()) throw ArgError{"\"media\" lists the media"};
            if (action == "file") {
                int n = 0;
                for (Id id : ids) n += useFileTimecode(l.project, id);
                if (!n) return fail("None of them takes its timecode from LTC");
                save(l);
                return ok(QStringLiteral("%1 media item(s) back to their own timecode").arg(n), QJsonObject{{"changed", n}});
            }
            const bool apply = a.value("apply").toBool(true);
            const double seconds = std::clamp(a.value("seconds").toDouble(30), 1.0, 120.0);
            QJsonArray out;
            int found = 0;
            for (Id id : ids) {
                const MediaItem* m = l.project.findMedia(id);
                LtcReading r;
                std::string err;
                QJsonObject o{{"media", double(id)}, {"name", QString::fromStdString(m->name)}};
                if (m->path.empty() || !readLtc(m->path, r, &err, seconds, nullptr, m->hasVideo ? m->fps : Rational{0, 1})) {
                    o["error"] = QString::fromStdString(err.empty() ? "Not a file" : err);
                    out.append(o);
                    continue;
                }
                const double fileTc = usesLtcTimecode(*m) ? m->fileTimecode : m->timecode;
                o["channel"] = r.channel + 1;
                o["fps"] = QStringLiteral("%1/%2").arg(r.fps.num).arg(r.fps.den);
                o["drop_frame"] = r.dropFrame;
                o["start"] = QString::fromStdString(r.startLabel());
                o["start_seconds"] = r.start;
                o["first_word"] = QString::fromStdString(r.label);
                o["first_word_at"] = r.at;
                o["words"] = r.words;
                o["speed"] = r.speed;
                o["user_bits"] = QStringLiteral("%1").arg(r.userBits, 8, 16, QChar('0'));
                if (fileTc >= 0) o["file_timecode_seconds"] = fileTc;
                if (apply) useLtcTimecode(l.project, id, r);
                ++found;
                out.append(o);
            }
            if (apply && found) save(l);
            return found ? ok(QStringLiteral("LTC in %1 of %2 media item(s)%3").arg(found).arg(ids.size()).arg(apply ? ", now their timecode" : ""),
                              QJsonObject{{"media", out}})
                         : ToolResult{QStringLiteral("No LTC found"), QJsonObject{{"media", out}}, {}, true};
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

    add("montage_ambisonics", "Ambisonic audio",
        "Ambisonic (spatial) sound, as Premiere's VR audio, Fairlight and YouTube's spatial audio take it: AmbiX (ACN, SN3D) "
        "from first order (W, Y, Z, X: 360° cameras, ambisonic microphones) to third (16 channels), found from a file's "
        "spatial audio metadata or marked here with `media` and `ambisonic` (to `order`, default the highest its channels "
        "hold). With `clip` (an audio clip of ambisonic media): turn its field with `yaw` (degrees right), `pitch` (up) and "
        "`roll` (clockwise), keyed at `at` when given; `follow_view` (default true) turns it with the linked picture's "
        "Reframe 360° view so the sound follows the picture; `decode` binaural (headphones, the default) or stereo "
        "(speakers) in a stereo mix (a surround mix decodes it to its speakers, AllRAD). A sequence whose audio layout is "
        "\"ambix\", \"ambix2\" or \"ambix3\" (montage_set_surround layout) mixes to a field of that order (tracks placed in "
        "it by their surround panner, ambisonic clips added as they are) and exports as PCM (or AAC) in MP4 or MOV with "
        "the SA3D box YouTube reads (first order). `hrtf` picks what binaural listening hears through: \"head\" (a "
        "spherical-head model), \"ku100\" (SADIE II's measured KU100 dummy head, once downloaded) or a SOFA file's path.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"media":{"type":"number"},"ambisonic":{"type":"boolean"},
            "order":{"type":"integer","minimum":1,"maximum":3},
            "clip":{"type":"number"},"yaw":{"type":"number"},"pitch":{"type":"number"},"roll":{"type":"number"},"at":{"type":"integer"},
            "follow_view":{"type":"boolean"},"decode":{"type":"string","enum":["binaural","stereo"]},"hrtf":{"type":"string"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            QStringList done;
            if (a.contains("media")) {
                MediaItem* m = l.project.findMedia(Id(a.value("media").toDouble()));
                if (!m || !m->hasAudio || m->kind == MediaKind::Sequence) throw ArgError{"No such sound media"};
                const bool on = a.value("ambisonic").toBool(true);
                if (on && m->channels < kFoaChannels) throw ArgError{"Ambisonic sound has at least four channels"};
                const int most = ambisonicOrderForChannels(m->channels);
                const int order = a.contains("order") ? a.value("order").toInt() : most;
                if (on && (order < 1 || order > most))
                    throw ArgError{QStringLiteral("Its %1 channels hold orders 1 to %2").arg(m->channels).arg(most)};
                m->ambisonic = on ? order : 0;
                for (MediaItem& sub : l.project.media)
                    if (sub.subclipOf == m->id) sub.ambisonic = m->ambisonic;
                done << QStringLiteral("%1 is %2").arg(QString::fromStdString(m->name),
                                                       on ? QStringLiteral("an ambisonic field (order %1)").arg(order) : QStringLiteral("channels for speakers"));
            }
            QJsonObject field;
            if (a.contains("clip")) {
                const Clip& c = clipArg(l, a);
                const MediaItem* m = c.mediaId ? l.project.findMedia(c.mediaId) : nullptr;
                if (!m || m->ambisonic <= 0) throw ArgError{"The clip does not play ambisonic sound (mark its media first)"};
                FrameTime key = -1;
                if (a.contains("at")) {
                    key = FrameTime(a.value("at").toInteger()) - c.start;
                    if (key < 0 || key >= c.duration) throw ArgError{"at is a frame inside the clip"};
                }
                Clip* clip = edit::clipById(l.seq(), c.id);
                auto it = std::find_if(clip->effects.begin(), clip->effects.end(), [](const Effect& e) { return e.type == "ambisonics"; });
                if (it == clip->effects.end()) {
                    clip->effects.push_back(makeEffect(l.project, "ambisonics"));
                    it = std::prev(clip->effects.end());
                }
                for (const char* name : {"yaw", "pitch", "roll"}) {
                    if (!a.contains(name)) continue;
                    const double v = a.value(name).toDouble();
                    if (key >= 0) it->params[name].addKey(key, v, Interp::Smooth);
                    else it->params[name] = Param(v);
                }
                if (a.contains("follow_view")) it->params["follow_view"] = Param(a.value("follow_view").toBool() ? 1.0 : 0.0);
                if (a.contains("decode")) {
                    const QString d = str(a, "decode");
                    if (d != "binaural" && d != "stereo") throw ArgError{"decode is binaural or stereo"};
                    it->params["decode"] = Param(d == "stereo" ? 1.0 : 0.0);
                }
                const FrameTime t = std::max<FrameTime>(0, key);
                field = QJsonObject{{"yaw", it->p("yaw", t)}, {"pitch", it->p("pitch", t)}, {"roll", it->p("roll", t)},
                                    {"follow_view", it->p("follow_view", t, 1) > 0.5}, {"decode", it->p("decode", t) > 0.5 ? "stereo" : "binaural"}};
                done << QStringLiteral("Clip %1: turned %2° right, %3° up, %4° roll%5").arg(clip->id).arg(it->p("yaw", t)).arg(it->p("pitch", t))
                            .arg(it->p("roll", t)).arg(it->p("follow_view", t, 1) > 0.5 ? QStringLiteral(", following the 360° view") : QString());
            }
            if (a.contains("hrtf")) {
                const QString h = str(a, "hrtf");
                std::string choice = h == "head" ? std::string() : h.toStdString();
                if (!choice.empty()) {
                    if (!sofaSupported()) throw ArgError{"This build of Montage cannot read SOFA files (it was built without libmysofa)"};
                    if (choice == "ku100" && !sadieKu100Pack().installed())
                        throw ArgError{"SADIE II's KU100 responses are not downloaded: run `scripts/fetch-models.sh --hrtf` or choose them once in the app"};
                    std::string why;
                    if (choice != "ku100" && !readSofa(choice, 48000, &why)) throw ArgError{QString::fromStdString(why)};
                }
                setBinauralHrtf(choice);
                done << QStringLiteral("Binaural listening through %1").arg(choice.empty() ? QStringLiteral("the spherical head model") : h);
            }
            if (done.isEmpty()) throw ArgError{"Give media, clip or hrtf"};
            save(l);
            const std::string hrtf = binauralHrtf();
            return ok(done.join('\n'), QJsonObject{{"field", field},
                                                   {"sequence_ambisonic", ambisonicLayout(l.seq().audioLayout)},
                                                   {"sequence_order", ambisonicOrder(l.seq().audioLayout)},
                                                   {"hrtf", hrtf.empty() ? QStringLiteral("head") : QString::fromStdString(hrtf)},
                                                   {"sofa_supported", sofaSupported()}});
        });

    add("montage_pan_follow", "Pan to follow the picture",
        "Panning that follows what makes a sound, as Resolve's IntelliTrack panning: a point of the picture is tracked "
        "through the shot both ways from timeline frame `at` (default: the middle of the clip) and written into the "
        "audio track's automation over the span tracked: its pan in stereo sequences (and for tracks going to a bus), its "
        "surround position in surround and ambisonic ones (a 360° sequence's picture is the whole circle round the "
        "listener). `clip` is the audio clip; the picture is what is seen at the point. "
        "`x`, `y` (fractions of the frame, 0..1) are the point, by default the shot's subject; `size` how much "
        "around it is followed (small, medium, large); `width` (0.1..1, default 1) how much of the stage the picture "
        "spans (1: its edges hard left and right, or at the front left and right speakers). The track is set to read "
        "its automation (a track whose lanes were off gets them replaced, keeping what was heard outside the span).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"clip":{"type":"number"},"at":{"type":"integer"},
            "x":{"type":"number"},"y":{"type":"number"},"size":{"type":"string","enum":["small","medium","large"]},
            "width":{"type":"number"}},"required":["project","clip"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const Clip& c = clipArg(l, a);
            const auto loc = edit::locate(l.seq(), c.id);
            if (!loc || loc->track.kind != TrackKind::Audio) throw ArgError{"clip is an audio clip"};
            if (a.contains("at") && (!a.value("at").isDouble() || std::floor(a.value("at").toDouble()) != a.value("at").toDouble()))
                throw ArgError{"at is a timeline frame (a whole number)"};
            const FrameTime at = a.contains("at") ? FrameTime(a.value("at").toDouble()) : c.start + c.duration / 2;
            if (at < c.start || at >= c.end()) throw ArgError{"at is a frame inside the clip"};
            if (a.contains("x") != a.contains("y")) throw ArgError{"Give both x and y, or neither"};
            double x = 0.5, y = 0.5;
            std::string err;
            if (a.contains("x")) {
                if (!a.value("x").isDouble() || !a.value("y").isDouble()) throw ArgError{"x and y are numbers (fractions of the frame)"};
                x = a.value("x").toDouble();
                y = a.value("y").toDouble();
                if (x < 0 || x > 1 || y < 0 || y > 1) throw ArgError{"x and y are fractions of the frame (0 to 1)"};
            } else if (!panFollowSubject(l.project, l.seq(), c, at, x, y, &err)) {
                throw ArgError{QString::fromStdString(err)};
            }
            const QString sizeName = a.contains("size") ? str(a, "size") : QStringLiteral("medium");
            const double size = sizeName == "small" ? 0.1 : sizeName == "large" ? 0.35 : sizeName == "medium" ? 0.2 : -1;
            if (size < 0) throw ArgError{"size is small, medium or large"};
            if (a.contains("width") && !a.value("width").isDouble()) throw ArgError{"width is a number, 0.1 to 1"};
            const double width = a.contains("width") ? a.value("width").toDouble() : 1.0;
            if (width < 0.1 || width > 1) throw ArgError{"width is 0.1 to 1"};
            std::vector<PanFollowKey> keys;
            if (!trackPanFollow(l.project, l.seq(), c, at, x, y, size, keys, {}, nullptr, &err)) throw ArgError{QString::fromStdString(err)};
            Track& track = *trackAt(l.seq(), loc->track);
            const bool stereo = panFollowStereo(l.seq(), track);
            applyPanFollow(l.seq(), track, keys, width);
            QJsonArray path;
            const size_t every = std::max<size_t>(1, keys.size() / 8);
            for (size_t i = 0; i < keys.size(); i += every) {
                QJsonObject k{{"frame", qint64(keys[i].t)}, {"x", keys[i].x}, {"y", keys[i].y}};
                if (stereo) {
                    k["pan"] = trackPanAt(track, double(keys[i].t));
                } else {
                    const SurroundPan sp = trackSurroundAt(track, double(keys[i].t));
                    k["surround_x"] = sp.x;
                    k["surround_y"] = sp.y;
                }
                path.append(k);
            }
            save(l);
            return ok(QStringLiteral("Followed from frame %1 to %2: track %3's %4 now moves with it")
                          .arg(keys.front().t).arg(keys.back().t).arg(QString::fromStdString(track.name), stereo ? QStringLiteral("pan") : QStringLiteral("surround position")),
                      QJsonObject{{"from", qint64(keys.front().t)}, {"to", qint64(keys.back().t)}, {"start", QJsonObject{{"x", x}, {"y", y}}},
                                  {"lane", stereo ? "pan" : "surround"}, {"path", path}});
        });

    add("montage_stereo", "Stereoscopic 3D",
        "Stereoscopic 3D editing, as Premiere's and Resolve's stereo workflows: with `sequence_3d` make the active "
        "sequence stereoscopic (each eye rendered, stereo footage giving each its own picture) or flat, and with "
        "`vr180` mark a 360° sequence as VR180 (the front half of the sphere); with `media` and `layout` say how a "
        "file packs its eyes (file, none, sbs, sbs_half, tb, tb_half, or mv for MV-HEVC spatial video's two views) and `swap_eyes` if they are the other way round; "
        "with `clip` and `depth` (% of the frame's width, -10 to 10: + further away, - nearer) set the clip's Stereo 3D "
        "convergence (titles and graphics placed in depth), keyed at `at` when given. Reports the sequence's stereo state.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"sequence_3d":{"type":"boolean"},"vr180":{"type":"boolean"},
            "media":{"type":"number"},"layout":{"type":"string","enum":["file","none","sbs","sbs_half","tb","tb_half","mv"]},"swap_eyes":{"type":"boolean"},
            "clip":{"type":"number"},"depth":{"type":"number"},"at":{"type":"integer"}},"required":["project"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            QStringList done;
            Sequence& seq = l.seq();
            if (a.contains("sequence_3d")) {
                seq.stereo3d = a.value("sequence_3d").toBool();
                done << (seq.stereo3d ? QStringLiteral("The sequence is stereoscopic 3D") : QStringLiteral("The sequence is flat (2D)"));
            }
            if (a.contains("vr180")) {
                if (a.value("vr180").toBool() && !seq.spherical) throw ArgError{"VR180 is for 360° sequences (montage_reframe_360 sequence_360)"};
                seq.vr180 = a.value("vr180").toBool();
                done << (seq.vr180 ? QStringLiteral("The sequence is VR180") : QStringLiteral("The sequence covers the whole sphere"));
            }
            if (a.contains("media")) {
                MediaItem* m = l.project.findMedia(Id(a.value("media").toDouble()));
                if (!m) throw ArgError{"No such media"};
                if (m->subclipOf) m = l.project.findMedia(m->subclipOf);
                if (m->kind != MediaKind::Video || !m->hasVideo) throw ArgError{"Stereo layouts are for video files"};
                Interpretation i = interpretationOf(*m);
                if (a.contains("layout")) i.stereo = str(a, "layout") == "file" ? "" : str(a, "layout").toStdString();
                if (a.contains("swap_eyes")) i.swapEyes = a.value("swap_eyes").toBool();
                const edit::Result r = edit::interpretFootage(l.project, m->id, i);
                if (!r.ok && !r.error.empty()) throw ArgError{QString::fromStdString(r.error)};
                m = l.project.findMedia(m->id);
                done << QStringLiteral("%1: %2 (%3 x %4 per eye)").arg(QString::fromStdString(m->name), m->stereo.empty() ? QStringLiteral("flat") : QString::fromStdString(m->stereo))
                            .arg(m->width).arg(m->height);
            }
            if (a.contains("clip")) {
                if (!a.contains("depth")) throw ArgError{"Give depth with clip"};
                const double depth = a.value("depth").toDouble();
                if (depth < -10 || depth > 10) throw ArgError{"depth is -10 to 10 (% of the width)"};
                const Clip& c = clipArg(l, a);
                FrameTime key = -1;
                if (a.contains("at")) {
                    key = FrameTime(a.value("at").toInteger()) - c.start;
                    if (key < 0 || key >= c.duration) throw ArgError{"at is a frame inside the clip"};
                }
                Clip* clip = edit::clipById(seq, c.id);
                if (!clip) throw ArgError{"No such clip"};
                auto it = std::find_if(clip->effects.begin(), clip->effects.end(), [](const Effect& e) { return e.type == "stereo_3d"; });
                if (it == clip->effects.end()) {
                    clip->effects.push_back(makeEffect(l.project, "stereo_3d"));
                    it = std::prev(clip->effects.end());
                }
                if (key >= 0) it->params["depth"].addKey(key, depth, Interp::Smooth);
                else it->params["depth"] = Param(depth);
                done << QStringLiteral("Clip %1 depth %2%%3").arg(clip->id).arg(depth).arg(key >= 0 ? QStringLiteral(" (key at frame %1)").arg(c.start + key) : QString());
            }
            if (done.isEmpty() && (a.contains("layout") || a.contains("swap_eyes"))) throw ArgError{"Give media with layout or swap_eyes"};
            if (!done.isEmpty()) save(l);
            int stereoClips = 0;
            for (const Track& t : seq.videoTracks)
                for (const Clip& c : t.clips)
                    if (const MediaItem* m = c.mediaId ? l.project.findMedia(c.mediaId) : nullptr; m && !m->stereo.empty()) ++stereoClips;
            const QJsonObject res{{"sequence_3d", seq.stereo3d}, {"vr180", seq.vr180}, {"spherical", seq.spherical}, {"stereo_clips", stereoClips}};
            if (done.isEmpty())
                done << QStringLiteral("%1, %2 stereo clip(s)").arg(seq.stereo3d ? QStringLiteral("Stereoscopic 3D") : QStringLiteral("Flat (2D)")).arg(stereoClips);
            return ok(done.join('\n'), res);
        });

    add("montage_amf", "ACES Metadata Files (AMF)",
        "read: what an AMF (`path`) holds: its clip, input transform, looks and output transform, and the colour spaces "
        "Montage reads them as. import: the AMF onto `clips` (default: the video clips whose name, file or tape match its "
        "clip): their media read as its input transform's colour space, its looks not yet applied (ASC CDL in its working "
        "space, CLF or .cube look files, the Reference Gamut Compression) as effects first in their chains (an earlier "
        "AMF's replaced); with `set_sequence_space` the sequence also delivers in its output transform's display space. "
        "export: an AMF for each of `clips` (one clip: `path` is the file; more: a folder, one per clip): the media's "
        "colour space as the input transform, its CDL, looks in ACES2065-1 and gamut compression as looks, the sequence's "
        "space as the output transform. Notes say what could not be carried.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"action":{"type":"string","enum":["read","import","export"]},
            "path":{"type":"string"},"clips":{"type":"array","items":{"type":"number"}},"set_sequence_space":{"type":"boolean"}},
            "required":["project","action","path"]})json",
        false, [](const QJsonObject& a) {
            Loaded l = open(a);
            const QString action = need(a, "action");
            const QString path = absolute(need(a, "path"));
            std::vector<Id> clips;
            for (const QJsonValue& v : a.value("clips").toArray()) clips.push_back(Id(v.toDouble()));
            auto notesOf = [](const std::vector<std::string>& notes) {
                QJsonArray arr;
                for (const std::string& n : notes) arr.append(QString::fromStdString(n));
                return arr;
            };
            if (action == "read" || action == "import") {
                AmfDocument amf;
                std::string err;
                if (!readAmf(path.toStdString(), amf, &err)) return fail(QString::fromStdString(err));
                if (action == "read") {
                    QJsonArray looks;
                    for (const AmfLook& lk : amf.looks) {
                        QJsonObject o{{"applied", lk.applied}, {"description", QString::fromStdString(lk.description)}};
                        if (lk.kind == AmfLook::Kind::Cdl) {
                            o["kind"] = "cdl";
                            o["asc_sop"] = QString::fromStdString(cdlSopText(lk.cdl));
                            o["saturation"] = lk.cdl.saturation;
                            o["space"] = QString::fromStdString(lk.cdlSpace);
                        } else if (lk.kind == AmfLook::Kind::File) {
                            o["kind"] = "file";
                            o["file"] = QString::fromStdString(lk.file);
                        } else {
                            o["kind"] = "transform";
                            o["transform_id"] = QString::fromStdString(lk.transformId);
                        }
                        looks.append(o);
                    }
                    const QJsonObject o{{"clip_name", QString::fromStdString(amf.clipName)}, {"clip_file", QString::fromStdString(amf.clipFile)},
                                        {"clip_sequence", QString::fromStdString(amf.clipSequence)}, {"clip_uuid", QString::fromStdString(amf.clipUuid)},
                                        {"input_transform", QString::fromStdString(amf.inputTransformId.empty() ? amf.inputFile : amf.inputTransformId)},
                                        {"input_applied", amf.inputApplied}, {"input_space", QString::fromStdString(amf.inputSpace)},
                                        {"looks", looks}, {"output_transform", QString::fromStdString(amf.outputTransformId)},
                                        {"output_space", QString::fromStdString(amf.outputSpace)}, {"output_peak_nits", amf.outputPeakNits}};
                    return ok(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)), o);
                }
                AmfImportOptions o;
                o.setSequenceSpace = a.value("set_sequence_space").toBool(false);
                const AmfImportResult r = applyAmf(l.project, l.seq(), clips, amf, o);
                if (r.clips) save(l);
                QString msg = QStringLiteral("Gave %1 clips the ACES pipeline").arg(r.clips);
                for (const std::string& n : r.notes) msg += "\n" + QString::fromStdString(n);
                return ok(msg, QJsonObject{{"clips", r.clips}, {"notes", notesOf(r.notes)}});
            }
            if (action != "export") throw ArgError{QStringLiteral("\"action\" is read, import or export")};
            const Sequence& s = l.seq();
            std::vector<const Clip*> chosen;
            for (Id id : clips)
                if (const Clip* c = edit::clipById(s, id)) chosen.push_back(c);
            if (chosen.empty()) throw ArgError{QStringLiteral("Give the clips to write ACES Metadata Files for")};
            QJsonArray files;
            std::vector<std::string> notes;
            std::set<QString> used;
            for (const Clip* c : chosen) {
                // One per clip, named after it as the app names them (numbered when two share a name).
                QString base = QString::fromStdString(c->name).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]+")), QStringLiteral("_"));
                if (base.isEmpty() || base.startsWith('.')) base = "clip" + base;
                QString name = base;
                for (int n = 2; used.count(name); ++n) name = base + QStringLiteral("_%1").arg(n);
                used.insert(name);
                const QString file = chosen.size() == 1 ? path : QDir(path).filePath(name + ".amf");
                std::vector<std::string> said;
                std::string err;
                if (!writeClipAmf(l.project, s, *c, file.toStdString(), &said, {}, &err)) return fail(QString::fromStdString(err));
                for (const std::string& n : said)
                    if (std::find(notes.begin(), notes.end(), n) == notes.end()) notes.push_back(n);
                files.append(file);
            }
            QString msg = QStringLiteral("Wrote %1 ACES Metadata Files").arg(files.size());
            for (const std::string& n : notes) msg += "\n" + QString::fromStdString(n);
            return ok(msg, QJsonObject{{"files", files}, {"notes", notesOf(notes)}});
        });

    add("montage_vfx_pull", "VFX pulls",
        "Pull shots for visual effects: each clip's source frames, untouched, at the footage's own size and rate, with "
        "`handles` frames either side (default 8, as far as the footage goes), as an image sequence in a folder per shot "
        "(format exr: half float in scene-linear light; dpx: 10-bit in the footage's own colour; tiff: 16-bit), numbered "
        "so the cut's first frame is `cut_in` (default 1001), with a pull_list.csv and (unless `amf` is false) an ACES "
        "Metadata File per shot giving the frames' colour space and the clip's CDL and looks. `clips` are clip ids "
        "(default: every footage clip on the video tracks). OpenEXR pulls can carry more in each frame: `source_layers` "
        "copies a multi-layer OpenEXR source's other layers (AOVs) and Cryptomattes as they are, `mattes` adds each of the "
        "clip's masks as a one-channel layer matte_<effect>; `compression` is any of OpenEXR's (none, rle, zips, zip, "
        "piz, pxr24, b44, b44a, dwaa, dwab, zstd, htj2k256, htj2k32; default zip). `st_maps` writes, for a shot whose "
        "clip has a Lens Correction, ST maps (32-bit OpenEXR, red the source's x over its width, green 1 minus its y over "
        "its height) that undistort the plate as Lens Correction does with no fit (all of the picture, at the overscan size "
        "returned), and redistort renders back.",
        R"json({"type":"object","properties":{"project":{"type":"string"},"folder":{"type":"string"},
            "clips":{"type":"array","items":{"type":"number"}},"format":{"type":"string","enum":["exr","dpx","tiff"],"default":"exr"},
            "handles":{"type":"integer","default":8},"cut_in":{"type":"integer","default":1001},"amf":{"type":"boolean","default":true},
            "compression":{"type":"string"},"source_layers":{"type":"boolean","default":false},"mattes":{"type":"boolean","default":false},
            "st_maps":{"type":"boolean","default":false}},
            "required":["project","folder"]})json",
        false, [this](const QJsonObject& a) {
            Loaded l = open(a);
            VfxPullOptions o;
            o.folder = absolute(need(a, "folder")).toStdString();
            o.format = str(a, "format", "exr").toStdString();
            o.handles = std::clamp(a.value("handles").toInt(8), 0, 1000);
            o.cutIn = a.value("cut_in").toInt(1001);
            o.amf = a.value("amf").toBool(true);
            o.compression = str(a, "compression").toStdString();
            o.sourceLayers = a.value("source_layers").toBool();
            o.mattes = a.value("mattes").toBool();
            o.stMaps = a.value("st_maps").toBool();
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
                QJsonObject sh{{"name", QString::fromStdString(s.name)}, {"folder", QString::fromStdString(s.folder)},
                               {"first_frame", s.firstFrame}, {"cut_in", s.cutIn}, {"cut_out", s.cutOut}, {"last_frame", s.lastFrame},
                               {"head_handle", s.headHandle}, {"tail_handle", s.tailHandle}, {"amf", QString::fromStdString(s.amf)}};
                if (!s.undistortMap.empty()) {
                    sh["undistort_map"] = QString::fromStdString(s.undistortMap);
                    sh["redistort_map"] = QString::fromStdString(s.redistortMap);
                    sh["overscan"] = QJsonArray{s.overscanWidth, s.overscanHeight};
                }
                if (!s.layers.empty()) {
                    QJsonArray layers;
                    for (const std::string& n : s.layers) layers.append(QString::fromStdString(n));
                    sh["layers"] = layers;
                }
                list.append(sh);
                lines << QStringLiteral("%1: frames %2-%3 (cut %4-%5)").arg(QString::fromStdString(s.name)).arg(s.firstFrame).arg(s.lastFrame).arg(s.cutIn).arg(s.cutOut);
            }
            const QString csv = QDir(QString::fromStdString(o.folder)).filePath("pull_list.csv");
            return ok(QStringLiteral("Pulled %1 shots:\n%2\nPull list: %3").arg(shots.size()).arg(lines.join('\n'), csv),
                      QJsonObject{{"shots", list}, {"pull_list", csv}});
        });

    add("montage_export_timeline", "Export the timeline",
        "Write the active sequence as an EDL, OpenTimelineIO, Final Cut Pro 7 XML (Premiere, Resolve), FCPXML (Final Cut "
        "Pro) or AAF (Pro Tools, Fairlight, Media Composer: the audio tracks linked to mono WAVs written to a \"<name> "
        "Media\" folder beside it, with crossfades, fades and clip gain; the video tracks linked to the original files, "
        "with dissolves and constant speed, unless picture is false).",
        R"json({"type":"object","properties":{"project":{"type":"string"},"format":{"type":"string","enum":["edl","otio","xml","fcpxml","aaf"]},
            "output":{"type":"string"},"picture":{"type":"boolean","description":"AAF: include the video tracks (default true)"}},
            "required":["project","format","output"]})json",
        true, [this](const QJsonObject& a) {
            Loaded l = open(a);
            const QString f = need(a, "format");
            if (f == "aaf") {
                const QString out = absolute(need(a, "output"));
                AafExportResult r;
                std::string err;
                AafExportOptions o;
                const QJsonValue pv = a.value("picture");
                o.picture = pv.isString() ? pv.toString().compare("false", Qt::CaseInsensitive) != 0 && pv.toString() != "0" : pv.toBool(true);
                if (!exportAaf(l.project, l.seq(), out.toStdString(), &r, [this](double x, FrameTime) { progress(x, "AAF"); }, nullptr, &err, o))
                    return fail(QString::fromStdString(err));
                QJsonArray files, warnings;
                for (const std::string& m : r.mediaFiles) files.append(QString::fromStdString(m));
                for (const std::string& w : r.warnings) warnings.append(QString::fromStdString(w));
                return ok(QStringLiteral("Wrote %1: %2 audio tracks, %3 clips, %4 crossfades, %5 WAV files; %6 video tracks, %7 clips, %8 dissolves")
                              .arg(out).arg(r.audioTracks).arg(r.clips).arg(r.transitions).arg(files.size())
                              .arg(r.videoTracks).arg(r.videoClips).arg(r.videoTransitions),
                          QJsonObject{{"output", out}, {"audio_tracks", r.audioTracks}, {"clips", r.clips},
                                      {"crossfades", r.transitions}, {"video_tracks", r.videoTracks}, {"video_clips", r.videoClips},
                                      {"dissolves", r.videoTransitions}, {"media", files}, {"warnings", warnings}});
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
        "Make a project from an EDL, OpenTimelineIO, Final Cut Pro 7 XML, FCPXML or AAF file (Media Composer and Pro "
        "Tools sequences: picture and sound tracks, clips followed to their files, dissolves, audio gain, markers; media "
        "found by path, else beside the file), or an IMF package (its folder, asset map or a composition playlist: the "
        "pictures, each sound track by language and kind with the first programme heard, the timed text as caption "
        "tracks; a supplemental's track files found in the packages beside it).",
        R"json({"type":"object","properties":{"input":{"type":"string"},"project":{"type":"string","description":"The .montage file to write"},
            "fps":{"type":"number","description":"Frame rate for an EDL (default 30)"}},"required":["input","project"]})json",
        false, [](const QJsonObject& a) {
            std::string in = absolute(need(a, "input")).toStdString();
            const MediaProber prober = [](const std::string& file, MediaItem& m) { return probeMedia(file, m, nullptr); };
            if (isImfPackage(in)) {
                Loaded l;
                l.path = absolute(need(a, "project"));
                const ImportResult r = importImf(l.project, in, prober);
                if (!r.ok) return fail(QString::fromStdString(r.error));
                save(l);
                QString note = QStringLiteral("Opened the IMF composition as %1 in %2").arg(QString::fromStdString(l.seq().name), l.path);
                for (const auto& o : r.offline) note += "\nOffline: " + QString::fromStdString(o);
                for (const auto& w : r.warnings) note += "\nWarning: " + QString::fromStdString(w);
                return ok(note, projectJson(l.project));
            }
            if (std::filesystem::is_directory(in)) in += "/Info.fcpxml";
            std::ifstream f(in, std::ios::binary);
            if (!f) return fail(QStringLiteral("Cannot read %1").arg(QString::fromStdString(in)));
            std::string ext = std::filesystem::path(in).extension().string();
            for (char& ch : ext) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            // An AAF is a binary compound file, read by its own importer.
            const std::string text = ext == ".aaf" ? std::string() : std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            Loaded l;
            l.path = absolute(need(a, "project"));
            l.project.name = std::filesystem::path(in).stem().string();
            const double fps = a.value("fps").toDouble(30);
            const Rational rate = std::fabs(fps - std::round(fps)) < 1e-6 ? Rational{int(std::lround(fps)), 1} : Rational{int(std::lround(fps * 1001)), 1001};
            const ImportResult r = ext == ".aaf" ? importAaf(l.project, in, prober)
                                   : ext == ".edl" ? importEdl(l.project, text, rate, prober, std::filesystem::path(in).parent_path().string())
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
            if (const LockStatus st = projectLockStatus(path.toStdString()); st.state == LockState::Theirs)
                return fail(QStringLiteral("%1 is editing %2 in Montage: nothing is undone behind their back")
                                .arg(QString::fromStdString(st.owner.describe()), QFileInfo(path).fileName()));
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
