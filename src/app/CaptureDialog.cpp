#include "CaptureDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include "EditorState.h"
#include "Settings.h"

namespace montage {

namespace {

QString deviceLabel(const QString& kind) {
    if (kind == QLatin1String("v4l2")) return QObject::tr("Video4Linux camera or capture card");
    if (kind == QLatin1String("alsa")) return QObject::tr("ALSA sound card");
    if (kind == QLatin1String("pulse")) return QObject::tr("PulseAudio sound");
    if (kind == QLatin1String("avfoundation")) return QObject::tr("AVFoundation camera or capture card");
    if (kind == QLatin1String("dshow")) return QObject::tr("DirectShow camera or capture card");
    if (kind == QLatin1String("lavfi")) return QObject::tr("Test pattern (FFmpeg filter)");
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
    form->addRow(tr("Address or device:"), url_);
    sound_ = new QLineEdit(this);
    sound_->setObjectName(QStringLiteral("captureSound"));
    sound_->setPlaceholderText(tr("The feed's own sound"));
    form->addRow(tr("Sound device:"), sound_);
    options_ = new QLineEdit(settings.value(QStringLiteral("capture/options")).toString(), this);
    options_->setObjectName(QStringLiteral("captureOptions"));
    options_->setPlaceholderText(tr("FFmpeg's input options, e.g. video_size=1920x1080 framerate=25"));
    form->addRow(tr("Options:"), options_);
    auto placeholders = [this] {
        const QString k = kind_->currentData().toString();
        url_->setPlaceholderText(k.isEmpty() ? tr("srt://0.0.0.0:9000?mode=listener") : k == QLatin1String("v4l2") ? QStringLiteral("/dev/video0")
                                               : k == QLatin1String("avfoundation")                                 ? QStringLiteral("0:0")
                                               : k == QLatin1String("dshow")                                        ? tr("video=Camera name")
                                                                                                                    : QString());
        sound_->setEnabled(!k.isEmpty());
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
    if (!kind.isEmpty() && !sound_->text().trimmed().isEmpty()) cs.audio = CaptureSource{sound_->text().trimmed().toStdString(), kind.toStdString(), {}};
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
    if (!ids.empty()) imported_ = ids.front();
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

void CaptureDialog::refresh() {
    if (!recorder_) {
        status_->setText(error_.isEmpty() ? tr("Not recording") : error_);
        return;
    }
    const CaptureStatus st = recorder_->status();
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
