#include "RenderFarm.h"

#include <QDateTime>
#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSysInfo>
#include <chrono>
#include <filesystem>
#include <set>
#include <thread>

#include "core/ProjectIO.h"
#include "render/BroadcastMxf.h"
#include "render/Dcp.h"
#include "render/DcpSecurity.h"
#include "render/Imf.h"
#include "render/SegmentedExport.h"

namespace montage {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string ss(const QString& s) { return s.toStdString(); }

std::string nowIso() { return ss(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)); }
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Settings both ways through one list of fields.
struct ToJson {
    QJsonObject& o;
    void operator()(const char* k, const std::string& v) { o[k] = qs(v); }
    void operator()(const char* k, bool v) { o[k] = v; }
    void operator()(const char* k, int v) { o[k] = v; }
    void operator()(const char* k, int64_t v) { o[k] = double(v); }
    void operator()(const char* k, double v) { o[k] = v; }
    void operator()(const char* k, Id v) { o[k] = QString::number(v); }  // (ids as text: JSON numbers hold 53 bits)
    void operator()(const char* k, const std::vector<bool>& v) {
        QJsonArray a;
        for (bool b : v) a.append(b);
        o[k] = a;
    }
    void operator()(const char* k, const std::vector<std::string>& v) {
        QJsonArray a;
        for (const std::string& x : v) a.append(qs(x));
        o[k] = a;
    }
    void operator()(const char* k, const std::vector<Id>& v) {
        QJsonArray a;
        for (Id x : v) a.append(QString::number(x));
        o[k] = a;
    }
};

struct FromJson {
    const QJsonObject& o;
    std::string error;
    bool has(const char* k, bool (QJsonValue::*is)() const) {
        if (!o.contains(k)) return false;
        if (!(o.value(k).*is)()) {
            if (error.empty()) error = std::string("the export setting \"") + k + "\" has the wrong type";
            return false;
        }
        return true;
    }
    void operator()(const char* k, std::string& v) {
        if (has(k, &QJsonValue::isString)) v = ss(o.value(k).toString());
    }
    void operator()(const char* k, bool& v) {
        if (has(k, &QJsonValue::isBool)) v = o.value(k).toBool();
    }
    void operator()(const char* k, int& v) {
        if (has(k, &QJsonValue::isDouble)) v = o.value(k).toInt();
    }
    void operator()(const char* k, int64_t& v) {
        if (has(k, &QJsonValue::isDouble)) v = int64_t(o.value(k).toDouble());
    }
    void operator()(const char* k, double& v) {
        if (has(k, &QJsonValue::isDouble)) v = o.value(k).toDouble();
    }
    // Every element of an array of the right kind, or an error.
    bool elements(const char* k, bool (QJsonValue::*is)() const) {
        for (const QJsonValue& x : o.value(k).toArray())
            if (!(x.*is)()) {
                if (error.empty()) error = std::string("the export setting \"") + k + "\" holds a value of the wrong type";
                return false;
            }
        return true;
    }
    bool id(const char* k, const QJsonValue& x, Id& v) {
        bool ok = false;
        v = x.toString().toULongLong(&ok);
        if (!ok && error.empty()) error = std::string("the export setting \"") + k + "\" is not an id";
        return ok;
    }
    void operator()(const char* k, Id& v) {
        if (has(k, &QJsonValue::isString)) id(k, o.value(k), v);
    }
    void operator()(const char* k, std::vector<bool>& v) {
        if (!has(k, &QJsonValue::isArray) || !elements(k, &QJsonValue::isBool)) return;
        v.clear();
        for (const QJsonValue& x : o.value(k).toArray()) v.push_back(x.toBool());
    }
    void operator()(const char* k, std::vector<std::string>& v) {
        if (!has(k, &QJsonValue::isArray) || !elements(k, &QJsonValue::isString)) return;
        v.clear();
        for (const QJsonValue& x : o.value(k).toArray()) v.push_back(ss(x.toString()));
    }
    void operator()(const char* k, std::vector<Id>& v) {
        if (!has(k, &QJsonValue::isArray) || !elements(k, &QJsonValue::isString)) return;
        v.clear();
        for (const QJsonValue& x : o.value(k).toArray()) {
            Id i = 0;
            if (!id(k, x, i)) return;
            v.push_back(i);
        }
    }
};

template <class S, class V>
void visitBurnIn(S& b, V& v) {
    v("timecode", b.timecode);
    v("clipName", b.clipName);
    v("footage", b.footage);
    v("keyNumber", b.keyNumber);
    v("inkNumber", b.inkNumber);
    v("frameLines", b.frameLines);
    v("text", b.text);
    v("corner", b.corner);
    v("size", b.size);
    v("watermark", b.watermark);
    v("watermarkCorner", b.watermarkCorner);
    v("watermarkWidth", b.watermarkWidth);
    v("watermarkOpacity", b.watermarkOpacity);
}

template <class S, class V>
void visitStream(S& a, V& v) {
    v("name", a.name);
    v("language", a.language);
    v("tracks", a.tracks);
    v("role", a.role);
    v("unmute", a.unmute);
    v("normalise", a.normalise);
    v("descriptions", a.descriptions);
}

// Every field but burnIn and extraAudio (objects of their own).
template <class S, class V>
void visitSettings(S& s, V& v) {
    v("path", s.path);
    v("videoCodec", s.videoCodec);
    v("startNumber", s.startNumber);
    v("audioCodec", s.audioCodec);
    v("width", s.width);
    v("height", s.height);
    v("fps", s.fps);
    v("crf", s.crf);
    v("gop", s.gop);
    v("videoBitrate", s.videoBitrate);
    v("twoPass", s.twoPass);
    v("hrd", s.hrd);
    v("vbvMaxrate", s.vbvMaxrate);
    v("vbvBuffer", s.vbvBuffer);
    v("vbvInit", s.vbvInit);
    v("pass", s.pass);
    v("parameterSetId", s.parameterSetId);
    v("passStats", s.passStats);
    v("preset", s.preset);
    v("profile", s.profile);
    v("pixFmt", s.pixFmt);
    v("audioBitrate", s.audioBitrate);
    v("sampleRate", s.sampleRate);
    v("in", s.in);
    v("out", s.out);
    v("useProxies", s.useProxies);
    v("alpha", s.alpha);
    v("stereo", s.stereo);
    v("spatialBaseline", s.spatialBaseline);
    v("spatialFieldOfView", s.spatialFieldOfView);
    v("burnInCaptions", s.burnInCaptions);
    v("embedCaptions", s.embedCaptions);
    v("captionTrack", s.captionTrack);
    v("cea608", s.cea608);
    v("cea708", s.cea708);
    v("exrCompression", s.exrCompression);
    v("jxlDepth", s.jxlDepth);
    v("jxlDistance", s.jxlDistance);
    v("jxlEffort", s.jxlEffort);
    v("jxlPremultiplied", s.jxlPremultiplied);
    v("colorSpace", s.colorSpace);
    v("toneMapping", s.toneMapping);
    v("trimTarget", s.trimTarget);
    v("hdr10Plus", s.hdr10Plus);
    v("loudnessTarget", s.loudnessTarget);
    v("peakCeiling", s.peakCeiling);
    v("dialogueGated", s.dialogueGated);
    v("loudnessReport", s.loudnessReport);
    v("leqmLimit", s.leqmLimit);
    v("downmixStereo", s.downmixStereo);
    v("audioTracks", s.audioTracks);
    v("chapters", s.chapters);
    v("smartRender", s.smartRender);
    v("describedStream", s.describedStream);
    v("describedName", s.describedName);
    v("audioName", s.audioName);
    v("audioLanguage", s.audioLanguage);
    v("monoAudioTracks", s.monoAudioTracks);
    v("startTimecode", s.startTimecode);
    v("extraCaptions", s.extraCaptions);
    v("interlaced", s.interlaced);
    v("rateChange", s.rateChange);
    v("rateNum", s.rate.num);
    v("rateDen", s.rate.den);
    v("cadence", s.cadence);
    v("rateKeepPitch", s.rateKeepPitch);
    v("mxfDelivery", s.mxfDelivery);
    v("as11Metadata", s.as11Metadata);
    v("ancCaptions", s.ancCaptions);
    v("ancTeletext", s.ancTeletext);
    v("teletextPage", s.teletextPage);
    v("credentialCertificate", s.credentialCertificate);
    v("credentialKey", s.credentialKey);
    v("credentialAllowAiTraining", s.credentialAllowAiTraining);
}

// A package's settings (render/Imf.h, render/Dcp.h): every field but the lists, which are objects of their own.
template <class S, class V>
void visitImf(S& s, V& v) {
    v("title", s.title);
    v("kind", s.kind);
    v("issuer", s.issuer);
    v("language", s.language);
    v("colour", s.colour);
    v("size", s.size);
    v("bits", s.bits);
    v("lossless", s.lossless);
    v("htj2k", s.htj2k);
    v("megabitsPerSecond", s.megabitsPerSecond);
    v("masteringPeak", s.masteringPeak);
    v("inOut", s.inOut);
    v("threads", s.threads);
    v("captions", s.captions);
    v("supplementOf", s.supplementOf);
    v("originalSequence", s.originalSequence);
    v("application", s.application);
    v("graded", s.graded);
    v("immersive", s.immersive);
    v("sadm", s.sadm);
    v("adm", s.adm);
    v("codestreams", s.codestreams);
    v("codeFrom", s.codeFrom);
    v("codeTo", s.codeTo);
}
template <class S, class V>
void visitDcp(S& s, V& v) {
    v("title", s.title);
    v("kind", s.kind);
    v("container", s.container);
    v("resolution", s.resolution);
    v("language", s.language);
    v("territory", s.territory);
    v("issuer", s.issuer);
    v("studio", s.studio);
    v("facility", s.facility);
    v("fps", s.fps);
    v("inOut", s.inOut);
    v("threads", s.threads);
    v("stereo", s.stereo);
    v("subtitles", s.subtitles);
    v("closedCaptions", s.closedCaptions);
    v("burnIn", s.burnIn);
    v("subtitleLanguage", s.subtitleLanguage);
    v("encrypt", s.encrypt);
    v("sign", s.sign);
    v("certificates", s.certificates);
    v("dkdms", s.dkdms);
    v("immersive", s.immersive);
    v("codestreams", s.codestreams);
    v("codeFrom", s.codeFrom);
    v("codeTo", s.codeTo);
    v("codestreamCertificate", s.codestreamCertificate);
    v("codestreamJob", s.codestreamJob);
    v("codestreamSegments", s.codestreamSegments);
}

QJsonObject imfToJson(const ImfSettings& s) {
    QJsonObject o;
    ToJson w{o};
    visitImf(s, w);
    QJsonArray sounds, texts;
    for (const ImfSound& x : s.sounds) {
        QJsonArray roles;
        for (const std::string& r : x.roles) roles.append(qs(r));
        sounds.append(QJsonObject{{"language", qs(x.language)}, {"kind", qs(x.kind)}, {"roles", roles}});
    }
    for (const ImfText& x : s.texts) texts.append(QJsonObject{{"track", QString::number(x.track)}, {"kind", qs(x.kind)}, {"image", x.image}});
    o["sounds"] = sounds;
    o["texts"] = texts;
    return o;
}
bool imfFromJson(const QJsonObject& o, ImfSettings& s, std::string* error) {
    FromJson r{o, {}};
    visitImf(s, r);
    s.sounds.clear();
    for (const QJsonValue& v : o.value("sounds").toArray()) {
        const QJsonObject x = v.toObject();
        ImfSound sound;
        FromJson xr{x, {}};
        xr("language", sound.language);
        xr("kind", sound.kind);
        xr("roles", sound.roles);
        if (r.error.empty()) r.error = xr.error;
        s.sounds.push_back(std::move(sound));
    }
    s.texts.clear();
    for (const QJsonValue& v : o.value("texts").toArray()) {
        const QJsonObject x = v.toObject();
        ImfText text;
        FromJson xr{x, {}};
        xr("track", text.track);
        xr("kind", text.kind);
        xr("image", text.image);
        if (r.error.empty()) r.error = xr.error;
        s.texts.push_back(std::move(text));
    }
    if (!r.error.empty() && error) *error = r.error;
    return r.error.empty();
}
QJsonObject dcpToJson(const DcpSettings& s) {
    QJsonObject o;
    ToJson w{o};
    visitDcp(s, w);
    return o;
}
bool dcpFromJson(const QJsonObject& o, DcpSettings& s, std::string* error) {
    FromJson r{o, {}};
    visitDcp(s, r);
    if (!r.error.empty() && error) *error = r.error;
    return r.error.empty();
}

QJsonObject readJson(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool writeJson(const QString& path, const QJsonObject& o) {
    QSaveFile f(path);
    // (Where a share will not rename over a file another computer has open, written in place instead.)
    f.setDirectWriteFallback(true);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return f.commit();
}

bool fail(std::string* error, const std::string& e) {
    if (error) *error = e;
    return false;
}

// Whether folder `inner` is `outer` or lies inside it, as the file system sees them: compared through links (from the
// nearest folder that is there, so one not made yet is caught too; macOS's /tmp is /private/tmp), on Windows and macOS
// whatever their case, and by the folders themselves (device and file number, or Windows's file id), so one share
// reached two ways (a UNC path and a mapped drive, two mounts of it) is caught where the system can tell.
bool folderWithin(const std::string& inner, const std::string& outer) {
    if (inner.empty() || outer.empty()) return false;
    auto nearest = [](const std::string& path, QString* rest) {
        QString at = QDir::cleanPath(QFileInfo(qs(path)).absoluteFilePath());
        while (!QFileInfo::exists(at)) {
            const QString up = QFileInfo(at).path();
            if (up == at) break;
            *rest = QFileInfo(at).fileName() + (rest->isEmpty() ? QString() : QStringLiteral("/") + *rest);
            at = up;
        }
        return at;
    };
    auto where = [&](const std::string& path) {
        QString rest;
        const QString at = nearest(path, &rest);
        const QString real = QFileInfo(at).canonicalFilePath();
        return QDir::cleanPath((real.isEmpty() ? at : real) + (rest.isEmpty() ? QString() : QStringLiteral("/") + rest));
    };
#if defined(_WIN32) || defined(__APPLE__)
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    const QString in = where(inner), out = where(outer);
    if (in.compare(out, cs) == 0 || in.startsWith(out + '/', cs)) return true;
    // (the same folder by another path: each folder `inner` is in, as named and through its links, against `outer`)
    const QFileInfo outerInfo(qs(outer));
    if (!outerInfo.isDir()) return false;
    const std::filesystem::path target = outerInfo.filesystemAbsoluteFilePath();
    QString rest;
    const QString there = nearest(inner, &rest);
    for (const QString& start : {there, QFileInfo(there).canonicalFilePath()}) {
        if (start.isEmpty()) continue;
        std::filesystem::path at = QFileInfo(start).filesystemAbsoluteFilePath();
        for (;;) {
            std::error_code ec;
            if (std::filesystem::equivalent(at, target, ec) && !ec) return true;
            const std::filesystem::path up = at.parent_path();
            if (up == at || up.empty()) break;
            at = up;
        }
    }
    return false;
}

// Whether the signing chain in folder `certificates` has its private key within `outer`: the folder is there, or its
// leaf.key is a link to a file there.
bool keyWithin(const std::string& certificates, const std::string& outer) {
    if (folderWithin(certificates, outer)) return true;
    const QString real = QFileInfo(QDir(qs(certificates)).filePath(QStringLiteral("leaf.key"))).canonicalFilePath();
    return !real.isEmpty() && folderWithin(ss(QFileInfo(real).path()), outer);
}

QString jobsDir(const std::string& farm) { return QDir(qs(farm)).filePath(QStringLiteral("jobs")); }
QString nodesDir(const std::string& farm) { return QDir(qs(farm)).filePath(QStringLiteral("nodes")); }
QString jobDir(const std::string& farm, const std::string& id) { return QDir(jobsDir(farm)).filePath(qs(id)); }

// A job id names a folder of jobs/ and nothing else.
bool validId(const std::string& id) {
    if (id.empty() || id.size() > 64 || id[0] == '.') return false;
    for (char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
    return true;
}

FarmState stateFrom(const QString& s) {
    if (s == QLatin1String("rendering")) return FarmState::Rendering;
    if (s == QLatin1String("done")) return FarmState::Done;
    if (s == QLatin1String("failed")) return FarmState::Failed;
    if (s == QLatin1String("cancelled")) return FarmState::Cancelled;
    return FarmState::Waiting;
}

bool ended(FarmState s) { return s == FarmState::Done || s == FarmState::Failed || s == FarmState::Cancelled; }

FarmJob readJob(const std::string& farm, const std::string& id) {
    const QDir d(jobDir(farm, id));
    const QJsonObject j = readJson(d.filePath(QStringLiteral("job.json")));
    FarmJob job;
    job.id = id;
    job.name = ss(j.value("name").toString());
    job.output = ss(j.value("settings").toObject().value("path").toString());
    job.submittedBy = ss(j.value("submittedBy").toString());
    job.submitted = ss(j.value("submitted").toString());
    job.group = ss(j.value("group").toString());
    job.segment = j.value("segment").toInt();
    job.segments = j.value("segments").toInt();
    job.join = j.value("join").toBool();
    job.pass = j.value("pass").toInt();
    job.signer = ss(j.value("signer").toString());
    QFile claim(d.filePath(QStringLiteral("claim")));
    const bool claimed = claim.open(QIODevice::ReadOnly);
    if (claimed) job.claim = ss(QString::fromUtf8(claim.readAll()));
    job.cancelRequested = QFileInfo::exists(d.filePath(QStringLiteral("cancel")));
    const QJsonObject st = claimed || QFileInfo::exists(d.filePath(QStringLiteral("status.json"))) ? readJson(d.filePath(QStringLiteral("status.json")))
                                                                                                  : QJsonObject();
    job.state = stateFrom(st.value("state").toString());
    // (Claimed with no status yet: being started.)
    if (claimed && job.state == FarmState::Waiting) job.state = FarmState::Rendering;
    // (A status left without its claim, not final: taken back from a node; it waits.)
    if (!claimed && !ended(job.state)) job.state = FarmState::Waiting;
    if (job.state == FarmState::Waiting && job.cancelRequested) job.state = FarmState::Cancelled;
    job.progress = job.state == FarmState::Done ? 1.0 : st.value("progress").toDouble();
    job.node = ss(st.value("node").toString());
    job.error = ss(st.value("error").toString());
    job.encoder = ss(st.value("encoder").toString());
    job.started = ss(st.value("started").toString());
    job.finished = ss(st.value("finished").toString());
    job.beat = int64_t(st.value("beat").toDouble());
    if (job.state == FarmState::Cancelled && job.error.empty()) job.error = "Cancelled";
    return job;
}

bool jobExists(const std::string& farm, const std::string& id) {
    return validId(id) && QFileInfo::exists(QDir(jobDir(farm, id)).filePath(QStringLiteral("job.json")));
}

// A join job's segments, in order.
std::vector<std::string> segmentIds(const std::string& farm, const std::string& id) {
    std::vector<std::string> out;
    for (const QJsonValue& v : readJson(QDir(jobDir(farm, id)).filePath(QStringLiteral("job.json"))).value("after").toArray())
        out.push_back(ss(v.toString()));
    return out;
}

// A join job's segments and, with two passes, their first passes before them: every job sent with it.
std::vector<std::string> groupIds(const std::string& farm, const std::string& id) {
    std::vector<std::string> out;
    const QJsonObject j = readJson(QDir(jobDir(farm, id)).filePath(QStringLiteral("job.json")));
    for (const char* key : {"passes", "after"})
        for (const QJsonValue& v : j.value(QLatin1String(key)).toArray()) out.push_back(ss(v.toString()));
    return out;
}

// A count kept in a job's folder (a segment's failures, a join's checks that sent segments back).
int jobCount(const QDir& d, const char* name) {
    QFile f(d.filePath(QString::fromLatin1(name)));
    return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed().toInt() : 0;
}
void setJobCount(const QDir& d, const char* name, int n) {
    QFile f(d.filePath(QString::fromLatin1(name)));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QByteArray::number(n));
}

std::string newJobId() {
    return ss(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz-")) +
              QString::number(QRandomGenerator::global()->bounded(0x1000000u), 16).rightJustified(6, '0'));
}

// Written aside, then put in place whole: a node never sees half a job.
bool putJob(const std::string& farm, const std::string& jobId, const QJsonObject& job, const QByteArray& project, std::string* error) {
    const QDir jobs(jobsDir(farm));
    const QString incoming = jobs.filePath(QStringLiteral(".incoming-") + qs(jobId));
    if (!QDir().mkpath(incoming)) return fail(error, "Cannot write in the render farm folder " + farm);
    auto drop = [&](const std::string& e) {
        QDir(incoming).removeRecursively();
        return fail(error, e);
    };
    QSaveFile p(QDir(incoming).filePath(QStringLiteral("project.json")));
    if (!p.open(QIODevice::WriteOnly)) return drop("Cannot write the job's project");
    p.write(project);
    if (!p.commit()) return drop("Cannot write the job's project");
    if (!writeJson(QDir(incoming).filePath(QStringLiteral("job.json")), job)) return drop("Cannot write the job");
    if (!QDir().rename(incoming, jobs.filePath(qs(jobId)))) return drop("Cannot put the job in the render farm");
    return true;
}

// A job's folder renamed away (no node can claim it from then on) and removed.
bool takeJobAway(const std::string& farm, const std::string& id) {
    if (!validId(id) || !QFileInfo::exists(jobDir(farm, id))) return false;
    const QString gone = QDir(jobsDir(farm)).filePath(QStringLiteral(".removed-") + qs(id));
    if (!QDir().rename(jobDir(farm, id), gone)) return false;
    QDir(gone).removeRecursively();
    return true;
}

}  // namespace

QJsonObject exportSettingsToJson(const ExportSettings& s) {
    QJsonObject o;
    ToJson w{o};
    visitSettings(s, w);
    QJsonObject b;
    ToJson bw{b};
    visitBurnIn(s.burnIn, bw);
    o["burnIn"] = b;
    QJsonArray extra;
    for (const ExportSettings::AudioStream& a : s.extraAudio) {
        QJsonObject x;
        ToJson xw{x};
        visitStream(a, xw);
        extra.append(x);
    }
    o["extraAudio"] = extra;
    return o;
}

bool exportSettingsFromJson(const QJsonObject& o, ExportSettings& s, std::string* error) {
    FromJson r{o, {}};
    visitSettings(s, r);
    if (o.contains("burnIn") && !o.value("burnIn").isObject() && r.error.empty()) r.error = "the export setting \"burnIn\" is not an object";
    if (o.contains("extraAudio") && !o.value("extraAudio").isArray() && r.error.empty()) r.error = "the export setting \"extraAudio\" is not a list";
    if (o.value("burnIn").isObject()) {
        const QJsonObject b = o.value("burnIn").toObject();
        FromJson br{b, {}};
        visitBurnIn(s.burnIn, br);
        if (r.error.empty()) r.error = br.error;
    }
    if (o.value("extraAudio").isArray()) {
        s.extraAudio.clear();
        for (const QJsonValue& v : o.value("extraAudio").toArray()) {
            if (!v.isObject() && r.error.empty()) r.error = "an \"extraAudio\" stream is not an object";
            const QJsonObject x = v.toObject();
            ExportSettings::AudioStream a;
            FromJson xr{x, {}};
            visitStream(a, xr);
            if (r.error.empty()) r.error = xr.error;
            s.extraAudio.push_back(std::move(a));
        }
    }
    if (!r.error.empty()) return fail(error, r.error);
    return true;
}

bool renderExport(const Project& p, Id sequence, const ExportSettings& s, int stems, const ExportProgress& progress,
                  const std::atomic<bool>* cancel, std::string* error, std::string* encoder, bool* keptUnsigned) {
    if (keptUnsigned) *keptUnsigned = false;
    const Sequence* seq = p.findSequence(sequence);
    if (!seq) return fail(error, "The sequence no longer exists");
    try {
        const double share = stems ? 0.5 : 1.0;  // the stems take the second half of the bar
        auto part = [&progress](double from, double share) {
            return ExportProgress([&progress, from, share](double f, FrameTime t) {
                if (progress) progress(from + f * share, t);
            });
        };
        std::string err;
        bool ok = exportSequence(p, *seq, s, part(0, share), cancel, &err, encoder, nullptr, nullptr, keptUnsigned);
        if (ok && stems) ok = exportStems(p, *seq, s, stems, nullptr, part(0.5, 0.5), cancel, &err);
        if (!ok) return fail(error, err);
        return true;
    } catch (const std::exception& e) {
        return fail(error, e.what());
    }
}

std::string mapFarmPath(const std::string& path, const std::vector<FarmPathMap>& maps) {
    auto norm = [](std::string s) {
        for (char& c : s)
            if (c == '\\') c = '/';
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        return s;
    };
    const std::string p = norm(path);
    for (const FarmPathMap& m : maps) {
        const std::string from = norm(m.from);
        if (from.empty() || p.size() < from.size()) continue;
        // Windows paths (a drive letter, a share or backslashes) match whatever their letters' case.
        const bool windows = (m.from.size() > 1 && m.from[1] == ':') || m.from.find('\\') != std::string::npos || m.from.rfind("//", 0) == 0;
        const bool same = windows ? QString::fromStdString(p.substr(0, from.size())).compare(qs(from), Qt::CaseInsensitive) == 0
                                  : p.compare(0, from.size(), from) == 0;
        if (!same) continue;
        if (p.size() > from.size() && p[from.size()] != '/' && from.back() != '/') continue;  // (a folder, not a prefix of a name)
        std::string rest = p.substr(from.size());
        if (!rest.empty() && rest.front() != '/') rest.insert(rest.begin(), '/');  // (from "/")
        const bool backslashes = m.to.find('\\') != std::string::npos && m.to.find('/') == std::string::npos;
        if (backslashes)
            for (char& c : rest)
                if (c == '/') c = '\\';
        std::string to = m.to;
        if (!to.empty() && (to.back() == '/' || to.back() == '\\') && !rest.empty()) to.pop_back();
        return to + rest;
    }
    return path;
}

bool parseFarmPathMap(const std::string& text, FarmPathMap& map) {
    const size_t eq = text.rfind('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= text.size()) return false;
    map.from = text.substr(0, eq);
    map.to = text.substr(eq + 1);
    return true;
}

const char* farmStateName(FarmState s) {
    switch (s) {
    case FarmState::Waiting: return "waiting";
    case FarmState::Rendering: return "rendering";
    case FarmState::Done: return "done";
    case FarmState::Failed: return "failed";
    case FarmState::Cancelled: return "cancelled";
    }
    return "waiting";
}

bool openFarm(const std::string& farm, std::string* error) {
    if (farm.empty()) return fail(error, "No render farm folder");
    QDir d(qs(farm));
    if (!d.exists() && !QDir().mkpath(d.absolutePath())) return fail(error, "Cannot make the render farm folder " + farm);
    if (!d.mkpath(QStringLiteral("jobs")) || !d.mkpath(QStringLiteral("nodes")))
        return fail(error, "Cannot write in the render farm folder " + farm);
    return true;
}

bool submitFarmJob(const std::string& farm, const std::string& name, const Project& project, Id sequence, const ExportSettings& settings,
                   int stems, std::string* id, std::string* error, int segments) {
    if (!openFarm(farm, error)) return false;
    const Sequence* seq = project.findSequence(sequence);
    if (!seq) return fail(error, "The sequence no longer exists");
    if (settings.path.empty()) return fail(error, "The job has no output file");
    std::vector<ExportSegment> parts;
    if (segments > 1) {
        std::string why;
        if (!exportSplits(settings, &why)) return fail(error, "This export cannot be split into segments: " + why);
        parts = planExportSegments(*seq, settings, segments);
        if (parts.empty()) return fail(error, "Nothing to export: the range is empty");
        if (settings.twoPass && settings.videoCodec == "libx264" && parts.size() > 32)
            return fail(error, "H.264 in two passes is split into 32 segments at most");
        // (A broadcast delivery checked now, not after every segment is rendered.)
        if (parts.size() > 1 && !settings.mxfDelivery.empty()) {
            BroadcastMxfOptions deliver;
            if (!broadcastMxfJoinOptions(*seq, settings, deliver, &why)) return fail(error, why);
        }
    }
    const QByteArray proj = QByteArray::fromStdString(projectToJson(project));
    // (The sequence's picture size, rate and stereoscopy: what a hardware encoder is picked by, with the settings.)
    const QJsonArray picture{seq->width, seq->height, int(seq->fps.num), int(seq->fps.den), seq->stereo3d};
    const QJsonObject job{{"name", qs(name)},
                          {"sequence", QString::number(sequence)},
                          {"stems", stems},
                          {"picture", picture},
                          {"settings", exportSettingsToJson(settings)},
                          {"submittedBy", QSysInfo::machineHostName()},
                          {"submitted", qs(nowIso())},
                          {"version", 1}};
    const std::string jobId = newJobId();
    if (parts.size() < 2) {
        if (!putJob(farm, jobId, job, proj, error)) return false;
        if (id) *id = jobId;
        return true;
    }
    // The segments first (each a job of its own, writing into a hidden folder beside the output, named as the sender
    // names it: each node maps it), then the join, which waits for them.
    const size_t slash = settings.path.find_last_of("/\\");
    const std::string folder = (slash == std::string::npos ? std::string() : settings.path.substr(0, slash + 1)) + ".montage-segments-" + jobId;
    const char sep = slash == std::string::npos ? '/' : settings.path[slash];
    std::vector<std::string> made;
    auto undo = [&](const std::string& e) {
        for (const std::string& m : made) takeJobAway(farm, m);
        return fail(error, e);
    };
    QJsonArray after, ranges, passes;
    const int n = int(parts.size());
    auto statsOf = [&](int k) {
        char stats[32];
        std::snprintf(stats, sizeof stats, "segment-%03d.log", k + 1);
        return folder + sep + stats;
    };
    // Two passes: each segment's first pass, a job of its own, before the segments (which wait for all of them).
    if (settings.twoPass)
        for (int k = 0; k < n; ++k) {
            const std::string passId = newJobId();
            ExportSettings x = segmentExportSettings(settings, parts[size_t(k)], folder + sep + segmentFileName(settings, k + 1), seq->fps);
            x.twoPass = false;
            x.pass = 1;
            x.passStats = statsOf(k);
            QJsonObject pj = job;
            pj["name"] = qs(name) + QStringLiteral(" - first pass %1 of %2").arg(k + 1).arg(n);
            pj["stems"] = 0;
            pj["settings"] = exportSettingsToJson(x);
            pj["group"] = qs(jobId);
            pj["segment"] = k + 1;
            pj["segments"] = n;
            pj["pass"] = 1;
            std::string err;
            if (!putJob(farm, passId, pj, proj, &err)) return undo(err);
            made.push_back(passId);
            passes.append(qs(passId));
        }
    for (int k = 0; k < n; ++k) {
        const std::string segId = newJobId();
        ExportSettings x = segmentExportSettings(settings, parts[size_t(k)], folder + sep + segmentFileName(settings, k + 1), seq->fps);
        if (settings.twoPass) {
            x.twoPass = false;
            x.pass = 2;  // (at its share of the bits, worked out when its node takes it)
            x.passStats = statsOf(k);
            x.parameterSetId = k;  // (each segment's own, as their rates differ)
        }
        QJsonObject sj = job;
        sj["name"] = qs(name) + QStringLiteral(" - segment %1 of %2").arg(k + 1).arg(n);
        sj["stems"] = 0;
        sj["settings"] = exportSettingsToJson(x);
        sj["group"] = qs(jobId);
        sj["segment"] = k + 1;
        sj["segments"] = n;
        if (settings.twoPass) {
            sj["pass"] = 2;
            sj["after"] = passes;
        }
        std::string err;
        if (!putJob(farm, segId, sj, proj, &err)) return undo(err);
        made.push_back(segId);
        after.append(qs(segId));
        ranges.append(QJsonArray{double(parts[size_t(k)].in), double(parts[size_t(k)].out)});
    }
    QJsonObject jj = job;
    jj["join"] = true;
    jj["segments"] = n;
    jj["after"] = after;
    jj["parts"] = ranges;
    if (!passes.isEmpty()) jj["passes"] = passes;
    std::string err;
    if (!putJob(farm, jobId, jj, proj, &err)) return undo(err);
    if (id) *id = jobId;
    return true;
}

namespace {

// A package's segments (each coding its share of the pictures) and the join that wraps them. `settingsFor` is given the
// codestreams' folder, the frames, the join's id and the number of segments; `extra` goes in every job.
bool submitPackage(const std::string& farm, const std::string& name, const Project& project, Id sequence, const char* kind,
                   const std::function<QJsonObject(const std::string&, int64_t, int64_t, const std::string&, int)>& settingsFor,
                   int64_t frames, const std::string& parent, const std::string& folderName, int segments, std::string* id,
                   std::string* error, const QJsonObject& extra = {}) {
    if (parent.empty()) return fail(error, "The job has no folder for the package");
    // (Its name taken already: refused now, not once every segment is coded.)
    if (QFileInfo::exists(QDir(qs(parent)).filePath(qs(folderName)))) return fail(error, "There is already a folder called " + folderName + " in " + parent);
    const int n = int(std::clamp<int64_t>(segments, 1, frames));
    const QByteArray proj = QByteArray::fromStdString(projectToJson(project));
    ExportSettings where;
    where.path = parent;  // (what the queue shows as its output)
    QJsonObject job{{"name", qs(name)},
                          {"sequence", QString::number(sequence)},
                          {"stems", 0},
                          {"package", kind},
                          {"settings", exportSettingsToJson(where)},
                          {"submittedBy", QSysInfo::machineHostName()},
                          {"submitted", qs(nowIso())},
                          {"version", 1}};
    for (auto it = extra.begin(); it != extra.end(); ++it) job[it.key()] = it.value();
    const std::string jobId = newJobId();
    const char sep = parent.find('/') == std::string::npos && parent.find('\\') != std::string::npos ? '\\' : '/';
    const std::string folder = (parent.back() == sep ? parent : parent + sep) + ".montage-segments-" + jobId;
    std::vector<std::string> made;
    QJsonArray after, ranges;
    for (int k = 0; k < n; ++k) {
        const int64_t from = frames * k / n, to = frames * (k + 1) / n;
        const std::string segId = newJobId();
        QJsonObject sj = job;
        sj["name"] = qs(name) + QStringLiteral(" - segment %1 of %2").arg(k + 1).arg(n);
        sj["packageSettings"] = settingsFor(folder, from, to, jobId, n);
        sj["group"] = qs(jobId);
        sj["segment"] = k + 1;
        sj["segments"] = n;
        std::string err;
        if (!putJob(farm, segId, sj, proj, &err)) {
            for (const std::string& m : made) takeJobAway(farm, m);
            return fail(error, err);
        }
        made.push_back(segId);
        after.append(qs(segId));
        ranges.append(QJsonArray{double(from), double(to)});
    }
    QJsonObject jj = job;
    jj["packageSettings"] = settingsFor(folder, 0, 0, jobId, n);
    jj["join"] = true;
    jj["segments"] = n;
    jj["after"] = after;
    jj["parts"] = ranges;
    std::string err;
    if (!putJob(farm, jobId, jj, proj, &err)) {
        for (const std::string& m : made) takeJobAway(farm, m);
        return fail(error, err);
    }
    if (id) *id = jobId;
    return true;
}

}  // namespace

bool submitFarmImf(const std::string& farm, const std::string& name, const Project& project, Id sequence, const ImfSettings& settings,
                   const std::string& parent, int segments, std::string* id, std::string* error) {
    if (!openFarm(farm, error)) return false;
    const Sequence* seq = project.findSequence(sequence);
    if (!seq) return fail(error, "The sequence no longer exists");
    if (settings.application != 2) return fail(error, "Only Application #2E masters are made on a render farm");
    if (!settings.supplementOf.empty()) return fail(error, "Supplemental packages are made in one go, beside the package they supplement");
    std::string why;
    const int64_t frames = imfCompositionFrames(*seq, settings, &why);
    if (frames <= 0) return fail(error, why);
    auto settingsFor = [&](const std::string& folder, int64_t from, int64_t to, const std::string&, int) {
        ImfSettings x = settings;
        x.codestreams = folder;
        x.codeFrom = from, x.codeTo = to;
        return imfToJson(x);
    };
    return submitPackage(farm, name, project, sequence, "imf", settingsFor, frames, parent, imfFolderName(settings), segments, id, error);
}

bool submitFarmDcp(const std::string& farm, const std::string& name, const Project& project, Id sequence, const DcpSettings& settings,
                   const std::string& parent, int segments, std::string* id, std::string* error) {
    if (!openFarm(farm, error)) return false;
    const Sequence* seq = project.findSequence(sequence);
    if (!seq) return fail(error, "The sequence no longer exists");
    if (settings.stereo && !seq->stereo3d) return fail(error, "The sequence is not stereoscopic (Sequence Settings: Stereoscopic 3D)");
    // (The DKDM is written by whichever node joins it: somewhere the sender reaches.)
    if (settings.encrypt && settings.dkdms.empty()) return fail(error, "An encrypted DCP made on a render farm needs a folder for its DKDM");
    const int64_t frames = dcpPackageFrames(*seq, settings);
    if (frames < dcpFrameRate(*seq, settings.fps)) return fail(error, "A DCP must last at least a second (cinema servers refuse shorter reels)");
    // An encrypted package's codestreams are sealed for the signer's leaf, so they are never in the clear in the farm's
    // folder. A coding node needs only that certificate; the join is made only by a node holding the chain itself (in
    // its own certificates folder, FarmNodeOptions::certificates), named in the jobs by the leaf's thumbprint. The chain
    // is never to be on the farm's share, where every node (and anyone reading it) would hold the key that opens the
    // codestreams and every DKDM made with it.
    std::string sealFor;
    QJsonObject extra;
    if (settings.encrypt) {
        dcp::CertificateChain chain;
        dcp::CertificateInfo leaf;
        std::string err;
        if (!dcp::securityAvailable()) return fail(error, "This build cannot encrypt (it has no OpenSSL)");
        const std::string certificates = settings.certificates.empty() ? dcp::signerFolder() : settings.certificates;
        for (const std::string& shared : {farm, parent})
            if (keyWithin(certificates, shared))
                return fail(error, "The certificates the package is signed with are in " + certificates +
                                       ", where every render node can read their key: keep them on the computers that join encrypted DCPs only");
        if (!dcp::signerCertificateChain(chain, &err, settings.certificates) || chain.empty() || !dcp::readCertificate(chain.certificates.back(), leaf, &err))
            return fail(error, "Cannot read the certificates the package is signed with: " + err);
        sealFor = chain.certificates.back();
        extra["signer"] = qs(leaf.thumbprint);
    }
    auto settingsFor = [&](const std::string& folder, int64_t from, int64_t to, const std::string& job, int n) {
        DcpSettings x = settings;
        x.codestreamCertificate = sealFor;
        x.codestreamJob = settings.encrypt ? job : std::string();
        x.codestreamSegments = settings.encrypt ? n : 0;
        x.codestreams = folder;
        x.codeFrom = from, x.codeTo = to;
        return dcpToJson(x);
    };
    return submitPackage(farm, name, project, sequence, "dcp", settingsFor, frames, parent, dcpFolderName(*seq, settings), segments, id, error,
                         extra);
}

std::vector<FarmJob> farmJobs(const std::string& farm) {
    std::vector<FarmJob> out;
    const QStringList ids = QDir(jobsDir(farm)).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& id : ids) {
        if (id.startsWith('.') || !validId(ss(id)) || !QFileInfo::exists(QDir(jobDir(farm, ss(id))).filePath(QStringLiteral("job.json")))) continue;
        out.push_back(readJob(farm, ss(id)));
    }
    return out;
}

std::vector<FarmNodeInfo> farmNodes(const std::string& farm) {
    std::vector<FarmNodeInfo> out;
    const QDir d(nodesDir(farm));
    for (const QString& f : d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        const QJsonObject o = readJson(d.filePath(f));
        if (o.isEmpty()) continue;
        FarmNodeInfo n;
        n.name = ss(o.value("name").toString());
        n.host = ss(o.value("host").toString());
        n.state = ss(o.value("state").toString());
        n.job = ss(o.value("job").toString());
        n.updated = ss(o.value("updated").toString());
        n.beat = int64_t(o.value("beat").toDouble());
        const QJsonObject enc = o.value("encoders").toObject();
        for (auto it = enc.begin(); it != enc.end(); ++it) n.encoders[ss(it.key())] = ss(it.value().toString());
        n.signer = ss(o.value("signer").toString());
        n.written = QFileInfo(d.filePath(f)).lastModified().toMSecsSinceEpoch();
        out.push_back(std::move(n));
    }
    return out;
}

bool cancelFarmJob(const std::string& farm, const std::string& id, std::string* error) {
    if (!validId(id) || !QFileInfo::exists(QDir(jobDir(farm, id)).filePath(QStringLiteral("job.json")))) return fail(error, "No job " + id);
    const FarmJob j = readJob(farm, id);
    if (ended(j.state)) return fail(error, "The job has already ended");
    QFile f(QDir(jobDir(farm, id)).filePath(QStringLiteral("cancel")));
    if (!f.open(QIODevice::WriteOnly)) return fail(error, "Cannot write in the render farm folder");
    f.write(QSysInfo::machineHostName().toUtf8());
    f.close();
    if (j.join)
        for (const std::string& seg : groupIds(farm, id))
            if (jobExists(farm, seg) && !ended(readJob(farm, seg).state)) {
                QFile c(QDir(jobDir(farm, seg)).filePath(QStringLiteral("cancel")));
                if (c.open(QIODevice::WriteOnly)) c.write(QSysInfo::machineHostName().toUtf8());
            }
    return true;
}

bool retryFarmJob(const std::string& farm, const std::string& id, std::string* error) {
    if (!validId(id) || !QFileInfo::exists(QDir(jobDir(farm, id)).filePath(QStringLiteral("job.json")))) return fail(error, "No job " + id);
    const FarmJob j = readJob(farm, id);
    if (j.state == FarmState::Rendering || j.state == FarmState::Waiting) return fail(error, "The job has not ended");
    const QDir d(jobDir(farm, id));
    // (The status first: without it and its claim the job waits.)
    // (A join's segments that did not end well wait again first, so it waits for them; one still stopping is waited
    // for, or it would end cancelled after the join waits again.)
    if (j.join)
        for (const std::string& seg : groupIds(farm, id))
            if (jobExists(farm, seg) && !ended(readJob(farm, seg).state))
                return fail(error, "A segment of the job is still stopping: retry it once it has");
    if (j.join)
        for (const std::string& seg : groupIds(farm, id))
            if (jobExists(farm, seg))
                if (const FarmState st = readJob(farm, seg).state; st == FarmState::Failed || st == FarmState::Cancelled) retryFarmJob(farm, seg);
    QFile::remove(d.filePath(QStringLiteral("failures")));
    QFile::remove(d.filePath(QStringLiteral("rechecks")));
    QFile::remove(d.filePath(QStringLiteral("cancel")));
    QFile::remove(d.filePath(QStringLiteral("status.json")));
    QFile::remove(d.filePath(QStringLiteral("claim")));
    return true;
}

bool removeFarmJob(const std::string& farm, const std::string& id, std::string* error) {
    if (!validId(id) || !QFileInfo::exists(jobDir(farm, id))) return fail(error, "No job " + id);
    const FarmJob j = readJob(farm, id);
    if (j.state == FarmState::Rendering) return fail(error, "The job is rendering: cancel it first");
    std::vector<std::string> segs;
    QString folder;
    if (j.join) {
        segs = groupIds(farm, id);
        for (const std::string& seg : segs)
            if (jobExists(farm, seg) && readJob(farm, seg).state == FarmState::Rendering)
                return fail(error, "A segment of the job is rendering: cancel the job first");
        // Its segments' files, when their folder is reachable as the sender named it.
        const QJsonObject jj = readJson(QDir(jobDir(farm, id)).filePath(QStringLiteral("job.json")));
        if (jj.contains("package")) {
            const QFileInfo codestreams(jj.value("packageSettings").toObject().value("codestreams").toString());
            if (codestreams.fileName().startsWith(QStringLiteral(".montage-segments-"))) folder = codestreams.absoluteFilePath();
        } else if (!segs.empty() && jobExists(farm, segs.back())) {
            const QJsonObject sj = readJson(QDir(jobDir(farm, segs.back())).filePath(QStringLiteral("job.json")));
            const QFileInfo first(sj.value("settings").toObject().value("path").toString());
            if (first.dir().dirName().startsWith(QStringLiteral(".montage-segments-"))) folder = first.absolutePath();
        }
    }
    // (Renamed away first, the join before its segments, so no node claims any of them while they go.)
    if (!takeJobAway(farm, id)) return fail(error, "Cannot remove the job");
    for (const std::string& seg : segs) takeJobAway(farm, seg);
    if (!folder.isEmpty()) QDir(folder).removeRecursively();
    return true;
}

bool FarmHeartbeats::stale(const std::string& key, int64_t beat, int64_t age) {
    const int64_t now = nowMs();
    const auto [it, first] = seen_.try_emplace(key);
    Seen& s = it->second;
    if (first || s.beat != beat) {
        // (a beat that moved while watched is fresh; one never seen before is as old as its file)
        s.beat = beat;
        s.since = first ? now - std::max<int64_t>(0, age) : now;
    }
    return now - s.since >= ms_;
}

namespace {

// Takes a job's claim away when `token` holds it: read first (a claim another node holds by now is left where it is, or
// that node would find it gone and stop rendering), then renamed aside (only one rename succeeds), read again, and put
// back when another took the job in between (whose node also finds its claim aside meanwhile: Node::holds). Its status
// goes with it, so the job waits.
bool releaseClaim(const QDir& d, const std::string& token) {
    const QString claim = d.filePath(QStringLiteral("claim"));
    {
        QFile f(claim);
        if (!f.open(QIODevice::ReadOnly) || f.readAll() != QByteArray::fromStdString(token)) return false;
    }
    const QString aside = d.filePath(QStringLiteral(".claim-") + QString::number(QRandomGenerator::global()->generate64(), 16));
    if (!QFile::rename(claim, aside)) return false;
    QFile f(aside);
    const QByteArray held = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    f.close();
    if (held != QByteArray::fromStdString(token)) {
        // (Another's: given back to it; when a node claimed the job meanwhile, that claim stands and this one goes.)
        if (!QFile::rename(aside, claim)) QFile::remove(aside);
        return false;
    }
    QFile::remove(d.filePath(QStringLiteral("status.json")));
    QFile::remove(aside);
    return true;
}

}  // namespace

bool reclaimFarmJob(const std::string& farm, const std::string& id, const std::string& claim, std::string* error) {
    if (!validId(id) || readJob(farm, id).state != FarmState::Rendering) return fail(error, "The job is not rendering");
    if (!releaseClaim(QDir(jobDir(farm, id)), claim)) return fail(error, "The job was taken back already, or another node has it now");
    return true;
}

namespace {

struct Node {
    const FarmNodeOptions& o;
    std::string name, token;
    int64_t beat = 0;
    FarmHeartbeats watched;
    std::map<std::string, int64_t> leftFor;  // segments this node failed, left for other nodes until then (its clock)
    std::map<std::string, std::string> found;  // the hardware encoders found, by family and size
    std::string signer;             // the leaf thumbprint of the DCP signing chain this node joins with (ownSigner)
    int64_t signerRead = -1;        // when its files were last looked at (this node's clock)
    std::string signerFiles;        // their sizes and times then: the key is read again only once one has changed
    std::set<std::string> told;     // what was said once (a job left for other nodes)

    explicit Node(const FarmNodeOptions& options) : o(options), watched(options.staleMs) {
        name = o.name.empty() ? ss(QSysInfo::machineHostName()) : o.name;
        if (name.empty()) name = "node";
        token = name + "-" + std::to_string(QRandomGenerator::global()->generate64());
    }
    void log(const std::string& s) const {
        if (o.log) o.log(s);
    }
    bool stopped() const { return o.stopped && o.stopped(); }
    QString nodeFile() const {
        QString safe = qs(name);
        for (QChar& c : safe)
            if (!(c.isLetterOrNumber() || c == '-' || c == '_' || c == '.')) c = '_';
        return QDir(nodesDir(o.farm)).filePath(safe + QStringLiteral(".json"));
    }
    void heartbeat(const char* state, const std::string& job = {}) {
        QJsonObject enc;
        for (const char* family : {"h264", "hevc"}) {
            Sequence hd;
            hd.width = 1920, hd.height = 1080, hd.fps = {25, 1};
            ExportSettings x;
            x.videoCodec = std::string("hw_") + family;
            enc[family] = qs(encoderFor(x, hd));
        }
        writeJson(nodeFile(), QJsonObject{{"name", qs(name)},
                                          {"host", QSysInfo::machineHostName()},
                                          {"state", state},
                                          {"job", qs(job)},
                                          {"updated", qs(nowIso())},
                                          {"beat", double(++beat)},
                                          {"encoders", enc},
                                          {"signer", qs(ownSigner())}});
    }
    void tellOnce(const std::string& key, const std::string& line) {
        if (told.insert(key).second) log(line);
    }
    // How long before this node's own heartbeat (written each time it looks for work, and while it renders) a file on
    // the share was written, both by the clock that stamps the share's files: how old a heartbeat or lock seen for the
    // first time is (FarmHeartbeats). 0 when either time cannot be read.
    int64_t ageOf(int64_t written) const {
        const QDateTime own = QFileInfo(nodeFile()).lastModified();
        if (!own.isValid() || written <= 0) return 0;
        return std::max<int64_t>(0, own.toMSecsSinceEpoch() - written);
    }
    int64_t ageOf(const QString& file) const {
        const QDateTime at = QFileInfo(file).lastModified();
        return at.isValid() ? ageOf(at.toMSecsSinceEpoch()) : 0;
    }
    // The leaf thumbprint of the signing chain in this node's certificates folder, "" with none there (never made here:
    // a chain made on the spot could open nothing sealed for the sender's), or with one that cannot open what is sealed
    // for its leaf (a file missing or unreadable, or a key that is not the leaf's). Its files looked at every ten
    // seconds, and the chain read again (the private key with it) only when one of them has changed.
    std::string ownSigner() {
        if (signerRead >= 0 && nowMs() - signerRead < 10000) return signer;
        signerRead = nowMs();
        const QDir dir(qs(certificatesFolder()));
        std::string files;
        for (const char* file : {"root.pem", "intermediate.pem", "leaf.pem", "leaf.key"}) {
            const QFileInfo f(dir.filePath(QString::fromLatin1(file)));
            files += f.exists() ? std::to_string((long long)f.size()) + '@' + std::to_string((long long)f.lastModified().toMSecsSinceEpoch()) : "-";
            files += ';';
        }
        if (files == signerFiles) return signer;
        signerFiles = files;
        signer.clear();
        if (!dcp::securityAvailable()) return signer;
        dcp::CertificateChain chain;
        dcp::CertificateInfo info;
        std::string why;
        if (!QFileInfo::exists(dir.filePath(QStringLiteral("leaf.pem")))) return signer;  // (none: nothing to say)
        if (dcp::signerCertificateChain(chain, &why, certificatesFolder(), false) && dcp::chainKeyMatches(chain, &why) &&
            dcp::readCertificate(chain.certificates.back(), info, &why))
            signer = info.thumbprint;
        else
            tellOnce("chain|" + why, "The DCP signing chain in " + certificatesFolder() + " cannot be used, so this node joins no encrypted DCP: " + why);
        dcp::cleanse(chain.privateKey.data(), chain.privateKey.size());
        return signer;
    }
    std::string certificatesFolder() const { return o.certificates.empty() ? dcp::signerFolder() : o.certificates; }
    // An encrypted DCP's join is taken only by a node holding the chain its codestreams were sealed for, and its
    // segments are coded only while such a node answers (this one or another), so none is coded for a join no node can
    // make. Those left wait, said once in the node's log.
    bool maySeal(const FarmJob& j) {
        const std::string mine = ownSigner();
        if (mine == j.signer) return true;
        if (j.join) {
            tellOnce(j.id, "Left the join of " + j.id + " for a node holding the certificates it is signed with (this node's --certificates holds others or none)");
            return false;
        }
        for (const FarmNodeInfo& n : farmNodes(o.farm)) {
            if (n.name == name || n.state == "stopped" || watched.stale("node|" + n.name, n.beat, ageOf(n.written))) continue;
            if (n.signer == j.signer) return true;
        }
        tellOnce(j.group, "The segments of " + j.group +
                              " wait: no node holding the certificates the package is signed with is answering, and only such a node can join it "
                              "(start one with them in its --certificates folder)");
        return false;
    }
    // The encoder this node codes a hardware export of `seq` with: the export's own choice (exportHwEncoder), asked
    // once for each picture.
    std::string encoderFor(const ExportSettings& st, const Sequence& seq) {
        const std::string family = st.videoCodec == "hw_hevc" ? "hevc" : "h264";
        if (const auto it = o.encoders.find(family); it != o.encoders.end()) return it->second;
        int w = 0, h = 0;
        codedPictureSize(seq, st, w, h);
        const std::string key = family + " " + std::to_string(w) + "x" + std::to_string(h) + "@" + std::to_string(seq.fps.num) + "/" +
                                std::to_string(seq.fps.den);
        if (const auto it = found.find(key); it != found.end()) return it->second;
        return found[key] = exportHwEncoder(seq, st);
    }
    // Whether this node may take a segment of a hardware-encoded export: its kind pinned to the export's join by the
    // first node that takes one (a file only one creation of succeeds), the others taking them only with that kind.
    // When no node coding with the pinned kind answers any more, the pin passes to this node if none of the segments
    // was coded with it yet; else the segment cannot be coded to match the others, and `stranded` says why. `pinned`
    // gets the kind the segment is to be coded with, checked again once it is claimed (pinHolds).
    bool mayCodeSegment(const FarmJob& j, std::string* stranded, std::string* pinned) {
        if (j.segment <= 0 || j.group.empty()) return true;
        ExportSettings st;
        const QJsonObject job = readJson(QDir(jobDir(o.farm, j.id)).filePath(QStringLiteral("job.json")));
        if (!exportSettingsFromJson(job.value("settings").toObject(), st)) return true;  // (failed when rendered, saying why)
        if (st.videoCodec != "hw_h264" && st.videoCodec != "hw_hevc") return true;
        // (The sequence's picture as the export codes it, sent with the job: its size, rate and stereoscopy.)
        const QJsonArray pic = job.value("picture").toArray();
        auto at = [&](int i, int otherwise) { return pic.size() >= 4 && pic.at(i).toInt() > 0 ? pic.at(i).toInt() : otherwise; };
        Sequence seq;
        seq.width = at(0, 1920), seq.height = at(1, 1080), seq.fps = {at(2, 25), at(3, 1)};
        seq.stereo3d = pic.size() >= 5 && pic.at(4).toBool();
        const std::string family = st.videoCodec.substr(3);
        const std::string mine = encoderFor(st, seq);
        if (pinned) *pinned = mine;
        const QString pin = QDir(jobDir(o.farm, j.group)).filePath(QStringLiteral("encoder"));
        auto pinIt = [&] {
            QFile f(pin);
            if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
            f.write(QByteArray::fromStdString(mine));
            f.close();
            log("Segments of " + j.group + " are coded with " + mine);
            return true;
        };
        if (pinIt()) return true;
        auto readPin = [&] {
            QByteArray kind;
            QFile r(pin);
            for (int tries = 0; tries < 20 && kind.isEmpty(); ++tries) {  // (another node's, being written)
                if (r.open(QIODevice::ReadOnly)) {
                    kind = r.readAll();
                    r.close();
                }
                if (kind.isEmpty()) std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            return kind;
        };
        const QByteArray kind = readPin();
        if (kind.isEmpty()) return false;
        if (kind == QByteArray::fromStdString(mine)) return true;
        // Another kind's: left to the nodes with it while one answers.
        const std::string was = ss(QString::fromUtf8(kind));
        for (const FarmNodeInfo& n : farmNodes(o.farm)) {
            if (n.name == name || n.state == "stopped" || watched.stale("node|" + n.name, n.beat, ageOf(n.written))) continue;
            if (const auto e = n.encoders.find(family); e != n.encoders.end() && e->second == was) return false;
        }
        // None does: taken over under a lock only one node makes. A node coding a segment claims it first and only then
        // looks for the lock and the pin (pinHolds), and this looks for claims only once it holds the lock, so either this
        // sees that claim and leaves the pin, or that node sees the lock or the new pin and gives its segment back: two
        // kinds never code one export. A lock left by a node that stopped holding it goes once seen unchanged that long.
        // (Each lock its own: this node's token and a number of its own, so one this node makes again is watched afresh.)
        const QString lock = pin + QStringLiteral(".lock");
        const QByteArray me = QByteArray::fromStdString(token + "-" + std::to_string(QRandomGenerator::global()->generate64()));
        {
            QFile l(lock);
            if (!l.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                pinLocked(j.group);  // (another node's, or one left)
                return false;
            }
            l.write(me);
        }
        auto unlock = [&] {
            QFile r(lock);
            const bool ours = r.open(QIODevice::ReadOnly) && r.readAll() == me;
            r.close();
            // (on Windows a file another node is reading cannot go for that moment: tried again)
            for (int tries = 0; ours && tries < 20 && !QFile::remove(lock) && QFileInfo::exists(lock); ++tries)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
        };
        // (Read again under the lock: another node may have taken it over just before.)
        if (readPin() != kind) return unlock(), false;
        // Its segments being coded are waited for (a silent node's are taken back); with one coded already, none can
        // match it.
        bool coded = false;
        for (const std::string& seg : segmentIds(o.farm, j.group)) {
            if (!jobExists(o.farm, seg)) continue;
            const FarmState state = readJob(o.farm, seg).state;  // (claimed with no status yet counts as rendering)
            if (state == FarmState::Rendering) return unlock(), false;
            coded |= state == FarmState::Done;
        }
        if (coded) {
            unlock();
            if (stranded)
                *stranded = "The export's other segments were coded with " + was + ", and no node coding with it is answering: start one, then retry the job";
            return false;
        }
        // (Still this node's lock: one judged left by a stopped node, wrongly, and replaced meanwhile is not.)
        {
            QFile r(lock);
            if (!r.open(QIODevice::ReadOnly) || r.readAll() != me) return unlock(), false;
        }
        QSaveFile f(pin);
        const bool taken = f.open(QIODevice::WriteOnly) && f.write(QByteArray::fromStdString(mine)) == qint64(mine.size()) && f.commit();
        unlock();
        if (taken) log("No node coding with " + was + " answers: segments of " + j.group + " are coded with " + mine);
        return taken;
    }
    // Whether a node is taking the pin of `group` over (its lock). A lock left by a node that stopped while holding it
    // goes once it is as old as a silent node's job is taken back at (still locked this time): that one lock, by what
    // it holds and when it was written, watched here or older than this node's heartbeat by that much. It is renamed
    // aside first (only one rename succeeds) and goes only when that is the lock judged left; another (made meanwhile,
    // after another node took the old one away) is put back, or, when yet another was made meanwhile, its node finds it
    // gone before it changes the pin.
    bool pinLocked(const std::string& group) {
        const QDir d(jobDir(o.farm, group));
        const QString lock = d.filePath(QStringLiteral("encoder.lock"));
        QFile r(lock);
        if (!r.open(QIODevice::ReadOnly)) return QFileInfo::exists(lock);
        const QByteArray held = r.readAll();
        r.close();
        const QDateTime at = QFileInfo(lock).lastModified();
        const std::string key = "lock|" + group + "|" + held.toStdString() + "|" + std::to_string(at.isValid() ? at.toMSecsSinceEpoch() : 0);
        if (!watched.stale(key, 0, at.isValid() ? ageOf(at.toMSecsSinceEpoch()) : 0)) return true;
        watched.forget(key);
        const QString aside = d.filePath(QStringLiteral(".encoder.lock-") + QString::number(QRandomGenerator::global()->generate64(), 16));
        if (!QFile::rename(lock, aside)) return true;  // (gone meanwhile, or open on Windows: looked at again next time)
        QFile a(aside);
        const QByteArray was = a.open(QIODevice::ReadOnly) ? a.readAll() : QByteArray();
        a.close();
        if (was == held && QFileInfo(aside).lastModified() == at) {
            QFile::remove(aside);
            log("Removed the encoder lock of " + group + ", left by a node that stopped");
        } else if (!QFile::rename(aside, lock)) {
            QFile::remove(aside);
        }
        return true;
    }
    // Whether a hardware-encoded segment just claimed is still to be coded with `kind`: no node taking the pin over
    // (its lock) and the pin still `kind`, looked for in that order (mayCodeSegment).
    bool pinHolds(const std::string& group, const std::string& kind) {
        if (pinLocked(group)) return false;
        QFile f(QDir(jobDir(o.farm, group)).filePath(QStringLiteral("encoder")));
        return f.open(QIODevice::ReadOnly) && f.readAll() == QByteArray::fromStdString(kind);
    }
    // Whether this node still holds the job's claim (another may have taken it back while this one was silent). Its
    // claim counts while another node has it aside for a moment, checking whether it was the stale one.
    bool holds(const QDir& d) const {
        const QString claim = d.filePath(QStringLiteral("claim"));
        auto mine = [&](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) && f.readAll() == QByteArray::fromStdString(token);
        };
        if (mine(claim)) return true;
        if (QFileInfo::exists(claim)) return false;  // another node's
        // No claim: one aside holding this node's token counts while there is still none (a claim made meanwhile is
        // another's, and wins); the claim is read again after, as it may have been put back while looking.
        for (const QString& f : d.entryList({QStringLiteral(".claim-*")}, QDir::Files | QDir::Hidden))
            if (mine(d.filePath(f))) return !QFileInfo::exists(claim) || mine(claim);
        return mine(claim);
    }

    // Jobs whose node stopped answering wait again.
    void takeBackStale() {
        for (const FarmJob& j : farmJobs(o.farm)) {
            if (j.state != FarmState::Rendering) continue;
            // (Watched per claim: a job taken by another node is watched afresh.)
            // (A job first seen is as old as its status, or its claim when it has none yet.)
            const QDir d(jobDir(o.farm, j.id));
            const QString status = d.filePath(QStringLiteral("status.json"));
            const std::string key = j.id + "|" + j.claim;
            if (watched.stale(key, j.beat, ageOf(QFileInfo::exists(status) ? status : d.filePath(QStringLiteral("claim"))))) {
                if (reclaimFarmJob(o.farm, j.id, j.claim)) log("Took back " + j.id + " from " + (j.node.empty() ? std::string("a node") : j.node) + ", which stopped answering");
                watched.forget(key);
            }
        }
    }

    // Claims the oldest waiting job; its id, or "".
    std::string claim() {
        for (const FarmJob& j : farmJobs(o.farm)) {
            if (j.state != FarmState::Waiting || j.cancelRequested) continue;
            if (const auto it = leftFor.find(j.id); it != leftFor.end() && nowMs() < it->second) continue;
            // (A join waits for its segments to end, well or not: taken then, it says which did not; a second pass for
            // every first pass likewise.)
            if (j.join || j.pass == 2) {
                const std::vector<std::string> segs = segmentIds(o.farm, j.id);
                if (std::any_of(segs.begin(), segs.end(), [&](const std::string& seg) { return jobExists(o.farm, seg) && !ended(readJob(o.farm, seg).state); }))
                    continue;
            }
            // (An encrypted DCP's: its join only on a node holding the signer's chain, its segments only while one answers.)
            if (!j.signer.empty() && !maySeal(j)) continue;
            std::string stranded, pinned;
            if (!mayCodeSegment(j, &stranded, &pinned) && stranded.empty()) continue;
            const QDir d(jobDir(o.farm, j.id));
            QFile c(d.filePath(QStringLiteral("claim")));
            if (!c.open(QIODevice::WriteOnly | QIODevice::NewOnly)) continue;  // (another node's)
            c.write(QByteArray::fromStdString(token));
            c.close();
            // (A hardware-encoded segment, claimed: given back when the pin was taken over meanwhile.)
            if (stranded.empty() && !pinned.empty() && !pinHolds(j.group, pinned)) {
                releaseClaim(d, token);
                tellOnce("gave back|" + j.group, "Gave back " + j.id + ": the encoder its export is coded with was being changed");
                continue;
            }
            // (A segment no node can code to match the others: it fails, and so its join, saying why.)
            if (!stranded.empty()) {
                writeJson(d.filePath(QStringLiteral("status.json")),
                          QJsonObject{{"state", "failed"}, {"node", qs(name)}, {"error", qs(stranded)}, {"finished", qs(nowIso())}});
                log("Failed " + j.id + ": " + stranded);
                continue;
            }
            // (Cancelled between the look and the claim: ended here.)
            if (QFileInfo::exists(d.filePath(QStringLiteral("cancel")))) {
                writeJson(d.filePath(QStringLiteral("status.json")),
                          QJsonObject{{"state", "cancelled"}, {"node", qs(name)}, {"error", "Cancelled"}, {"finished", qs(nowIso())}});
                continue;
            }
            return j.id;
        }
        return {};
    }

    // Renders a claimed job; whether it ended (false: given back, this node stopping or losing it).
    bool render(const std::string& id) {
        const QDir d(jobDir(o.farm, id));
        const QJsonObject job = readJson(d.filePath(QStringLiteral("job.json")));
        const std::string started = nowIso();
        int64_t jobBeat = 0;
        auto status = [&](const char* state, double progress, const std::string& error = {}, const std::string& encoder = {}) {
            QJsonObject s{{"state", state}, {"progress", progress}, {"node", qs(name)}, {"started", qs(started)}, {"beat", double(++jobBeat)}};
            if (!error.empty()) s["error"] = qs(error);
            if (!encoder.empty()) s["encoder"] = qs(encoder);
            if (std::string(state) != "rendering") s["finished"] = qs(nowIso());
            return holds(d) && writeJson(d.filePath(QStringLiteral("status.json")), s);
        };
        // An ending written until it is (a share busy for a moment), as long as the job is this node's.
        auto finish = [&](const char* state, double progress, const std::string& error = {}, const std::string& encoder = {}) {
            for (int tries = 0; tries < 40 && holds(d); ++tries) {
                if (status(state, progress, error, encoder)) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            return false;
        };
        const int segment = job.value("segment").toInt();
        const bool join = job.value("join").toBool();
        const int pass = job.value("pass").toInt();
        auto failJob = [&](const std::string& e) {
            // A segment is tried again on its own (twice), by whichever node is free.
            if (segment > 0 && !QFileInfo::exists(d.filePath(QStringLiteral("cancel"))) && jobCount(d, "failures") < 2 && holds(d)) {
                setJobCount(d, "failures", jobCount(d, "failures") + 1);
                releaseClaim(d, token);
                // (Left a while for another node, as the failure may be this computer's: a node working alone, or
                // ending when out of work, tries it again itself.)
                if (!o.once) leftFor[id] = nowMs() + std::max<int64_t>(4 * o.pollMs, 15000);
                log("Segment " + id + " failed (" + e + "): it waits to be rendered again");
                return false;
            }
            log("Failed " + id + ": " + e);
            finish("failed", 0, e);
            return true;
        };
        log("Rendering " + id + " (" + ss(job.value("name").toString()) + ")");
        heartbeat("rendering", id);
        status("rendering", 0);
        ExportSettings st;
        std::string err;
        if (!exportSettingsFromJson(job.value("settings").toObject(), st, &err)) return failJob(err);
        // A package's segment or join: its settings (the codestreams' folder, and which frames a segment codes).
        const std::string package = ss(job.value("package").toString());
        ImfSettings imf;
        DcpSettings dcp;
        if (package == "imf" ? !imfFromJson(job.value("packageSettings").toObject(), imf, &err)
                             : package == "dcp" && !dcpFromJson(job.value("packageSettings").toObject(), dcp, &err))
            return failJob(err);
        if (!package.empty() && package != "imf" && package != "dcp") return failJob("The job's package is IMF or DCP, not " + package);
        // The project with every path in it mapped (media, proxies, LUTs, pictures), then the output's.
        QFile pf(d.filePath(QStringLiteral("project.json")));
        if (!pf.open(QIODevice::ReadOnly)) return failJob("Cannot read the job's project");
        QJsonDocument doc = QJsonDocument::fromJson(pf.readAll());
        if (!o.maps.empty()) {
            std::function<QJsonValue(const QJsonValue&)> map = [&](const QJsonValue& v) -> QJsonValue {
                if (v.isString()) return qs(mapFarmPath(ss(v.toString()), o.maps));
                if (v.isArray()) {
                    QJsonArray a;
                    for (const QJsonValue& x : v.toArray()) a.append(map(x));
                    return a;
                }
                if (v.isObject()) {
                    QJsonObject ob = v.toObject();
                    for (auto it = ob.begin(); it != ob.end(); ++it) *it = map(*it);
                    return ob;
                }
                return v;
            };
            doc = QJsonDocument(map(doc.object()).toObject());
            st.path = mapFarmPath(st.path, o.maps);
            st.passStats = mapFarmPath(st.passStats, o.maps);
            st.burnIn.watermark = mapFarmPath(st.burnIn.watermark, o.maps);
            st.credentialCertificate = mapFarmPath(st.credentialCertificate, o.maps);
            st.credentialKey = mapFarmPath(st.credentialKey, o.maps);
            imf.codestreams = mapFarmPath(imf.codestreams, o.maps);
            dcp.codestreams = mapFarmPath(dcp.codestreams, o.maps);
            dcp.certificates = mapFarmPath(dcp.certificates, o.maps);
            dcp.dkdms = mapFarmPath(dcp.dkdms, o.maps);
        }
        // An encrypted DCP's: a segment seals for the leaf its job names, and the join opens them with this node's own
        // chain (claim made sure it is that leaf's), never one at a path the sender named.
        if (package == "dcp" && dcp.encrypt) {
            dcp::CertificateInfo sealFor;
            if (job.value("signer").toString().isEmpty() || !dcp::readCertificate(dcp.codestreamCertificate, sealFor) ||
                sealFor.thumbprint != ss(job.value("signer").toString()))
                return failJob("The certificate the package's codestreams are sealed for is not its signer's");
            if (join) dcp.certificates = o.certificates;
            // (and never one where the package goes, beside the codestreams, for whoever is given it to read)
            if (join && keyWithin(certificatesFolder(), st.path))
                return failJob("This node's DCP certificates are in " + certificatesFolder() + ", inside " + st.path +
                               " where the package goes: anyone given it could read their key, which opens its codestreams and every DKDM "
                               "made with them. Keep them elsewhere on this computer (--certificates) and retry the job");
        }
        const std::string codestreams = package == "imf" ? imf.codestreams : dcp.codestreams;
        Project p;
        if (!projectFromJson(ss(QString::fromUtf8(doc.toJson(QJsonDocument::Compact))), p, &err)) return failJob("Cannot read the job's project: " + err);
        const Id seq = job.value("sequence").toString().toULongLong();
        const int stems = job.value("stems").toInt();
        // A join: every segment done, each with its settings (its file as this node reaches it) and what its node found
        // in it.
        std::vector<std::string> segs;
        std::vector<ExportSegment> parts;
        std::vector<ExportSettings> segSettings;
        std::vector<SegmentCheck> recorded;
        if (join) {
            if (!p.findSequence(seq)) return failJob("The sequence no longer exists");
            segs = segmentIds(o.farm, id);
            const QJsonArray ranges = job.value("parts").toArray();
            if (segs.empty() || ranges.size() != qsizetype(segs.size())) return failJob("The job's segments are not listed");
            for (size_t k = 0; k < segs.size(); ++k) {
                const std::string which = "Segment " + std::to_string(k + 1) + " of " + std::to_string(segs.size());
                if (!jobExists(o.farm, segs[k])) return failJob(which + " was removed");
                const FarmJob sj = readJob(o.farm, segs[k]);
                if (sj.state == FarmState::Cancelled) return failJob(which + " was cancelled");
                if (sj.state != FarmState::Done) return failJob(which + " failed" + (sj.node.empty() ? std::string() : " on " + sj.node) + ": " + sj.error);
                const QDir sd(jobDir(o.farm, segs[k]));
                ExportSettings x;
                if (!exportSettingsFromJson(readJson(sd.filePath(QStringLiteral("job.json"))).value("settings").toObject(), x, &err)) return failJob(err);
                x.path = mapFarmPath(x.path, o.maps);
                segSettings.push_back(x);
                const QJsonObject c = readJson(sd.filePath(QStringLiteral("segment.json")));
                recorded.push_back({int64_t(c.value("frames").toDouble()), int64_t(c.value("bytes").toDouble()), ss(c.value("sha256").toString())});
                const QJsonArray r = ranges.at(qsizetype(k)).toArray();
                parts.push_back({FrameTime(r.at(0).toDouble()), FrameTime(r.at(1).toDouble())});
            }
        }
        // A second pass: its share of the bits, by what every segment's first pass measured.
        if (pass == 2) {
            const QJsonObject group = readJson(QDir(jobDir(o.farm, job.value("group").toString().toStdString())).filePath(QStringLiteral("job.json")));
            const QJsonArray firsts = group.value("passes").toArray(), ranges = group.value("parts").toArray();
            if (firsts.isEmpty() || firsts.size() != ranges.size() || segment < 1 || segment > firsts.size())
                return failJob("The job's first passes are not listed");
            std::vector<ExportSegment> all;
            std::vector<int64_t> measured;
            for (qsizetype k = 0; k < firsts.size(); ++k) {
                const std::string first = ss(firsts.at(k).toString());
                const std::string which = "The first pass of segment " + std::to_string(k + 1);
                if (!jobExists(o.farm, first)) return failJob(which + " was removed");
                const FarmJob fj = readJob(o.farm, first);
                if (fj.state != FarmState::Done) return failJob(which + (fj.state == FarmState::Cancelled ? " was cancelled" : " failed: " + fj.error));
                measured.push_back(int64_t(readJson(QDir(jobDir(o.farm, first)).filePath(QStringLiteral("pass.json"))).value("bytes").toDouble()));
                const QJsonArray r = ranges.at(k).toArray();
                all.push_back({FrameTime(r.at(0).toDouble()), FrameTime(r.at(1).toDouble())});
            }
            st.videoBitrate = twoPassBitrates(st.videoBitrate, all, measured)[size_t(segment - 1)];
        }
        // Rendered into a folder of this claim's own beside the output, and moved into place only while the job is
        // still this node's: a node that lost it (silent too long) never touches what another renders there.
        const QFileInfo out(qs(st.path));
        const QDir outDir(package.empty() ? out.absolutePath() : out.absoluteFilePath());  // (a package's: the folder it goes in)
        const QString scratch = outDir.filePath(QStringLiteral(".montage-farm-") + QString::number(QRandomGenerator::global()->generate64(), 16));
        if (!QDir().mkpath(scratch)) return failJob("Cannot write in the output's folder " + ss(outDir.path()));
        const std::string finalPath = st.path;
        st.path = ss(QDir(scratch).filePath(out.fileName()));
        const QString ownCodestreams = QDir(scratch).filePath(QStringLiteral("codestreams"));  // (a package segment's)
        // A first pass's statistics written there too, and put beside the segments only while the job is this node's.
        const std::string sharedStats = st.passStats;
        if (pass == 1) st.passStats = ss(QDir(scratch).filePath(QFileInfo(qs(sharedStats)).fileName()));

        // The render on its own thread; here, every half second, its status and heartbeats, and whether to stop.
        std::atomic<bool> cancel{false}, done{false};
        std::atomic<double> progress{0};
        bool ok = false, kept = false;
        std::string encoder;
        err.clear();
        std::vector<size_t> bad;  // a join's segments that did not check out
        SegmentCheck made;        // a segment's
        std::string dkdm;         // an encrypted DCP join's, removed unless its package is put in place
        bool placed = false;
        std::thread worker([&] {
            const ExportProgress report = [&progress](double f, FrameTime) { progress = f; };
            const Sequence* sequence = p.findSequence(seq);
            if (!package.empty() && !sequence) {
                err = "The sequence no longer exists";
            } else if (!package.empty() && join) {
                // Each segment's codestreams checked (as its node hashed them), then wrapped with the rest of the package.
                const double checking = 0.2;
                const bool stereo = package == "dcp" && dcp.stereo;
                size_t unread = 0;
                for (size_t k = 0; k < parts.size() && !cancel; ++k) {
                    SegmentCheck c;
                    const auto part = [&](double f) { progress = checking * (double(k) + f) / double(parts.size()); };
                    const bool read = checkCodestreams(codestreams, parts[k].in, parts[k].out, stereo, c, &err, &cancel, part) && c.frames > 0;
                    unread += !read;
                    if (!read || c.frames != parts[k].out - parts[k].in || c.sha256 != recorded[k].sha256) bad.push_back(k);
                }
                if (!cancel && unread == parts.size()) {
                    bad.clear();
                    err = "This node cannot read the segments' pictures in " + codestreams + ": check its --map for the package's folder";
                }
                const auto wrapping = [&progress, &cancel, checking](double f) {
                    progress = checking + (1 - checking) * f;
                    return !cancel;
                };
                if (cancel) err = "Cancelled";
                else if (bad.empty() && unread < parts.size()) {
                    err.clear();
                    const std::string into = ss(scratch);
                    if (package == "imf") {
                        ImfSettings x = imf;
                        x.codeFrom = x.codeTo = 0;
                        ok = exportImf(p, *sequence, x, into, nullptr, wrapping, &err);
                    } else {
                        DcpSettings x = dcp;
                        x.codeFrom = x.codeTo = 0;
                        DcpResult result;
                        ok = exportDcp(p, *sequence, x, into, &result, wrapping, &err);
                        dkdm = result.dkdm;
                    }
                }
            } else if (!package.empty()) {
                // A package's segment: its share of the pictures coded into this claim's folder, then counted and
                // hashed for the join (and moved beside the others' only while the job is this node's).
                const auto coding = [&progress, &cancel](double f) {
                    progress = 0.95 * f;
                    return !cancel;
                };
                const int64_t from = package == "imf" ? imf.codeFrom : dcp.codeFrom, to = package == "imf" ? imf.codeTo : dcp.codeTo;
                ImfSettings ix = imf;
                DcpSettings dx = dcp;
                ix.codestreams = dx.codestreams = ss(ownCodestreams);
                // (The package's folder named now, not once every segment is coded.)
                const QString named = qs(package == "imf" ? imfFolderName(imf) : dcpFolderName(*sequence, dcp));
                if (QFileInfo::exists(outDir.filePath(named)))
                    err = "There is already a folder called " + ss(named) + " in " + ss(outDir.path());
                else
                    ok = package == "imf" ? exportImf(p, *sequence, ix, ss(scratch), nullptr, coding, &err)
                                          : exportDcp(p, *sequence, dx, ss(scratch), nullptr, coding, &err);
                if (ok) ok = checkCodestreams(ss(ownCodestreams), from, to, package == "dcp" && dcp.stereo, made, &err, &cancel);
                if (ok && made.frames != to - from) {
                    ok = false;
                    err = "The segment has " + std::to_string(made.frames) + " pictures, not " + std::to_string(to - from);
                }
            } else if (join) {
                // Each segment checked (its frames and bytes as its node found them), then joined.
                const double checking = 0.3;
                size_t unread = 0;
                std::string unreadWhy;
                for (size_t k = 0; k < segSettings.size() && !cancel; ++k) {
                    SegmentCheck c;
                    std::string why;
                    const auto part = [&](double f) { progress = checking * (double(k) + f) / double(segSettings.size()); };
                    const bool read = checkExportSegment(segSettings[k], c, &why, &cancel, part) && c.frames > 0;
                    if (!read) {
                        ++unread;
                        if (unreadWhy.empty()) unreadWhy = why.empty() ? "no frames at " + segSettings[k].path : why;
                    }
                    if (!read || c.frames != parts[k].out - parts[k].in || c.sha256 != recorded[k].sha256) bad.push_back(k);
                }
                // None of them readable here: this node's path to them (its maps, the share) rather than the segments.
                if (!cancel && unread == segSettings.size()) {
                    bad.clear();
                    err = "This node cannot read the segments (" + unreadWhy + "): check its --map for the output's folder";
                }
                std::vector<std::string> files;
                if (!exportIsImageSequence(st))
                    for (const ExportSettings& x : segSettings) files.push_back(x.path);
                if (cancel) err = "Cancelled";
                else if (bad.empty() && err.empty())
                    ok = joinExportSegments(p, *p.findSequence(seq), st, stems, files, parts,
                                            [&progress, checking](double f, FrameTime) { progress = checking + (1 - checking) * f; }, &cancel, &err);
            } else {
                ok = renderExport(p, seq, st, stems, report, &cancel, &err, &encoder, &kept);
                // A first pass: what it took at constant quality, its measure (the statistics are beside the segments).
                if (ok && pass == 1) made.bytes = QFileInfo(qs(st.path)).size();
                // A segment: its frames counted and its bytes hashed, for the join to check against.
                else if (ok && segment > 0) {
                    ok = checkExportSegment(st, made, &err, &cancel);
                    if (ok && made.frames != st.out - st.in) {
                        ok = false;
                        err = "The segment has " + std::to_string(made.frames) + " frames, not " + std::to_string(st.out - st.in);
                    }
                }
            }
            done = true;
        });
        bool cancelled = false, givenBack = false, lost = false;
        int64_t last = nowMs();
        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (nowMs() - last < 500) continue;
            last = nowMs();
            if (QFileInfo::exists(d.filePath(QStringLiteral("cancel")))) cancelled = cancel = true;
            if (stopped()) givenBack = cancel = true;
            if (!holds(d)) lost = cancel = true;
            if (!cancel) {
                status("rendering", progress);
                heartbeat("rendering", id);
            }
        }
        worker.join();
        // The finished files (the output, its stems and sidecars, an image sequence's frames) put in place; an export
        // that could not be signed too (it is kept, unsigned, and the job fails saying where).
        if (ok && !package.empty() && !join && holds(d)) {
            // (A package segment's codestreams, beside the other segments'.)
            if (!QDir().mkpath(qs(codestreams))) {
                ok = false;
                err = "Cannot write in " + codestreams;
            }
            for (const QFileInfo& f : QDir(ownCodestreams).entryInfoList(QDir::Files)) {
                const QString to = QDir(qs(codestreams)).filePath(f.fileName());
                QFile::remove(to);
                if (ok && !QFile::rename(f.filePath(), to)) {
                    ok = false;
                    err = "Cannot move the segment's pictures into " + codestreams;
                }
            }
        } else if ((ok || kept) && holds(d) && pass != 1) {
            // (A package's folder, which never replaces one there.)
            for (const QFileInfo& f : QDir(scratch).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString to = outDir.filePath(f.fileName());
                if (package.empty()) continue;
                if (QFileInfo::exists(to)) {
                    ok = false;
                    err = "There is already a folder called " + ss(f.fileName()) + " in " + ss(outDir.path());
                } else if (!QDir().rename(f.filePath(), to)) {
                    ok = false;
                    err = "Cannot move the package into place as " + ss(to);
                } else {
                    placed = true;
                }
            }
            for (const QFileInfo& f : QDir(scratch).entryInfoList(QDir::Files)) {
                const QString to = outDir.filePath(f.fileName());
                QFile::remove(to);
                if (!QFile::rename(f.filePath(), to)) {
                    ok = kept = false;
                    err = "Cannot move the render into place as " + ss(to);
                }
            }
            if (kept)
                if (const size_t at = err.rfind(st.path); at != std::string::npos) err.replace(at, st.path.size(), finalPath);
        }
        if (ok && pass == 1 && holds(d)) {
            // (Its encoder's files: the statistics and those beside them, such as x264's .mbtree and x265's .cutree.)
            const QString stem = QFileInfo(qs(sharedStats)).fileName();
            const QDir statsDir = QFileInfo(qs(sharedStats)).absoluteDir();
            for (const QFileInfo& f : QDir(scratch).entryInfoList(QDir::Files | QDir::Hidden))
                if (f.fileName().startsWith(stem)) {
                    QFile::remove(statsDir.filePath(f.fileName()));
                    if (!QFile::rename(f.filePath(), statsDir.filePath(f.fileName()))) {
                        ok = false;
                        err = "Cannot move the first pass's statistics to " + ss(statsDir.path());
                    }
                }
        }
        QDir(scratch).removeRecursively();
        // (The keys of a package that was not put in place: no DKDM is left for it.)
        if (!dkdm.empty() && !placed) QFile::remove(qs(dkdm));
        if (ok && pass == 1 && holds(d) && !writeJson(d.filePath(QStringLiteral("pass.json")), QJsonObject{{"bytes", double(made.bytes)}})) {
            ok = false;
            err = "Cannot write the first pass's measure in the render farm folder";
        }
        if (ok && segment > 0 && pass != 1 && holds(d) &&
            !writeJson(d.filePath(QStringLiteral("segment.json")),
                       QJsonObject{{"frames", double(made.frames)}, {"bytes", double(made.bytes)}, {"sha256", qs(made.sha256)}})) {
            ok = false;
            err = "Cannot write the segment's check in the render farm folder";
        }
        if (lost || (ok && !holds(d))) {
            log("Lost " + id + " (taken back by another node)");
            return false;
        }
        if (givenBack && !ok) {
            // Left for another node: no status, no claim (unless another has it by now).
            releaseClaim(d, token);
            log("Gave back " + id);
            return false;
        }
        if (!bad.empty() && !cancelled && !givenBack) {
            // Segments that do not check out (changed or lost on the way) are rendered again alone, and the join waits
            // for them; after two rounds of that, it fails.
            std::string which;
            for (size_t k : bad) which += (which.empty() ? "" : ", ") + std::to_string(k + 1);
            if (jobCount(d, "rechecks") >= 2) return failJob("Segment " + which + " does not check out (its frames or bytes are not what its node wrote)");
            setJobCount(d, "rechecks", jobCount(d, "rechecks") + 1);
            for (size_t k : bad) retryFarmJob(o.farm, segs[k]);
            releaseClaim(d, token);
            log("Segment " + which + " of " + id + " did not check out: rendered again before the join");
            return false;
        }
        if (ok) {
            // The segments' files, joined, go once the join is done (an image sequence's frames are the output).
            const bool finished = finish("done", 1, {}, encoder);
            if (finished && join && !package.empty()) {
                if (QFileInfo(qs(codestreams)).fileName().startsWith(QStringLiteral(".montage-segments-"))) QDir(qs(codestreams)).removeRecursively();
            } else if (finished && join && !exportIsImageSequence(st) && !segSettings.empty()) {
                const QFileInfo first(qs(segSettings.front().path));
                if (first.dir().dirName().startsWith(QStringLiteral(".montage-segments-"))) QDir(first.absolutePath()).removeRecursively();
            }
            log("Done " + id + " -> " + finalPath);
        } else if (cancelled) {
            finish("cancelled", progress, "Cancelled");
            log("Cancelled " + id);
        } else {
            return failJob(err);
        }
        return true;
    }
};

}  // namespace

bool runFarmNode(const FarmNodeOptions& options, int* rendered, std::string* error) {
    if (rendered) *rendered = 0;
    if (!openFarm(options.farm, error)) return false;
    // The DCP signing chain this node joins with is never on the farm's share, where every node (and anyone reading it)
    // would hold the key that opens every encrypted DCP's codestreams and DKDMs (submitFarmDcp refuses the sender's).
    const std::string certificates = options.certificates.empty() ? dcp::signerFolder() : options.certificates;
    if (keyWithin(certificates, options.farm))
        return fail(error, "This node's DCP certificates folder " + certificates + " is in the render farm's folder " + options.farm +
                               ", where every render node can read their key: keep them on the computers that join encrypted DCPs only (--certificates)");
    Node n(options);
    n.log("Node " + n.name + " on " + options.farm);
    while (!n.stopped()) {
        n.heartbeat("idle");
        n.takeBackStale();
        const std::string id = n.claim();
        if (!id.empty()) {
            if (n.render(id) && rendered) ++*rendered;
            continue;
        }
        if (options.once) break;
        // (Looking now and then for work, ready to stop.)
        for (int waited = 0; waited < options.pollMs && !n.stopped(); waited += 100) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    n.heartbeat("stopped");
    return true;
}

}  // namespace montage
