#include "media/WhipEndpoint.h"

#include <QHostAddress>
#include <QUrl>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef MONTAGE_WITH_WEBRTC
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QNetworkInterface>
#include <QPointer>
#include <QRandomGenerator>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <optional>
#include <thread>

#ifdef Q_OS_WIN
#include <winsock2.h>
#else
#include <unistd.h>
#endif
#include <rtc/rtc.hpp>

#include "render/ReviewGate.h"
#include "render/WebRtcReview.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/version.h>
}
#endif

namespace montage::whip {

bool isSource(const std::string& url) {
    return url.size() > 7 && std::equal(url.begin(), url.begin() + 7, "whip://", [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

bool settingsFrom(const std::string& url, const std::map<std::string, std::string>& options, Settings& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!isSource(url)) return fail("A WHIP endpoint's address is whip://host:port/path");
    const QUrl u(QString::fromStdString("http" + url.substr(4)), QUrl::StrictMode);
    if (!u.isValid() || u.host().isEmpty() || !u.userInfo().isEmpty() || u.hasQuery() || u.hasFragment())
        return fail("A WHIP endpoint's address is whip://host:port/path");
    Settings s;
    s.host = u.host().toStdString();
    s.port = u.port(0);
    s.path = u.path().isEmpty() || u.path() == "/" ? std::string("/whip") : u.path(QUrl::FullyEncoded).toStdString();
    auto option = [&](const char* name) {
        const auto it = options.find(name);
        return it == options.end() ? std::string() : it->second;
    };
    s.token = option("token");
    for (const char ch : s.token)
        if (static_cast<unsigned char>(ch) <= 0x20 || static_cast<unsigned char>(ch) >= 0x7f) return fail("The token may hold letters, digits and signs, without spaces");
    auto readPem = [&](const std::string& path, std::string& into) {
        if (path.empty()) return true;
        QFile f(QString::fromStdString(path));
        if (!f.open(QIODevice::ReadOnly)) return fail("Cannot read " + path);
        into = f.read(1 << 20).toStdString();
        return true;
    };
    if (!readPem(option("certificate"), s.certificatePem) || !readPem(option("key"), s.keyPem)) return false;
    if (s.certificatePem.empty() != s.keyPem.empty()) return fail("A certificate needs its key, and a key its certificate");
    for (const QString& i : QString::fromStdString(option("ice")).split(' ', Qt::SkipEmptyParts)) s.iceServers.push_back(i.toStdString());
    out = std::move(s);
    return true;
}

const char* guestPage() {
    return R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Send to Montage</title>
<style>
body { font: 16px system-ui, sans-serif; margin: 2rem auto; max-width: 40rem; padding: 0 1rem; color: #1b1b1b; background: #fafafa; }
h1 { font-size: 1.4rem; }
label { display: block; margin: 1rem 0; }
input { font: inherit; padding: 0.3rem; width: 100%; box-sizing: border-box; }
button { font: inherit; padding: 0.5rem 0.9rem; margin: 0 0.5rem 0.5rem 0; }
video { width: 100%; background: #000; margin-top: 1rem; }
#state { min-height: 1.5rem; }
@media (prefers-color-scheme: dark) { body { color: #eee; background: #1b1b1b; } }
</style>
</head>
<body>
<h1>Send to Montage</h1>
<label>Token <input id="token" type="password" autocomplete="off"></label>
<p>
<button id="camera" type="button">Send camera and microphone</button>
<button id="screen" type="button">Send screen</button>
<button id="stop" type="button" disabled>Stop</button>
</p>
<p id="state" role="status" aria-live="polite"></p>
<video id="preview" muted autoplay playsinline></video>
<script>
(() => {
  const $ = id => document.getElementById(id);
  const whip = window.__montageWhip = {stage: 'idle', pc: null, session: null, stream: null};
  if (location.hash.length > 1) $('token').value = decodeURIComponent(location.hash.slice(1));
  const say = (stage, text) => { whip.stage = stage; $('state').textContent = text; };
  const sending = on => { $('camera').disabled = $('screen').disabled = on; $('stop').disabled = !on; };
  const headers = extra => Object.assign($('token').value ? {Authorization: 'Bearer ' + $('token').value} : {}, extra || {});
  whip.stop = async (text) => {
    const session = whip.session;
    whip.session = null;
    if (whip.pc) whip.pc.close();
    whip.pc = null;
    if (whip.stream) for (const t of whip.stream.getTracks()) t.stop();
    whip.stream = null;
    $('preview').srcObject = null;
    sending(false);
    if (session) try { await fetch(session, {method: 'DELETE', headers: headers()}); } catch (e) {}
    say('idle', text || 'Stopped');
  };
  whip.send = async (screen, made) => {
    sending(true);
    say('opening', screen ? 'Opening the screen' : 'Opening the camera');
    try {
      whip.stream = made || (screen ? await navigator.mediaDevices.getDisplayMedia({video: true, audio: true})
                                    : await navigator.mediaDevices.getUserMedia({video: {width: {ideal: 1280}, height: {ideal: 720}}, audio: true}));
    } catch (e) {
      sending(false);
      return say('failed', (screen ? 'The screen could not be opened: ' : 'The camera could not be opened: ') + e.message);
    }
    $('preview').srcObject = whip.stream;
    try {
      const pc = whip.pc = new RTCPeerConnection();
      for (const t of whip.stream.getTracks()) pc.addTransceiver(t, {direction: 'sendonly', streams: [whip.stream]});
      await pc.setLocalDescription(await pc.createOffer());
      // (this computer's addresses are gathered first, for two seconds at most: the offer carries them)
      await new Promise(done => {
        if (pc.iceGatheringState === 'complete') return done();
        const give = setTimeout(done, 2000);
        pc.addEventListener('icegatheringstatechange', () => { if (pc.iceGatheringState === 'complete') { clearTimeout(give); done(); } });
      });
      say('offering', 'Connecting');
      const answer = await fetch(location.pathname, {method: 'POST', headers: headers({'Content-Type': 'application/sdp'}), body: pc.localDescription.sdp});
      if (answer.status === 401) return whip.stop('That token is not the one Montage was given');
      if (answer.status !== 201) return whip.stop((await answer.text()) || ('Montage answered ' + answer.status));
      whip.session = answer.headers.get('Location');
      await pc.setRemoteDescription({type: 'answer', sdp: await answer.text()});
      pc.addEventListener('connectionstatechange', () => {
        if (pc !== whip.pc) return;
        if (pc.connectionState === 'connected') say('sending', 'Sending');
        if (pc.connectionState === 'failed') whip.stop('The connection to Montage was lost');
      });
      for (const t of whip.stream.getTracks()) t.addEventListener('ended', () => { if (pc === whip.pc) whip.stop(); });
    } catch (e) {
      whip.stop('Montage could not be reached: ' + e.message);
    }
  };
  $('camera').addEventListener('click', () => whip.send(false));
  $('screen').addEventListener('click', () => whip.send(true));
  $('stop').addEventListener('click', () => whip.stop());
  if (!navigator.mediaDevices) say('failed', 'This browser gives this page no camera: open it over https');
})();
</script>
</body>
</html>
)html";
}

namespace {
std::mutex traceMutex;
std::function<void(const std::string&)> traceTo;
std::atomic<bool> tracing{false};
}  // namespace

void setTimingTrace(std::function<void(const std::string&)> trace) {
    std::lock_guard lock(traceMutex);
    traceTo = std::move(trace);
    tracing = bool(traceTo);
}

#ifndef MONTAGE_WITH_WEBRTC

struct Endpoint::Impl {};
bool available(std::string* why) {
    if (why) *why = "This build has no WebRTC";
    return false;
}
Endpoint::Endpoint() = default;
Endpoint::~Endpoint() = default;
bool Endpoint::start(const Settings&, std::string* error) { return available(error); }
void Endpoint::stop() {}
int Endpoint::port() const { return 0; }
std::string Endpoint::url() const { return {}; }
std::string Endpoint::certificateFingerprint() const { return {}; }
bool Endpoint::waitForMedia(std::string&, const std::function<bool()>&, std::string* error) { return available(error); }
int Endpoint::read(uint8_t*, int, int) { return -1; }
double Endpoint::frameRate() const { return 0; }
Status Endpoint::status() const { return {}; }

#else

namespace {

using Clock = std::chrono::steady_clock;
using output::PassphraseGate;

constexpr qsizetype kMaxHeader = 16 * 1024, kMaxBody = 256 * 1024;
constexpr int kMaxConnections = 32;
constexpr size_t kHeldBytes = size_t(16) << 20;     // pictures kept before the feed begins (from the last keyframe)
constexpr size_t kQueuedBytes = size_t(64) << 20;   // packets waiting to be read: past this, pictures are dropped
constexpr double kReportsWait = 3.0;                // seconds the sender's reports are waited for before arrival lines the sound up
constexpr int kFramesHeld = 8;                      // pictures seen before the feed begins (their rate is measured from them)
constexpr double kTimingKept = 15.0;                // seconds of the sender's reports (and of its sound's packets) a stream's timing is taken from

double secondsNow() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// A line for setTimingTrace's, when one is set.
void trace(const char* format, ...) {
    if (!tracing) return;
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    std::lock_guard lock(traceMutex);
    if (traceTo) traceTo(line);
}

bool sameBytes(const QByteArray& a, const QByteArray& b) {
    const QByteArray x = QCryptographicHash::hash(a, QCryptographicHash::Sha256), y = QCryptographicHash::hash(b, QCryptographicHash::Sha256);
    int diff = 0;
    for (qsizetype i = 0; i < x.size(); ++i) diff |= x[i] ^ y[i];
    return diff == 0;
}

// Where a connection comes from, as wrong tokens are counted: its address, or its /64 for IPv6; and the network
// around it (an IPv4 /24, an IPv6 /48).
QString guesser(const QHostAddress& a) {
    bool v4 = false;
    a.toIPv4Address(&v4);
    if (v4 || a.protocol() != QAbstractSocket::IPv6Protocol) return a.toString();
    Q_IPV6ADDR ip = a.toIPv6Address();
    for (int i = 8; i < 16; ++i) ip[i] = 0;
    return QHostAddress(ip).toString() + "/64";
}
QString network(const QHostAddress& a) {
    bool v4 = false;
    const quint32 ip4 = a.toIPv4Address(&v4);
    if (v4) return QHostAddress(ip4 & 0xffffff00u).toString() + "/24";
    if (a.protocol() != QAbstractSocket::IPv6Protocol) return a.toString();
    Q_IPV6ADDR ip = a.toIPv6Address();
    for (int i = 6; i < 16; ++i) ip[i] = 0;
    return QHostAddress(ip).toString() + "/48";
}

struct Request {
    QByteArray method, path;
    std::map<QByteArray, QByteArray> headers;  // names in lower case
    QByteArray body;
    QByteArray header(const char* name) const {
        auto it = headers.find(name);
        return it == headers.end() ? QByteArray() : it->second;
    }
};

// Parses a whole request from the start of `in` (removing it); 0 when more is needed, -1 when it is not one Montage
// takes, -2 when it is too large.
int takeRequest(QByteArray& in, Request& r) {
    const qsizetype end = in.indexOf("\r\n\r\n");
    if (end < 0) return in.size() > kMaxHeader ? -2 : 0;
    if (end > kMaxHeader) return -2;
    const QList<QByteArray> lines = in.left(end).split('\n');
    const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
    if (first.size() != 3 || !first[2].startsWith("HTTP/1.")) return -1;
    r.method = first[0];
    r.path = first[1];
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) continue;
        r.headers[line.left(colon).trimmed().toLower()] = line.mid(colon + 1).trimmed();
    }
    bool ok = true;
    const QByteArray cl = r.header("content-length");
    const qint64 length = cl.isEmpty() ? 0 : cl.toLongLong(&ok);
    if (!ok || length < 0) return -1;
    if (length > kMaxBody) return -2;
    if (!r.header("transfer-encoding").isEmpty()) return -1;
    if (in.size() < end + 4 + length) return 0;
    r.body = in.mid(end + 4, length);
    in.remove(0, end + 4 + length);
    return 1;
}

class HttpServer : public QTcpServer {
public:
    std::function<void(qintptr)> incoming;

protected:
    void incomingConnection(qintptr fd) override { incoming(fd); }
};

// An RTP packet's header fields and where its payload lies.
struct Rtp {
    int pt = 0;
    bool marker = false;
    uint16_t seq = 0;
    uint32_t ts = 0, ssrc = 0;
    const uint8_t* payload = nullptr;
    size_t size = 0;
};

bool parseRtp(const uint8_t* d, size_t n, Rtp& r) {
    if (n < 12 || (d[0] >> 6) != 2) return false;
    size_t off = 12 + 4 * size_t(d[0] & 0x0f);
    if (d[0] & 0x10) {
        if (n < off + 4) return false;
        off += 4 + 4 * ((size_t(d[off + 2]) << 8) | d[off + 3]);
    }
    if (n < off) return false;
    size_t end = n;
    if (d[0] & 0x20) {
        const size_t pad = d[n - 1];
        if (pad == 0 || pad > n - off) return false;
        end = n - pad;
    }
    r.pt = d[1] & 0x7f;
    r.marker = (d[1] & 0x80) != 0;
    r.seq = uint16_t((d[2] << 8) | d[3]);
    r.ts = (uint32_t(d[4]) << 24) | (uint32_t(d[5]) << 16) | (uint32_t(d[6]) << 8) | d[7];
    r.ssrc = (uint32_t(d[8]) << 24) | (uint32_t(d[9]) << 16) | (uint32_t(d[10]) << 8) | d[11];
    r.payload = d + off;
    r.size = end - off;
    return true;
}

// Whether a picture's packet begins a keyframe, as each codec's RTP payload says it.
bool beginsKeyframe(const std::string& codec, const uint8_t* p, size_t n) {
    if (n < 1) return false;
    if (codec == "h264") {
        // (a sequence parameter set, by itself or first among those put together in one packet: browsers send one
        // before each keyframe)
        const int type = p[0] & 0x1f;
        if (type == 7) return true;
        if (type == 24) {
            for (size_t at = 1; at + 3 <= n;) {
                const size_t size = (size_t(p[at]) << 8) | p[at + 1];
                if ((p[at + 2] & 0x1f) == 7) return true;
                at += 2 + size;
            }
        }
        return false;
    }
    if (codec == "vp8") {
        // (the payload descriptor, then the frame's first byte, whose lowest bit is 0 in a keyframe: only in the
        // packet that starts the first partition)
        size_t at = 1;
        if (!(p[0] & 0x10) || (p[0] & 0x07)) return false;
        if (p[0] & 0x80) {
            if (n < 2) return false;
            const uint8_t x = p[1];
            at = 2;
            if (x & 0x80) at += (at < n && (p[at] & 0x80)) ? 2 : 1;
            if (x & 0x40) ++at;
            if (x & 0x30) ++at;
        }
        return at < n && !(p[at] & 0x01);
    }
    if (codec == "vp9") return !(p[0] & 0x40) && (p[0] & 0x08);  // (not predicted from another picture, and its beginning)
    if (codec == "av1") return (p[0] & 0x08) != 0;               // (a new coded video sequence)
    return false;
}

struct Stream {
    bool present = false;
    int pt = 0, clock = 90000;
    std::string codec, fmtp;
    uint32_t ssrc = 0;
    // The sender's clock (NTP, seconds) at an RTP time (unwrapped): the latest report's RTP time, at the clock the
    // least delayed of the last kTimingKept seconds' reports puts it at (anchor()).
    bool reported = false;
    double reportNtp = 0;
    int64_t reportRtp = 0;
    // Each of those reports, when it came: the sender's clock at RTP time 0 by it. And for sound, the same by this
    // computer's clock from its packets as they came, the least of each fifth of a second.
    std::deque<std::pair<double, double>> origins, came;
    // RTP times unwrapped, and packets counted by their numbers.
    bool seen = false;
    uint32_t lastTs = 0;
    int64_t lastExt = 0;
    bool numbered = false;
    uint16_t lastSeq = 0;
    struct Held {
        std::vector<uint8_t> data;
        int64_t ts = 0;
        double arrived = 0;
    };
    std::deque<Held> held;
    size_t heldBytes = 0;
    double arrival = 0;    // this computer's time at RTP time 0, by the packets held (the feed's start, without reports)
    int64_t firstExt = 0;  // the RTP time of the first packet handed on
    int64_t unwrap(uint32_t ts) const { return seen ? lastExt + int32_t(ts - lastTs) : int64_t(ts); }
};

// What a sender's tracks hand over, between libdatachannel's threads and the reader.
struct Feed {
    std::mutex m;
    std::condition_variable cv;
    Stream video, audio;
    bool started = false, ended = false, reading = false;
    bool holding = false;  // pictures are held from a keyframe on
    bool heldLost = false; // a picture's packet was lost since that keyframe (the pictures held do not all decode)
    double keyAt = 0;      // when that keyframe came
    bool byReports = false;
    // Each report, when it came: how far this computer's clock was ahead of the sender's then (and the report's way here).
    std::deque<std::pair<double, double>> gaps;
    std::string sdp;
    double fps = 0;
    std::deque<std::vector<uint8_t>> out;
    size_t outBytes = 0;
    int64_t packets = 0, lost = 0, dropped = 0;
    std::atomic<int> keyframesAsked{0};
    double askedAt = 0;
    // The pictures' RTP times as handed on: moved to stay beside the sound by the sender's reports.
    int64_t videoShift = 0, videoShiftNext = 0;
    int64_t videoTsNow = -1;

    static void setTs(std::vector<uint8_t>& d, uint32_t ts) {
        d[4] = uint8_t(ts >> 24), d[5] = uint8_t(ts >> 16), d[6] = uint8_t(ts >> 8), d[7] = uint8_t(ts);
    }

    // A stream's time for an RTP time: by the sender's reports, or by when its held packets arrived (`arrival`).
    static double timeOf(const Stream& s, int64_t ts, bool reports) {
        if (reports) return s.reportNtp + double(ts - s.reportRtp) / s.clock;
        return s.arrival + double(ts) / s.clock;
    }
    // Where a stream's RTP times stand against this computer's clock by arrival: from the packet that came soonest
    // after its time, of all those held. Not from the first: a sender's first packets come late and bunched (its
    // sound starting, its pictures paced out while it learns the link), by a different amount in each stream.
    static void setArrival(Stream& s) {
        s.arrival = 0;
        bool any = false;
        for (const Stream::Held& h : s.held) {
            const double at = h.arrived - double(h.ts) / s.clock;
            if (!any || at < s.arrival) s.arrival = at, any = true;
        }
    }

    // Where a stream's RTP times stand against the sender's clock: by the report, of the last kTimingKept seconds',
    // that puts them earliest. Not simply by the latest: a sender tells the time it sent its sound at, not the time
    // it was made at, so sound held up in a busy sender's encoder is reported as made that much later, and lining the
    // pictures up by that would move them ahead of their sound for as long as it is held up. Sound is also placed by
    // its packets as they came (this computer's clock put on the sender's by how soon the reports came): many more
    // moments than the reports to find one where nothing held it up. (Within the time kept, a sender's clocks drift
    // apart by a millisecond or so at most; a jump of a second or more starts that timing afresh.)
    static double least(std::deque<std::pair<double, double>>& q, double now, bool keepOne) {
        while (q.size() > (keepOne ? 1u : 0u) && q.front().first < now - kTimingKept) q.pop_front();
        double m = q.empty() ? 0 : q.back().second;
        for (const auto& e : q) m = std::min(m, e.second);
        return m;
    }
    static void note(std::deque<std::pair<double, double>>& q, double now, double value, double block) {
        if (!q.empty() && std::fabs(value - q.back().second) >= 1) q.clear();
        if (!q.empty() && block > 0 && q.back().first > now - block)
            q.back().second = std::min(q.back().second, value);
        else
            q.emplace_back(now, value);
    }
    void anchor(Stream& s, double now) {
        double origin = least(s.origins, now, true);
        if (&s == &audio && !gaps.empty()) {
            const double gap = least(gaps, now, true);
            if (const double soonest = least(s.came, now, false); !s.came.empty()) origin = std::min(origin, soonest - gap);
        }
        s.reportNtp = origin + double(s.reportRtp) / s.clock;
    }

    // The pictures' shift that puts a picture where the sound of the same moment is, by both streams' reports (in
    // the pictures' RTP units, added to their unwrapped times).
    void placePictures() {
        if (!started || !video.present || !audio.present || !video.reported || !audio.reported) return;
        const double soundAt = double(audio.reportRtp - audio.firstExt) / audio.clock + (video.reportNtp - audio.reportNtp);
        const int64_t shift = video.firstExt + int64_t(std::llround(soundAt * video.clock)) - video.reportRtp;
        // (a millisecond or more: the reports' own jitter moves nothing)
        if (std::llabs(shift - videoShiftNext) >= video.clock / 1000) videoShiftNext = shift;
        byReports = true;
    }

    void push(std::vector<uint8_t>&& d, bool isVideo) {
        if (outBytes + d.size() > kQueuedBytes && isVideo) {
            ++dropped;
            return;
        }
        outBytes += d.size();
        out.push_back(std::move(d));
    }

    void handOn(Stream& s, std::vector<uint8_t>&& d, int64_t ts) {
        if (&s == &video) {
            // (a picture's packets all carry one time: the shift changes only between pictures, and never for the
            // first, whose time is where FFmpeg's count begins)
            if (ts != videoTsNow) {
                if (videoTsNow >= 0) videoShift = videoShiftNext;
                videoTsNow = ts;
                trace("picture %lld %lld", (long long)ts, (long long)(ts + videoShift));
                if (started) {
                    stepTo(ts);
                    if (steps.size() < 600) measureRate();
                }
            }
            setTs(d, uint32_t(uint64_t(ts + videoShift)));
        }
        push(std::move(d), &s == &video);
    }

    // The pictures' rate, from the steps between their times: those held before the feed began, then each handed
    // on, for the first 20 seconds (so a sender's jitter is evened out).
    std::vector<int64_t> steps;
    int64_t lastPictureTs = -1;
    void stepTo(int64_t ts) {
        if (lastPictureTs >= 0 && ts > lastPictureTs && steps.size() < 600) steps.push_back(ts - lastPictureTs);
        if (ts > lastPictureTs) lastPictureTs = ts;
    }
    void measureRate() {
        if (steps.size() < 2) return;
        // (the middle half of the steps, evened out: a sender's first pictures come bunched, and its clock in whole
        // milliseconds)
        std::vector<int64_t> sorted = steps;
        std::sort(sorted.begin(), sorted.end());
        const size_t from = sorted.size() / 4, to = sorted.size() - sorted.size() / 4;
        double sum = 0;
        for (size_t i = from; i < to; ++i) sum += double(sorted[i]);
        const double step = sum / double(to - from);
        // The rate cameras and encoders send nearest the steps, within 3 percent (the nearest two, 24 and 25, are 4
        // apart); else the whole rate nearest.
        double best = 0, off = 1;
        for (const double r : {60.0, 60000.0 / 1001, 50.0, 30.0, 30000.0 / 1001, 25.0, 24.0, 24000.0 / 1001, 20.0, 15.0, 12.5, 12.0, 10.0}) {
            const double o = std::fabs(step * r / video.clock - 1);
            if (o < off) best = r, off = o;
        }
        fps = off <= 0.03 ? best : std::max(1.0, std::round(video.clock / step));
    }

    int distinctHeld() const {
        int n = 0;
        int64_t last = -1;
        for (const Stream::Held& h : video.held)
            if (h.ts != last) ++n, last = h.ts;
        return n;
    }

    // The feed begins once there is a keyframe to begin at and the sound can be lined up with it.
    void tryStart(double now) {
        if (started || ended) return;
        const bool sound = audio.present;
        if (video.present) {
            if (!holding) return;
            const bool waited = now - keyAt >= kReportsWait;
            if (!waited && distinctHeld() < kFramesHeld) return;
            if (sound && !waited && !(video.reported && audio.reported)) return;
            if (sound && audio.held.empty() && !waited) return;
        } else if (!sound || audio.held.empty()) {
            return;
        }
        const bool reports = video.present && sound && video.reported && audio.reported;
        if (video.present) {
            for (const Stream::Held& h : video.held) stepTo(h.ts);
            measureRate();
        }
        // (sound that came to nothing in all that time is left out: a reader would wait for it)
        if (video.present && sound && audio.held.empty()) audio.present = false;
        std::vector<std::pair<double, std::vector<uint8_t>>> first;
        for (Stream* st : {&video, &audio})
            if (st->present) setArrival(*st);
        if (video.present) {
            const double begins = timeOf(video, video.held.front().ts, reports);
            video.firstExt = video.held.front().ts;
            if (audio.present) {
                // The sound from the pictures' first moment: what came before it is let go, and where the sound
                // begins later, silence stands in until it does (whole packets of 20 ms, so the sound begins within
                // 10 ms of the first picture; the pictures after the first are then placed against the sound exactly).
                while (audio.held.size() > 1 && timeOf(audio, audio.held.front().ts, reports) < begins - 0.010) {
                    audio.heldBytes -= audio.held.front().data.size();
                    audio.held.pop_front();
                }
                const Stream::Held& a = audio.held.front();
                const double gap = timeOf(audio, a.ts, reports) - begins;
                const int silent = int(std::clamp(std::llround(gap / 0.020), 0LL, 250LL));
                Rtp r;
                parseRtp(a.data.data(), a.data.size(), r);
                audio.firstExt = a.ts - int64_t(silent) * 960;
                for (int k = silent; k >= 1; --k) {
                    // (an Opus packet of 20 ms of silence, numbered and timed before the first real one)
                    std::vector<uint8_t> d(12 + 3);
                    d[0] = 0x80;
                    d[1] = uint8_t(audio.pt);
                    const uint16_t seq = uint16_t(r.seq - k);
                    d[2] = uint8_t(seq >> 8), d[3] = uint8_t(seq);
                    setTs(d, uint32_t(uint64_t(a.ts - int64_t(k) * 960)));
                    d[8] = uint8_t(r.ssrc >> 24), d[9] = uint8_t(r.ssrc >> 16), d[10] = uint8_t(r.ssrc >> 8), d[11] = uint8_t(r.ssrc);
                    d[12] = 0xF8, d[13] = 0xFF, d[14] = 0xFE;
                    first.emplace_back(begins + (silent - k) * 0.020, std::move(d));
                }
            }
        } else {
            audio.firstExt = audio.held.front().ts;
        }
        started = true;
        trace("start reports %d video %lld audio %lld", int(reports), (long long)(video.present ? video.firstExt : 0), (long long)(audio.present ? audio.firstExt : 0));
        // (what was held goes first, the two streams in the order of their times)
        std::vector<std::tuple<double, int, Stream*, Stream::Held*>> order;
        int n = 0;
        for (Stream* s : {&video, &audio})
            if (s->present)
                for (Stream::Held& h : s->held) order.emplace_back(timeOf(*s, h.ts, reports), n++, s, &h);
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
        size_t silentAt = 0;
        for (auto& [t, i, s, h] : order) {
            while (silentAt < first.size() && first[silentAt].first <= t) push(std::move(first[silentAt++].second), false);
            handOn(*s, std::move(h->data), h->ts);
        }
        while (silentAt < first.size()) push(std::move(first[silentAt++].second), false);
        video.held.clear();
        audio.held.clear();
        video.heldBytes = audio.heldBytes = 0;
        placePictures();
        // What FFmpeg's RTP reader is told: the streams by their payload types (the ports are never opened).
        std::string text = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=Montage WHIP\r\nc=IN IP4 127.0.0.1\r\nt=0 0\r\n";
        if (video.present) {
            const char* name = video.codec == "h264" ? "H264" : video.codec == "vp8" ? "VP8" : video.codec == "vp9" ? "VP9" : "AV1";
            text += "m=video 5004 RTP/AVP " + std::to_string(video.pt) + "\r\na=rtpmap:" + std::to_string(video.pt) + " " + name + "/90000\r\n";
            if (video.codec == "h264") text += "a=fmtp:" + std::to_string(video.pt) + " " + (video.fmtp.empty() ? std::string("packetization-mode=1") : video.fmtp) + "\r\n";
        }
        if (audio.present) text += "m=audio 5006 RTP/AVP " + std::to_string(audio.pt) + "\r\na=rtpmap:" + std::to_string(audio.pt) + " opus/48000/2\r\n";
        sdp = text;
        cv.notify_all();
    }

    // A packet from a track. True when a keyframe is to be asked for.
    bool rtp(bool isVideo, const uint8_t* d, size_t n) {
        Rtp r;
        if (!parseRtp(d, n, r)) return false;
        std::lock_guard lock(m);
        Stream& s = isVideo ? video : audio;
        if (!s.present || r.pt != s.pt || ended) return false;
        const double now = secondsNow();
        bool ask = false;
        ++packets;
        if (s.numbered) {
            const int16_t step = int16_t(r.seq - s.lastSeq);
            if (step > 1) {
                lost += step - 1;
                ask = isVideo;
                if (isVideo && holding && !started) heldLost = true;
            }
            if (step > 0) s.lastSeq = r.seq;
        } else {
            s.numbered = true;
            s.lastSeq = r.seq;
        }
        const int64_t ts = s.unwrap(r.ts);
        trace("packet %c ts %lld bytes %zu came %.6f", isVideo ? 'V' : 'A', (long long)ts, n, now);
        if (!isVideo) note(s.came, now, now - double(ts) / s.clock, 0.2);
        if (!s.seen || ts > s.lastExt) s.lastExt = ts, s.lastTs = r.ts;
        s.seen = true;
        s.ssrc = r.ssrc;
        std::vector<uint8_t> data(d, d + n);
        if (started) {
            const size_t before = dropped;
            handOn(s, std::move(data), ts);
            if (isVideo && dropped != before) ask = true;
            cv.notify_all();
        } else {
            if (isVideo) {
                if (beginsKeyframe(s.codec, r.payload, r.size) && (s.held.empty() || s.held.back().ts != ts) && (!holding || heldLost)) {
                    // (the first keyframe; or a later one before the feed has begun, after a loss: the feed begins
                    // there instead. A later one with nothing lost is held with the rest: a sender sends keyframes
                    // of its own while the sender's reports are waited for, and the feed begins at its first)
                    s.held.clear();
                    s.heldBytes = 0;
                    if (!holding) keyAt = now;
                    holding = true;
                    heldLost = false;
                }
                if (!holding) {
                    ask = true;
                } else if (s.heldBytes + n > kHeldBytes) {
                    s.held.clear();
                    s.heldBytes = 0;
                    holding = false;
                    heldLost = false;
                    ask = true;
                } else {
                    s.heldBytes += n;
                    s.held.push_back({std::move(data), ts, now});
                }
            } else {
                s.heldBytes += n;
                s.held.push_back({std::move(data), ts, now});
                while (s.held.size() > 500) {
                    s.heldBytes -= s.held.front().data.size();
                    s.held.pop_front();
                }
            }
            tryStart(now);
        }
        // (a keyframe is asked for once a second at most)
        if (ask && now - askedAt < 1.0) ask = false;
        if (ask) askedAt = now, ++keyframesAsked;
        return ask;
    }

    // The sender's reports among a track's RTCP: its clock at an RTP time.
    void rtcp(bool isVideo, const uint8_t* d, size_t n) {
        std::lock_guard lock(m);
        Stream& s = isVideo ? video : audio;
        for (size_t at = 0; at + 4 <= n;) {
            const size_t size = 4 * (((size_t(d[at + 2]) << 8) | d[at + 3]) + 1);
            if (size > n - at) break;
            if (d[at + 1] == 200 && size >= 28 && s.present && s.seen) {
                const uint32_t ssrc = (uint32_t(d[at + 4]) << 24) | (uint32_t(d[at + 5]) << 16) | (uint32_t(d[at + 6]) << 8) | d[at + 7];
                if (ssrc == s.ssrc) {
                    uint64_t ntp = 0;
                    for (int k = 0; k < 8; ++k) ntp = (ntp << 8) | d[at + 8 + k];
                    const uint32_t ts = (uint32_t(d[at + 16]) << 24) | (uint32_t(d[at + 17]) << 16) | (uint32_t(d[at + 18]) << 8) | d[at + 19];
                    if (ntp != 0) {
                        const double now = secondsNow(), sent = double(ntp >> 32) + double(ntp & 0xffffffffu) / 4294967296.0;
                        s.reported = true;
                        s.reportRtp = s.unwrap(ts);
                        note(s.origins, now, sent - double(s.reportRtp) / s.clock, 0);
                        note(gaps, now, now - sent, 0);
                        trace("report %c came %.6f ntp %.6f rtp %lld", isVideo ? 'V' : 'A', now, sent, (long long)s.reportRtp);
                        for (Stream* st : {&video, &audio})
                            if (st->reported) {
                                anchor(*st, now);
                                trace("anchor %c ntp %.6f rtp %lld", st == &video ? 'V' : 'A', st->reportNtp, (long long)st->reportRtp);
                            }
                        placePictures();
                        if (!started) tryStart(now);
                    }
                }
            }
            at += size;
        }
    }

    void end() {
        std::lock_guard lock(m);
        ended = true;
        cv.notify_all();
    }
};

// What a track's packets pass through first: each is shown to the feed, and a keyframe asked for when one is needed.
class Tap final : public rtc::MediaHandler {
public:
    Tap(std::shared_ptr<Feed> feed, bool video) : feed_(std::move(feed)), video_(video) {}
    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override {
        bool ask = false;
        uint32_t ssrc = 0;
        for (const rtc::message_ptr& m : messages) {
            if (!m) continue;
            const auto* d = reinterpret_cast<const uint8_t*>(m->data());
            if (m->type == rtc::Message::Binary) {
                if (feed_->rtp(video_, d, m->size())) ask = true;
                if (m->size() >= 12) ssrc = (uint32_t(d[8]) << 24) | (uint32_t(d[9]) << 16) | (uint32_t(d[10]) << 8) | d[11];
            } else if (m->type == rtc::Message::Control) {
                feed_->rtcp(video_, d, m->size());
            }
        }
        if (ask && video_ && ssrc) {
            auto message = rtc::make_message(rtc::RtcpPli::Size(), rtc::Message::Control);
            reinterpret_cast<rtc::RtcpPli*>(message->data())->preparePacket(ssrc);
            send(message);
        }
    }

private:
    std::shared_ptr<Feed> feed_;
    bool video_;
};

struct Session {
    std::string id;
    QString from;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::vector<std::shared_ptr<rtc::Track>> tracks;
    std::shared_ptr<Feed> feed;
};

QString lower(const std::string& s) { return QString::fromStdString(s).toLower(); }

bool av1Unpacked() {
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(61, 7, 100)
    return avcodec_find_decoder_by_name("libdav1d") != nullptr;
#else
    return false;
#endif
}

}  // namespace

bool available(std::string* why) {
    if (!QSslSocket::supportsSsl()) {
        if (why) *why = "This Qt has no TLS (OpenSSL) for https";
        return false;
    }
    return true;
}

struct Endpoint::Impl {
    Settings settings;
    QHostAddress host;
    int port = 0;
    bool https = false;
    QSslCertificate certificate;
    QSslKey key;
    std::vector<rtc::IceServer> iceServers;
    bool relayInUse = false;

    std::thread thread;
    mutable std::mutex m;
    QObject* home = nullptr;   // (under m) on the server's thread
    QEventLoop* loop = nullptr;
    std::shared_ptr<Session> session;  // (under m) the sender connected, if any
    std::shared_ptr<Feed> active;      // (under m) the feed being read, once it began
    int refused = 0;
    bool listening = false;
    PassphraseGate gate;  // (the server's thread)

    void run(std::promise<std::string>& started) {
        QEventLoop l;
        HttpServer server;
        server.setMaxPendingConnections(16);
        server.incoming = [this, &server](qintptr fd) { accept(server, fd); };
        if (!server.listen(host, quint16(port))) {
            started.set_value("Cannot listen on port " + std::to_string(port) + ": " + server.errorString().toStdString());
            return;
        }
        port = server.serverPort();
        {
            std::lock_guard lock(m);
            home = &server;
            loop = &l;
            listening = true;
        }
        started.set_value({});
        l.exec();
        std::lock_guard lock(m);
        home = nullptr;
        loop = nullptr;
        listening = false;
    }

    void accept(HttpServer& server, qintptr fd) {
        auto* c = new QSslSocket(&server);
        if (!c->setSocketDescriptor(fd) || server.findChildren<QSslSocket*>().size() > kMaxConnections) {
            if (c->socketDescriptor() == -1) {
#ifdef Q_OS_WIN
                ::closesocket(SOCKET(fd));
#else
                ::close(int(fd));
#endif
            } else {
                c->abort();
            }
            delete c;
            return;
        }
        QObject::connect(c, &QAbstractSocket::disconnected, c, &QObject::deleteLater);
        QObject::connect(c, &QAbstractSocket::readyRead, c, [this, c] { read(c); });
        QObject::connect(c, &QAbstractSocket::errorOccurred, c, [c] { c->abort(); });
        auto* idle = new QTimer(c);
        idle->setSingleShot(true);
        QObject::connect(idle, &QTimer::timeout, c, [c] { c->abort(); });
        idle->start(https ? 5000 : 20000);
        if (https) {
            QObject::connect(c, &QSslSocket::encrypted, idle, [idle] { idle->start(20000); });
            QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
            conf.setLocalCertificate(certificate);
            conf.setPrivateKey(key);
            conf.setProtocol(QSsl::TlsV1_2OrLater);
            conf.setPeerVerifyMode(QSslSocket::VerifyNone);
            c->setSslConfiguration(conf);
            c->startServerEncryption();
        }
    }

    void read(QSslSocket* c) {
        if (c->property("answered").toBool()) {
            c->readAll();
            return;
        }
        QByteArray in = c->property("in").toByteArray() + c->readAll();
        Request r;
        const int got = takeRequest(in, r);
        c->setProperty("in", in);
        if (got == 0) return;
        c->setProperty("answered", true);
        if (got == -2) return respond(c, "413 Content Too Large", {}, "Too large");
        if (got < 0) return respond(c, "400 Bad Request", {}, "Not understood");
        handle(c, r);
    }

    void respond(QSslSocket* c, const QByteArray& status, std::vector<std::pair<QByteArray, QByteArray>> headers, const QByteArray& body,
                 const QByteArray& type = "text/plain; charset=utf-8") {
        QByteArray out = "HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n";
        if (!body.isEmpty()) out += "Content-Type: " + type + "\r\n";
        out += "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nCache-Control: no-store\r\n";
        for (const auto& [k, v] : headers) out += k + ": " + v + "\r\n";
        out += "\r\n" + body;
        c->write(out);
        c->disconnectFromHost();
    }

    // The token as a bearer token, compared only when the gate lets it be tried (render/ReviewGate.h: an address
    // that guessed wrong too often is refused at once, and each address and network may try only so often).
    bool authorised(QSslSocket* c, const Request& r) {
        if (settings.token.empty()) return true;
        const QString peer = guesser(c->peerAddress());
        const QString around = network(c->peerAddress());
        const auto now = Clock::now();
        const PassphraseGate::Verdict may = gate.ask(peer, around, now);
        if (may.answer != PassphraseGate::Answer::Try) {
            respond(c, "429 Too Many Requests", {{"Retry-After", QByteArray::number(std::max(2, may.retryAfter))}}, "Too many wrong tokens from this address: try again later");
            return false;
        }
        QByteArray given = r.header("authorization");
        if (given.toLower().startsWith("bearer ")) given = given.mid(7).trimmed();
        else given.clear();
        if (sameBytes(given, QByteArray::fromStdString(settings.token))) {
            gate.right(peer, around, now, may);
            return true;
        }
        gate.wrong(peer, around, now);
        {
            std::lock_guard lock(m);
            ++refused;
        }
        respond(c, "401 Unauthorized", {{"WWW-Authenticate", "Bearer realm=\"Montage WHIP\""}}, "Wrong token");
        return false;
    }

    std::vector<std::pair<QByteArray, QByteArray>> iceLinks() const {
        std::vector<std::pair<QByteArray, QByteArray>> headers;
        for (const std::string& s : settings.iceServers) {
            // ("turn:user:password@host:port" as a Link: the address, and the login apart)
            const QString u = QString::fromStdString(s);
            const int colon = u.indexOf(':'), at = u.lastIndexOf('@');
            if (colon < 0) continue;
            QByteArray link = "<" + (at > colon ? u.left(colon + 1) + u.mid(at + 1) : u).toUtf8() + ">; rel=\"ice-server\"";
            if (at > colon) {
                const QString who = u.mid(colon + 1, at - colon - 1);
                const int c2 = who.indexOf(':');
                link += "; username=\"" + QUrl::fromPercentEncoding(who.left(c2).toUtf8()).toUtf8() + "\"; credential=\"" +
                        (c2 >= 0 ? QUrl::fromPercentEncoding(who.mid(c2 + 1).toUtf8()).toUtf8() : QByteArray()) + "\"; credential-type=\"password\"";
            }
            headers.emplace_back("Link", link);
        }
        return headers;
    }

    void handle(QSslSocket* c, const Request& r) {
        const QByteArray path = QByteArray::fromStdString(settings.path);
        const QByteArray asked = r.path.left(r.path.indexOf('?') < 0 ? r.path.size() : r.path.indexOf('?'));
        const bool endpoint = asked == path;
        const bool resource = asked.startsWith(path + "/") && asked.size() > path.size() + 1;
        if (!endpoint && !resource) {
            if (r.method == "GET" && asked == "/") return respond(c, "303 See Other", {{"Location", path}}, {});
            return respond(c, "404 Not Found", {}, "Not found");
        }
        if (r.method == "OPTIONS") {
            auto headers = iceLinks();
            headers.emplace_back("Accept-Post", "application/sdp");
            headers.emplace_back("Allow", endpoint ? "GET, POST, OPTIONS" : "PATCH, DELETE, OPTIONS");
            return respond(c, "204 No Content", headers, {});
        }
        if (endpoint && r.method == "GET") {
            return respond(c, "200 OK", {{"Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; media-src 'self' blob:"}},
                           guestPage(), "text/html; charset=utf-8");
        }
        if (endpoint && r.method == "POST") {
            if (!authorised(c, r)) return;
            if (!r.header("content-type").toLower().startsWith("application/sdp")) return respond(c, "415 Unsupported Media Type", {}, "An offer is application/sdp");
            return offer(c, r.body);
        }
        if (resource && (r.method == "PATCH" || r.method == "DELETE")) {
            if (!authorised(c, r)) return;
            std::shared_ptr<Session> s;
            {
                std::lock_guard lock(m);
                s = session;
            }
            if (!s || asked.mid(path.size() + 1) != QByteArray::fromStdString(s->id)) return respond(c, "404 Not Found", {}, "No such session");
            if (r.method == "DELETE") {
                endSession(s, true);
                return respond(c, "200 OK", {}, {});
            }
            // More of the sender's addresses (RFC 8840's fragment: candidates under each medium's mid).
            std::string mid;
            for (const QByteArray& raw : r.body.split('\n')) {
                const QByteArray line = raw.trimmed();
                if (line.startsWith("a=mid:")) mid = line.mid(6).toStdString();
                if (!line.startsWith("a=candidate:")) continue;
                try {
                    s->pc->addRemoteCandidate(mid.empty() ? rtc::Candidate(line.mid(2).toStdString()) : rtc::Candidate(line.mid(2).toStdString(), mid));
                } catch (const std::exception&) {
                    return respond(c, "400 Bad Request", {}, "A candidate was not understood");
                }
            }
            return respond(c, "204 No Content", {}, {});
        }
        respond(c, "405 Method Not Allowed", {{"Allow", endpoint ? "GET, POST, OPTIONS" : "PATCH, DELETE, OPTIONS"}}, "Not allowed");
    }

    // A session is over: one whose feed had begun ends the feed (what was sent is all read, and then the reader is
    // told it ended); one that never began is forgotten, and the endpoint waits for another sender.
    void endSession(const std::shared_ptr<Session>& s, bool asked) {
        bool mine = false;
        {
            std::lock_guard lock(m);
            mine = session == s;
            if (mine) {
                bool began = false;
                {
                    std::lock_guard feedLock(s->feed->m);
                    began = s->feed->started;
                }
                if (began || asked) active = s->feed;
                session.reset();
                if (began || asked) ended = true;
            }
        }
        if (!mine) return;
        s->feed->end();
        try {
            s->pc->resetCallbacks();
            for (const auto& t : s->tracks) t->resetCallbacks();
            s->pc->close();
        } catch (const std::exception&) {
        }
        // (let go a little later, outside libdatachannel's own calls)
        std::lock_guard lock(m);
        if (home) QTimer::singleShot(2000, home, [s] {});
    }
    bool ended = false;  // (under m) a session that had begun, or was told to end, is over: no other is taken

    void offer(QSslSocket* c, const QByteArray& sdp) {
        {
            std::lock_guard lock(m);
            if (session) return respond(c, "409 Conflict", {}, "A sender is already connected");
            if (ended) return respond(c, "409 Conflict", {}, "This recording has ended");
        }
        std::shared_ptr<Session> s;
        std::string why;
        try {
            s = makeSession(sdp.toStdString(), c->peerAddress().toString(), why);
        } catch (const std::exception& e) {
            why = std::string("The offer is not one Montage can answer: ") + e.what();
        }
        if (!s) return respond(c, "400 Bad Request", {}, QByteArray::fromStdString(why));
        auto done = std::make_shared<std::atomic<bool>>(false);
        QPointer<QSslSocket> sock(c);
        std::weak_ptr<Session> ws = s;
        auto finish = [this, sock, ws] {
            std::shared_ptr<Session> s = ws.lock();
            if (!sock) return;
            std::optional<rtc::Description> local = s ? s->pc->localDescription() : std::nullopt;
            if (!local) return respond(sock, "500 Internal Server Error", {}, "No answer");
            auto headers = iceLinks();
            headers.emplace_back("Location", QByteArray::fromStdString(settings.path + "/" + s->id));
            headers.emplace_back("ETag", "\"" + QByteArray::fromStdString(s->id) + "\"");
            respond(sock, "201 Created", headers, QByteArray::fromStdString(std::string(*local)), "application/sdp");
        };
        auto later = [this, done, finish] {
            if (done->exchange(true)) return;
            std::lock_guard lock(m);
            if (home) QMetaObject::invokeMethod(home, finish, Qt::QueuedConnection);
        };
        s->pc->onGatheringStateChange([later](rtc::PeerConnection::GatheringState g) {
            if (g == rtc::PeerConnection::GatheringState::Complete) later();
        });
        s->pc->onStateChange([this, ws](rtc::PeerConnection::State state) {
            if (state != rtc::PeerConnection::State::Failed && state != rtc::PeerConnection::State::Closed) return;
            // (on the server's thread: not inside libdatachannel's own call)
            std::lock_guard lock(m);
            if (home)
                QMetaObject::invokeMethod(
                    home,
                    [this, ws] {
                        if (std::shared_ptr<Session> s = ws.lock()) endSession(s, false);
                    },
                    Qt::QueuedConnection);
        });
        {
            std::lock_guard lock(m);
            session = s;
        }
        try {
            s->pc->setRemoteDescription(rtc::Description(sdp.toStdString(), rtc::Description::Type::Offer));
        } catch (const std::exception& e) {
            {
                std::lock_guard lock(m);
                session.reset();
            }
            try {
                s->pc->resetCallbacks();
                s->pc->close();
            } catch (const std::exception&) {
            }
            return respond(c, "400 Bad Request", {}, QByteArray("The offer is not one Montage can answer: ") + e.what());
        }
        if (s->pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete) later();
        // (a sender whose connection never comes up is let go after half a minute, for another to try)
        std::lock_guard lock(m);
        if (home)
            QTimer::singleShot(30000, home, [this, ws] {
                std::shared_ptr<Session> s = ws.lock();
                if (!s) return;
                const auto state = s->pc->state();
                if (state != rtc::PeerConnection::State::Connected) endSession(s, false);
            });
    }

    // The sender's first picture and first sound that Montage takes, each answered receive-only with one codec.
    std::shared_ptr<Session> makeSession(const std::string& sdp, const QString& from, std::string& why) {
        const rtc::Description offered(sdp, rtc::Description::Type::Offer);
        auto s = std::make_shared<Session>();
        s->id = QByteArray::number(QRandomGenerator::system()->generate64(), 16).toStdString() + QByteArray::number(QRandomGenerator::system()->generate64(), 16).toStdString();
        s->from = from;
        s->feed = std::make_shared<Feed>();
        rtc::Configuration conf;
        conf.iceServers = iceServers;
        // (media on the endpoint's own port number, over UDP: one port to open; with a relay, a port of its own)
        if (!relayInUse) {
            conf.enableIceUdpMux = true;
            conf.portRangeBegin = conf.portRangeEnd = uint16_t(port);
        }
        if (host != QHostAddress::Any && host != QHostAddress::AnyIPv6 && host != QHostAddress::AnyIPv4) conf.bindAddress = host.toString().toStdString();
        s->pc = std::make_shared<rtc::PeerConnection>(conf);
        std::vector<std::string> pictures{"h264", "vp8", "vp9"};
        if (av1Unpacked()) pictures.push_back("av1");
        for (int i = 0; i < offered.mediaCount(); ++i) {
            const auto entry = offered.media(i);
            const rtc::Description::Media* const* mp = std::get_if<const rtc::Description::Media*>(&entry);
            if (!mp || !*mp) continue;
            const rtc::Description::Media& media = **mp;
            const auto dir = media.direction();
            if (dir != rtc::Description::Direction::SendOnly && dir != rtc::Description::Direction::SendRecv) continue;
            const bool isVideo = media.type() == "video", isAudio = media.type() == "audio";
            Stream& stream = isVideo ? s->feed->video : s->feed->audio;
            if ((!isVideo && !isAudio) || stream.present) continue;
            // The first codec in Montage's order that the offer has.
            int pt = -1;
            std::string codec, fmtp;
            for (const std::string& want : isVideo ? pictures : std::vector<std::string>{"opus"}) {
                for (int candidate : media.payloadTypes()) {
                    const rtc::Description::Media::RtpMap* map = media.rtpMap(candidate);
                    if (!map || lower(map->format) != QString::fromStdString(want)) continue;
                    std::string f;
                    for (const std::string& one : map->fmtps) f += (f.empty() ? "" : ";") + one;
                    // (H.264 in fragments, as every sender makes it; Opus at its one rate)
                    if (want == "h264" && !lower(f).contains("packetization-mode=1")) continue;
                    if (want == "opus" && map->clockRate != 48000) continue;
                    pt = candidate, codec = want, fmtp = f;
                    break;
                }
                if (pt >= 0) break;
            }
            if (pt < 0) continue;
            std::unique_ptr<rtc::Description::Media> answer;
            if (isVideo) {
                auto d = std::make_unique<rtc::Description::Video>(media.mid(), rtc::Description::Direction::RecvOnly);
                const char* name = codec == "h264" ? "H264" : codec == "vp8" ? "VP8" : codec == "vp9" ? "VP9" : "AV1";
                d->addVideoCodec(pt, name, fmtp.empty() ? std::nullopt : std::optional<std::string>(fmtp));
                answer = std::move(d);
            } else {
                auto d = std::make_unique<rtc::Description::Audio>(media.mid(), rtc::Description::Direction::RecvOnly);
                d->addOpusCodec(pt, fmtp.empty() ? std::nullopt : std::optional<std::string>(fmtp));
                answer = std::move(d);
            }
            rtc::Description::Media copy = media;  // (extIds is not const)
            for (int id : copy.extIds())
                if (const rtc::Description::Media::ExtMap* e = copy.extMap(id); e && e->uri == "urn:ietf:params:rtp-hdrext:sdes:mid")
                    answer->addExtMap(rtc::Description::Media::ExtMap(id, e->uri));
            stream.present = true;
            stream.pt = pt;
            stream.clock = isVideo ? 90000 : 48000;
            stream.codec = codec;
            stream.fmtp = fmtp;
            std::shared_ptr<rtc::Track> track = s->pc->addTrack(*answer);
            // (the tap sees each packet and report first; libdatachannel's own session then answers the reports)
            auto receiving = std::make_shared<rtc::RtcpReceivingSession>();
            receiving->addToChain(std::make_shared<Tap>(s->feed, isVideo));
            track->setMediaHandler(receiving);
            track->onMessage([](rtc::message_variant) {});
            if (isVideo) {
                std::weak_ptr<rtc::Track> wt = track;
                track->onOpen([wt] {
                    if (auto t = wt.lock()) {
                        try {
                            t->requestKeyframe();
                            t->requestBitrate(8'000'000);
                        } catch (const std::exception&) {
                        }
                    }
                });
            }
            s->tracks.push_back(std::move(track));
        }
        if (!s->feed->video.present && !s->feed->audio.present) {
            why = "The offer sends no picture or sound Montage takes: H.264, VP8 or VP9 pictures and Opus sound";
            return nullptr;
        }
        return s;
    }
};

Endpoint::Endpoint() : d_(std::make_unique<Impl>()) {}

Endpoint::~Endpoint() { stop(); }

bool Endpoint::start(const Settings& settings, std::string* error) {
    Impl& d = *d_;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (d.thread.joinable()) return fail("Already listening");
    std::string why;
    if (!available(&why)) return fail(why);
    d.settings = settings;
    if (d.settings.path.empty() || d.settings.path[0] != '/') d.settings.path = "/" + d.settings.path;
    while (d.settings.path.size() > 1 && d.settings.path.back() == '/') d.settings.path.pop_back();
    if (d.settings.path == "/") d.settings.path = "/whip";
    const std::string host = settings.host == "localhost" ? std::string("127.0.0.1") : settings.host;
    d.host = QHostAddress(QString::fromStdString(host));
    if (d.host.isNull()) return fail("The address to listen on is a number, such as 127.0.0.1 or 0.0.0.0: " + settings.host);
    if (settings.port < 0 || settings.port > 65535) return fail("The port is between 1 and 65535");
    d.port = settings.port;
    const bool loopback = d.host.isLoopback();
    if (!loopback && settings.token.empty()) return fail("A WHIP endpoint other computers can reach needs a token");
    d.https = !loopback || !settings.certificatePem.empty();
    if (d.https) {
        std::string certPem = settings.certificatePem, keyPem = settings.keyPem;
        if (certPem.empty()) {
            std::vector<std::string> hosts{"localhost", "127.0.0.1"};
            if (d.host == QHostAddress::Any || d.host == QHostAddress::AnyIPv6 || d.host == QHostAddress::AnyIPv4) {
                for (const QHostAddress& a : QNetworkInterface::allAddresses())
                    if (!a.isLoopback() && !a.isLinkLocal()) hosts.push_back(a.toString().toStdString());
            } else {
                hosts.push_back(d.host.toString().toStdString());
            }
            if (!output::makeReviewCertificate(hosts, certPem, keyPem, &why)) return fail(why);
        }
        const QList<QSslCertificate> chain = QSslCertificate::fromData(QByteArray::fromStdString(certPem), QSsl::Pem);
        if (chain.isEmpty()) return fail("The certificate is not one in PEM");
        d.certificate = chain.first();
        d.key = QSslKey(QByteArray::fromStdString(keyPem), QSsl::Ec, QSsl::Pem);
        if (d.key.isNull()) d.key = QSslKey(QByteArray::fromStdString(keyPem), QSsl::Rsa, QSsl::Pem);
        if (d.key.isNull()) return fail("The private key is not an EC or RSA key in PEM (without a password)");
    }
    d.iceServers.clear();
    d.relayInUse = false;
    for (const std::string& s : settings.iceServers) {
        try {
            rtc::IceServer server(s);
            // (Montage's ICE reaches relays over UDP)
            if (server.type == rtc::IceServer::Type::Turn && server.relayType != rtc::IceServer::RelayType::TurnUdp) continue;
            d.relayInUse = d.relayInUse || server.type == rtc::IceServer::Type::Turn;
            d.iceServers.push_back(std::move(server));
        } catch (const std::exception& e) {
            return fail(std::string("ICE server ") + s + ": " + e.what());
        }
    }
    {
        std::lock_guard lock(d.m);
        d.ended = false;
        d.refused = 0;
        d.session.reset();
        d.active.reset();
    }
    std::promise<std::string> started;
    std::future<std::string> result = started.get_future();
    d.thread = std::thread([&d, &started] { d.run(started); });
    why = result.get();
    if (!why.empty()) {
        d.thread.join();
        return fail(why);
    }
    return true;
}

void Endpoint::stop() {
    Impl& d = *d_;
    std::shared_ptr<Session> s;
    std::shared_ptr<Feed> feed;
    {
        std::lock_guard lock(d.m);
        s = std::move(d.session);
        d.session.reset();
        feed = d.active;
        d.ended = true;
        if (d.loop) QMetaObject::invokeMethod(d.loop, &QEventLoop::quit, Qt::QueuedConnection);
    }
    if (feed) feed->end();
    if (s) {
        s->feed->end();
        try {
            s->pc->resetCallbacks();
            for (const auto& t : s->tracks) t->resetCallbacks();
            s->pc->close();
        } catch (const std::exception&) {
        }
    }
    if (d.thread.joinable()) d.thread.join();
}

int Endpoint::port() const { return d_->port; }

std::string Endpoint::url() const {
    const Impl& d = *d_;
    QHostAddress shown = d.host;
    // (listening on every interface: the first address that is not this computer's own loopback, for a sender elsewhere)
    if (shown == QHostAddress::Any || shown == QHostAddress::AnyIPv6 || shown == QHostAddress::AnyIPv4) {
        shown = QHostAddress(QHostAddress::LocalHost);
        for (const QHostAddress& a : QNetworkInterface::allAddresses())
            if (!a.isLoopback() && !a.isLinkLocal() && a.protocol() == QAbstractSocket::IPv4Protocol) {
                shown = a;
                break;
            }
    }
    const std::string host = shown.protocol() == QAbstractSocket::IPv6Protocol ? "[" + shown.toString().toStdString() + "]" : shown.toString().toStdString();
    return std::string(d.https ? "https://" : "http://") + host + ":" + std::to_string(d.port) + d.settings.path;
}

std::string Endpoint::certificateFingerprint() const {
    return d_->https ? d_->certificate.digest(QCryptographicHash::Sha256).toHex(':').toStdString() : std::string();
}

bool Endpoint::waitForMedia(std::string& sdp, const std::function<bool()>& stopped, std::string* error) {
    Impl& d = *d_;
    for (;;) {
        std::shared_ptr<Feed> feed;
        bool ended = false;
        {
            std::lock_guard lock(d.m);
            feed = d.session ? d.session->feed : d.active;
            ended = d.ended;
        }
        if (feed) {
            std::unique_lock lock(feed->m);
            feed->cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return feed->started || feed->ended; });
            if (feed->started) {
                sdp = feed->sdp;
                feed->reading = true;
                lock.unlock();
                std::lock_guard all(d.m);
                d.active = feed;
                return true;
            }
        } else if (!ended) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (ended) {
            if (error) *error = "The sender ended the feed before any picture came";
            return false;
        }
        if (stopped && stopped()) {
            if (error) *error = "Stopped";
            return false;
        }
    }
}

int Endpoint::read(uint8_t* buf, int size, int waitMs) {
    std::shared_ptr<Feed> feed;
    {
        std::lock_guard lock(d_->m);
        feed = d_->active;
    }
    if (!feed) return -1;
    std::unique_lock lock(feed->m);
    for (;;) {
        if (!feed->out.empty()) {
            std::vector<uint8_t> d = std::move(feed->out.front());
            feed->out.pop_front();
            feed->outBytes -= d.size();
            if (int(d.size()) > size) continue;  // (larger than any RTP packet a reader takes)
            std::memcpy(buf, d.data(), d.size());
            return int(d.size());
        }
        if (feed->ended) return -1;
        if (feed->cv.wait_for(lock, std::chrono::milliseconds(std::max(0, waitMs))) == std::cv_status::timeout && feed->out.empty()) return feed->ended ? -1 : 0;
    }
}

double Endpoint::frameRate() const {
    std::shared_ptr<Feed> feed;
    {
        std::lock_guard lock(d_->m);
        feed = d_->active;
    }
    if (!feed) return 0;
    std::lock_guard lock(feed->m);
    return feed->fps;
}

Status Endpoint::status() const {
    const Impl& d = *d_;
    Status st;
    std::shared_ptr<Feed> feed;
    {
        std::lock_guard lock(d.m);
        st.listening = d.listening;
        st.sender = d.session != nullptr;
        if (d.session) st.from = d.session->from.toStdString();
        st.refused = d.refused;
        st.ended = d.ended;
        feed = d.session ? d.session->feed : d.active;
    }
    if (feed) {
        std::lock_guard lock(feed->m);
        if (feed->video.present) st.video = feed->video.codec;
        if (feed->audio.present) st.audio = feed->audio.codec;
        st.reports = feed->byReports;
        st.packets = feed->packets;
        st.lost = feed->lost;
        st.dropped = feed->dropped;
        st.keyframesAsked = feed->keyframesAsked;
    }
    return st;
}

#endif

}  // namespace montage::whip
