#include "RenderFarm.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSysInfo>
#include <chrono>
#include <thread>

#include "core/ProjectIO.h"

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
    v("burnInCaptions", s.burnInCaptions);
    v("embedCaptions", s.embedCaptions);
    v("captionTrack", s.captionTrack);
    v("cea608", s.cea608);
    v("cea708", s.cea708);
    v("colorSpace", s.colorSpace);
    v("trimTarget", s.trimTarget);
    v("hdr10Plus", s.hdr10Plus);
    v("loudnessTarget", s.loudnessTarget);
    v("peakCeiling", s.peakCeiling);
    v("dialogueGated", s.dialogueGated);
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
    v("credentialCertificate", s.credentialCertificate);
    v("credentialKey", s.credentialKey);
    v("credentialAllowAiTraining", s.credentialAllowAiTraining);
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
                   int stems, std::string* id, std::string* error) {
    if (!openFarm(farm, error)) return false;
    if (!project.findSequence(sequence)) return fail(error, "The sequence no longer exists");
    if (settings.path.empty()) return fail(error, "The job has no output file");
    const std::string jobId = ss(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz-")) +
                                 QString::number(QRandomGenerator::global()->bounded(0x1000000u), 16).rightJustified(6, '0'));
    // Written aside, then put in place whole: a node never sees half a job.
    const QDir jobs(jobsDir(farm));
    const QString incoming = jobs.filePath(QStringLiteral(".incoming-") + qs(jobId));
    if (!QDir().mkpath(incoming)) return fail(error, "Cannot write in the render farm folder " + farm);
    auto drop = [&](const std::string& e) {
        QDir(incoming).removeRecursively();
        return fail(error, e);
    };
    const QJsonObject job{{"name", qs(name)},
                          {"sequence", QString::number(sequence)},
                          {"stems", stems},
                          {"settings", exportSettingsToJson(settings)},
                          {"submittedBy", QSysInfo::machineHostName()},
                          {"submitted", qs(nowIso())},
                          {"version", 1}};
    QSaveFile p(QDir(incoming).filePath(QStringLiteral("project.json")));
    if (!p.open(QIODevice::WriteOnly)) return drop("Cannot write the job's project");
    p.write(QByteArray::fromStdString(projectToJson(project)));
    if (!p.commit()) return drop("Cannot write the job's project");
    if (!writeJson(QDir(incoming).filePath(QStringLiteral("job.json")), job)) return drop("Cannot write the job");
    if (!QDir().rename(incoming, jobs.filePath(qs(jobId)))) return drop("Cannot put the job in the render farm");
    if (id) *id = jobId;
    return true;
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
    return true;
}

bool retryFarmJob(const std::string& farm, const std::string& id, std::string* error) {
    if (!validId(id) || !QFileInfo::exists(QDir(jobDir(farm, id)).filePath(QStringLiteral("job.json")))) return fail(error, "No job " + id);
    const FarmJob j = readJob(farm, id);
    if (j.state == FarmState::Rendering || j.state == FarmState::Waiting) return fail(error, "The job has not ended");
    const QDir d(jobDir(farm, id));
    // (The status first: without it and its claim the job waits.)
    QFile::remove(d.filePath(QStringLiteral("cancel")));
    QFile::remove(d.filePath(QStringLiteral("status.json")));
    QFile::remove(d.filePath(QStringLiteral("claim")));
    return true;
}

bool removeFarmJob(const std::string& farm, const std::string& id, std::string* error) {
    if (!validId(id) || !QFileInfo::exists(jobDir(farm, id))) return fail(error, "No job " + id);
    if (readJob(farm, id).state == FarmState::Rendering) return fail(error, "The job is rendering: cancel it first");
    // (Renamed away first, so no node claims it while it goes.)
    const QString gone = QDir(jobsDir(farm)).filePath(QStringLiteral(".removed-") + qs(id));
    if (!QDir().rename(jobDir(farm, id), gone)) return fail(error, "Cannot remove the job");
    QDir(gone).removeRecursively();
    return true;
}

bool FarmHeartbeats::stale(const std::string& key, int64_t beat) {
    const int64_t now = nowMs();
    Seen& s = seen_[key];
    if (s.beat != beat) {
        s.beat = beat;
        s.since = now;
        return false;
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
        QFile::rename(aside, claim);  // (another's: given back to it)
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
        writeJson(nodeFile(), QJsonObject{{"name", qs(name)},
                                          {"host", QSysInfo::machineHostName()},
                                          {"state", state},
                                          {"job", qs(job)},
                                          {"updated", qs(nowIso())},
                                          {"beat", double(++beat)}});
    }
    // Whether this node still holds the job's claim (another may have taken it back while this one was silent). Its
    // claim counts while another node has it aside for a moment, checking whether it was the stale one.
    bool holds(const QDir& d) const {
        auto mine = [&](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) && f.readAll() == QByteArray::fromStdString(token);
        };
        if (mine(d.filePath(QStringLiteral("claim")))) return true;
        for (const QString& f : d.entryList({QStringLiteral(".claim-*")}, QDir::Files | QDir::Hidden))
            if (mine(d.filePath(f))) return true;
        return false;
    }

    // Jobs whose node stopped answering wait again.
    void takeBackStale() {
        for (const FarmJob& j : farmJobs(o.farm)) {
            if (j.state != FarmState::Rendering) continue;
            // (Watched per claim: a job taken by another node is watched afresh.)
            const std::string key = j.id + "|" + j.claim;
            if (watched.stale(key, j.beat)) {
                if (reclaimFarmJob(o.farm, j.id, j.claim)) log("Took back " + j.id + " from " + (j.node.empty() ? std::string("a node") : j.node) + ", which stopped answering");
                watched.forget(key);
            }
        }
    }

    // Claims the oldest waiting job; its id, or "".
    std::string claim() {
        for (const FarmJob& j : farmJobs(o.farm)) {
            if (j.state != FarmState::Waiting || j.cancelRequested) continue;
            const QDir d(jobDir(o.farm, j.id));
            QFile c(d.filePath(QStringLiteral("claim")));
            if (!c.open(QIODevice::WriteOnly | QIODevice::NewOnly)) continue;  // (another node's)
            c.write(QByteArray::fromStdString(token));
            c.close();
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
        auto failJob = [&](const std::string& e) {
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
            st.burnIn.watermark = mapFarmPath(st.burnIn.watermark, o.maps);
            st.credentialCertificate = mapFarmPath(st.credentialCertificate, o.maps);
            st.credentialKey = mapFarmPath(st.credentialKey, o.maps);
        }
        Project p;
        if (!projectFromJson(ss(QString::fromUtf8(doc.toJson(QJsonDocument::Compact))), p, &err)) return failJob("Cannot read the job's project: " + err);
        const Id seq = job.value("sequence").toString().toULongLong();
        const int stems = job.value("stems").toInt();
        // Rendered into a folder of this claim's own beside the output, and moved into place only while the job is
        // still this node's: a node that lost it (silent too long) never touches what another renders there.
        const QFileInfo out(qs(st.path));
        const QDir outDir(out.absolutePath());
        const QString scratch = outDir.filePath(QStringLiteral(".montage-farm-") + QString::number(QRandomGenerator::global()->generate64(), 16));
        if (!QDir().mkpath(scratch)) return failJob("Cannot write in the output's folder " + ss(outDir.path()));
        const std::string finalPath = st.path;
        st.path = ss(QDir(scratch).filePath(out.fileName()));

        // The render on its own thread; here, every half second, its status and heartbeats, and whether to stop.
        std::atomic<bool> cancel{false}, done{false};
        std::atomic<double> progress{0};
        bool ok = false, kept = false;
        std::string encoder;
        err.clear();
        std::thread worker([&] {
            ok = renderExport(p, seq, st, stems, [&progress](double f, FrameTime) { progress = f; }, &cancel, &err, &encoder, &kept);
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
        if ((ok || kept) && holds(d)) {
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
        QDir(scratch).removeRecursively();
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
        if (ok) {
            finish("done", 1, {}, encoder);
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
