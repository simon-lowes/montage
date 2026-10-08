// Montage — export dialog.
#include "ExportDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointF>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <exception>

#include "EditorState.h"
#include "SequenceSettingsDialog.h"
#include "Theme.h"
#include "core/History.h"
#include "render/ColorSpace.h"

namespace montage {

namespace {

const QString kSettingsDir = QStringLiteral("export/lastDirectory");
const QString kSettingsPreset = QStringLiteral("export/preset");

QSettings appSettings() { return QSettings(QStringLiteral("Montage"), QStringLiteral("Montage")); }

bool hasVideo(const ExportSettings& s) { return !s.videoCodec.empty() && s.videoCodec != "none"; }
bool hasAudio(const ExportSettings& s) { return !s.audioCodec.empty() && s.audioCodec != "none"; }

bool usesCrf(const std::string& codec) {
    return codec == "libx264" || codec == "libx265" || codec == "libvpx-vp9" || codec == "libsvtav1";
}
// VP9 and SVT-AV1 use a 0-63 quantiser scale, x264 / x265 0-51.
int maxCrf(const std::string& codec) { return (codec == "libvpx-vp9" || codec == "libsvtav1") ? 63 : 51; }

QString videoCodecName(const ExportSettings& s) {
    const std::string& c = s.videoCodec;
    const bool tenBit = s.pixFmt.find("10") != std::string::npos;
    if (c == "libx264") return QStringLiteral("H.264");
    if (c == "libx265") return tenBit ? QStringLiteral("H.265 10-bit") : QStringLiteral("H.265");
    if (c == "prores_ks") {
        if (s.profile == "hq") return QStringLiteral("ProRes 422 HQ");
        if (s.profile == "lt") return QStringLiteral("ProRes 422 LT");
        if (s.profile == "proxy") return QStringLiteral("ProRes 422 Proxy");
        if (s.profile.rfind("4444", 0) == 0) return QStringLiteral("ProRes 4444");
        return QStringLiteral("ProRes 422");
    }
    if (c == "dnxhd") {
        if (s.profile.rfind("dnxhr_", 0) == 0) return QStringLiteral("DNxHR ") + QString::fromStdString(s.profile.substr(6)).toUpper();
        return QStringLiteral("DNxHD");
    }
    if (c == "libvpx-vp9") return QStringLiteral("VP9");
    if (c == "libsvtav1") return QStringLiteral("AV1");
    if (c == "mjpeg") return QStringLiteral("Motion JPEG");
    return QString::fromStdString(c);
}

QString audioCodecName(const ExportSettings& s) {
    const std::string& c = s.audioCodec;
    const QString kbps = QString::number(s.audioBitrate / 1000);
    if (c == "aac") return QStringLiteral("AAC %1 kb/s").arg(kbps);
    if (c == "libopus") return QStringLiteral("Opus %1 kb/s").arg(kbps);
    if (c == "pcm_s16le") return QStringLiteral("PCM 16-bit");
    if (c == "pcm_s24le") return QStringLiteral("PCM 24-bit");
    return QString::fromStdString(c);
}

QString fpsLabel(Rational fps) {
    QString s = QString::number(fps.toDouble(), 'f', 3);
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith('.')) s.chop(1);
    return s;
}

// "m:ss" or "h:mm:ss".
QString clockString(qint64 ms) {
    const qint64 total = std::max<qint64>(0, (ms + 500) / 1000);
    const qint64 h = total / 3600, m = (total / 60) % 60, s = total % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

QString withExtension(const QString& path, const QString& ext) {
    const int slash = std::max(path.lastIndexOf('/'), path.lastIndexOf('\\'));
    const int dot = path.lastIndexOf('.');
    const QString base = dot > slash + 1 ? path.left(dot) : path;
    return ext.isEmpty() ? base : base + '.' + ext;
}

QString safeFileName(QString name) {
    static const QRegularExpression invalid(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])"));
    name.replace(invalid, QStringLiteral("_"));
    return name.trimmed();
}

int roundEven(double v) { return std::clamp(int(std::lround(v / 2.0)) * 2, 16, 8192); }

}  // namespace

ExportDialog::ExportDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Export"));
    const Sequence* seq = state_ ? state_->sequence() : nullptr;
    QPalette dim = palette();
    dim.setColor(QPalette::WindowText, theme::kTextDim);

    form_ = new QWidget(this);

    preset_ = new QComboBox(form_);
    for (const ExportPreset& p : exportPresets()) preset_->addItem(QString::fromStdString(p.name));
    presetDescription_ = new QLabel(form_);
    presetDescription_->setPalette(dim);

    path_ = new QLineEdit(form_);
    path_->setMinimumWidth(340);
    path_->setClearButtonEnabled(true);
    browse_ = new QPushButton(tr("Browse..."), form_);
    auto* pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->addWidget(path_, 1);
    pathRow->addWidget(browse_);

    range_ = new QComboBox(form_);
    range_->addItem(tr("Entire Sequence"));
    range_->addItem(tr("In to Out"));
    const bool hasMarks = seq && (seq->inPoint >= 0 || seq->outPoint >= 0);
    range_->setEnabled(hasMarks);
    range_->setCurrentIndex(hasMarks ? 1 : 0);
    if (!hasMarks) range_->setToolTip(tr("Set In and Out points in the timeline to export part of the sequence"));

    const int seqW = seq ? seq->width : 1920;
    const int seqH = seq ? seq->height : 1080;
    matchSize_ = new QCheckBox(tr("Match sequence resolution (%1 × %2)").arg(seqW).arg(seqH), form_);
    matchSize_->setChecked(true);
    width_ = new EvenSpinBox(form_);
    height_ = new EvenSpinBox(form_);
    width_->setValue(roundEven(seqW));
    height_->setValue(roundEven(seqH));
    auto* sizeRow = new QHBoxLayout;
    sizeRow->setContentsMargins(0, 0, 0, 0);
    sizeRow->addWidget(width_);
    sizeRow->addWidget(new QLabel(QStringLiteral("×"), form_));
    sizeRow->addWidget(height_);
    sizeRow->addStretch(1);

    quality_ = new QSpinBox(form_);
    quality_->setRange(0, 51);
    quality_->setToolTip(tr("Constant rate factor: lower values give higher quality and larger files"));

    summary_ = new QLabel(form_);
    summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary_->setFrameShape(QFrame::StyledPanel);
    summary_->setMargin(6);

    auto* form = new QFormLayout(form_);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->addRow(tr("Preset:"), preset_);
    form->addRow(QString(), presetDescription_);
    form->addRow(tr("Output file:"), pathRow);
    form->addRow(tr("Range:"), range_);
    form->addRow(tr("Size:"), matchSize_);
    form->addRow(QString(), sizeRow);
    form->addRow(tr("Quality (CRF):"), quality_);
    captions_ = new QComboBox(form_);
    captions_->addItem(tr("None"), 0);
    captions_->addItem(tr("Burn into the picture"), 1);
    captions_->addItem(tr("Embed as a subtitle track"), 2);
    captions_->addItem(tr("Burn in and embed"), 3);
    captions_->setCurrentIndex(std::clamp(appSettings().value("export/captions", 0).toInt(), 0, 3));
    form->addRow(tr("Captions:"), captions_);
    color_ = new QComboBox(form_);
    color_->setObjectName(QStringLiteral("exportColor"));
    color_->addItem(tr("Same as sequence (%1)").arg(QString::fromStdString(seq ? sequenceColorSpace(*seq).label : "Rec.709")),
                    QString());
    for (const ColorSpace* cs : displayColorSpaces())
        color_->addItem(QString::fromStdString(cs->label), QString::fromStdString(cs->id));
    color_->setToolTip(tr("Deliver in another colour space: an HDR sequence delivered in Rec.709 is tone mapped.\n"
                          "HDR output is 10-bit and tagged; PQ carries HDR10 metadata."));
    form->addRow(tr("Colour:"), color_);
    // Loudness for where it is going: (target LUFS, true peak ceiling dBTP).
    loudness_ = new QComboBox(form_);
    loudness_->setObjectName(QStringLiteral("exportLoudness"));
    loudness_->addItem(tr("As mixed"), QPointF(0, 0));
    loudness_->addItem(tr("-14 LUFS, -1 dBTP (YouTube, Spotify, TikTok)"), QPointF(-14, -1));
    loudness_->addItem(tr("-16 LUFS, -1 dBTP (Apple Podcasts, Amazon Music)"), QPointF(-16, -1));
    loudness_->addItem(tr("-23 LUFS, -1 dBTP (EBU R128 broadcast)"), QPointF(-23, -1));
    loudness_->addItem(tr("-24 LKFS, -2 dBTP (ATSC A/85, US broadcast)"), QPointF(-24, -2));
    loudness_->setCurrentIndex(std::clamp(appSettings().value("export/loudness", 0).toInt(), 0, loudness_->count() - 1));
    loudness_->setToolTip(tr("Measures the whole mix first, then sets its level to the target, with a limiter "
                             "keeping peaks under the ceiling"));
    form->addRow(tr("Loudness:"), loudness_);
    form->addRow(tr("Summary:"), summary_);

    progress_ = new QProgressBar(this);
    progress_->setRange(0, 1000);
    progress_->setTextVisible(true);
    progress_->setFormat(QStringLiteral("%p%"));
    progress_->hide();
    eta_ = new QLabel(this);
    eta_->setPalette(dim);
    eta_->hide();

    auto* buttons = new QDialogButtonBox(this);
    exportButton_ = buttons->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
    closeButton_ = buttons->addButton(QDialogButtonBox::Close);
    exportButton_->setDefault(true);
    connect(exportButton_, &QPushButton::clicked, this, [this] { exporting_ ? requestCancel() : startExport(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &ExportDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(form_);
    layout->addSpacing(4);
    layout->addWidget(progress_);
    layout->addWidget(eta_);
    layout->addStretch(1);
    layout->addWidget(buttons);

    // Restore the last preset; the path is derived from it in presetChanged().
    const QString lastPreset = appSettings().value(kSettingsPreset).toString();
    if (const int i = preset_->findText(lastPreset); i >= 0) preset_->setCurrentIndex(i);

    connect(preset_, &QComboBox::currentIndexChanged, this, [this] { presetChanged(); });
    connect(browse_, &QPushButton::clicked, this, [this] { browse(); });
    connect(path_, &QLineEdit::textChanged, this, [this] { updateControls(); });
    connect(range_, &QComboBox::currentIndexChanged, this, [this] {
        updateControls();
        updateSummary();
    });
    connect(matchSize_, &QCheckBox::toggled, this, [this, seqW, seqH](bool on) {
        if (on) {
            updatingSize_ = true;
            width_->setValue(roundEven(seqW));
            height_->setValue(roundEven(seqH));
            updatingSize_ = false;
        }
        updateControls();
        updateSummary();
    });
    connect(width_, &QSpinBox::valueChanged, this, [this](int w) { widthChanged(w); });
    connect(height_, &QSpinBox::valueChanged, this, [this](int h) { heightChanged(h); });
    connect(quality_, &QSpinBox::valueChanged, this, [this] { updateSummary(); });
    connect(color_, &QComboBox::currentIndexChanged, this, [this] { updateSummary(); });
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] { exportFinished(); });

    presetChanged();
}

ExportDialog::~ExportDialog() {
    // The worker references this dialog (progress, cancel flag): stop it first.
    stopAndWait();
}

const ExportPreset* ExportDialog::currentPreset() const {
    const auto& presets = exportPresets();
    const int i = preset_->currentIndex();
    return i >= 0 && i < int(presets.size()) ? &presets[size_t(i)] : nullptr;
}

void ExportDialog::presetChanged() {
    const ExportPreset* p = currentPreset();
    if (!p) return;
    presetDescription_->setText(QString::fromStdString(p->description));

    const QString ext = QString::fromStdString(p->extension);
    const QString path = path_->text().trimmed();
    path_->setText(path.isEmpty() ? defaultOutputPath(ext) : withExtension(path, ext));

    if (usesCrf(p->settings.videoCodec)) {
        quality_->setMaximum(maxCrf(p->settings.videoCodec));
        quality_->setValue(p->settings.crf);
    }
    updateControls();
    updateSummary();
}

QString ExportDialog::defaultOutputPath(const QString& extension) const {
    QString dir = appSettings().value(kSettingsDir).toString();
    if (dir.isEmpty() || !QDir(dir).exists()) dir = QDir::homePath();

    QString base;
    if (state_) {
        const std::string& projectName = state_->project().name;
        if (!projectName.empty() && projectName != "Untitled") base = safeFileName(QString::fromStdString(projectName));
        if (base.isEmpty() && state_->sequence()) base = safeFileName(QString::fromStdString(state_->sequence()->name));
    }
    if (base.isEmpty()) base = tr("Export");
    return QDir(dir).filePath(extension.isEmpty() ? base : base + '.' + extension);
}

void ExportDialog::browse() {
    const ExportPreset* p = currentPreset();
    const QString ext = p ? QString::fromStdString(p->extension) : QString();
    const QString filter = ext.isEmpty() ? tr("All Files (*)")
                                         : tr("%1 Files (*.%2);;All Files (*)").arg(ext.toUpper(), ext);
    QString start = path_->text().trimmed();
    if (start.isEmpty()) start = defaultOutputPath(ext);

    QString chosen = QFileDialog::getSaveFileName(this, tr("Export To"), start, filter);
    if (chosen.isEmpty()) return;
    if (QFileInfo(chosen).suffix().isEmpty() && !ext.isEmpty()) {
        chosen += '.' + ext;  // overwrite of the new name was not confirmed by the file dialog
        confirmedPath_.clear();
    } else {
        confirmedPath_ = chosen;
    }
    path_->setText(chosen);
    appSettings().setValue(kSettingsDir, QFileInfo(chosen).absolutePath());
}

void ExportDialog::widthChanged(int w) {
    const Sequence* seq = state_ ? state_->sequence() : nullptr;
    if (!updatingSize_ && seq && seq->width > 0) {
        updatingSize_ = true;
        height_->setValue(roundEven(double(w) * seq->height / seq->width));
        updatingSize_ = false;
    }
    updateSummary();
}

void ExportDialog::heightChanged(int h) {
    const Sequence* seq = state_ ? state_->sequence() : nullptr;
    if (!updatingSize_ && seq && seq->height > 0) {
        updatingSize_ = true;
        width_->setValue(roundEven(double(h) * seq->width / seq->height));
        updatingSize_ = false;
    }
    updateSummary();
}

bool ExportDialog::rangeIsInOut() const { return range_->isEnabled() && range_->currentIndex() == 1; }

bool ExportDialog::rangeFrames(FrameTime& in, FrameTime& out) const {
    const Sequence* seq = state_ ? state_->sequence() : nullptr;
    in = 0;
    out = seq ? seq->duration() : 0;
    if (seq && rangeIsInOut()) {
        // In / Out marks are inclusive; the export range end is exclusive.
        if (seq->inPoint >= 0) in = seq->inPoint;
        if (seq->outPoint >= 0) out = seq->outPoint + 1;
    }
    return out > in;
}

void ExportDialog::updateControls() {
    const ExportPreset* p = currentPreset();
    const bool video = p && hasVideo(p->settings);
    matchSize_->setEnabled(video);
    width_->setEnabled(video && !matchSize_->isChecked());
    height_->setEnabled(video && !matchSize_->isChecked());
    color_->setEnabled(video);
    loudness_->setEnabled(p && hasAudio(p->settings));
    quality_->setEnabled(p && usesCrf(p->settings.videoCodec));
    const Sequence* seq = state_ ? state_->sequence() : nullptr;
    const CaptionTrack* ct = seq ? captionTrackFor(*seq) : nullptr;
    captions_->setEnabled(video && ct && !ct->captions.empty());
    captions_->setToolTip(captions_->isEnabled() ? tr("Uses the caption track \"%1\"").arg(QString::fromStdString(ct->name))
                                                 : tr("Add a visible caption track (Captions panel) to export captions"));

    FrameTime in = 0, out = 0;
    const bool canExport = p && state_ && state_->sequence() && rangeFrames(in, out) && !path_->text().trimmed().isEmpty();
    exportButton_->setEnabled(exporting_ ? !cancel_.load() : canExport);
}

void ExportDialog::updateSummary() {
    const Sequence* seq = state_ ? state_->sequence() : nullptr;
    const ExportPreset* p = currentPreset();
    if (!seq || !p) {
        summary_->setText(tr("No sequence is open."));
        return;
    }
    const ExportSettings& s = p->settings;
    QStringList lines;

    if (hasVideo(s)) {
        const int w = matchSize_->isChecked() ? seq->width : width_->value();
        const int h = matchSize_->isChecked() ? seq->height : height_->value();
        QString line = tr("Video: %1 (%2), %3 × %4, %5 fps")
                           .arg(videoCodecName(s), QString::fromStdString(s.videoCodec))
                           .arg(w)
                           .arg(h)
                           .arg(fpsLabel(seq->fps));
        if (usesCrf(s.videoCodec)) line += tr(", CRF %1").arg(quality_->value());
        if (s.alpha) line += tr(", with alpha");
        const ColorSpace* out = findColorSpace(color_->currentData().toString().toStdString());
        const ColorSpace& space = out ? *out : sequenceColorSpace(*seq);
        line += ", " + QString::fromStdString(space.label);
        if (&space != &sequenceColorSpace(*seq) && sequenceColorSpace(*seq).hdr() && !space.hdr()) line += tr(" (tone mapped)");
        lines << line;
    } else {
        lines << tr("Video: none");
    }

    if (hasAudio(s)) {
        int rate = s.sampleRate > 0 ? s.sampleRate : seq->sampleRate;
        if (s.audioCodec == "libopus") rate = 48000;  // Opus always encodes at 48 kHz
        lines << tr("Audio: %1, %2 kHz stereo").arg(audioCodecName(s), QString::number(rate / 1000.0, 'g', 4));
    } else {
        lines << tr("Audio: none");
    }

    FrameTime in = 0, out = 0;
    if (rangeFrames(in, out)) {
        const FrameTime frames = out - in;
        lines << tr("Range: %1 to %2")
                     .arg(QString::fromStdString(formatTimecode(in, seq->fps)),
                          QString::fromStdString(formatTimecode(out, seq->fps)));
        lines << tr("Frames: %1, duration %2")
                     .arg(frames)
                     .arg(QString::fromStdString(formatTimecode(frames, seq->fps)));
    } else {
        lines << tr("Nothing to export: the range is empty.");
    }
    summary_->setText(lines.join('\n'));
}

void ExportDialog::startExport() {
    const Sequence* seq = state_ ? state_->sequence() : nullptr;
    const ExportPreset* p = currentPreset();
    FrameTime in = 0, out = 0;
    if (!seq || !p || exporting_ || !rangeFrames(in, out)) return;

    QSettings settings = appSettings();
    const QString ext = QString::fromStdString(p->extension);
    QString path = path_->text().trimmed();
    if (path.isEmpty()) return;
    if (QFileInfo(path).isRelative()) {
        QString dir = settings.value(kSettingsDir).toString();
        if (dir.isEmpty()) dir = QDir::homePath();
        path = QDir(dir).absoluteFilePath(path);
    }
    if (QFileInfo(path).suffix().isEmpty() && !ext.isEmpty()) path += '.' + ext;
    path = QDir::cleanPath(path);
    if (path != path_->text()) {
        const bool confirmed = path_->text() == confirmedPath_;
        path_->setText(path);
        if (confirmed) confirmedPath_ = path;
    }

    const QFileInfo fi(path);
    if (fi.isDir()) {
        QMessageBox::warning(this, tr("Export"), tr("%1 is a folder. Choose a file name.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    if (!fi.absoluteDir().exists()) {
        QMessageBox::warning(this, tr("Export"),
                             tr("The folder %1 does not exist.").arg(QDir::toNativeSeparators(fi.absolutePath())));
        return;
    }
    if (fi.exists() && path != confirmedPath_) {
        const auto answer = QMessageBox::question(
            this, tr("Export"), tr("%1 already exists.\nDo you want to replace it?").arg(fi.fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
        confirmedPath_ = path;
    }

    ExportSettings s = p->settings;
    s.path = path.toStdString();
    if (hasVideo(s) && !matchSize_->isChecked()) {
        s.width = width_->value();
        s.height = height_->value();
    }
    if (usesCrf(s.videoCodec)) s.crf = quality_->value();
    if (captions_->isEnabled()) {
        const int mode = captions_->currentData().toInt();
        s.burnInCaptions = mode & 1;
        s.embedCaptions = mode & 2;
    }
    settings.setValue("export/captions", captions_->currentIndex());
    if (hasVideo(s)) s.colorSpace = color_->currentData().toString().toStdString();
    if (hasAudio(s) && loudness_->currentIndex() > 0) {
        const QPointF target = loudness_->currentData().toPointF();
        s.loudnessTarget = target.x();
        s.peakCeiling = target.y();
    }
    settings.setValue("export/loudness", loudness_->currentIndex());
    if (rangeIsInOut()) {
        s.in = in;
        s.out = out;
    }

    settings.setValue(kSettingsPreset, preset_->currentText());
    settings.setValue(kSettingsDir, fi.absolutePath());

    exportPath_ = path;
    exportIn_ = in;
    exportFrames_ = out - in;
    fileExisted_ = fi.exists();
    fileModified_ = fileExisted_ ? fi.lastModified().toMSecsSinceEpoch() : 0;
    cancel_ = false;
    progressFraction_ = 0.0;
    progressFrame_ = in;
    progressQueued_ = false;
    setExporting(true);
    elapsed_.start();

    // Called on the worker thread: publish the latest values and post one
    // queued update to the UI thread unless one is already pending.
    ExportProgress onProgress = [this](double fraction, FrameTime frame) {
        progressFraction_ = fraction;
        progressFrame_ = frame;
        if (!progressQueued_.exchange(true))
            QMetaObject::invokeMethod(this, [this] { showProgress(); }, Qt::QueuedConnection);
    };
    const std::atomic<bool>* cancel = &cancel_;
    Project snap = state_->project();
    const Id seqId = seq->id;
    watcher_.setFuture(QtConcurrent::run([snap = std::move(snap), seqId, s, onProgress, cancel]() -> Result {
        Result r;
        try {
            const Sequence* sq = snap.findSequence(seqId);
            if (!sq) {
                r.error = tr("The sequence no longer exists");
                return r;
            }
            std::string error, encoder;
            r.ok = exportSequence(snap, *sq, s, onProgress, cancel, &error, &encoder);
            r.error = QString::fromStdString(error);
            r.encoder = QString::fromStdString(encoder);
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = QString::fromLocal8Bit(e.what());
        }
        return r;
    }));
}

void ExportDialog::requestCancel() {
    if (!exporting_) return;
    cancel_ = true;
    exportButton_->setEnabled(false);
    eta_->setText(tr("Cancelling..."));
}

void ExportDialog::showProgress() {
    progressQueued_ = false;
    if (!exporting_) return;
    const double fraction = std::clamp(progressFraction_.load(), 0.0, 1.0);
    progress_->setValue(int(std::lround(fraction * 1000)));
    if (cancel_) return;

    const qint64 ms = elapsed_.elapsed();
    const FrameTime done = std::clamp<FrameTime>(progressFrame_.load() - exportIn_ + 1, 0, exportFrames_);
    QString text = tr("Frame %1 of %2  ·  Elapsed %3").arg(done).arg(exportFrames_).arg(clockString(ms));
    if (fraction > 0.005 && ms > 1000) {
        const qint64 remaining = qint64(double(ms) * (1.0 - fraction) / fraction);
        text += tr("  ·  About %1 remaining").arg(clockString(remaining));
        text += tr("  ·  %1 fps").arg(QString::number(double(done) * 1000.0 / double(ms), 'f', 1));
    }
    eta_->setText(text);
}

void ExportDialog::exportFinished() {
    if (!exporting_) return;
    const Result r = watcher_.result();
    const bool cancelled = cancel_.load();
    setExporting(false);
    if (r.ok) {
        const QString where = QDir::toNativeSeparators(exportPath_);
        state_->message(r.encoder.isEmpty() ? tr("Exported %1").arg(where)
                                            : tr("Exported %1 (video encoder: %2)").arg(where, r.encoder),
                        8000);
        accept();
        return;
    }
    removePartialFile();
    if (cancelled) {
        eta_->setText(tr("Export cancelled."));
        eta_->show();
        return;
    }
    QMessageBox::warning(this, tr("Export Failed"),
                         tr("Could not export %1.\n\n%2").arg(QDir::toNativeSeparators(exportPath_), r.error));
}

void ExportDialog::stopAndWait() {
    if (!exporting_) return;
    cancel_ = true;
    watcher_.waitForFinished();
    setExporting(false);  // the queued finished() then finds nothing to do
    const Result r = watcher_.result();
    if (r.ok)
        state_->message(tr("Exported %1").arg(QDir::toNativeSeparators(exportPath_)));
    else
        removePartialFile();
}

void ExportDialog::reject() {
    stopAndWait();
    QDialog::reject();
}

void ExportDialog::setExporting(bool on) {
    exporting_ = on;
    form_->setEnabled(!on);
    progress_->setVisible(on);
    eta_->setVisible(on);
    if (on) {
        progress_->setValue(0);
        eta_->setText(tr("Starting..."));
    }
    exportButton_->setText(on ? tr("Cancel") : tr("Export"));
    updateControls();
}

void ExportDialog::removePartialFile() {
    // Only delete what this export wrote: a file that is new, or one it has already overwritten.
    const QFileInfo fi(exportPath_);
    if (exportPath_.isEmpty() || !fi.exists() || fi.isDir()) return;
    if (!fileExisted_ || fi.lastModified().toMSecsSinceEpoch() != fileModified_) QFile::remove(exportPath_);
}

}  // namespace montage
