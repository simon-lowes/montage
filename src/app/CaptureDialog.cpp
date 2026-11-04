#include "CaptureDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include "EditorState.h"
#include "ReferenceOutputPanel.h"
#include "Settings.h"
#include "media/ImageSequence.h"
#include "render/Nmos.h"
#include "render/St2110Receive.h"
#ifdef MONTAGE_WITH_OMT
#include "media/Omt.h"
#endif

namespace montage {

namespace {

QString deviceLabel(const QString& kind) {
    if (kind == QLatin1String("v4l2")) return QObject::tr("Video4Linux camera or capture card");
    if (kind == QLatin1String("alsa")) return QObject::tr("ALSA sound card");
    if (kind == QLatin1String("pulse")) return QObject::tr("PulseAudio sound");
    if (kind == QLatin1String("avfoundation")) return QObject::tr("AVFoundation camera or capture card");
    if (kind == QLatin1String("dshow")) return QObject::tr("DirectShow camera or capture card");
    if (kind == QLatin1String("lavfi")) return QObject::tr("Test pattern (FFmpeg filter)");
    if (kind == QLatin1String("omt")) return QObject::tr("Open Media Transport source (on the network)");
    if (kind == QLatin1String("st2110")) return QObject::tr("SMPTE ST 2110 streams (from their SDP files)");
    return kind;
}

QString clock(double seconds) {
    const int s = int(seconds);
    return QStringLiteral("%1:%2:%3").arg(s / 3600, 2, 10, QLatin1Char('0')).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

CaptureDialog::CaptureDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Capture"));
    setObjectName(QStringLiteral("captureDialog"));
    QSettings settings = appSettings();
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Records a live feed (a network stream or a camera, capture card or sound card on this computer) "
                                "with its timecode. A QuickTime recording is in the Captures bin a second after it starts and "
                                "grows there as it records, so it can be cut in straight away."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* form = new QFormLayout;

    kind_ = new QComboBox(this);
    kind_->setObjectName(QStringLiteral("captureKind"));
    kind_->addItem(tr("Network stream (SRT, RIST, UDP, RTMP...)"), QString());
    for (const std::string& k : captureDeviceKinds()) kind_->addItem(deviceLabel(QString::fromStdString(k)), QString::fromStdString(k));
    form->addRow(tr("Feed:"), kind_);
    url_ = new QLineEdit(settings.value(QStringLiteral("capture/url")).toString(), this);
    url_->setObjectName(QStringLiteral("captureUrl"));
    auto* urlRow = new QHBoxLayout;
    urlRow->addWidget(url_, 1);
    // Open Media Transport sources announced on the network, to choose from.
    find_ = new QPushButton(tr("Find…"), this);
    find_->setObjectName(QStringLiteral("captureFindSources"));
    urlRow->addWidget(find_);
    connect(find_, &QPushButton::clicked, this, [this] {
        if (kind_->currentData().toString() == QLatin1String("st2110")) {
            // (the video's SDP, and the sound's and ancillary data's when they come in files of their own)
            const QStringList files = QFileDialog::getOpenFileNames(this, tr("SDP Files"), QString(), tr("SDP files (*.sdp);;All files (*)"));
            if (!files.isEmpty()) url_->setText(files.join(QLatin1Char(';')));
            return;
        }
#ifdef MONTAGE_WITH_OMT
        setCursor(Qt::WaitCursor);
        std::string why;
        const std::vector<omt::Source> sources = omt::discover(1.5, &why);
        unsetCursor();
        QMenu menu(this);
        for (const omt::Source& s : sources) {
            const QString url = QStringLiteral("omt://%1:%2").arg(QString::fromStdString(s.host.find(':') != std::string::npos ? "[" + s.host + "]" : s.host)).arg(s.port);
            menu.addAction(QString::fromStdString(s.name), this, [this, url] { url_->setText(url); });
        }
        if (sources.empty()) menu.addAction(why.empty() ? tr("No sources found") : tr("No sources found (%1)").arg(QString::fromStdString(why)))->setEnabled(false);
        menu.exec(find_->mapToGlobal(QPoint(0, find_->height())));
#endif
    });
    form->addRow(tr("Address or device:"), urlRow);
    sound_ = new QLineEdit(this);
    sound_->setObjectName(QStringLiteral("captureSound"));
    sound_->setPlaceholderText(tr("The feed's own sound"));
    form->addRow(tr("Sound device:"), sound_);
    options_ = new QLineEdit(settings.value(QStringLiteral("capture/options")).toString(), this);
    options_->setObjectName(QStringLiteral("captureOptions"));
    options_->setPlaceholderText(tr("FFmpeg's input options, e.g. video_size=1920x1080 framerate=25"));
    form->addRow(tr("Options:"), options_);
    // ST 2110 receivers for a facility's broadcast controller to connect (its registry as the reference output's).
    nmos_ = new QCheckBox(tr("Offer to NMOS controllers (their connection becomes the feed)"), this);
    nmos_->setObjectName(QStringLiteral("captureNmos"));
    nmosStatus_ = new QLabel(this);
    nmosStatus_->setObjectName(QStringLiteral("captureNmosStatus"));
    nmosStatus_->setWordWrap(true);
    auto* nmosBox = new QVBoxLayout;
    nmosBox->setContentsMargins(0, 0, 0, 0);
    nmosBox->addWidget(nmos_);
    nmosBox->addWidget(nmosStatus_);
    form->addRow(QString(), nmosBox);
    connect(nmos_, &QCheckBox::toggled, this, [this](bool on) {
        if (bool(node_ && node_->running()) == on) return;
        QString why;
        if (!offerNmos(on, &why)) nmosStatus_->setText(why);
    });
    auto placeholders = [this] {
        const QString k = kind_->currentData().toString();
        url_->setPlaceholderText(k.isEmpty() ? tr("srt://0.0.0.0:9000?mode=listener") : k == QLatin1String("v4l2") ? QStringLiteral("/dev/video0")
                                               : k == QLatin1String("avfoundation")                                 ? QStringLiteral("0:0")
                                               : k == QLatin1String("dshow")                                        ? tr("video=Camera name")
                                               : k == QLatin1String("omt")                                          ? tr("omt://host:6400, or HOST (Name)")
                                               : k == QLatin1String("st2110")                                       ? tr("SDP files, apart by ;")
                                                                                                                    : QString());
        const bool omt = k == QLatin1String("omt"), ip = k == QLatin1String("st2110");
        sound_->setEnabled(!k.isEmpty() && !omt && !ip);
        options_->setEnabled(!omt);
        options_->setPlaceholderText(ip ? tr("interface=address interface2=address (ST 2022-7's second path)")
                                        : tr("FFmpeg's input options, e.g. video_size=1920x1080 framerate=25"));
        find_->setText(ip ? tr("Choose…") : tr("Find…"));
        find_->setVisible(omt || ip);
        nmos_->setVisible(ip);
        nmosStatus_->setVisible(ip && !nmosStatus_->text().isEmpty());
    };
    connect(kind_, &QComboBox::currentIndexChanged, this, placeholders);
    if (const int i = kind_->findData(settings.value(QStringLiteral("capture/kind")).toString()); i >= 0) kind_->setCurrentIndex(i);
    placeholders();

    codec_ = new QComboBox(this);
    codec_->setObjectName(QStringLiteral("captureCodec"));
    for (const auto& [id, name] : captureCodecs()) codec_->addItem(QString::fromStdString(name), QString::fromStdString(id));
    if (const int i = codec_->findData(settings.value(QStringLiteral("capture/codec"), QStringLiteral("prores_hq")).toString()); i >= 0)
        codec_->setCurrentIndex(i);
    form->addRow(tr("Record as:"), codec_);
    wrapper_ = new QComboBox(this);
    wrapper_->setObjectName(QStringLiteral("captureWrapper"));
    wrapper_->addItem(tr("QuickTime (edit while it records)"), QStringLiteral("mov"));
    wrapper_->addItem(tr("MXF (imported once it stops)"), QStringLiteral("mxf"));
    if (const int i = wrapper_->findData(settings.value(QStringLiteral("capture/wrapper"), QStringLiteral("mov")).toString()); i >= 0)
        wrapper_->setCurrentIndex(i);
    form->addRow(tr("File:"), wrapper_);

    auto* folderRow = new QHBoxLayout;
    folder_ = new QLineEdit(settings.value(QStringLiteral("capture/folder"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString(), this);
    folder_->setObjectName(QStringLiteral("captureFolder"));
    auto* browse = new QPushButton(tr("Choose…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, tr("Record Into"), folder_->text());
        if (!d.isEmpty()) setFolder(d);
    });
    folderRow->addWidget(folder_, 1);
    folderRow->addWidget(browse);
    form->addRow(tr("Folder:"), folderRow);
    name_ = new QLineEdit(tr("Capture 001"), this);
    name_->setObjectName(QStringLiteral("captureName"));
    form->addRow(tr("Clip name:"), name_);
    timecode_ = new QLineEdit(this);
    timecode_->setObjectName(QStringLiteral("captureTimecode"));
    timecode_->setPlaceholderText(tr("The feed's own, else the time of day (hh:mm:ss:ff, ; for drop frame)"));
    timecode_->setInputMask(QString());
    form->addRow(tr("Start timecode:"), timecode_);
    seconds_ = new QDoubleSpinBox(this);
    seconds_->setObjectName(QStringLiteral("captureSeconds"));
    seconds_->setRange(0, 24 * 3600);
    seconds_->setDecimals(1);
    seconds_->setSuffix(tr(" s"));
    seconds_->setSpecialValueText(tr("Until stopped"));
    form->addRow(tr("Length:"), seconds_);
    import_ = new QCheckBox(tr("Import into the Captures bin"), this);
    import_->setObjectName(QStringLiteral("captureImport"));
    import_->setChecked(settings.value(QStringLiteral("capture/import"), true).toBool());
    form->addRow(QString(), import_);
    layout->addLayout(form);

    status_ = new QLabel(tr("Not recording"), this);
    status_->setObjectName(QStringLiteral("captureStatus"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    record_ = buttons->addButton(tr("Record"), QDialogButtonBox::ActionRole);
    record_->setObjectName(QStringLiteral("captureRecord"));
    connect(record_, &QPushButton::clicked, this, [this] { isRecording() ? stop() : (void)record(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);  // (recording goes on with it hidden)
    layout->addWidget(buttons);

    tick_ = new QTimer(this);
    tick_->setInterval(250);
    connect(tick_, &QTimer::timeout, this, &CaptureDialog::refresh);
}

CaptureDialog::~CaptureDialog() {
    // (the recorder's notices, queued to this, go with it)
    if (recorder_) recorder_->stop();
    if (node_) node_->stop();
}

bool CaptureDialog::offerNmos(bool on, QString* error) {
    auto show = [this](const QString& text) {
        nmosStatus_->setText(text);
        nmosStatus_->setVisible(nmos_->isVisible() && !text.isEmpty());
    };
    if (!on) {
        if (node_) node_->stop();
        const QSignalBlocker block(nmos_);
        nmos_->setChecked(false);
        show(QString());
        return true;
    }
    if (!node_) {
        node_ = new nmos::Node(this);
        node_->onReceiverActivated = [this](const nmos::ReceiverConnection&) { nmosConnected(); };
        node_->onRegistered = [this](bool) { nmosStatus_->setText(nmosStatus()); };
    }
    nmos::NodeOptions o = ReferenceOutputController::loadNmos();
    o.label = tr("Montage capture");
    o.port = 0;  // (the reference output's node may hold the one set)
    o.senders = false;
    o.receivers = true;
    // (the paths' interfaces, as the options give them)
    for (const QString& part : options_->text().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (part.startsWith(QLatin1String("interface="))) o.address = part.mid(10);
        if (part.startsWith(QLatin1String("interface2="))) o.interface2 = part.mid(11);
    }
    if (node_->running()) node_->stop();
    QString why;
    if (!node_->start(o, &why)) {
        const QSignalBlocker block(nmos_);
        nmos_->setChecked(false);
        why = tr("NMOS: %1").arg(why);
        show(why);
        if (error) *error = why;
        return false;
    }
    const QSignalBlocker block(nmos_);
    nmos_->setChecked(true);
    show(nmosStatus());
    return true;
}

QString CaptureDialog::nmosStatus() const {
    if (!node_ || !node_->running()) return QString();
    const QString where = node_->registered() ? tr("registered with %1").arg(node_->registry()) : tr("at %1, no registry yet").arg(node_->apiUrl());
    const nmos::ReceiverConnection c = node_->receiverConnection(0);
    if (!c.active) return tr("Offered to NMOS controllers (%1): not connected").arg(where);
    return tr("Connected by an NMOS controller (%1)%2").arg(where, isRecording() ? tr(", for the next recording") : QString());
}

void CaptureDialog::nmosConnected() {
    // What the receivers now take, as the feed: the streams' SDPs (their paths as the controller placed them) and the
    // legs' interfaces.
    QString why;
    const std::vector<st2110::SdpStream> streams = node_->receiverStreams(&why);
    std::string text;
    for (const st2110::SdpStream& s : streams) text += st2110::sdpText(s);
    if (!streams.empty()) {
        setSource(QStringLiteral("st2110"), QString::fromStdString(text));
        const nmos::ReceiverConnection c = node_->receiverConnection(0);
        QStringList options;
        if (c.leg[0].enabled) options << QStringLiteral("interface=") + c.leg[0].interfaceIp;
        if (c.leg[1].enabled && c.leg[0].enabled) options << QStringLiteral("interface2=") + c.leg[1].interfaceIp;
        setOptions(options.join(QLatin1Char(' ')));
    }
    nmosStatus_->setText(why.isEmpty() ? nmosStatus() : tr("NMOS: %1").arg(why));
    nmosStatus_->setVisible(nmos_->isVisible());
}

void CaptureDialog::setSource(const QString& kind, const QString& url) {
    if (const int i = kind_->findData(kind); i >= 0) kind_->setCurrentIndex(i);
    url_->setText(url);
}
void CaptureDialog::setSoundDevice(const QString& name) { sound_->setText(name); }
void CaptureDialog::setOptions(const QString& options) { options_->setText(options); }
void CaptureDialog::setCodec(const QString& id) {
    if (const int i = codec_->findData(id); i >= 0) codec_->setCurrentIndex(i);
}
void CaptureDialog::setWrapper(const QString& ext) {
    if (const int i = wrapper_->findData(ext); i >= 0) wrapper_->setCurrentIndex(i);
}
void CaptureDialog::setFolder(const QString& dir) { folder_->setText(dir); }
void CaptureDialog::setClipName(const QString& name) { name_->setText(name); }
void CaptureDialog::setTimecode(const QString& tc) { timecode_->setText(tc); }
void CaptureDialog::setSeconds(double s) { seconds_->setValue(s); }

bool CaptureDialog::isRecording() const { return recording_; }

bool CaptureDialog::record() {
    if (isRecording()) return false;
    error_.clear();
    imported_ = 0;
    const QString tc = timecode_->text().trimmed();
    if (!tc.isEmpty() && !QRegularExpression(QStringLiteral("^\\d{2}:\\d{2}:\\d{2}[:;]\\d{2}$")).match(tc).hasMatch()) {
        error_ = tr("Timecode is hh:mm:ss:ff");
        refresh();
        return false;
    }
    const QString dir = folder_->text().trimmed();
    if (dir.isEmpty() || !QDir().mkpath(dir)) {
        error_ = tr("Choose a folder to record into");
        refresh();
        return false;
    }
    // (a name taken: the next number)
    QString name = name_->text().trimmed();
    if (name.isEmpty()) name = tr("Capture 001");
    name.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("_"));
    const QString ext = wrapper_->currentData().toString();
    while (QFileInfo::exists(QDir(dir).filePath(name + '.' + ext))) {
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^(.*?)(\\d+)$")).match(name);
        name = m.hasMatch() ? m.captured(1) + QStringLiteral("%1").arg(m.captured(2).toInt() + 1, m.captured(2).size(), 10, QLatin1Char('0'))
                            : name + QStringLiteral(" 2");
    }
    path_ = QDir(dir).filePath(name + '.' + ext);

    CaptureSettings cs;
    const QString kind = kind_->currentData().toString();
    cs.video.device = kind.toStdString();
    cs.video.url = url_->text().trimmed().toStdString();
    for (const QString& o : options_->text().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        const int eq = int(o.indexOf(QLatin1Char('=')));
        if (eq <= 0) {
            error_ = tr("Options are name=value, apart by spaces");
            refresh();
            return false;
        }
        cs.video.options[o.left(eq).toStdString()] = o.mid(eq + 1).toStdString();
    }
    if (!kind.isEmpty() && kind != QLatin1String("omt") && kind != QLatin1String("st2110") && !sound_->text().trimmed().isEmpty()) cs.audio = CaptureSource{sound_->text().trimmed().toStdString(), kind.toStdString(), {}};
    cs.codec = codec_->currentData().toString().toStdString();
    cs.path = path_.toStdString();
    cs.timecode = tc.toStdString();
    cs.seconds = seconds_->value();
    if (recorder_) recorder_->stop();
    recorder_ = std::make_unique<Recorder>();
    const int generation = ++generation_;
    QPointer<CaptureDialog> self(this);
    recorder_->onFirstFragment = [self, generation] {
        QMetaObject::invokeMethod(self.data(), [self, generation] {
            if (self && self->generation_ == generation) self->importNow();
        }, Qt::QueuedConnection);
    };
    recorder_->onFinished = [self, generation](bool ok, const std::string& message) {
        const QString m = QString::fromStdString(message);
        QMetaObject::invokeMethod(self.data(), [self, generation, ok, m] {
            if (self) self->finished(generation, ok, m);
        }, Qt::QueuedConnection);
    };
    std::string err;
    if (!recorder_->start(cs, &err)) {
        error_ = QString::fromStdString(err);
        recorder_.reset();
        refresh();
        return false;
    }
    QSettings settings = appSettings();
    settings.setValue(QStringLiteral("capture/kind"), kind);
    settings.setValue(QStringLiteral("capture/url"), url_->text().trimmed());
    settings.setValue(QStringLiteral("capture/codec"), codec_->currentData());
    settings.setValue(QStringLiteral("capture/wrapper"), ext);
    settings.setValue(QStringLiteral("capture/folder"), dir);
    settings.setValue(QStringLiteral("capture/import"), import_->isChecked());
    settings.setValue(QStringLiteral("capture/options"), options_->text().trimmed());
    recording_ = true;
    notedFrames_ = -1;
    importedPath_.clear();
    record_->setText(tr("Stop"));
    tick_->start();
    refresh();
    emit recordingChanged();
    return true;
}

void CaptureDialog::stop() {
    if (recorder_) recorder_->stop();  // (finished() follows, queued)
}

void CaptureDialog::importNow() {
    if (imported_ || !import_->isChecked() || path_.isEmpty() || !QFileInfo::exists(path_)) return;
    QStringList errors;
    const std::vector<Id> ids = state_->importFiles({path_}, &errors, tr("Captures"));
    if (!ids.empty()) {
        imported_ = ids.front();
        // (as the project names it; kept, as another project opened meanwhile may have a media item of the same id)
        if (const MediaItem* m = state_->project().findMedia(imported_)) importedPath_ = QString::fromStdString(mediaFileOnDisk(m->path));
    }
    else if (!errors.isEmpty()) error_ = errors.front();
    refresh();
}

void CaptureDialog::finished(int generation, bool ok, const QString& message) {
    if (generation != generation_) return;  // (an earlier recording's)
    recording_ = false;
    tick_->stop();
    record_->setText(tr("Record"));
    if (!message.isEmpty()) error_ = message;  // (why it failed, or what went wrong in a recording kept)
    // (an MXF file, or a recording shorter than its first fragment, is imported now it is whole)
    if (ok) importNow();
    // The next clip's name: the next number.
    const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^(.*?)(\\d+)$")).match(QFileInfo(path_).completeBaseName());
    if (ok && m.hasMatch()) name_->setText(m.captured(1) + QStringLiteral("%1").arg(m.captured(2).toInt() + 1, m.captured(2).size(), 10, QLatin1Char('0')));
    refresh();
    emit recordingChanged();
}

QString CaptureDialog::statusText() const { return status_->text(); }

// ---- The agent link's tool

QByteArray CaptureDialog::toolJson() {
    return QByteArrayLiteral(R"json({"name":"montage_capture","title":"Capture (record a live feed)",
"description":"Records a live feed to ProRes or DNxHR with its timecode, as File > Capture does; a QuickTime recording is imported into the Captures bin a second after it starts and grows there. \"kinds\": the devices this build opens (\"st2110\" for SMPTE ST 2110 streams, \"omt\" for Open Media Transport, FFmpeg's capture devices) and the codecs. \"record\": kind (\"\" a network stream's URL), url (for st2110 the SDP files apart by ';', or the SDP text itself; ST 2022-7's two paths come from the SDP's DUP group), options (\"name=value\" apart by spaces: FFmpeg's input options, or for st2110 interface= and interface2=, the network interfaces of the two paths), sound (a sound device of the same kind), codec, wrapper (mov or mxf), folder, name, timecode (\"\" the feed's own: ST 2110's from its ancillary data), seconds (0 until stopped). \"stop\" and \"status\". \"nmos\" (offer true or false): ST 2110 receivers offered to a facility's NMOS controller (IS-04 and IS-05, registered as the reference output's node is); what it connects them to becomes the st2110 feed, recorded by \"record\" with kind st2110 and no url.",
"inputSchema":{"type":"object","properties":{
"action":{"type":"string","enum":["kinds","record","stop","status","nmos"]},"offer":{"type":"boolean"},
"kind":{"type":"string"},"url":{"type":"string"},"options":{"type":"string"},"sound":{"type":"string"},
"codec":{"type":"string"},"wrapper":{"type":"string","enum":["mov","mxf"]},"folder":{"type":"string"},"name":{"type":"string"},
"timecode":{"type":"string"},"seconds":{"type":"number","minimum":0}},"required":["action"]}})json");
}

QString CaptureDialog::runTool(const QJsonObject& args, QJsonObject& structured, bool& isError) {
    isError = false;
    const QString action = args.value("action").toString();
    if (action == QLatin1String("kinds")) {
        QJsonArray kinds, codecs;
        for (const std::string& k : captureDeviceKinds()) kinds.append(QString::fromStdString(k));
        for (const auto& [id, name] : captureCodecs()) codecs.append(QJsonObject{{"id", QString::fromStdString(id)}, {"name", QString::fromStdString(name)}});
        structured["kinds"] = kinds;
        structured["codecs"] = codecs;
        return tr("%n kind(s) of device", nullptr, int(kinds.size()));
    }
    if (action == QLatin1String("record")) {
        if (isRecording()) {
            isError = true;
            return tr("Already recording %1").arg(QDir::toNativeSeparators(path_));
        }
        const QString kind = args.value("kind").toString();
        if (kind_->findData(kind) < 0) {
            isError = true;
            return tr("This build does not open \"%1\" (see \"kinds\")").arg(kind);
        }
        // (no url: the feed as it is, an NMOS controller's connection say)
        const bool same = !args.contains("url") && kind == kind_->currentData().toString();
        setSource(kind, same ? url_->text() : args.value("url").toString());
        setSoundDevice(args.value("sound").toString());
        if (!same || args.contains("options")) setOptions(args.value("options").toString());
        if (args.contains("codec")) setCodec(args.value("codec").toString());
        if (args.contains("wrapper")) setWrapper(args.value("wrapper").toString());
        if (args.contains("folder")) setFolder(args.value("folder").toString());
        if (args.contains("name")) setClipName(args.value("name").toString());
        setTimecode(args.value("timecode").toString());
        setSeconds(args.value("seconds").toDouble(0));
        if (!record()) {
            isError = true;
            return statusText();
        }
        structured["path"] = path_;
        return tr("Recording to %1").arg(QDir::toNativeSeparators(path_));
    }
    if (action == QLatin1String("stop")) {
        stop();
        return isRecording() ? tr("Stopping") : tr("Not recording");
    }
    if (action == QLatin1String("status")) {
        refresh();
        structured["recording"] = isRecording();
        structured["path"] = path_;
        structured["status"] = statusText();
        if (imported_) structured["media"] = double(imported_);
        return statusText();
    }
    if (action == QLatin1String("nmos")) {
        QString why;
        if (!offerNmos(args.value("offer").toBool(true), &why)) {
            isError = true;
            return why;
        }
        if (node_ && node_->running()) {
            structured["node"] = node_->apiUrl();
            QJsonArray ids;
            for (int i = 0; i < 3; ++i) ids.append(node_->receiverId(i).toString(QUuid::WithoutBraces));
            structured["receivers"] = ids;
            structured["connected"] = node_->receiverConnection(0).active;
        }
        return node_ && node_->running() ? nmosStatus() : tr("Not offered to NMOS controllers");
    }
    isError = true;
    return tr("Unknown action %1").arg(action);
}

void CaptureDialog::refresh() {
    if (!recorder_) {
        status_->setText(error_.isEmpty() ? tr("Not recording") : error_);
        return;
    }
    const CaptureStatus st = recorder_->status();
    // The file imported is being written by this program: said so as it records, not left to the system's notice of
    // writes (late on macOS and Windows), so it is followed as growing at once.
    if (st.recording && st.frames != notedFrames_ && !importedPath_.isEmpty()) {
        state_->noteMediaFileWritten(importedPath_);
        notedFrames_ = st.frames;
    }
    QString text;
    if (st.recording && st.frames == 0) {
        text = tr("Waiting for the feed…");
    } else if (st.width > 0) {
        text = (st.recording ? tr("Recording %1") : tr("Recorded %1")).arg(clock(st.seconds));
        text += QStringLiteral(" · ") + tr("%1×%2 at %3 fps").arg(st.width).arg(st.height).arg(st.fps, 0, 'g', 4);
        text += QStringLiteral(" · ") + (st.channels ? tr("%n sound channel(s)", nullptr, st.channels) : tr("no sound"));
        text += QStringLiteral(" · TC ") + QString::fromStdString(st.timecode);
        if (st.dropped || st.repeated) text += QStringLiteral(" · ") + tr("%1 dropped, %2 repeated").arg(st.dropped).arg(st.repeated);
        if (!st.recording) text += QStringLiteral("\n") + QDir::toNativeSeparators(path_);
    }
    const QString note = !error_.isEmpty() ? error_ : QString::fromStdString(st.warning);
    if (!note.isEmpty()) text += (text.isEmpty() ? QString() : QStringLiteral("\n")) + note;
    status_->setText(text.isEmpty() ? tr("Not recording") : text);
}

}  // namespace montage
