// Montage — sequence settings dialog.
#include "SequenceSettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numeric>

#include "EditorState.h"
#include "Theme.h"
#include "core/History.h"

namespace montage {

namespace {

constexpr int kMinSize = 16;
constexpr int kMaxSize = 8192;

struct SizePreset {
    const char* label;
    int width;
    int height;
};

const SizePreset kSizePresets[] = {
    {QT_TRANSLATE_NOOP("montage::SequenceSettingsDialog", "3840 × 2160  UHD"), 3840, 2160},
    {QT_TRANSLATE_NOOP("montage::SequenceSettingsDialog", "2560 × 1440  QHD"), 2560, 1440},
    {QT_TRANSLATE_NOOP("montage::SequenceSettingsDialog", "1920 × 1080  HD"), 1920, 1080},
    {QT_TRANSLATE_NOOP("montage::SequenceSettingsDialog", "1280 × 720  HD"), 1280, 720},
    {QT_TRANSLATE_NOOP("montage::SequenceSettingsDialog", "1080 × 1920  Vertical"), 1080, 1920},
    {QT_TRANSLATE_NOOP("montage::SequenceSettingsDialog", "1080 × 1080  Square"), 1080, 1080},
};

struct RatePreset {
    const char* label;
    Rational fps;
};

const RatePreset kRatePresets[] = {
    {"23.976", {24000, 1001}}, {"24", {24, 1}}, {"25", {25, 1}},          {"29.97", {30000, 1001}},
    {"30", {30, 1}},           {"50", {50, 1}}, {"59.94", {60000, 1001}}, {"60", {60, 1}},
};

const int kSampleRates[] = {44100, 48000, 96000};

QString fpsLabel(Rational fps) {
    QString s = QString::number(fps.toDouble(), 'f', 3);
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith('.')) s.chop(1);
    return s;
}

QString aspectLabel(int w, int h) {
    if (w <= 0 || h <= 0) return {};
    const int g = std::gcd(w, h);
    const int aw = w / g, ah = h / g;
    if (aw <= 64 && ah <= 64) return QStringLiteral("%1:%2").arg(aw).arg(ah);
    return QString::number(double(w) / double(h), 'f', 2) + QStringLiteral(":1");
}

int evenClamp(int v, int lo, int hi) {
    v = std::clamp(v, lo, hi);
    if (v % 2 != 0) v = (v + 1 <= hi) ? v + 1 : v - 1;
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// EvenSpinBox

EvenSpinBox::EvenSpinBox(QWidget* parent) : QSpinBox(parent) {
    setRange(kMinSize, kMaxSize);
    setSingleStep(2);
    setAccelerated(true);
    setKeyboardTracking(false);
}

QValidator::State EvenSpinBox::validate(QString& input, int& pos) const {
    const QValidator::State st = QSpinBox::validate(input, pos);
    if (st == QValidator::Acceptable && valueFromText(input) % 2 != 0) return QValidator::Intermediate;
    return st;
}

void EvenSpinBox::fixup(QString& input) const {
    QSpinBox::fixup(input);
    bool ok = false;
    const int v = locale().toInt(input.trimmed(), &ok);
    if (ok) input = textFromValue(evenClamp(v, minimum(), maximum()));
}

// ---------------------------------------------------------------------------
// SequenceSettingsDialog

SequenceSettingsDialog::SequenceSettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Sequence Settings"));

    name_ = new QLineEdit(this);
    name_->setMaxLength(200);

    sizePreset_ = new QComboBox(this);
    for (const SizePreset& p : kSizePresets) sizePreset_->addItem(tr(p.label), QSize(p.width, p.height));
    sizePreset_->addItem(tr("Custom"), QSize());

    width_ = new EvenSpinBox(this);
    height_ = new EvenSpinBox(this);
    width_->setToolTip(tr("Frame width in pixels (even numbers only)"));
    height_->setToolTip(tr("Frame height in pixels (even numbers only)"));
    auto* sizeRow = new QHBoxLayout;
    sizeRow->setContentsMargins(0, 0, 0, 0);
    sizeRow->addWidget(width_);
    sizeRow->addWidget(new QLabel(QStringLiteral("×"), this));
    sizeRow->addWidget(height_);
    sizeRow->addWidget(new QLabel(tr("pixels"), this));
    sizeRow->addStretch(1);

    frameRate_ = new QComboBox(this);
    for (const RatePreset& r : kRatePresets)
        frameRate_->addItem(tr("%1 fps").arg(QString::fromLatin1(r.label)), QPoint(r.fps.num, r.fps.den));

    sampleRate_ = new QComboBox(this);
    for (int sr : kSampleRates) sampleRate_->addItem(tr("%1 Hz").arg(sr), sr);

    summary_ = new QLabel(this);
    QPalette dim = summary_->palette();
    dim.setColor(QPalette::WindowText, theme::kTextDim);
    summary_->setPalette(dim);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->addRow(tr("Name:"), name_);
    form->addRow(tr("Frame size:"), sizePreset_);
    form->addRow(QString(), sizeRow);
    form->addRow(tr("Frame rate:"), frameRate_);
    form->addRow(tr("Sample rate:"), sampleRate_);
    form->addRow(QString(), summary_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    okButton_ = buttons->button(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addStretch(1);
    layout->addWidget(buttons);

    connect(name_, &QLineEdit::textChanged, this, [this] { updateSummary(); });
    connect(sizePreset_, &QComboBox::currentIndexChanged, this, [this] { applyPreset(); });
    connect(width_, &QSpinBox::valueChanged, this, [this] { selectPresetForSize(); });
    connect(height_, &QSpinBox::valueChanged, this, [this] { selectPresetForSize(); });
    connect(frameRate_, &QComboBox::currentIndexChanged, this, [this] { updateSummary(); });
    connect(sampleRate_, &QComboBox::currentIndexChanged, this, [this] { updateSummary(); });

    setSpec(NewSequenceSpec{tr("Sequence 1")});
    setMinimumWidth(420);
}

void SequenceSettingsDialog::setSpec(const NewSequenceSpec& spec) {
    updating_ = true;
    name_->setText(spec.name);
    width_->setValue(evenClamp(spec.width, kMinSize, kMaxSize));
    height_->setValue(evenClamp(spec.height, kMinSize, kMaxSize));
    updating_ = false;
    selectPresetForSize();
    selectFrameRate(spec.fps.valid() ? spec.fps : Rational{30, 1});
    selectSampleRate(spec.sampleRate > 0 ? spec.sampleRate : 48000);
    updateSummary();
}

NewSequenceSpec SequenceSettingsDialog::spec() const {
    NewSequenceSpec s;
    s.name = name_->text().trimmed();
    s.width = evenClamp(width_->value(), kMinSize, kMaxSize);
    s.height = evenClamp(height_->value(), kMinSize, kMaxSize);
    const QPoint r = frameRate_->currentData().toPoint();
    s.fps = Rational{r.x(), r.y()};
    s.sampleRate = sampleRate_->currentData().toInt();
    return s;
}

void SequenceSettingsDialog::setFrameRateLocked(bool locked, const QString& reason) {
    frameRate_->setEnabled(!locked);
    frameRate_->setToolTip(locked ? reason : QString());
}

void SequenceSettingsDialog::selectPresetForSize() {
    if (updating_) return;
    const QSize size(width_->value(), height_->value());
    int index = sizePreset_->count() - 1;  // Custom
    for (int i = 0; i < sizePreset_->count() - 1; ++i)
        if (sizePreset_->itemData(i).toSize() == size) index = i;
    updating_ = true;
    sizePreset_->setCurrentIndex(index);
    updating_ = false;
    updateSummary();
}

void SequenceSettingsDialog::applyPreset() {
    if (updating_) return;
    const QSize size = sizePreset_->currentData().toSize();
    if (size.isValid()) {
        updating_ = true;
        width_->setValue(size.width());
        height_->setValue(size.height());
        updating_ = false;
    }
    updateSummary();
}

void SequenceSettingsDialog::selectFrameRate(Rational fps) {
    for (int i = 0; i < frameRate_->count(); ++i) {
        const QPoint r = frameRate_->itemData(i).toPoint();
        // Compare by value so 48/2 matches 24/1.
        if (int64_t(r.x()) * fps.den == int64_t(fps.num) * r.y()) {
            frameRate_->setCurrentIndex(i);
            return;
        }
    }
    // Rates outside the standard list (e.g. from an imported project) are kept as an extra entry.
    frameRate_->addItem(tr("%1 fps").arg(fpsLabel(fps)), QPoint(fps.num, fps.den));
    frameRate_->setCurrentIndex(frameRate_->count() - 1);
}

void SequenceSettingsDialog::selectSampleRate(int rate) {
    int index = sampleRate_->findData(rate);
    if (index < 0) {
        sampleRate_->addItem(tr("%1 Hz").arg(rate), rate);
        index = sampleRate_->count() - 1;
    }
    sampleRate_->setCurrentIndex(index);
}

void SequenceSettingsDialog::updateSummary() {
    const NewSequenceSpec s = spec();
    QStringList parts;
    parts << tr("Aspect %1").arg(aspectLabel(s.width, s.height));
    parts << (isDropFrameRate(s.fps) ? tr("drop-frame timecode") : tr("non-drop-frame timecode"));
    parts << tr("%1 kHz stereo").arg(QString::number(s.sampleRate / 1000.0, 'g', 4));
    summary_->setText(parts.join(QStringLiteral("  ·  ")));
    if (okButton_) okButton_->setEnabled(!s.name.isEmpty());
}

bool SequenceSettingsDialog::editActive(EditorState* state, QWidget* parent) {
    if (!state || !state->sequence()) return false;
    const Sequence& seq = *state->sequence();
    const NewSequenceSpec before{QString::fromStdString(seq.name), seq.width, seq.height, seq.fps, seq.sampleRate};

    SequenceSettingsDialog dlg(parent);
    dlg.setWindowTitle(tr("Sequence Settings"));
    dlg.setSpec(before);
    const bool hasClips = std::any_of(seq.videoTracks.begin(), seq.videoTracks.end(),
                                      [](const Track& t) { return !t.clips.empty(); }) ||
                          std::any_of(seq.audioTracks.begin(), seq.audioTracks.end(),
                                      [](const Track& t) { return !t.clips.empty(); });
    if (hasClips) dlg.setFrameRateLocked(true, tr("The frame rate cannot be changed once the sequence contains clips"));
    if (dlg.exec() != QDialog::Accepted) return false;

    const NewSequenceSpec after = dlg.spec();
    if (after.name == before.name && after.width == before.width && after.height == before.height &&
        after.fps == before.fps && after.sampleRate == before.sampleRate)
        return false;

    const std::string name = after.name.toStdString();
    return state->edit(tr("Sequence Settings"), [=](Project& p, Sequence& s) {
        s.name = name;
        s.width = after.width;
        s.height = after.height;
        s.fps = after.fps;
        s.sampleRate = after.sampleRate;
        // Keep the media item that represents this sequence (for nesting) in sync.
        for (MediaItem& m : p.media) {
            if (m.kind != MediaKind::Sequence || m.sequenceId != s.id) continue;
            m.name = name;
            m.width = after.width;
            m.height = after.height;
            m.fps = after.fps;
        }
        return true;
    });
}

std::optional<NewSequenceSpec> SequenceSettingsDialog::askNew(QWidget* parent, const QString& defaultName) {
    QSettings settings(QStringLiteral("Montage"), QStringLiteral("Montage"));
    NewSequenceSpec spec;
    spec.name = defaultName.isEmpty() ? tr("Sequence") : defaultName;
    spec.width = settings.value(QStringLiteral("newSequence/width"), spec.width).toInt();
    spec.height = settings.value(QStringLiteral("newSequence/height"), spec.height).toInt();
    spec.fps.num = settings.value(QStringLiteral("newSequence/fpsNum"), spec.fps.num).toInt();
    spec.fps.den = settings.value(QStringLiteral("newSequence/fpsDen"), spec.fps.den).toInt();
    spec.sampleRate = settings.value(QStringLiteral("newSequence/sampleRate"), spec.sampleRate).toInt();

    SequenceSettingsDialog dlg(parent);
    dlg.setWindowTitle(tr("New Sequence"));
    dlg.setSpec(spec);
    dlg.name_->selectAll();
    if (dlg.exec() != QDialog::Accepted) return std::nullopt;

    spec = dlg.spec();
    settings.setValue(QStringLiteral("newSequence/width"), spec.width);
    settings.setValue(QStringLiteral("newSequence/height"), spec.height);
    settings.setValue(QStringLiteral("newSequence/fpsNum"), spec.fps.num);
    settings.setValue(QStringLiteral("newSequence/fpsDen"), spec.fps.den);
    settings.setValue(QStringLiteral("newSequence/sampleRate"), spec.sampleRate);
    return spec;
}

}  // namespace montage
