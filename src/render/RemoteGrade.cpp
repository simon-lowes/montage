#include "render/RemoteGrade.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "core/ProjectIO.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

namespace montage::remote {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kVersion = 1;
constexpr uint32_t kMaxWork = 512u << 20;  // a message from the colourist
constexpr uint32_t kMaxBack = 2u << 20;    // one from the client
constexpr auto kHashEvery = std::chrono::seconds(2);
constexpr auto kProjectEvery = std::chrono::milliseconds(100);
constexpr size_t kMaxQueued = 1000;

std::string averr(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof buf);
    return buf;
}

// The address with another port (an IPv6 host in brackets).
std::string withPort(const std::string& host, int port) {
    const bool v6 = host.find(':') != std::string::npos;
    return "srt://" + (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

// A reliable SRT stream (file mode), AES-256 with the passphrase.
int openSrt(AVIOContext** pb, const std::string& url, bool listen, bool write, const GradeSettings& s, const AVIOInterruptCB* cb) {
    AVDictionary* d = nullptr;
    av_dict_set(&d, "mode", listen ? "listener" : "caller", 0);
    av_dict_set(&d, "transtype", "file", 0);
    av_dict_set_int(&d, "latency", int64_t(std::clamp(s.latencyMs, 20, 8000)) * 1000, 0);
    av_dict_set(&d, "passphrase", s.passphrase.c_str(), 0);
    av_dict_set_int(&d, "pbkeylen", 32, 0);
    if (!listen) av_dict_set_int(&d, "connect_timeout", 3000, 0);
    const int r = avio_open2(pb, url.c_str(), write ? AVIO_FLAG_WRITE : AVIO_FLAG_READ, cb, &d);
    av_dict_free(&d);
    return r;
}

// Each message: its length (4 bytes, big-endian), then its JSON (not compressed: nothing a peer sends grows on
// reading past the length it gives).
bool writeMessage(AVIOContext* pb, const QJsonObject& o, int64_t* bytes = nullptr) {
    const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Compact);
    if (json.size() > qsizetype(kMaxWork)) return false;
    avio_wb32(pb, uint32_t(json.size()));
    avio_write(pb, reinterpret_cast<const unsigned char*>(json.constData()), int(json.size()));
    avio_flush(pb);
    if (bytes) *bytes += 4 + json.size();
    return pb->error == 0;
}

bool readMessage(AVIOContext* pb, uint32_t limit, QJsonObject& out) {
    const uint32_t n = avio_rb32(pb);
    if (pb->error || pb->eof_reached || n < 2 || n > limit) return false;
    QByteArray json(qsizetype(n), Qt::Uninitialized);
    if (avio_read(pb, reinterpret_cast<unsigned char*>(json.data()), int(n)) != int(n)) return false;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    out = doc.object();
    return true;
}

QByteArray compact(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

std::string hashOf(const QJsonObject& o) { return QCryptographicHash::hash(compact(o), QCryptographicHash::Sha256).toHex().toStdString(); }

std::string hashBytes(const std::string& b) {
    if (b.empty()) return {};
    return QCryptographicHash::hash(QByteArray::fromStdString(b), QCryptographicHash::Sha256).toHex().toStdString();
}

// ---- Patches ----

void diff(const QJsonValue& a, const QJsonValue& b, std::vector<QJsonValue>& path, QJsonArray& ops) {
    if (a == b) return;
    auto here = [&] {
        QJsonArray p;
        for (const QJsonValue& v : path) p.append(v);
        return p;
    };
    if (a.isObject() && b.isObject()) {
        const QJsonObject oa = a.toObject(), ob = b.toObject();
        for (auto it = oa.begin(); it != oa.end(); ++it)
            if (!ob.contains(it.key())) {
                path.push_back(it.key());
                ops.append(QJsonObject{{"p", here()}, {"x", 1}});
                path.pop_back();
            }
        for (auto it = ob.begin(); it != ob.end(); ++it) {
            path.push_back(it.key());
            if (!oa.contains(it.key())) ops.append(QJsonObject{{"p", here()}, {"v", it.value()}});
            else diff(oa.value(it.key()), it.value(), path, ops);
            path.pop_back();
        }
        return;
    }
    if (a.isArray() && b.isArray() && a.toArray().size() == b.toArray().size()) {
        const QJsonArray aa = a.toArray(), ab = b.toArray();
        for (qsizetype i = 0; i < aa.size(); ++i) {
            path.push_back(double(i));
            diff(aa.at(i), ab.at(i), path, ops);
            path.pop_back();
        }
        return;
    }
    ops.append(QJsonObject{{"p", here()}, {"v", b}});
}

// Sets (or, with no value, removes) what `p` names from step `i` on.
bool patchAt(QJsonValue& v, const QJsonArray& p, qsizetype i, const QJsonValue* value) {
    if (i == p.size()) {
        if (!value) return false;
        v = *value;
        return true;
    }
    if (i > 64) return false;
    const QJsonValue step = p.at(i);
    if (step.isString()) {
        if (!v.isObject()) return false;
        QJsonObject o = v.toObject();
        v = QJsonValue();  // (o alone holds it: changed without a copy)
        const QString key = step.toString();
        if (i + 1 == p.size() && !value) {
            if (!o.contains(key)) return false;
            o.remove(key);
        } else {
            if (i + 1 < p.size() && !o.contains(key)) return false;
            QJsonValue child = o.value(key);
            o.remove(key);
            if (!patchAt(child, p, i + 1, value)) return false;
            o.insert(key, child);
        }
        v = o;
        return true;
    }
    if (!step.isDouble() || !v.isArray()) return false;
    QJsonArray a = v.toArray();
    v = QJsonValue();
    const double d = step.toDouble();
    if (d < 0 || d >= double(a.size()) || d != std::floor(d) || (i + 1 == p.size() && !value)) return false;
    const qsizetype at = qsizetype(d);
    QJsonValue child = a.at(at);
    a.replace(at, QJsonValue());
    if (!patchAt(child, p, i + 1, value)) return false;
    a.replace(at, child);
    v = a;
    return true;
}

bool applyOps(QJsonObject& doc, const QJsonArray& ops) {
    QJsonValue v = doc;
    doc = QJsonObject();
    for (const QJsonValue& op : ops) {
        const QJsonObject o = op.toObject();
        const QJsonArray p = o.value("p").toArray();
        if (p.isEmpty()) return false;
        const QJsonValue value = o.value("v");
        if (!patchAt(v, p, 0, o.contains("v") ? &value : nullptr)) return false;
    }
    if (!v.isObject()) return false;
    doc = v.toObject();
    return true;
}

QJsonObject markerJson(const output::RemoteMarker& m) {
    return {{"t", "note"},          {"frame", double(m.frame)}, {"duration", double(m.duration)}, {"sequence", double(m.sequence)},
            {"timecode", QString::fromStdString(m.timecode)}, {"author", QString::fromStdString(m.author)},
            {"text", QString::fromStdString(m.text)},         {"drawing", QString::fromStdString(m.drawing)}, {"id", QString::fromStdString(m.id)}};
}

std::string text(const QJsonObject& o, const char* key, qsizetype most) { return o.value(key).toString().left(most).toStdString(); }

FrameTime frameOf(const QJsonObject& o, const char* key) {
    const double d = o.value(key).toDouble();
    return std::isfinite(d) ? FrameTime(std::clamp(d, -1e15, 1e15)) : 0;
}

uint64_t revOf(const QJsonObject& o) { return uint64_t(std::max<FrameTime>(0, frameOf(o, "rev"))); }

}  // namespace

std::string checkGradeSettings(const GradeSettings& s) {
    std::string scheme, host;
    int port = 0;
    if (!output::parseStreamAddress(s.address, scheme, host, port) || scheme != "srt")
        return "The address is srt://host:port (srt://0.0.0.0:9100 to take a client on any of this computer's addresses)";
    if (port >= 65535) return "The notes come back on the next port, so the port must be below 65535";
    if (s.passphrase.size() < 10 || s.passphrase.size() > 79) return "Remote grading needs a passphrase of 10 to 79 characters";
    return {};
}

bool gradingAvailable(std::string* why) {
    void* it = nullptr;
    while (const char* name = avio_enum_protocols(&it, 1))
        if (std::string(name) == "srt") return true;
    if (why) *why = "This FFmpeg was built without SRT (libsrt)";
    return false;
}

FrameTime followPlayhead(const ViewState& v, Rational fps, int64_t ageMs) {
    if (!v.playing || fps.num <= 0 || fps.den <= 0) return v.playhead;
    const double moved = double(std::max<int64_t>(0, ageMs)) / 1000.0 * fps.toDouble() * v.speed;
    return std::max<FrameTime>(0, v.playhead + FrameTime(std::floor(moved)));
}

std::string jsonPatch(const std::string& from, const std::string& to) {
    const QJsonDocument a = QJsonDocument::fromJson(QByteArray::fromStdString(from)), b = QJsonDocument::fromJson(QByteArray::fromStdString(to));
    QJsonArray ops;
    std::vector<QJsonValue> path;
    diff(a.object(), b.object(), path, ops);
    return QJsonDocument(ops).toJson(QJsonDocument::Compact).toStdString();
}

bool applyJsonPatch(const std::string& doc, const std::string& patch, std::string& out) {
    QJsonParseError e1, e2;
    const QJsonDocument d = QJsonDocument::fromJson(QByteArray::fromStdString(doc), &e1);
    const QJsonDocument p = QJsonDocument::fromJson(QByteArray::fromStdString(patch), &e2);
    if (e1.error != QJsonParseError::NoError || e2.error != QJsonParseError::NoError || !d.isObject() || !p.isArray()) return false;
    QJsonObject o = d.object();
    if (!applyOps(o, p.array())) return false;
    out = compact(o).toStdString();
    return true;
}

std::string jsonHash(const std::string& json) { return hashOf(QJsonDocument::fromJson(QByteArray::fromStdString(json)).object()); }

void keepLocalMedia(const Project& local, Project& received) {
    for (MediaItem& m : received.media)
        if (const MediaItem* l = local.findMedia(m.id)) {
            m.path = l->path;
            m.proxyPath = l->proxyPath;
        }
}

bool needsStream(const Project& p, const Sequence& s, FrameTime t, const std::function<bool(Id)>& missing) {
    for (const Track& track : s.videoTracks) {
        if (track.muted) continue;
        for (const Clip& c : track.clips)
            if (c.start <= t && t < c.end() && c.enabled && c.mediaId != 0 && p.findMedia(c.mediaId) && missing(c.mediaId)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The colourist's end.

struct GradeHost::Impl {
    GradeSettings settings;
    std::string host;
    int port = 0;
    std::atomic<bool> stopping{false};
    mutable std::mutex m;
    std::condition_variable cv;
    bool running = false;
    std::optional<Project> pending;  // published, not yet turned into JSON
    uint64_t revision = 0;
    ViewState view;
    uint64_t viewSerial = 0;
    bool resync = false;
    std::vector<output::RemoteMarker> notes;
    std::vector<Pointer> pointers;
    Status status;
    std::thread work, back;

    static int interrupted(void* o) { return static_cast<Impl*>(o)->stopping.load() ? 1 : 0; }

    void fail(const std::string& why) {
        std::lock_guard lock(m);
        status.error = why;
    }

    bool waitStop(std::chrono::milliseconds ms) {
        std::unique_lock lock(m);
        cv.wait_for(lock, ms, [&] { return stopping.load(); });
        return stopping;
    }

    void runWork() {
        const AVIOInterruptCB cb{&Impl::interrupted, this};
        QJsonObject current;  // the project as JSON, revision `currentRev`
        uint64_t currentRev = 0;
        bool haveCurrent = false;
        while (!stopping) {
            AVIOContext* pb = nullptr;
            const int r = openSrt(&pb, withPort(host, port), true, true, settings, &cb);
            if (r < 0) {
                if (stopping) break;
                fail("Cannot take a client on port " + std::to_string(port) + ": " + averr(r));
                if (waitStop(std::chrono::milliseconds(500))) break;
                continue;
            }
            {
                std::lock_guard lock(m);
                status.connected = true;
                status.error.clear();
                resync = false;
            }
            QJsonObject sent;
            uint64_t sentRev = 0, sentView = 0;
            bool haveSent = false;
            std::string sentCompare;
            auto lastHash = Clock::now(), lastProject = Clock::now() - kProjectEvery;
            int64_t bytes = 0;
            bool ok = writeMessage(pb, {{"t", "hello"}, {"montage", "grade"}, {"version", kVersion}}, &bytes);
            while (ok && !stopping) {
                std::optional<Project> project;
                ViewState v;
                uint64_t serial = 0;
                bool again = false;
                {
                    std::unique_lock lock(m);
                    cv.wait_for(lock, std::chrono::milliseconds(50), [&] {
                        return stopping.load() || resync || (pending && Clock::now() - lastProject >= kProjectEvery) || viewSerial != sentView;
                    });
                    if (pending && Clock::now() - lastProject >= kProjectEvery) {
                        project = std::move(pending);
                        pending.reset();
                        currentRev = revision;
                    }
                    v = view;
                    serial = viewSerial;
                    again = resync;
                    resync = false;
                }
                if (stopping) break;
                if (project) {
                    current = QJsonDocument::fromJson(QByteArray::fromStdString(projectToJson(*project))).object();
                    haveCurrent = true;
                }
                if (haveCurrent && (!haveSent || again || currentRev != sentRev)) {
                    QJsonArray ops;
                    if (haveSent && !again) {
                        std::vector<QJsonValue> path;
                        diff(sent, current, path, ops);
                    }
                    const bool whole = !haveSent || again || compact(QJsonObject{{"o", ops}}).size() > compact(current).size() / 2;
                    ok = whole ? writeMessage(pb, {{"t", "snapshot"}, {"rev", double(currentRev)}, {"doc", current}}, &bytes)
                               : writeMessage(pb, {{"t", "patch"}, {"rev", double(currentRev)}, {"ops", ops}}, &bytes);
                    std::lock_guard lock(m);
                    (whole ? status.snapshots : status.patches) += 1;
                    if (again && haveSent) ++status.resyncs;
                    sent = current;
                    sentRev = currentRev;
                    haveSent = true;
                    lastProject = Clock::now();
                }
                if (ok && serial != sentView) {
                    const std::string compareHash = hashBytes(v.compare);
                    QJsonObject o{{"t", "view"},
                                  {"sequence", double(v.sequence)},
                                  {"playhead", double(v.playhead)},
                                  {"playing", v.playing},
                                  {"speed", v.speed},
                                  {"viewer", QString::fromStdString(v.viewer)},
                                  {"ch", QString::fromStdString(compareHash)}};
                    if (compareHash != sentCompare && !v.compare.empty())
                        o.insert("compare", QString::fromLatin1(QByteArray::fromStdString(v.compare).toBase64()));
                    ok = writeMessage(pb, o, &bytes);
                    sentCompare = compareHash;
                    sentView = serial;
                    std::lock_guard lock(m);
                    ++status.views;
                }
                if (ok && haveSent && Clock::now() - lastHash >= kHashEvery) {
                    ok = writeMessage(pb, {{"t", "hash"}, {"rev", double(sentRev)}, {"sha", QString::fromStdString(hashOf(sent))}}, &bytes);
                    lastHash = Clock::now();
                }
                std::lock_guard lock(m);
                status.bytes += bytes;
                bytes = 0;
            }
            avio_closep(&pb);
            std::lock_guard lock(m);
            status.connected = false;
            if (!stopping) status.error = "The client went away";
        }
    }

    void runBack() {
        const AVIOInterruptCB cb{&Impl::interrupted, this};
        while (!stopping) {
            AVIOContext* pb = nullptr;
            const int r = openSrt(&pb, withPort(host, port + 1), true, false, settings, &cb);
            if (r < 0) {
                if (stopping) break;
                fail("Cannot take notes on port " + std::to_string(port + 1) + ": " + averr(r));
                if (waitStop(std::chrono::milliseconds(500))) break;
                continue;
            }
            QJsonObject o;
            while (!stopping && readMessage(pb, kMaxBack, o)) {
                const QString t = o.value("t").toString();
                std::lock_guard lock(m);
                if (t == "hello") {
                    status.client = text(o, "name", 200);
                } else if (t == "resync") {
                    resync = true;
                    cv.notify_all();
                } else if (t == "note" && notes.size() < kMaxQueued) {
                    output::RemoteMarker mk;
                    mk.frame = std::max<FrameTime>(0, frameOf(o, "frame"));
                    mk.duration = std::max<FrameTime>(0, frameOf(o, "duration"));
                    mk.sequence = Id(std::max<FrameTime>(0, frameOf(o, "sequence")));
                    mk.timecode = text(o, "timecode", 40);
                    mk.author = text(o, "author", 200);
                    if (mk.author.empty()) mk.author = status.client;
                    mk.text = text(o, "text", 64 * 1024);
                    mk.drawing = text(o, "drawing", 1 << 20);
                    mk.id = text(o, "id", 100);
                    notes.push_back(std::move(mk));
                    ++status.notes;
                } else if (t == "pointer" && pointers.size() < kMaxQueued) {
                    Pointer p;
                    p.sequence = Id(std::max<FrameTime>(0, frameOf(o, "sequence")));
                    p.frame = std::max<FrameTime>(0, frameOf(o, "frame"));
                    p.x = std::clamp(o.value("x").toDouble(), 0.0, 1.0);
                    p.y = std::clamp(o.value("y").toDouble(), 0.0, 1.0);
                    p.author = text(o, "author", 200);
                    if (p.author.empty()) p.author = status.client;
                    pointers.push_back(std::move(p));
                    ++status.pointers;
                }
            }
            avio_closep(&pb);
        }
    }
};

GradeHost::GradeHost() : d_(std::make_unique<Impl>()) {}
GradeHost::~GradeHost() { stop(); }

bool GradeHost::start(const GradeSettings& settings, std::string* error) {
    stop();
    std::string why = checkGradeSettings(settings);
    if (why.empty()) gradingAvailable(&why);
    if (!why.empty()) {
        if (error) *error = why;
        return false;
    }
    std::string scheme;
    Impl& d = *d_;
    output::parseStreamAddress(settings.address, scheme, d.host, d.port);
    d.settings = settings;
    d.stopping = false;
    {
        std::lock_guard lock(d.m);
        d.status = Status{};
        d.status.port = d.port;
        d.status.notesPort = d.port + 1;
        d.resync = false;
        d.viewSerial = 0;
        d.notes.clear();
        d.pointers.clear();
        d.running = true;
    }
    d.work = std::thread([&d] { d.runWork(); });
    d.back = std::thread([&d] { d.runBack(); });
    return true;
}

void GradeHost::stop() {
    Impl& d = *d_;
    {
        std::lock_guard lock(d.m);
        d.stopping = true;
    }
    d.cv.notify_all();
    if (d.work.joinable()) d.work.join();
    if (d.back.joinable()) d.back.join();
    std::lock_guard lock(d.m);
    d.running = false;
    d.status.connected = false;
}

bool GradeHost::running() const {
    std::lock_guard lock(d_->m);
    return d_->running;
}

void GradeHost::publish(const Project& p) {
    std::lock_guard lock(d_->m);
    d_->pending = p;
    d_->status.revision = ++d_->revision;
    d_->cv.notify_all();
}

void GradeHost::setView(const ViewState& v) {
    std::lock_guard lock(d_->m);
    const ViewState& was = d_->view;
    if (d_->viewSerial && was.sequence == v.sequence && was.playhead == v.playhead && was.playing == v.playing && was.speed == v.speed &&
        was.viewer == v.viewer && was.compare == v.compare)
        return;
    d_->view = v;
    ++d_->viewSerial;
    d_->cv.notify_all();
}

std::vector<output::RemoteMarker> GradeHost::takeNotes() {
    std::lock_guard lock(d_->m);
    return std::exchange(d_->notes, {});
}

std::vector<Pointer> GradeHost::takePointers() {
    std::lock_guard lock(d_->m);
    return std::exchange(d_->pointers, {});
}

GradeHost::Status GradeHost::status() const {
    std::lock_guard lock(d_->m);
    return d_->status;
}

// ---------------------------------------------------------------------------
// The client's end.

struct GradeClient::Impl {
    GradeSettings settings;
    std::string host;
    int port = 0;
    std::atomic<bool> stopping{false};
    mutable std::mutex m;
    std::condition_variable cv;
    bool running = false;
    QJsonObject doc;
    uint64_t rev = 0;
    bool haveDoc = false, changed = false;
    ViewState view;
    std::string compareHash;
    bool haveView = false;
    Clock::time_point viewAt;
    std::vector<QJsonObject> outbox;
    bool damage = false;
    Status status;
    std::thread work, back;

    static int interrupted(void* o) { return static_cast<Impl*>(o)->stopping.load() ? 1 : 0; }

    bool waitStop(std::chrono::milliseconds ms) {
        std::unique_lock lock(m);
        cv.wait_for(lock, ms, [&] { return stopping.load(); });
        return stopping;
    }

    void queue(QJsonObject o) {
        std::lock_guard lock(m);
        if (outbox.size() >= kMaxQueued) {
            // (a request for the whole project is never dropped; the oldest pointer or note makes room)
            if (o.value("t").toString() != "resync") return;
            const auto it = std::find_if(outbox.begin(), outbox.end(), [](const QJsonObject& q) { return q.value("t").toString() != "resync"; });
            if (it == outbox.end()) return;
            outbox.erase(it);
        }
        outbox.push_back(std::move(o));
        cv.notify_all();
    }

    void runWork() {
        const AVIOInterruptCB cb{&Impl::interrupted, this};
        while (!stopping) {
            AVIOContext* pb = nullptr;
            const int r = openSrt(&pb, withPort(host, port), false, false, settings, &cb);
            if (r < 0) {
                if (stopping) break;
                {
                    std::lock_guard lock(m);
                    status.error = "Cannot connect (is Montage taking a remote grading client there, with this passphrase?): " + averr(r);
                }
                if (waitStop(std::chrono::milliseconds(1000))) break;
                continue;
            }
            QJsonObject o;
            bool hello = false;
            while (!stopping && readMessage(pb, kMaxWork, o)) {
                const QString t = o.value("t").toString();
                if (!hello) {
                    if (t != "hello" || o.value("montage").toString() != "grade" || o.value("version").toInt() != kVersion) {
                        {
                            std::lock_guard lock(m);
                            status.error = "That is not a Montage remote grading session this Montage can follow";
                        }
                        avio_closep(&pb);
                        if (waitStop(std::chrono::milliseconds(2000))) return;
                        break;
                    }
                    hello = true;
                    std::lock_guard lock(m);
                    status.connected = true;
                    status.error.clear();
                    continue;
                }
                std::unique_lock lock(m);
                if (t == "snapshot") {
                    doc = o.value("doc").toObject();
                    rev = revOf(o);
                    haveDoc = changed = true;
                    ++status.snapshots;
                    status.revision = rev;
                } else if ((t == "patch" || t == "hash") && !haveDoc) {
                    // (waiting for the whole project: asked again, in case the last request was lost)
                    lock.unlock();
                    if (t == "hash") queue({{"t", "resync"}, {"rev", double(rev)}});
                    continue;
                } else if (t == "patch") {
                    if (!applyOps(doc, o.value("ops").toArray())) {
                        // (cannot follow it: the whole project again)
                        haveDoc = false;
                        lock.unlock();
                        queue({{"t", "resync"}, {"rev", o.value("rev")}});
                        continue;
                    }
                    rev = revOf(o);
                    changed = true;
                    ++status.patches;
                    status.revision = rev;
                } else if (t == "view") {
                    ViewState v;
                    v.sequence = Id(std::max<FrameTime>(0, frameOf(o, "sequence")));
                    v.playhead = std::max<FrameTime>(0, frameOf(o, "playhead"));
                    v.playing = o.value("playing").toBool();
                    v.speed = std::clamp(o.value("speed").toDouble(1), -64.0, 64.0);
                    v.viewer = o.value("viewer").toString().toStdString();
                    const std::string ch = o.value("ch").toString().toStdString();
                    if (ch.empty()) {
                        v.compare.clear();
                    } else if (o.contains("compare")) {
                        v.compare = QByteArray::fromBase64(o.value("compare").toString().toLatin1()).toStdString();
                    } else {
                        v.compare = ch == compareHash ? view.compare : std::string();
                    }
                    compareHash = ch;
                    view = std::move(v);
                    viewAt = Clock::now();
                    haveView = true;
                } else if (t == "hash") {
                    if (damage) {
                        doc.insert("name", "(damaged)");
                        damage = false;
                    }
                    ++status.checks;
                    if (revOf(o) == rev && hashOf(doc) != o.value("sha").toString().toStdString()) {
                        ++status.mismatches;
                        lock.unlock();
                        queue({{"t", "resync"}, {"rev", double(rev)}});
                    }
                }
            }
            avio_closep(&pb);
            std::lock_guard lock(m);
            if (status.connected && !stopping) status.error = "The colourist's Montage went away";
            status.connected = false;
            if (stopping) break;
        }
    }

    void runBack() {
        const AVIOInterruptCB cb{&Impl::interrupted, this};
        while (!stopping) {
            AVIOContext* pb = nullptr;
            const int r = openSrt(&pb, withPort(host, port + 1), false, true, settings, &cb);
            if (r < 0) {
                if (waitStop(std::chrono::milliseconds(1000))) break;
                continue;
            }
            bool ok = writeMessage(pb, {{"t", "hello"}, {"name", QString::fromStdString(settings.name)}});
            while (ok && !stopping) {
                std::vector<QJsonObject> send;
                {
                    std::unique_lock lock(m);
                    cv.wait_for(lock, std::chrono::milliseconds(1000), [&] { return stopping.load() || !outbox.empty(); });
                    send.swap(outbox);
                }
                if (send.empty()) send.push_back({{"t", "alive"}});  // (finds a dropped connection)
                for (size_t i = 0; i < send.size() && ok; ++i) {
                    ok = writeMessage(pb, send[i]);
                    if (!ok) {  // (kept for the next connection)
                        std::lock_guard lock(m);
                        for (size_t k = i; k < send.size(); ++k)
                            if (send[k].value("t").toString() != "alive") outbox.insert(outbox.begin() + std::ptrdiff_t(k - i), send[k]);
                    }
                }
            }
            avio_closep(&pb);
        }
    }
};

GradeClient::GradeClient() : d_(std::make_unique<Impl>()) {}
GradeClient::~GradeClient() { stop(); }

bool GradeClient::start(const GradeSettings& settings, std::string* error) {
    stop();
    std::string why = checkGradeSettings(settings);
    if (why.empty()) gradingAvailable(&why);
    if (!why.empty()) {
        if (error) *error = why;
        return false;
    }
    std::string scheme;
    Impl& d = *d_;
    output::parseStreamAddress(settings.address, scheme, d.host, d.port);
    d.settings = settings;
    d.stopping = false;
    {
        std::lock_guard lock(d.m);
        d.status = Status{};
        d.doc = QJsonObject();
        d.haveDoc = d.changed = d.haveView = false;
        d.rev = 0;
        d.view = ViewState{};
        d.compareHash.clear();
        d.outbox.clear();
        d.running = true;
    }
    d.work = std::thread([&d] { d.runWork(); });
    d.back = std::thread([&d] { d.runBack(); });
    return true;
}

void GradeClient::stop() {
    Impl& d = *d_;
    {
        std::lock_guard lock(d.m);
        d.stopping = true;
    }
    d.cv.notify_all();
    if (d.work.joinable()) d.work.join();
    if (d.back.joinable()) d.back.join();
    std::lock_guard lock(d.m);
    d.running = false;
    d.status.connected = false;
}

bool GradeClient::running() const {
    std::lock_guard lock(d_->m);
    return d_->running;
}

bool GradeClient::takeProject(Project& out, uint64_t* revision, std::string* error) {
    QByteArray json;
    uint64_t rev = 0;
    {
        std::lock_guard lock(d_->m);
        if (!d_->changed || !d_->haveDoc) return false;
        d_->changed = false;
        json = compact(d_->doc);
        rev = d_->rev;
    }
    Project p;
    if (!projectFromJson(json.toStdString(), p, error)) return false;
    out = std::move(p);
    if (revision) *revision = rev;
    return true;
}

bool GradeClient::view(ViewState& out, int64_t* ageMs) const {
    std::lock_guard lock(d_->m);
    if (!d_->haveView) return false;
    out = d_->view;
    if (ageMs) *ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - d_->viewAt).count();
    return true;
}

void GradeClient::sendNote(const output::RemoteMarker& m) {
    QJsonObject o = markerJson(m);
    if (m.author.empty()) o.insert("author", QString::fromStdString(d_->settings.name));
    d_->queue(std::move(o));
}

void GradeClient::sendPointer(const Pointer& p) {
    d_->queue({{"t", "pointer"},
               {"sequence", double(p.sequence)},
               {"frame", double(p.frame)},
               {"x", p.x},
               {"y", p.y},
               {"author", QString::fromStdString(p.author.empty() ? d_->settings.name : p.author)}});
}

GradeClient::Status GradeClient::status() const {
    std::lock_guard lock(d_->m);
    return d_->status;
}

void GradeClient::damageForTest() {
    std::lock_guard lock(d_->m);
    d_->damage = true;
}

}  // namespace montage::remote
