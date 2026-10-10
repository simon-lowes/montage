// Montage — export dialog.
#include "ExportDialog.h"
#include "Settings.h"

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
#include "RenderQueue.h"
#include "core/AudioDescription.h"
#include "core/Surround.h"
#include "SequenceSettingsDialog.h"
#include "Theme.h"
#include "core/History.h"
#include "render/ColorSpace.h"
#include "render/Stereo.h"

namespace montage {

namespace {

const QString kSettingsDir = QStringLiteral("export/lastDirectory");
const QString kSettingsPreset = QStringLiteral("export/preset");


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
        if (s.profile == "4444xq") return QStringLiteral("ProRes 4444 XQ");
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
    if (c == "cfhd") return s.alpha ? QStringLiteral("CineForm RGBA") : QStringLiteral("CineForm");
    if (c == "ffv1") return QStringLiteral("FFV1 (lossless)");
    if (c == "v210") return QStringLiteral("Uncompressed 10-bit");
    return QString::fromStdString(c);
}

QString audioCodecName(const ExportSettings& s) {
    const std::string& c = s.audioCodec;
    const QString kbps = QString::number(s.audioBitrate / 1000);
    if (c == "aac") return QStringLiteral("AAC %1 kb/s").arg(kbps);
    if (c == "libopus") return QStringLiteral("Opus %1 kb/s").arg(kbps);
    if (c == "pcm_s16le") return QStringLiteral("PCM 16-bit");
    if (c == "pcm_s24le") return QStringLiteral("PCM 24-bit");
    if (c == "flac") return QStringLiteral("FLAC (lossless)");
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
    preset_->setObjectName(QStringLiteral("exportPreset"));
    for (const ExportPreset& p : exportPresets()) preset_->addItem(QString::fromStdString(p.name));
    presetDescription_ = new QLabel(form_);
    presetDescription_->setPalette(dim);

    path_ = new QLineEdit(form_);
    path_->setObjectName(QStringLiteral("exportPath"));
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
    allCaptions_ = new QCheckBox(tr("Every caption track (a stream each)"), form_);
    allCaptions_->setObjectName(QStringLiteral("exportAllCaptions"));
    allCaptions_->setToolTip(tr("Embed each caption track as its own subtitle stream, tagged with its language, so players offer them all"));
    allCaptions_->setChecked(appSettings().value("export/allCaptions", false).toBool());
    form->addRow(QString(), allCaptions_);
    cea608_ = new QCheckBox(tr("CEA-608 closed captions in the video (broadcast, streaming platforms)"), form_);
    cea608_->setObjectName(QStringLiteral("exportCea608"));
    cea608_->setToolTip(tr("The caption track as line-21 (A/53) captions inside H.264 or HEVC video, as US broadcast and "
                           "streaming deliveries ask; players show them as CC1"));
    cea608_->setChecked(appSettings().value("export/cea608", false).toBool());
    form->addRow(QString(), cea608_);
    streams_ = new QComboBox(form_);
    streams_->setObjectName(QStringLiteral("exportAudioStreams"));
    streams_->addItem(tr("The mix only"));
    streams_->addItem(tr("The mix, then one per role"));
    streams_->addItem(tr("The mix, then one per track"));
    streams_->setToolTip(tr("A master with more audio streams after the mix (Dialogue, Music, Effects, or each track and dub), "
                            "each named and tagged with its language"));
    streams_->setCurrentIndex(std::clamp(appSettings().value("export/audioStreams", 0).toInt(), 0, 2));
    form->addRow(tr("Audio streams:"), streams_);
    described_ = new QCheckBox(tr("Audio description: the programme with its descriptions as a stream after the mix"), form_);
    described_->setObjectName(QStringLiteral("exportDescribed"));
    described_->setToolTip(tr("The mix without the descriptions (clips of the role Description), then a stream with them, as "
                              "broadcasters and streaming services ask for described video"));
    described_->setChecked(appSettings().value("export/described", true).toBool());
    form->addRow(QString(), described_);
    chapters_ = new QCheckBox(tr("From chapter markers"), form_);
    chapters_->setObjectName(QStringLiteral("exportChapters"));
    chapters_->setChecked(appSettings().value("export/chapters", true).toBool());
    form->addRow(tr("Chapters:"), chapters_);
    startTc_ = new QLineEdit(form_);
    startTc_->setObjectName(QStringLiteral("exportStartTimecode"));
    startTc_->setPlaceholderText(QStringLiteral("00:00:00:00"));
    startTc_->setToolTip(tr("The file's starting timecode, as broadcasters ask (often 10:00:00:00); MXF and MOV keep it"));
    startTc_->setText(appSettings().value("export/startTimecode").toString());
    form->addRow(tr("Start timecode:"), startTc_);
    smart_ = new QCheckBox(tr("Copy untouched footage (smart render)"), form_);
    smart_->setObjectName(QStringLiteral("exportSmartRender"));
    smart_->setChecked(appSettings().value("export/smartRender", true).toBool());
    form->addRow(QString(), smart_);
    color_ = new QComboBox(form_);
    color_->setObjectName(QStringLiteral("exportColor"));
    color_->addItem(tr("Same as sequence (%1)").arg(QString::fromStdString(seq ? sequenceColorSpace(*seq).label : "Rec.709")),
                    QString());
    for (const ColorSpace* cs : displayColorSpaces())
        color_->addItem(QString::fromStdString(cs->label), QString::fromStdString(cs->id));
    color_->setToolTip(tr("Deliver in another colour space: an HDR sequence delivered in Rec.709 is tone mapped.\n"
                          "HDR output is 10-bit and tagged; PQ carries HDR10 metadata."));
    form->addRow(tr("Colour:"), color_);
    // A stereoscopic 3D sequence: how its eyes are delivered (render/Stereo.h).
    stereo_ = new QComboBox(form_);
    stereo_->setObjectName(QStringLiteral("exportStereo"));
    stereo_->addItem(tr("Side by side, full size (both eyes)"), QStringLiteral("sbs"));
    stereo_->addItem(tr("Side by side, squeezed into one frame (3D TV)"), QStringLiteral("sbs_half"));
    stereo_->addItem(tr("Top and bottom, full size (VR players)"), QStringLiteral("tb"));
    stereo_->addItem(tr("Top and bottom, squeezed into one frame"), QStringLiteral("tb_half"));
    stereo_->addItem(tr("Left eye only (2D)"), QStringLiteral("left"));
    stereo_->addItem(tr("Right eye only (2D)"), QStringLiteral("right"));
    stereo_->addItem(tr("Anaglyph (red-cyan)"), QStringLiteral("anaglyph"));
    stereo_->setCurrentIndex(std::max(0, stereo_->findData(appSettings().value("export/stereo", "sbs").toString())));
    stereo_->setToolTip(tr("Packed exports say how their eyes are packed (MP4 and MOV st3d, Matroska StereoMode), so 3D "
                           "players, headsets and YouTube show them in depth"));
    if (seq && seq->stereo3d) form->addRow(tr("Stereo 3D:"), stereo_);
    else stereo_->hide();
    connect(stereo_, &QComboBox::currentIndexChanged, this, &ExportDialog::updateSummary);
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
    // Surround sequences: every channel, or the stereo fold-down.
    {
        const Sequence* sq = state_->sequence();
        const std::string layout = sq ? sq->audioLayout : "stereo";
        audioOut_ = new QComboBox(form_);
        audioOut_->setObjectName(QStringLiteral("exportAudioChannels"));
        audioOut_->addItem(tr("%1 (%n channels)", "", layoutChannels(layout)).arg(QString::fromStdString(layout)));
        audioOut_->addItem(tr("Stereo (folded down)"));
        if (layoutChannels(layout) > 2) form->addRow(tr("Audio channels:"), audioOut_);
        else audioOut_->hide();
        connect(audioOut_, &QComboBox::currentIndexChanged, this, &ExportDialog::updateSummary);
    }
    stems_ = new QComboBox(form_);
    stems_->setObjectName(QStringLiteral("exportStems"));
    stems_->addItem(tr("None"));
    stems_->addItem(tr("Also one WAV per audio track"));
    stems_->addItem(tr("Also one WAV per bus (and Main)"));
    stems_->addItem(tr("Also one WAV per role (Dialogue, Music, Effects...)"));
    stems_->setToolTip(tr("Stems for delivery: each track's, bus's or role's part of the mix, exactly as it plays in it,\n"
                          "written beside the export as \"<name> - <track>.wav\". Together they add up to the mix."));
    form->addRow(tr("Stems:"), stems_);
    // Burn-ins: what a review copy carries in the picture.
    {
        QSettings st = appSettings();
        const QStringList corners = {tr("Top left"), tr("Top centre"), tr("Top right"), tr("Bottom left"), tr("Bottom centre"), tr("Bottom right")};
        auto* row = new QWidget(form_);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        burnTimecode_ = new QCheckBox(tr("Timecode"), row);
        burnTimecode_->setObjectName(QStringLiteral("burnTimecode"));
        burnTimecode_->setChecked(st.value("export/burnTimecode", false).toBool());
        burnClipName_ = new QCheckBox(tr("Clip name"), row);
        burnClipName_->setObjectName(QStringLiteral("burnClipName"));
        burnClipName_->setChecked(st.value("export/burnClipName", false).toBool());
        burnCorner_ = new QComboBox(row);
        burnCorner_->setObjectName(QStringLiteral("burnCorner"));
        burnCorner_->addItems(corners);
        burnCorner_->setCurrentIndex(std::clamp(st.value("export/burnCorner", 0).toInt(), 0, 5));
        rl->addWidget(burnTimecode_);
        rl->addWidget(burnClipName_);
        rl->addWidget(burnCorner_, 1);
        form->addRow(tr("Burn in:"), row);
        burnText_ = new QLineEdit(form_);
        burnText_->setObjectName(QStringLiteral("burnText"));
        burnText_->setPlaceholderText(tr("Text, e.g. \"DRAFT - not for broadcast\""));
        burnText_->setText(st.value("export/burnText").toString());
        form->addRow(QString(), burnText_);
        auto* wrow = new QWidget(form_);
        auto* wl = new QHBoxLayout(wrow);
        wl->setContentsMargins(0, 0, 0, 0);
        watermark_ = new QLineEdit(wrow);
        watermark_->setObjectName(QStringLiteral("watermark"));
        watermark_->setPlaceholderText(tr("Logo or watermark image (optional)"));
        watermark_->setText(st.value("export/watermark").toString());
        auto* pick = new QPushButton(tr("Choose…"), wrow);
        watermarkCorner_ = new QComboBox(wrow);
        watermarkCorner_->setObjectName(QStringLiteral("watermarkCorner"));
        watermarkCorner_->addItems(corners);
        watermarkCorner_->setCurrentIndex(std::clamp(st.value("export/watermarkCorner", 5).toInt(), 0, 5));
        wl->addWidget(watermark_, 1);
        wl->addWidget(pick);
        wl->addWidget(watermarkCorner_);
        form->addRow(tr("Watermark:"), wrow);
        connect(pick, &QPushButton::clicked, this, [this] {
            const QString f = QFileDialog::getOpenFileName(this, tr("Watermark Image"), QString(), tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
            if (!f.isEmpty()) watermark_->setText(f);
        });
    }
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
    exportButton_->setObjectName(QStringLiteral("exportButton"));
    queueButton_ = buttons->addButton(tr("Add to Queue"), QDialogButtonBox::ActionRole);
    queueButton_->setObjectName(QStringLiteral("addToQueue"));
    queueButton_->setToolTip(tr("Render it later from the Render Queue panel, and keep editing"));
    queueButton_->hide();  // until there is a queue (setQueue)
    connect(queueButton_, &QPushButton::clicked, this, &ExportDialog::addToQueue);
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
    connect(captions_, &QComboBox::currentIndexChanged, this, [this] { updateControls(); });
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
    allCaptions_->setEnabled(captions_->isEnabled() && (captions_->currentData().toInt() & 2) && seq && seq->captionTracks.size() > 1);
    {
        const std::string vc = p ? p->settings.videoCodec : std::string();
        const bool avc = vc == "libx264" || vc == "libx265" || vc == "hw_h264" || vc == "hw_hevc";
        cea608_->setEnabled(video && ct && !ct->captions.empty() && avc);
    }
    {
        const QString e = p ? QString::fromStdString(p->extension).toLower() : QString();
        const bool container = e == QLatin1String("mp4") || e == QLatin1String("mov") || e == QLatin1String("mkv") || e == QLatin1String("mxf");
        streams_->setEnabled(p && hasAudio(p->settings) && container);
        described_->setEnabled(p && hasAudio(p->settings) && container && state_ && state_->sequence() &&
                               hasDescriptionClips(*state_->sequence()));
    }
    const QString ext = p ? QString::fromStdString(p->extension).toLower() : QString();
    const bool chapterFile = ext == QLatin1String("mp4") || ext == QLatin1String("mov") || ext == QLatin1String("m4v") ||
                             ext == QLatin1String("mkv") || ext == QLatin1String("webm");
    const bool anyChapters = seq && std::any_of(seq->markers.begin(), seq->markers.end(), [](const Marker& m) { return m.chapter; });
    chapters_->setEnabled(chapterFile && anyChapters);
    startTc_->setEnabled(ext == QLatin1String("mxf") || ext == QLatin1String("mov"));
    const bool intra = p && (p->settings.videoCodec == "prores_ks" || p->settings.videoCodec == "dnxhd");
    smart_->setEnabled(intra);
    smart_->setToolTip(intra ? tr("Frames that are one untouched clip already in this ProRes or DNxHR flavour, size and rate are copied from "
                                  "the source instead of being encoded again: faster, and exactly the original pictures")
                             : tr("Smart rendering works with ProRes and DNxHR"));
    chapters_->setToolTip(!chapterFile ? tr("Chapters can be written into MP4, MOV and MKV files")
                          : anyChapters ? tr("Players and YouTube show the chapter markers as chapters")
                                        : tr("Add chapter markers (Sequence > Add Chapter Marker) to export chapters"));

    FrameTime in = 0, out = 0;
    const bool canExport = p && state_ && state_->sequence() && rangeFrames(in, out) && !path_->text().trimmed().isEmpty();
    exportButton_->setEnabled(exporting_ ? !cancel_.load() : canExport);
    queueButton_->setEnabled(!exporting_ && canExport);
}

void ExportDialog::setQueue(RenderQueue* queue) {
    queue_ = queue;
    queueButton_->setVisible(queue != nullptr);
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
        int w = matchSize_->isChecked() ? seq->width : width_->value();
        int h = matchSize_->isChecked() ? seq->height : height_->value();
        StereoView view = StereoView::Left;
        if (seq->stereo3d) {
            stereoViewFromName(stereo_->currentData().toString().toStdString(), view);
            int across = 1, down = 1;
            stereoPacking(view, across, down);
            w *= across, h *= down;
        }
        QString line = tr("Video: %1 (%2), %3 × %4, %5 fps")
                           .arg(videoCodecName(s), QString::fromStdString(s.videoCodec))
                           .arg(w)
                           .arg(h)
                           .arg(fpsLabel(seq->fps));
        if (usesCrf(s.videoCodec)) line += tr(", CRF %1").arg(quality_->value());
        if (s.alpha && !seq->stereo3d) line += tr(", with alpha");
        if (seq->stereo3d) line += ", " + stereo_->currentText();
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
        // The mix's layout, folded where the codec has fewer channels than the sequence (an immersive mix in AAC).
        const bool fold = audioOut_ && !audioOut_->isHidden() && audioOut_->currentIndex() == 1;
        const std::string layout = fold ? std::string("stereo") : exportAudioLayout(seq->audioLayout, s.audioCodec);
        QString line = tr("Audio: %1, %2 kHz %3").arg(audioCodecName(s), QString::number(rate / 1000.0, 'g', 4), QString::fromStdString(layout));
        if (!fold && layout != seq->audioLayout)
            line += tr(" (%1 carries %2 channels at most: the %3 mix folds down; WAV or MOV PCM, or File › Export ADM Master, keep "
                       "every channel)")
                        .arg(audioCodecName(s))
                        .arg(maxAudioChannels(s.audioCodec))
                        .arg(QString::fromStdString(seq->audioLayout));
        lines << line;
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

// The settings the dialog shows, with the output path checked (and an existing file confirmed).
bool ExportDialog::prepare(ExportSettings& s, FrameTime& in, FrameTime& out) {
    const Sequence* seq = state_ ? state_->sequence() : nullptr;
    const ExportPreset* p = currentPreset();
    in = out = 0;
    if (!seq || !p || exporting_ || !rangeFrames(in, out)) return false;

    QSettings settings = appSettings();
    const QString ext = QString::fromStdString(p->extension);
    QString path = path_->text().trimmed();
    if (path.isEmpty()) return false;
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
        return false;
    }
    if (!fi.absoluteDir().exists()) {
        QMessageBox::warning(this, tr("Export"),
                             tr("The folder %1 does not exist.").arg(QDir::toNativeSeparators(fi.absolutePath())));
        return false;
    }
    if (fi.exists() && path != confirmedPath_) {
        const auto answer = QMessageBox::question(
            this, tr("Export"), tr("%1 already exists.\nDo you want to replace it?").arg(fi.fileName()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return false;
        confirmedPath_ = path;
    }

    s = p->settings;
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
    if (allCaptions_->isEnabled() && allCaptions_->isChecked() && state_->sequence())
        for (const CaptionTrack& t : state_->sequence()->captionTracks)
            if (t.name != kDescriptionTrackName) s.extraCaptions.push_back(t.id);  // descriptions are spoken, not subtitles
    settings.setValue("export/allCaptions", allCaptions_->isChecked());
    s.cea608 = cea608_->isEnabled() && cea608_->isChecked();
    settings.setValue("export/cea608", cea608_->isChecked());
    if (streams_->isEnabled() && streams_->currentIndex() > 0 && state_->sequence()) {
        s.extraAudio = stemStreams(*state_->sequence(), streams_->currentIndex() == 1 ? StemsByRole : StemsByTrack);
        s.audioName = "Mix";
        if (!state_->sequence()->captionTracks.empty()) s.audioLanguage = state_->sequence()->captionTracks.front().language;
    }
    settings.setValue("export/audioStreams", streams_->currentIndex());
    s.describedStream = described_->isEnabled() && described_->isChecked();
    settings.setValue("export/described", described_->isChecked());
    s.chapters = chapters_->isChecked();
    settings.setValue("export/chapters", chapters_->isChecked());
    if (startTc_->isEnabled() && !startTc_->text().trimmed().isEmpty() && state_->sequence()) {
        FrameTime t = 0;
        const Rational fps = state_->sequence()->fps;
        if (!parseTimecode(startTc_->text().trimmed().toStdString(), fps, t) || t < 0) {
            QMessageBox::warning(this, tr("Export"), tr("The start timecode should look like 10:00:00:00."));
            return false;
        }
        s.startTimecode = formatTimecode(t, fps);
    }
    settings.setValue("export/startTimecode", startTc_->text().trimmed());
    s.smartRender = smart_->isChecked();
    settings.setValue("export/smartRender", smart_->isChecked());
    if (hasVideo(s)) s.colorSpace = color_->currentData().toString().toStdString();
    if (hasVideo(s) && state_->sequence() && state_->sequence()->stereo3d) {
        s.stereo = stereo_->currentData().toString().toStdString();
        settings.setValue("export/stereo", stereo_->currentData().toString());
    }
    if (hasAudio(s) && loudness_->currentIndex() > 0) {
        const QPointF target = loudness_->currentData().toPointF();
        s.loudnessTarget = target.x();
        s.peakCeiling = target.y();
    }
    settings.setValue("export/loudness", loudness_->currentIndex());
    if (hasAudio(s)) s.downmixStereo = !audioOut_->isHidden() && audioOut_->currentIndex() == 1;
    stemsMode_ = hasAudio(s) ? stems_->currentIndex() : 0;
    if (hasVideo(s)) {
        s.burnIn.timecode = burnTimecode_->isChecked();
        s.burnIn.clipName = burnClipName_->isChecked();
        s.burnIn.text = burnText_->text().trimmed().toStdString();
        s.burnIn.corner = burnCorner_->currentIndex();
        s.burnIn.watermark = watermark_->text().trimmed().toStdString();
        s.burnIn.watermarkCorner = watermarkCorner_->currentIndex();
        if (!s.burnIn.watermark.empty() && !QFileInfo::exists(watermark_->text().trimmed())) {
            QMessageBox::warning(this, tr("Export"), tr("The watermark image %1 does not exist.").arg(watermark_->text().trimmed()));
            return false;
        }
    }
    settings.setValue("export/burnTimecode", burnTimecode_->isChecked());
    settings.setValue("export/burnClipName", burnClipName_->isChecked());
    settings.setValue("export/burnText", burnText_->text());
    settings.setValue("export/burnCorner", burnCorner_->currentIndex());
    settings.setValue("export/watermark", watermark_->text());
    settings.setValue("export/watermarkCorner", watermarkCorner_->currentIndex());
    if (rangeIsInOut()) {
        s.in = in;
        s.out = out;
    }

    settings.setValue(kSettingsPreset, preset_->currentText());
    settings.setValue(kSettingsDir, fi.absolutePath());
    return true;
}

void ExportDialog::addToQueue() {
    ExportSettings s;
    FrameTime in = 0, out = 0;
    if (!queue_ || !prepare(s, in, out)) return;
    const Sequence* seq = state_->sequence();
    const QString preset = preset_->currentText();
    queue_->add(QString::fromStdString(seq->name), preset, state_->project(), seq->id, s, stemsMode_);
    state_->message(tr("Added %1 to the render queue").arg(QFileInfo(QString::fromStdString(s.path)).fileName()), 5000);
    accept();
}

void ExportDialog::startExport() {
    ExportSettings s;
    FrameTime in = 0, out = 0;
    if (!prepare(s, in, out)) return;
    const Sequence* seq = state_->sequence();
    const QString path = QString::fromStdString(s.path);
    const QFileInfo fi(path);

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
    const int stems = stemsMode_;
    watcher_.setFuture(QtConcurrent::run([snap = std::move(snap), seqId, s, onProgress, cancel, stems]() -> Result {
        Result r;
        try {
            const Sequence* sq = snap.findSequence(seqId);
            if (!sq) {
                r.error = tr("The sequence no longer exists");
                return r;
            }
            std::string error, encoder;
            // With stems, the export fills the first half of the bar and the stems the second.
            const double share = stems > 0 ? 0.5 : 1.0;
            r.ok = exportSequence(
                snap, *sq, s, [&](double f, FrameTime t) { onProgress(f * share, t); }, cancel, &error, &encoder);
            if (r.ok && stems > 0)
                r.ok = exportStems(
                    snap, *sq, s, stems, nullptr, [&](double f, FrameTime t) { onProgress(0.5 + f * 0.5, t); }, cancel, &error);
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
