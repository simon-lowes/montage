#include "RemoteReviewPanel.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QNetworkInterface>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSpinBox>
#include <QSslCertificate>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>

#include "Settings.h"
#include "render/ColorSpace.h"

namespace montage {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString codecText(const std::string& encoder) {
    if (encoder.find("hevc") != std::string::npos || encoder.find("265") != std::string::npos) return QStringLiteral("HEVC 10-bit");
    return QStringLiteral("H.264 8-bit");
}

// A browser review's encoders ("libx264, libsvtav1") as codecs ("H.264 and AV1").
QString browserCodecs(const std::string& encoders) {
    QStringList out;
    for (const QString& e : QString::fromStdString(encoders).split(QStringLiteral(", "), Qt::SkipEmptyParts))
        out << (e.contains("264") ? QStringLiteral("H.264") : e.contains("av1") ? QStringLiteral("AV1") : QStringLiteral("HEVC"));
    return out.join(QObject::tr(" and "));
}

}  // namespace

RemoteReviewController::RemoteReviewController(PlaybackController* program, QObject* parent)
    : QObject(parent), program_(program), settings_(loadSettings()), options_(loadOptions()) {
    watch_ = new QTimer(this);
    watch_->setInterval(250);
    connect(watch_, &QTimer::timeout, this, [this] {
        if (!scheduler_) return;
        std::vector<output::RemoteMarker> got = backend_->takeMarkers();
        if (!got.empty()) emit markersReceived(sequence_, got);
        const size_t pointing = backend_->pointers().size();
        if (pointing || pointerCount_) emit pointersChanged();
        pointerCount_ = pointing;
        if (scheduler_->running()) return;
        const std::string why = scheduler_->stats().error;
        stop();
        lastError_ = why.empty() ? tr("The stream stopped") : qs(why);
    });
}

RemoteReviewController::~RemoteReviewController() {
    blockSignals(true);  // (the window going: nothing to hand markers to)
    stop();
}

output::StreamSettings RemoteReviewController::loadSettings() {
    QSettings s = appSettings();
    output::StreamSettings c;
    c.address = s.value("review/address", qs(c.address)).toString().toStdString();
    c.listen = s.value("review/listen", true).toBool();
    c.passphrase = s.value("review/passphrase").toString().toStdString();
    const QString codec = s.value("review/codec", "hevc").toString();
    c.codec = codec == "h264" || codec == "av1" ? codec.toStdString() : "hevc";
    c.hardware = s.value("review/hardware", true).toBool();
    c.bitrateKbps = std::clamp(s.value("review/bitrateKbps", c.bitrateKbps).toInt(), 250, 200000);
    c.latencyMs = std::clamp(s.value("review/latencyMs", c.latencyMs).toInt(), 20, 8000);
    c.burnTimecode = s.value("review/burnTimecode", true).toBool();
    c.markersPort = std::clamp(s.value("review/markersPort", 0).toInt(), -1, 65535);
    c.certificateFile = s.value("review/certificateFile").toString().toStdString();
    c.keyFile = s.value("review/keyFile").toString().toStdString();
    for (const QString& ice : s.value("review/iceServers").toStringList()) c.iceServers.push_back(ice.toStdString());
    c.maxViewers = std::clamp(s.value("review/maxViewers", c.maxViewers).toInt(), 1, 64);
    return c;
}

RemoteReviewOptions RemoteReviewController::loadOptions() {
    QSettings s = appSettings();
    RemoteReviewOptions o;
    o.maxHeight = std::clamp(s.value("review/maxHeight", o.maxHeight).toInt(), 0, 4320);
    o.rec709 = s.value("review/rec709", true).toBool();
    o.audio = s.value("review/audio", true).toBool();
    return o;
}

void RemoteReviewController::saveSettings(const output::StreamSettings& c, const RemoteReviewOptions& o) {
    QSettings s = appSettings();
    s.setValue("review/address", qs(c.address));
    s.setValue("review/listen", c.listen);
    s.setValue("review/passphrase", qs(c.passphrase));
    s.setValue("review/codec", qs(c.codec));
    s.setValue("review/hardware", c.hardware);
    s.setValue("review/bitrateKbps", c.bitrateKbps);
    s.setValue("review/latencyMs", c.latencyMs);
    s.setValue("review/burnTimecode", c.burnTimecode);
    s.setValue("review/markersPort", c.markersPort);
    s.setValue("review/certificateFile", qs(c.certificateFile));
    s.setValue("review/keyFile", qs(c.keyFile));
    QStringList ice;
    for (const std::string& i : c.iceServers) ice << qs(i);
    s.setValue("review/iceServers", ice);
    s.setValue("review/maxViewers", c.maxViewers);
    s.setValue("review/maxHeight", o.maxHeight);
    s.setValue("review/rec709", o.rec709);
    s.setValue("review/audio", o.audio);
}

output::Mode RemoteReviewController::modeFor(const Sequence& s, const RemoteReviewOptions& o) {
    int h = std::max(2, s.height), w = std::max(2, s.width);
    if (o.maxHeight > 0 && h > o.maxHeight) {
        w = int(std::lround(double(w) * o.maxHeight / h));
        h = o.maxHeight;
    }
    w = std::max(2, w & ~1);
    h = std::max(2, h & ~1);
    const Rational fps = s.fps.valid() ? s.fps : Rational{25, 1};
    return {output::modeName(w, h, fps, false), w, h, fps, false, 0};
}

QString RemoteReviewController::formatKey() const {
    const Sequence* s = program_ ? program_->sequence() : nullptr;
    if (!s) return {};
    return QStringLiteral("%1 %2x%3@%4/%5 %6 %7 %8")
        .arg(s->id)
        .arg(s->width)
        .arg(s->height)
        .arg(s->fps.num)
        .arg(s->fps.den)
        .arg(qs(sequenceColorSpace(*s).id))
        .arg(s->sampleRate)
        .arg(s->hdrPeakNits);
}

bool RemoteReviewController::start(const output::StreamSettings& settings, const RemoteReviewOptions& options, QString* error) {
    stop();
    settings_ = settings;
    options_ = options;
    auto fail = [&](const QString& why) {
        lastError_ = why;
        if (error) *error = why;
        emit runningChanged(false);
        return false;
    };
    const Sequence* s = program_ ? program_->sequence() : nullptr;
    if (!s) return fail(tr("Open a sequence first"));
    const bool web = output::isBrowserAddress(settings.address);
    std::shared_ptr<output::ReviewBackend> backend;
    if (web) backend = std::make_shared<output::BrowserReviewBackend>();
    else backend = std::make_shared<output::StreamBackend>();
    std::string why;
    if (!backend->available(&why)) return fail(qs(why));
    output::StreamSettings used = settings;
    if (web && settings.address.rfind("https", 0) == 0 && settings.certificateFile.empty()) {
        // A certificate of Montage's own for this computer, kept so a reviewer's browser accepts it once.
        QString cert, key, err;
        if (!ownCertificate(cert, key, &err)) return fail(err);
        used.certificateFile = cert.toStdString();
        used.keyFile = key.toStdString();
    }
    backend->setSettings(used);
    backend->setSequence(s->id);
    output::Config config;
    config.backend = backend->name();
    config.depth = web || settings.codec == "h264" ? 8 : 10;
    config.rec709 = web || options.rec709;  // (browsers are sent the picture as the viewer shows it)
    config.audioChannels = options.audio ? 2 : 0;
    config.scaling = output::Config::Scaling::Fit;
    const std::vector<output::Device> devices = backend->devices();
    auto sched = std::make_shared<output::Scheduler>(backend);
    if (!sched->start(devices.front(), modeFor(*s, options), *s, config, &why)) return fail(qs(why));
    saveSettings(settings, options);
    backend_ = backend;
    scheduler_ = sched;
    sequence_ = s->id;
    startedFor_ = formatKey();
    lastError_.clear();
    program_->setReviewOutput(scheduler_);
    watch_->start();
    emit runningChanged(true);
    return true;
}

void RemoteReviewController::stop() {
    if (!scheduler_) return;
    watch_->stop();
    if (program_) program_->setReviewOutput(nullptr);
    // (markers that came in the last moments are still handed on)
    std::vector<output::RemoteMarker> last = backend_->takeMarkers();
    scheduler_->stop();
    for (output::RemoteMarker& m : backend_->takeMarkers()) last.push_back(std::move(m));
    scheduler_.reset();
    backend_.reset();
    if (!last.empty()) emit markersReceived(sequence_, last);
    if (std::exchange(pointerCount_, 0)) emit pointersChanged();
    emit runningChanged(false);
}

bool RemoteReviewController::running() const { return scheduler_ && scheduler_->running(); }

void RemoteReviewController::sequenceChanged() {
    if (!scheduler_ || formatKey() == startedFor_) return;
    QString why;
    if (!start(settings_, options_, &why)) lastError_ = why;
}

std::vector<output::ReviewPointer> RemoteReviewController::pointers() const { return backend_ ? backend_->pointers() : std::vector<output::ReviewPointer>{}; }

void RemoteReviewController::paintOverlay(QPainter& p, const QRectF& picture) const {
    const std::vector<output::ReviewPointer> list = pointers();
    if (list.empty()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (const output::ReviewPointer& r : list) {
        const QPointF at(picture.left() + r.x * picture.width(), picture.top() + r.y * picture.height());
        const QColor colour(qs(r.color));
        p.setPen(QPen(Qt::white, 2));
        p.setBrush(colour.isValid() ? colour : QColor(Qt::red));
        p.drawEllipse(at, 7, 7);
        p.setPen(Qt::white);
        if (!r.author.empty()) p.drawText(at + QPointF(12, -8), qs(r.author));
    }
    p.restore();
}

bool RemoteReviewController::ownCertificate(QString& certificateFile, QString& keyFile, QString* error) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/review");
    certificateFile = dir + QStringLiteral("/certificate.pem");
    keyFile = dir + QStringLiteral("/key.pem");
    const std::vector<std::string> hosts = output::reviewCertificateHosts();
    // Kept while it names every address this computer has now and has a month left.
    const QList<QSslCertificate> had = QSslCertificate::fromPath(certificateFile);
    if (!had.isEmpty() && QFileInfo::exists(keyFile) && had.first().expiryDate() > QDateTime::currentDateTimeUtc().addDays(30)) {
        const auto names = had.first().subjectAlternativeNames();
        bool all = true;
        for (const std::string& h : hosts) all = all && (names.values(QSsl::DnsEntry).contains(qs(h)) || names.values(QSsl::IpAddressEntry).contains(qs(h)));
        if (all) return true;
    }
    std::string cert, key, why;
    if (!output::makeReviewCertificate(hosts, cert, key, &why)) {
        if (error) *error = qs(why);
        return false;
    }
    QDir().mkpath(dir);
    QSaveFile c(certificateFile), k(keyFile);
    k.setDirectWriteFallback(true);
    c.setDirectWriteFallback(true);
    if (!k.open(QIODevice::WriteOnly) || !c.open(QIODevice::WriteOnly)) {
        if (error) *error = tr("Cannot write %1").arg(dir);
        return false;
    }
    k.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);  // (the private key: this user's alone)
    k.write(QByteArray::fromStdString(key));
    c.write(QByteArray::fromStdString(cert));
    if (!k.commit() || !c.commit()) {
        QFile::remove(keyFile);  // (so a new key is never kept beside the old certificate: both are made again next time)
        if (error) *error = tr("Cannot write %1").arg(dir);
        return false;
    }
    return true;
}

QStringList RemoteReviewController::viewerAddresses() const {
    std::string scheme, host;
    int port = 0;
    if (!output::parseStreamAddress(settings_.address, scheme, host, port)) return {};
    if (browser()) {
        if (host != "0.0.0.0" && host != "::") return {QStringLiteral("%1://%2:%3/").arg(qs(scheme), qs(host)).arg(port)};
        QStringList out;
        for (const QHostAddress& a : QNetworkInterface::allAddresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal())
                out << QStringLiteral("%1://%2:%3/").arg(qs(scheme), a.toString()).arg(port);
        if (out.isEmpty()) out << QStringLiteral("%1://127.0.0.1:%2/").arg(qs(scheme)).arg(port);
        return out;
    }
    if (!settings_.listen || (host != "0.0.0.0" && host != "::")) return {qs(settings_.address)};
    QStringList out;
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal())
            out << QStringLiteral("%1://%2:%3").arg(qs(scheme), a.toString()).arg(port);
    if (out.isEmpty()) out << QStringLiteral("%1://127.0.0.1:%2").arg(qs(scheme)).arg(port);
    return out;
}

QString RemoteReviewController::status() const {
    if (!scheduler_) return lastError_.isEmpty() ? tr("Off") : lastError_;
    if (!scheduler_->running()) {
        const std::string why = scheduler_->stats().error;
        return why.empty() ? tr("Stopped") : qs(why);
    }
    const output::ReviewStats st = backend_->stats();
    const output::Mode& m = scheduler_->mode();
    if (browser()) {
        const QString links = viewerAddresses().join(tr(" or "));
        QString text = st.viewers > 0 ? tr("%n viewer(s) watching in a browser: %1 × %2", "", st.viewers).arg(m.width).arg(m.height)
                                      : tr("Waiting for viewers at %1 (%2 × %3)").arg(links).arg(m.width).arg(m.height);
        if (st.viewers > 0 && !st.encoder.empty()) text += tr(", %1, %2 Mb/s each").arg(browserCodecs(st.encoder)).arg(st.kbps / 1000, 0, 'f', 1);
        if (st.skipped > 0) text += tr(", %n frame(s) skipped (encoding fell behind)", "", int(std::min<int64_t>(st.skipped, 1 << 30)));
        if (!st.error.empty()) text += QStringLiteral(" (%1)").arg(qs(st.error));
        return text + tr("; %n note(s) received", "", st.markers);
    }
    const QString picture = tr("%1 %2 × %3 (%4)").arg(codecText(st.encoder)).arg(m.width).arg(m.height).arg(qs(st.encoder));
    QString text;
    if (st.connected) {
        text = settings_.listen && settings_.address.rfind("srt", 0) == 0 ? tr("Streaming to a viewer: %1, %2 Mb/s").arg(picture).arg(st.kbps / 1000, 0, 'f', 1)
                                                                           : tr("Streaming: %1, %2 Mb/s").arg(picture).arg(st.kbps / 1000, 0, 'f', 1);
        if (st.skipped > 0) text += tr(", %n frame(s) skipped (encoding fell behind)", "", int(std::min<int64_t>(st.skipped, 1 << 30)));
    } else {
        text = settings_.listen ? tr("Waiting for a viewer to call: %1").arg(picture) : tr("Connecting to %1: %2").arg(qs(settings_.address), picture);
        if (!st.error.empty()) text += QStringLiteral(" (%1)").arg(qs(st.error));
    }
    if (st.markersPort >= 0) text += tr("; markers on port %1: %n received", "", st.markers).arg(st.markersPort);
    return text;
}

QByteArray RemoteReviewController::toolJson() {
    return QByteArrayLiteral(R"json({"name":"montage_remote_review","title":"Live remote review",
"description":"Streams the Program monitor to reviewers elsewhere as it plays or parks, at low latency, the timecode burnt in. Over SRT (or UDP): HEVC 10-bit (or H.264 8-bit) with stereo sound in an MPEG transport stream (AES-256 with a passphrase; Montage listening for a viewer, or calling a relay that viewers watch); viewers watch in Montage (File > Watch Remote Review) or any player that reads SRT (ffplay), and a Montage viewer's notes come back as markers. With webrtc: to any web browser, which opens a link to a page Montage serves (HTTPS) and plays H.264 (or AV1 or HEVC) with Opus sound over WebRTC, several viewers at once, each asked for the passphrase; their notes and drawings come back as markers at the frame they saw, and other WHEP players can pull the stream from /whep. \"status\": whether it is on, the viewers connected, the rate and the markers received. \"start\": settings left out are the last used (a passphrase is made when there is none; it is given back). \"stop\".",
"inputSchema":{"type":"object","properties":{
"action":{"type":"string","enum":["status","start","stop"]},
"webrtc":{"type":"boolean","description":"To web browsers over WebRTC (the address https://0.0.0.0:8443 unless one is given); false: over SRT (srt://0.0.0.0:9000 unless given)"},
"address":{"type":"string","description":"srt://0.0.0.0:9000 (listening on every address), srt://relay:port?streamid=… (with listen false), udp://host:port, or for browsers https://0.0.0.0:8443 (TCP for the page and UDP for the media, both on that port)"},
"listen":{"type":"boolean","description":"SRT: wait for a viewer to call (true) or call the address (false)"},
"passphrase":{"type":"string","description":"SRT's key and the marker channel's, 10 to 79 characters"},
"codec":{"type":"string","enum":["hevc","h264","av1"],"description":"Over SRT hevc or h264; for browsers the one preferred (h264, av1 or hevc), another where a browser lacks it"},
"ice_servers":{"type":"array","items":{"type":"string"},"description":"Browsers: STUN and TURN servers for viewers on other networks, such as stun:stun.example.com:3478 or turn:user:password@turn.example.com:3478 (none by default)"},
"certificate":{"type":"string","description":"Browsers: a PEM certificate file for https (with key); none: one Montage makes for this computer, which browsers warn about once"},
"key":{"type":"string","description":"Browsers: the certificate's private key (PEM, without a password)"},
"max_viewers":{"type":"number","description":"Browsers: viewers at once (1 to 64)"},
"hardware":{"type":"boolean","description":"Encode on NVENC where there is one"},
"bitrate_kbps":{"type":"number","description":"250 to 200000"},
"height":{"type":"number","description":"The picture scaled down to this height (0: the sequence's)"},
"rec709":{"type":"boolean","description":"The picture as the viewer shows it (HDR tone mapped); false: the sequence's own colour"},
"burn_timecode":{"type":"boolean"},
"audio":{"type":"boolean"},
"latency_ms":{"type":"number","description":"SRT's buffer against loss (20 to 8000)"},
"markers_port":{"type":"number","description":"TCP port reviewers send markers to: 0 the stream's port + 1, -1 none"}},
"required":["action"]},
"annotations":{"readOnlyHint":false,"destructiveHint":false,"openWorldHint":true}})json");
}

QString RemoteReviewController::runTool(const QJsonObject& args, QJsonObject& structured, bool& isError) {
    const QString action = args.value("action").toString();
    if (action == "stop") {
        stop();
        lastError_.clear();
        structured["running"] = false;
        return tr("Remote review off");
    }
    if (action == "start") {
        output::StreamSettings c = settings_;
        RemoteReviewOptions o = options_;
        auto has = [&](const char* k) { return args.contains(QLatin1String(k)); };
        if (has("webrtc") && args.value("webrtc").toBool() != output::isBrowserAddress(c.address) && !has("address")) {
            c.address = args.value("webrtc").toBool() ? "https://0.0.0.0:8443" : "srt://0.0.0.0:9000";
            if (args.value("webrtc").toBool() && c.codec == "hevc") c.codec = "h264";
            if (!args.value("webrtc").toBool() && c.codec == "av1") c.codec = "hevc";
        }
        if (has("address")) c.address = args.value("address").toString().toStdString();
        if (has("listen")) c.listen = args.value("listen").toBool();
        if (has("passphrase")) c.passphrase = args.value("passphrase").toString().toStdString();
        if (has("codec")) c.codec = args.value("codec").toString().toStdString();
        if (has("hardware")) c.hardware = args.value("hardware").toBool();
        if (has("bitrate_kbps")) c.bitrateKbps = args.value("bitrate_kbps").toInt();
        if (has("burn_timecode")) c.burnTimecode = args.value("burn_timecode").toBool();
        if (has("latency_ms")) c.latencyMs = std::clamp(args.value("latency_ms").toInt(), 20, 8000);
        if (has("markers_port")) c.markersPort = args.value("markers_port").toInt();
        if (has("ice_servers")) {
            c.iceServers.clear();
            for (const QJsonValue& v : args.value("ice_servers").toArray()) c.iceServers.push_back(v.toString().toStdString());
        }
        if (has("certificate")) c.certificateFile = args.value("certificate").toString().toStdString();
        if (has("key")) c.keyFile = args.value("key").toString().toStdString();
        if (has("max_viewers")) c.maxViewers = args.value("max_viewers").toInt();
        if (has("height")) o.maxHeight = std::clamp(args.value("height").toInt(), 0, 4320);
        if (has("rec709")) o.rec709 = args.value("rec709").toBool();
        if (has("audio")) o.audio = args.value("audio").toBool();
        if (c.passphrase.empty() && (c.address.rfind("srt", 0) == 0 || output::isBrowserAddress(c.address))) c.passphrase = output::generatePassphrase();
        const std::string why = output::checkStreamSettings(c);
        if (!why.empty()) {
            isError = true;
            return qs(why);
        }
        QString err;
        if (!start(c, o, &err)) {
            isError = true;
            return err;
        }
        structured["passphrase"] = qs(c.passphrase);
    } else if (action != "status") {
        isError = true;
        return tr("action is status, start or stop");
    }
    structured["running"] = running();
    if (backend_) {
        const output::ReviewStats st = backend_->stats();
        structured["webrtc"] = browser();
        structured["viewers"] = st.viewers;
        structured["viewer_connected"] = st.connected;
        structured["encoder"] = qs(st.encoder);
        structured["kbps"] = std::round(st.kbps);
        structured["markers_received"] = st.markers;
        if (st.markersPort >= 0) structured["markers_port"] = st.markersPort;
        structured["addresses"] = QJsonArray::fromStringList(viewerAddresses());
        structured["mode"] = qs(scheduler_->mode().name);
    }
    return status();
}

// ---------------------------------------------------------------------------

RemoteReviewPanel::RemoteReviewPanel(RemoteReviewController* controller, QWidget* parent) : QDialog(parent), controller_(controller) {
    setObjectName(QStringLiteral("remoteReviewDialog"));
    setWindowTitle(tr("Remote Review"));
    setAttribute(Qt::WA_DeleteOnClose);
    auto* v = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("The Program streamed live to reviewers elsewhere, with its sound and the timecode burnt in. They watch in "
                                "Montage (File › Watch Remote Review…, which can send notes back as markers) or any player that reads "
                                "SRT, such as FFmpeg's ffplay; or in any web browser, from a link to a page this computer serves, "
                                "where they type notes and draw on the picture. Over SRT the stream is encrypted with the passphrase, "
                                "and in a browser everything is (HTTPS and WebRTC). The passphrase is kept in Montage's settings on this "
                                "computer."),
                             this);
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto* form = new QFormLayout;
    form_ = form;
    v->addLayout(form);
    const output::StreamSettings st = controller->settings();
    const RemoteReviewOptions op = controller->options();
    const bool web = output::isBrowserAddress(st.address);
    srtAddress_ = web ? QStringLiteral("srt://0.0.0.0:9000") : qs(st.address);
    browserAddress_ = web ? qs(st.address) : QStringLiteral("https://0.0.0.0:8443");

    viewers_ = new QComboBox(this);
    viewers_->setObjectName(QStringLiteral("reviewStreamViewers"));
    viewers_->addItem(tr("In Montage or an SRT player"), false);
    viewers_->addItem(tr("In any web browser"), true);
    viewers_->setCurrentIndex(web ? 1 : 0);
    form->addRow(tr("Viewers watch:"), viewers_);
    address_ = new QLineEdit(qs(st.address), this);
    address_->setObjectName(QStringLiteral("reviewStreamAddress"));
    address_->setToolTip(tr("srt://0.0.0.0:9000 listens on every network address of this computer; srt://server:port?streamid=… calls a relay; "
                            "udp://host:port sends unencrypted on a local network"));
    form->addRow(tr("Address:"), address_);
    direction_ = new QComboBox(this);
    direction_->setObjectName(QStringLiteral("reviewStreamDirection"));
    direction_->addItem(tr("Viewers call this computer (one at a time)"), true);
    direction_->addItem(tr("Send to a server that viewers watch"), false);
    direction_->setCurrentIndex(st.listen ? 0 : 1);
    form->addRow(tr("Connection:"), direction_);
    auto* passRow = new QHBoxLayout;
    passphrase_ = new QLineEdit(qs(st.passphrase.empty() ? output::generatePassphrase() : st.passphrase), this);
    passphrase_->setObjectName(QStringLiteral("reviewStreamPassphrase"));
    auto* generate = new QPushButton(tr("New"), this);
    generate->setObjectName(QStringLiteral("reviewStreamGenerate"));
    connect(generate, &QPushButton::clicked, this, [this] { passphrase_->setText(qs(output::generatePassphrase())); });
    passRow->addWidget(passphrase_, 1);
    passRow->addWidget(generate);
    form->addRow(tr("Passphrase:"), passRow);
    codec_ = new QComboBox(this);
    codec_->setObjectName(QStringLiteral("reviewStreamCodec"));
    codec_->setProperty("wanted", qs(st.codec));
    form->addRow(tr("Codec:"), codec_);
    hardware_ = new QCheckBox(tr("Encode on the graphics card (NVENC) where there is one"), this);
    hardware_->setObjectName(QStringLiteral("reviewStreamHardware"));
    hardware_->setChecked(st.hardware);
    form->addRow(QString(), hardware_);
    size_ = new QComboBox(this);
    size_->setObjectName(QStringLiteral("reviewStreamSize"));
    for (int h : {0, 2160, 1080, 720, 540}) size_->addItem(h ? tr("Up to %1 lines").arg(h) : tr("The sequence's size"), h);
    size_->setCurrentIndex(std::max(0, size_->findData(op.maxHeight)));
    form->addRow(tr("Size:"), size_);
    bitrate_ = new QSpinBox(this);
    bitrate_->setObjectName(QStringLiteral("reviewStreamBitrate"));
    bitrate_->setRange(250, 200000);
    bitrate_->setSingleStep(1000);
    bitrate_->setSuffix(tr(" kb/s"));
    bitrate_->setValue(st.bitrateKbps);
    form->addRow(tr("Bit rate:"), bitrate_);
    colour_ = new QComboBox(this);
    colour_->addItem(tr("As the viewer shows it (Rec. 709)"), true);
    colour_->addItem(tr("The sequence's own (HDR as HDR)"), false);
    colour_->setCurrentIndex(op.rec709 ? 0 : 1);
    form->addRow(tr("Colour:"), colour_);
    burn_ = new QCheckBox(tr("Burn in the timecode"), this);
    burn_->setChecked(st.burnTimecode);
    form->addRow(QString(), burn_);
    audio_ = new QCheckBox(tr("Sound (a stereo mix)"), this);
    audio_->setChecked(op.audio);
    form->addRow(QString(), audio_);
    auto* markersRow = new QWidget(this);
    markersRow->setObjectName(QStringLiteral("reviewStreamMarkersRow"));
    auto* markRow = new QHBoxLayout(markersRow);
    markRow->setContentsMargins(0, 0, 0, 0);
    markers_ = new QCheckBox(tr("Take markers from reviewers, on port"), this);
    markers_->setObjectName(QStringLiteral("reviewStreamMarkers"));
    markers_->setChecked(st.markersPort >= 0);
    markersPort_ = new QSpinBox(this);
    markersPort_->setObjectName(QStringLiteral("reviewStreamMarkersPort"));
    markersPort_->setRange(0, 65535);
    markersPort_->setSpecialValueText(tr("the stream's + 1"));
    markersPort_->setValue(std::max(0, st.markersPort));
    connect(markers_, &QCheckBox::toggled, markersPort_, &QWidget::setEnabled);
    markersPort_->setEnabled(markers_->isChecked());
    markRow->addWidget(markers_);
    markRow->addWidget(markersPort_);
    markRow->addStretch(1);
    form->addRow(QString(), markersRow);
    latency_ = new QSpinBox(this);
    latency_->setRange(20, 8000);
    latency_->setSuffix(tr(" ms"));
    latency_->setValue(st.latencyMs);
    latency_->setToolTip(tr("SRT's buffer against lost packets: about four times the round trip to the viewer"));
    form->addRow(tr("Latency:"), latency_);
    // Browsers only.
    certificate_ = new QLineEdit(qs(st.certificateFile), this);
    certificate_->setObjectName(QStringLiteral("reviewStreamCertificate"));
    certificate_->setPlaceholderText(tr("None: one Montage makes for this computer"));
    certificate_->setToolTip(tr("A certificate (PEM) for this computer's name, such as one from your own domain, so browsers trust the page "
                                "without a warning. Without one, reviewers accept Montage's own once."));
    form->addRow(tr("Certificate:"), certificate_);
    key_ = new QLineEdit(qs(st.keyFile), this);
    key_->setObjectName(QStringLiteral("reviewStreamKey"));
    key_->setPlaceholderText(tr("The certificate's private key (PEM)"));
    form->addRow(tr("Private key:"), key_);
    QStringList ice;
    for (const std::string& i : st.iceServers) ice << qs(i);
    ice_ = new QLineEdit(ice.join(QLatin1Char(' ')), this);
    ice_->setObjectName(QStringLiteral("reviewStreamIce"));
    ice_->setPlaceholderText(tr("None (viewers on this network, or reaching this computer's port)"));
    ice_->setToolTip(tr("STUN and TURN servers that help viewers on other networks reach this computer, separated by spaces: "
                        "stun:host:3478, turn:user:password@host:3478. None is used unless named here."));
    form->addRow(tr("ICE servers:"), ice_);
    maxViewers_ = new QSpinBox(this);
    maxViewers_->setObjectName(QStringLiteral("reviewStreamMaxViewers"));
    maxViewers_->setRange(1, 64);
    maxViewers_->setValue(st.maxViewers);
    form->addRow(tr("Viewers at once:"), maxViewers_);
    connect(viewers_, &QComboBox::currentIndexChanged, this, [this] {
        // Each kind keeps its own address while the other is chosen.
        const bool web = viewers_->currentData().toBool();
        const QString now = address_->text().trimmed();
        if (web && !output::isBrowserAddress(now.toStdString())) {
            srtAddress_ = now;
            address_->setText(browserAddress_);
        } else if (!web && output::isBrowserAddress(now.toStdString())) {
            browserAddress_ = now;
            address_->setText(srtAddress_);
        }
        viewersChanged();
    });
    viewersChanged();

    howTo_ = new QLabel(this);
    howTo_->setObjectName(QStringLiteral("reviewStreamHowTo"));
    howTo_->setWordWrap(true);
    howTo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(howTo_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("reviewStreamStatus"));
    status_->setWordWrap(true);
    v->addWidget(status_);
    auto* buttons = new QHBoxLayout;
    start_ = new QPushButton(this);
    start_->setObjectName(QStringLiteral("reviewStreamStart"));
    auto* close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    buttons->addStretch(1);
    buttons->addWidget(start_);
    buttons->addWidget(close);
    v->addLayout(buttons);
    connect(start_, &QPushButton::clicked, this, [this] {
        if (controller_->running()) {
            controller_->stop();
        } else {
            const output::StreamSettings s = chosenSettings();
            const std::string why = output::checkStreamSettings(s);
            if (!why.empty()) {
                status_->setText(qs(why));
                return;
            }
            QString err;
            if (!controller_->start(s, chosenOptions(), &err)) status_->setText(err);
        }
        refresh();
    });
    connect(controller_, &RemoteReviewController::runningChanged, this, [this] { refresh(); });
    timer_ = new QTimer(this);
    timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
    timer_->start();
    refresh();
}

void RemoteReviewPanel::viewersChanged() {
    const bool web = viewers_->currentData().toBool();
    const QString wanted = codec_->currentData().isValid() ? codec_->currentData().toString() : codec_->property("wanted").toString();
    codec_->clear();
    if (web) {
        codec_->addItem(tr("H.264 (every browser)"), QStringLiteral("h264"));
        codec_->addItem(tr("AV1, where the browser plays it"), QStringLiteral("av1"));
        codec_->addItem(tr("HEVC, where the browser plays it"), QStringLiteral("hevc"));
    } else {
        codec_->addItem(tr("HEVC, 10-bit"), QStringLiteral("hevc"));
        codec_->addItem(tr("H.264, 8-bit (older players)"), QStringLiteral("h264"));
    }
    codec_->setCurrentIndex(std::max(0, codec_->findData(wanted == "hevc" && web ? QStringLiteral("h264") : wanted)));
    address_->setToolTip(web ? tr("https://0.0.0.0:8443 serves the page on every network address of this computer; the picture goes out over "
                                  "UDP on the same port number")
                             : tr("srt://0.0.0.0:9000 listens on every network address of this computer; srt://server:port?streamid=… calls a "
                                  "relay; udp://host:port sends unencrypted on a local network"));
    for (QWidget* w : std::initializer_list<QWidget*>{direction_, latency_}) form_->setRowVisible(w, !web);
    form_->setRowVisible(findChild<QWidget*>(QStringLiteral("reviewStreamMarkersRow")), !web);
    for (QWidget* w : std::initializer_list<QWidget*>{certificate_, key_, ice_, maxViewers_}) form_->setRowVisible(w, web);
    if (web) colour_->setCurrentIndex(0);
    colour_->setEnabled(!web && !controller_->running());
}

output::StreamSettings RemoteReviewPanel::chosenSettings() const {
    output::StreamSettings s;
    s.address = address_->text().trimmed().toStdString();
    s.listen = direction_->currentData().toBool();
    s.passphrase = passphrase_->text().toStdString();
    s.codec = codec_->currentData().toString().toStdString();
    s.hardware = hardware_->isChecked();
    s.bitrateKbps = bitrate_->value();
    s.latencyMs = latency_->value();
    s.burnTimecode = burn_->isChecked();
    s.markersPort = markers_->isChecked() ? markersPort_->value() : -1;
    s.certificateFile = certificate_->text().trimmed().toStdString();
    s.keyFile = key_->text().trimmed().toStdString();
    for (const QString& i : ice_->text().split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts)) s.iceServers.push_back(i.toStdString());
    s.maxViewers = maxViewers_->value();
    return s;
}

RemoteReviewOptions RemoteReviewPanel::chosenOptions() const {
    RemoteReviewOptions o;
    o.maxHeight = size_->currentData().toInt();
    o.rec709 = colour_->currentData().toBool();
    o.audio = audio_->isChecked();
    return o;
}

void RemoteReviewPanel::refresh() {
    const bool on = controller_->running();
    start_->setText(on ? tr("Stop") : tr("Start"));
    for (QWidget* w : std::initializer_list<QWidget*>{viewers_, address_, direction_, passphrase_, codec_, hardware_, size_, bitrate_, colour_, burn_, audio_,
                                                      markers_, latency_, certificate_, key_, ice_, maxViewers_})
        w->setEnabled(!on);
    markersPort_->setEnabled(!on && markers_->isChecked());
    colour_->setEnabled(!on && !viewers_->currentData().toBool());
    status_->setText(controller_->status());
    if (!on) {
        howTo_->clear();
        return;
    }
    const output::StreamSettings s = controller_->settings();
    const QStringList at = controller_->viewerAddresses();
    if (controller_->browser()) {
        QString text = tr("Reviewers open %1 in a web browser and give the passphrase %2. Notes and drawings they add come back as markers at the "
                          "frame they saw.")
                           .arg(at.join(tr(" or ")), qs(s.passphrase));
        if (s.certificateFile.empty() && s.address.rfind("https", 0) == 0)
            text += tr(" The browser warns about Montage's own certificate the first time: they go on to the page.");
        if (s.iceServers.empty())
            text += tr(" From another network they need TCP and UDP port %1 forwarded here, or an ICE server named above.").arg(qs(s.address.substr(s.address.rfind(':') + 1)));
        howTo_->setText(text);
        return;
    }
    QString text = tr("Viewers open %1").arg(at.join(tr(" or ")));
    if (!s.passphrase.empty()) text += tr(" with the passphrase %1").arg(qs(s.passphrase));
    text += tr(". In Montage: File › Watch Remote Review…");
    if (!at.isEmpty() && s.address.rfind("srt", 0) == 0 && s.listen)
        text += tr("; with FFmpeg: ffplay -fflags nobuffer \"%1?passphrase=%2\"").arg(at.front(), qs(s.passphrase));
    if (const int port = output::markersPortFor(s); port >= 0) {
        // (sent to this computer, whichever way the stream goes)
        QStringList hosts;
        for (const QHostAddress& a : QNetworkInterface::allAddresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal()) hosts << QStringLiteral("%1:%2").arg(a.toString()).arg(port);
        if (hosts.isEmpty()) hosts << QStringLiteral("127.0.0.1:%1").arg(port);
        text += s.listen && s.address.rfind("srt", 0) == 0 ? tr(". Notes come back as markers over TCP port %1.").arg(port)
                                                            : tr(". Notes come back as markers: viewers set Markers to %1.").arg(hosts.join(tr(" or ")));
    }
    howTo_->setText(text);
}

// ---------------------------------------------------------------------------

ReviewPicture::ReviewPicture(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void ReviewPicture::setImage(const QImage& img) {
    image_ = img;
    update();
}

void ReviewPicture::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (image_.isNull()) return;
    QSize s = image_.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect r((width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height());
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(r, image_);
}

ReviewViewerWindow::ReviewViewerWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setObjectName(QStringLiteral("reviewViewer"));
    setWindowTitle(tr("Watch Remote Review"));
    setAttribute(Qt::WA_DeleteOnClose);
    QSettings settings = appSettings();
    auto* v = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    address_ = new QLineEdit(settings.value("reviewViewer/address", QStringLiteral("srt://")).toString(), this);
    address_->setObjectName(QStringLiteral("reviewAddress"));
    address_->setPlaceholderText(tr("srt://editor's-address:9000"));
    passphrase_ = new QLineEdit(this);
    passphrase_->setObjectName(QStringLiteral("reviewPassphrase"));
    passphrase_->setEchoMode(QLineEdit::Password);
    passphrase_->setPlaceholderText(tr("Passphrase"));
    connect_ = new QPushButton(tr("Watch"), this);
    connect_->setObjectName(QStringLiteral("reviewConnect"));
    top->addWidget(new QLabel(tr("Stream:"), this));
    top->addWidget(address_, 2);
    top->addWidget(passphrase_, 1);
    top->addWidget(connect_);
    v->addLayout(top);
    picture_ = new ReviewPicture(this);
    picture_->setObjectName(QStringLiteral("reviewPicture"));
    v->addWidget(picture_, 1);
    auto* bottom = new QHBoxLayout;
    timecode_ = new QLabel(QStringLiteral("--:--:--:--"), this);
    timecode_->setObjectName(QStringLiteral("reviewTimecode"));
    QFont mono = timecode_->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    timecode_->setFont(mono);
    author_ = new QLineEdit(settings.value("reviewViewer/author").toString(), this);
    author_->setObjectName(QStringLiteral("reviewAuthor"));
    author_->setPlaceholderText(tr("Your name"));
    author_->setMaximumWidth(140);
    note_ = new QLineEdit(this);
    note_->setObjectName(QStringLiteral("reviewNote"));
    note_->setPlaceholderText(tr("A note at this frame (Return sends it as a marker)"));
    mark_ = new QPushButton(tr("Add Marker"), this);
    mark_->setObjectName(QStringLiteral("reviewAddMarker"));
    markersTo_ = new QLineEdit(settings.value("reviewViewer/markersTo").toString(), this);
    markersTo_->setObjectName(QStringLiteral("reviewMarkersTo"));
    markersTo_->setPlaceholderText(tr("Markers to: the stream's host, port + 1"));
    markersTo_->setToolTip(tr("Where the editor's Montage takes markers (host:port), when the stream comes through a server or over UDP"));
    markersTo_->setMaximumWidth(220);
    bottom->addWidget(timecode_);
    bottom->addWidget(author_);
    bottom->addWidget(note_, 1);
    bottom->addWidget(mark_);
    bottom->addWidget(markersTo_);
    v->addLayout(bottom);
    status_ = new QLabel(tr("Not watching"), this);
    status_->setObjectName(QStringLiteral("reviewStatus"));
    status_->setWordWrap(true);
    v->addWidget(status_);
    connect(connect_, &QPushButton::clicked, this, [this] { toggle(); });
    connect(mark_, &QPushButton::clicked, this, [this] { addMarker(); });
    connect(note_, &QLineEdit::returnPressed, this, [this] { addMarker(); });
    timer_ = new QTimer(this);
    timer_->setInterval(15);
    connect(timer_, &QTimer::timeout, this, [this] { poll(); });
    resize(960, 640);
}

ReviewViewerWindow::~ReviewViewerWindow() {
    receiver_.stop();
    stopSound();
}

void ReviewViewerWindow::stopSound() {
    if (sink_) {
        sink_->stop();
        delete sink_;
    }
    sink_ = nullptr;
    sound_ = nullptr;
}

void ReviewViewerWindow::toggle() {
    if (watching_) {
        receiver_.stop();
        stopSound();
        timer_->stop();
        watching_ = false;
        connect_->setText(tr("Watch"));
        status_->setText(tr("Not watching"));
        return;
    }
    std::string why;
    const std::string address = address_->text().trimmed().toStdString();
    if (!receiver_.start(address, passphrase_->text().toStdString(), 120, false, &why)) {
        status_->setText(qs(why));
        return;
    }
    QSettings settings = appSettings();
    settings.setValue("reviewViewer/address", address_->text().trimmed());
    watching_ = true;
    serial_ = 0;
    tag_ = {};
    connect_->setText(tr("Stop"));
    status_->setText(tr("Connecting…"));
    // Its sound on this computer's speakers, as it comes (a small buffer: a review is live).
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(2);
    fmt.setSampleFormat(QAudioFormat::Float);
    if (!dev.isNull() && dev.isFormatSupported(fmt)) {
        sink_ = new QAudioSink(dev, fmt);
        sink_->setBufferSize(fmt.bytesForDuration(150000));
        sound_ = sink_->start();
    }
    timer_->start();
}

void ReviewViewerWindow::poll() {
    output::ReviewReceiver::Picture pic;
    if (receiver_.latest(pic, serial_)) {
        serial_ = pic.serial;
        QImage img(pic.rgba.data(), pic.width, pic.height, pic.width * 4, QImage::Format_RGBA8888);
        picture_->setImage(img.copy());
        if (pic.tagged) {
            tag_ = pic.tag;
            timecode_->setText(qs(pic.tag.timecode));
        }
    }
    std::vector<float> sound = receiver_.takeAudio();
    if (sound_ && sink_ && !sound.empty()) {
        // As much as there is room for, the latest kept (a review is live: sound that would lag is let go).
        const qint64 room = sink_->bytesFree() / 8 * 8, bytes = std::min<qint64>(room, qint64(sound.size() * sizeof(float)));
        if (bytes > 0) sound_->write(reinterpret_cast<const char*>(sound.data() + sound.size()) - bytes, bytes);
    }
    const output::ReviewReceiver::Status st = receiver_.status();
    if (!st.connected && tag_.frame >= 0) {  // (a frame from before the stream dropped is not marked)
        tag_ = {};
        timecode_->setText(QStringLiteral("--:--:--:--"));
    }
    QString text = st.connected ? tr("Watching: %1%2").arg(qs(st.video), st.audio ? tr(", with sound") : QString())
                                : st.error.empty() ? tr("Connecting…") : qs(st.error);
    if (!sent_.isEmpty()) text += QStringLiteral(" · ") + sent_;
    status_->setText(text);
}

void ReviewViewerWindow::addMarker() {
    if (!watching_ || tag_.frame < 0) {
        sent_ = tr("Markers need a picture from Montage's stream first");
        poll();
        return;
    }
    std::string scheme, host;
    int port = 0;
    output::parseStreamAddress(address_->text().trimmed().toStdString(), scheme, host, port);
    int markersPort = port + 1;
    if (const QString to = markersTo_->text().trimmed(); !to.isEmpty()) {
        std::string s2, h2;
        int p2 = 0;
        if (!output::parseStreamAddress("tcp://" + to.toStdString(), s2, h2, p2)) {
            sent_ = tr("Markers to is host:port");
            poll();
            return;
        }
        host = h2;
        markersPort = p2;
    }
    output::RemoteMarker m;
    m.frame = tag_.frame;
    m.sequence = tag_.sequence;
    m.timecode = tag_.timecode;
    m.author = author_->text().trimmed().toStdString();
    m.text = note_->text().trimmed().toStdString();
    QSettings settings = appSettings();
    settings.setValue("reviewViewer/author", author_->text().trimmed());
    settings.setValue("reviewViewer/markersTo", markersTo_->text().trimmed());
    note_->clear();
    const std::string pass = passphrase_->text().toStdString();
    ++sending_;
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        sent_ = watcher->result();
        --sending_;
        watcher->deleteLater();
        poll();
    });
    watcher->setFuture(QtConcurrent::run([host, markersPort, pass, m] {
        std::string why;
        if (!output::sendRemoteMarkers(host, markersPort, pass, {m}, &why)) return QString::fromStdString(why);
        return QObject::tr("Marker sent at %1").arg(QString::fromStdString(m.timecode));
    }));
}

}  // namespace montage
