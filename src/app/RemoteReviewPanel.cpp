#include "RemoteReviewPanel.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QCheckBox>
#include <QComboBox>
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
#include <QSpinBox>
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

}  // namespace

RemoteReviewController::RemoteReviewController(PlaybackController* program, QObject* parent)
    : QObject(parent), program_(program), settings_(loadSettings()), options_(loadOptions()) {
    watch_ = new QTimer(this);
    watch_->setInterval(250);
    connect(watch_, &QTimer::timeout, this, [this] {
        if (!scheduler_) return;
        std::vector<output::RemoteMarker> got = backend_->takeMarkers();
        if (!got.empty()) emit markersReceived(sequence_, got);
        if (scheduler_->running()) return;
        const std::string why = scheduler_->stats().error;
        stop();
        lastError_ = why.empty() ? tr("The stream stopped") : qs(why);
    });
}

RemoteReviewController::~RemoteReviewController() { stop(); }

output::StreamSettings RemoteReviewController::loadSettings() {
    QSettings s = appSettings();
    output::StreamSettings c;
    c.address = s.value("review/address", qs(c.address)).toString().toStdString();
    c.listen = s.value("review/listen", true).toBool();
    c.passphrase = s.value("review/passphrase").toString().toStdString();
    c.codec = s.value("review/codec", "hevc").toString() == "h264" ? "h264" : "hevc";
    c.hardware = s.value("review/hardware", true).toBool();
    c.bitrateKbps = std::clamp(s.value("review/bitrateKbps", c.bitrateKbps).toInt(), 250, 200000);
    c.latencyMs = std::clamp(s.value("review/latencyMs", c.latencyMs).toInt(), 20, 8000);
    c.burnTimecode = s.value("review/burnTimecode", true).toBool();
    c.markersPort = std::clamp(s.value("review/markersPort", 0).toInt(), -1, 65535);
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
    saveSettings(settings, options);
    auto fail = [&](const QString& why) {
        lastError_ = why;
        if (error) *error = why;
        emit runningChanged(false);
        return false;
    };
    const Sequence* s = program_ ? program_->sequence() : nullptr;
    if (!s) return fail(tr("Open a sequence first"));
    auto backend = std::make_shared<output::StreamBackend>();
    std::string why;
    if (!backend->available(&why)) return fail(qs(why));
    backend->setSettings(settings);
    output::Config config;
    config.backend = "Stream";
    config.depth = settings.codec == "h264" ? 8 : 10;
    config.rec709 = options.rec709;
    config.audioChannels = options.audio ? 2 : 0;
    config.scaling = output::Config::Scaling::Fit;
    const std::vector<output::Device> devices = backend->devices();
    auto sched = std::make_shared<output::Scheduler>(backend);
    if (!sched->start(devices.front(), modeFor(*s, options), *s, config, &why)) return fail(qs(why));
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
    emit runningChanged(false);
}

bool RemoteReviewController::running() const { return scheduler_ && scheduler_->running(); }

void RemoteReviewController::sequenceChanged() {
    if (!scheduler_ || formatKey() == startedFor_) return;
    QString why;
    if (!start(settings_, options_, &why)) lastError_ = why;
}

QStringList RemoteReviewController::viewerAddresses() const {
    std::string scheme, host;
    int port = 0;
    if (!output::parseStreamAddress(settings_.address, scheme, host, port)) return {};
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
    const output::StreamBackend::Stats st = backend_->stats();
    const output::Mode& m = scheduler_->mode();
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
"description":"Streams the Program monitor to reviewers elsewhere as it plays or parks, at low latency: HEVC 10-bit (or H.264 8-bit) with stereo sound in an MPEG transport stream over SRT (AES-256 with a passphrase; Montage listening for a viewer, or calling a relay that viewers watch) or UDP, the timecode burnt in. Viewers watch in Montage (File > Watch Remote Review), VLC or ffplay; a Montage viewer's notes come back as markers at the frame they saw. \"status\": whether it is on, the viewer connected, its rate and the markers received. \"start\": settings left out are the last used (a passphrase is made when there is none; it is given back). \"stop\".",
"inputSchema":{"type":"object","properties":{
"action":{"type":"string","enum":["status","start","stop"]},
"address":{"type":"string","description":"srt://0.0.0.0:9000 (listening on every address), srt://relay:port?streamid=… (with listen false) or udp://host:port"},
"listen":{"type":"boolean","description":"SRT: wait for a viewer to call (true) or call the address (false)"},
"passphrase":{"type":"string","description":"SRT's key and the marker channel's, 10 to 79 characters"},
"codec":{"type":"string","enum":["hevc","h264"]},
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
        if (has("address")) c.address = args.value("address").toString().toStdString();
        if (has("listen")) c.listen = args.value("listen").toBool();
        if (has("passphrase")) c.passphrase = args.value("passphrase").toString().toStdString();
        if (has("codec")) c.codec = args.value("codec").toString().toStdString();
        if (has("hardware")) c.hardware = args.value("hardware").toBool();
        if (has("bitrate_kbps")) c.bitrateKbps = args.value("bitrate_kbps").toInt();
        if (has("burn_timecode")) c.burnTimecode = args.value("burn_timecode").toBool();
        if (has("latency_ms")) c.latencyMs = std::clamp(args.value("latency_ms").toInt(), 20, 8000);
        if (has("markers_port")) c.markersPort = args.value("markers_port").toInt();
        if (has("height")) o.maxHeight = std::clamp(args.value("height").toInt(), 0, 4320);
        if (has("rec709")) o.rec709 = args.value("rec709").toBool();
        if (has("audio")) o.audio = args.value("audio").toBool();
        if (c.passphrase.empty() && c.address.rfind("srt", 0) == 0) c.passphrase = output::generatePassphrase();
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
        const output::StreamBackend::Stats st = backend_->stats();
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
                                "Montage (File › Watch Remote Review…, which can send notes back as markers), VLC or ffplay. Over SRT "
                                "the stream is encrypted with the passphrase."),
                             this);
    intro->setWordWrap(true);
    v->addWidget(intro);
    auto* form = new QFormLayout;
    v->addLayout(form);
    const output::StreamSettings st = controller->settings();
    const RemoteReviewOptions op = controller->options();

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
    codec_->addItem(tr("HEVC, 10-bit"), QStringLiteral("hevc"));
    codec_->addItem(tr("H.264, 8-bit (older players)"), QStringLiteral("h264"));
    codec_->setCurrentIndex(st.codec == "h264" ? 1 : 0);
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
    auto* markRow = new QHBoxLayout;
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
    form->addRow(QString(), markRow);
    latency_ = new QSpinBox(this);
    latency_->setRange(20, 8000);
    latency_->setSuffix(tr(" ms"));
    latency_->setValue(st.latencyMs);
    latency_->setToolTip(tr("SRT's buffer against lost packets: about four times the round trip to the viewer"));
    form->addRow(tr("Latency:"), latency_);

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
    for (QWidget* w : std::initializer_list<QWidget*>{address_, direction_, passphrase_, codec_, hardware_, size_, bitrate_, colour_, burn_, audio_, markers_, latency_})
        w->setEnabled(!on);
    markersPort_->setEnabled(!on && markers_->isChecked());
    status_->setText(controller_->status());
    if (!on) {
        howTo_->clear();
        return;
    }
    const output::StreamSettings s = controller_->settings();
    const QStringList at = controller_->viewerAddresses();
    QString text = tr("Viewers open %1").arg(at.join(tr(" or ")));
    if (!s.passphrase.empty()) text += tr(" with the passphrase %1").arg(qs(s.passphrase));
    text += tr(". In Montage: File › Watch Remote Review…");
    if (!at.isEmpty() && s.address.rfind("srt", 0) == 0 && s.listen)
        text += tr("; with FFmpeg: ffplay -fflags nobuffer \"%1?passphrase=%2\"").arg(at.front(), qs(s.passphrase));
    if (const int port = output::markersPortFor(s); port >= 0) text += tr(". Notes come back as markers over TCP port %1.").arg(port);
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
    markersPort_ = new QSpinBox(this);
    markersPort_->setObjectName(QStringLiteral("reviewMarkersPort"));
    markersPort_->setRange(0, 65535);
    markersPort_->setSpecialValueText(tr("port: stream's + 1"));
    markersPort_->setValue(settings.value("reviewViewer/markersPort", 0).toInt());
    markersPort_->setToolTip(tr("The TCP port of the editor's marker channel"));
    bottom->addWidget(timecode_);
    bottom->addWidget(author_);
    bottom->addWidget(note_, 1);
    bottom->addWidget(mark_);
    bottom->addWidget(markersPort_);
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
        const qint64 bytes = qint64(sound.size() * sizeof(float));
        if (sink_->bytesFree() >= bytes) sound_->write(reinterpret_cast<const char*>(sound.data()), bytes);
    }
    const output::ReviewReceiver::Status st = receiver_.status();
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
    const int markersPort = markersPort_->value() > 0 ? markersPort_->value() : port + 1;
    output::RemoteMarker m;
    m.frame = tag_.frame;
    m.timecode = tag_.timecode;
    m.author = author_->text().trimmed().toStdString();
    m.text = note_->text().trimmed().toStdString();
    QSettings settings = appSettings();
    settings.setValue("reviewViewer/author", author_->text().trimmed());
    settings.setValue("reviewViewer/markersPort", markersPort_->value());
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
