#include "InterpretFootageDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QVBoxLayout>
#include <cmath>

#include "media/ImageSequence.h"
#include "media/Interpret.h"

namespace montage {

namespace {

QString rateText(double fps) { return QString::number(fps, 'f', std::fabs(fps - std::round(fps)) < 1e-6 ? 0 : 3); }

struct ParPreset {
    const char* name;
    double par;
};
const ParPreset kParPresets[] = {
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "Square Pixels (1.0)"), 1.0},
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "D1/DV NTSC (0.9091)"), 10.0 / 11.0},
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "D1/DV NTSC Widescreen 16:9 (1.2121)"), 40.0 / 33.0},
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "D1/DV PAL (1.0940)"), 59.0 / 54.0},
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "D1/DV PAL Widescreen 16:9 (1.4587)"), 118.0 / 81.0},
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "HDV 1080 / DVCPRO HD 720 (1.3333)"), 4.0 / 3.0},
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "DVCPRO HD 1080 (1.5)"), 1.5},
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "Anamorphic 1.33x"), 1.33},
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "Anamorphic 1.5x"), 1.5},
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "Anamorphic 1.65x"), 1.65},
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "Anamorphic 1.8x"), 1.8},
    {QT_TRANSLATE_NOOP("InterpretFootageDialog", "Anamorphic 2x"), 2.0},
};

}  // namespace

InterpretFootageDialog::InterpretFootageDialog(const MediaItem& first, int count, QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("interpretFootageDialog"));
    setWindowTitle(count > 1 ? tr("Interpret Footage (%1 items)").arg(count) : tr("Interpret Footage: %1").arg(QString::fromStdString(first.name)));
    const Interpretation now = interpretationOf(first);
    const bool video = first.kind == MediaKind::Video;
    const bool sequence = isImageSequencePath(first.path);
    auto* lay = new QVBoxLayout(this);

    // Frame rate.
    auto* rateBox = new QGroupBox(tr("Frame Rate"), this);
    auto* rateLay = new QVBoxLayout(rateBox);
    const double fileFps = fileFrameRate(first).toDouble();
    fileRate_ = new QRadioButton(sequence ? tr("Keep the sequence's frame rate: %1 fps").arg(rateText(fileFps))
                                          : tr("Use frame rate from file: %1 fps").arg(rateText(fileFps)), rateBox);
    fileRate_->setObjectName(QStringLiteral("interpretFileRate"));
    assumeRate_ = new QRadioButton(tr("Assume this frame rate:"), rateBox);
    assumeRate_->setObjectName(QStringLiteral("interpretAssumeRate"));
    rate_ = new QComboBox(rateBox);
    rate_->setObjectName(QStringLiteral("interpretRate"));
    rate_->setEditable(true);
    for (const char* r : {"23.976", "24", "25", "29.97", "30", "48", "50", "59.94", "60"}) rate_->addItem(QString::fromLatin1(r));
    rate_->setCurrentText(rateText(now.conformed() ? now.fps.toDouble() : fileFps > 0 ? fileFps : 24));
    auto* assumeRow = new QHBoxLayout;
    assumeRow->addWidget(assumeRate_);
    assumeRow->addWidget(rate_);
    assumeRow->addStretch();
    keepPitch_ = new QCheckBox(tr("Keep the audio's pitch"), rateBox);
    keepPitch_->setObjectName(QStringLiteral("interpretKeepPitch"));
    keepPitch_->setToolTip(tr("Conformed sound is time-stretched to the new length rather than played at the new speed"));
    keepPitch_->setChecked(now.keepPitch);
    rateLay->addWidget(fileRate_);
    rateLay->addLayout(assumeRow);
    rateLay->addWidget(keepPitch_);
    auto* rates = new QButtonGroup(this);
    rates->addButton(fileRate_);
    rates->addButton(assumeRate_);
    (now.conformed() ? assumeRate_ : fileRate_)->setChecked(true);
    auto syncRate = [this, video, sequence] {
        rate_->setEnabled(assumeRate_->isChecked());
        keepPitch_->setEnabled(assumeRate_->isChecked() && video && !sequence);
    };
    connect(assumeRate_, &QRadioButton::toggled, this, syncRate);
    syncRate();
    rateBox->setEnabled(video);
    lay->addWidget(rateBox);

    // Pixel aspect ratio.
    auto* parBox = new QGroupBox(tr("Pixel Aspect Ratio"), this);
    auto* parLay = new QVBoxLayout(parBox);
    filePar_ = new QRadioButton(tr("Use pixel aspect ratio from file"), parBox);
    filePar_->setObjectName(QStringLiteral("interpretFilePar"));
    conformPar_ = new QRadioButton(tr("Conform to:"), parBox);
    conformPar_->setObjectName(QStringLiteral("interpretConformPar"));
    parPreset_ = new QComboBox(parBox);
    parPreset_->setObjectName(QStringLiteral("interpretParPreset"));
    for (const ParPreset& p : kParPresets) parPreset_->addItem(tr(p.name), p.par);
    parPreset_->addItem(tr("Custom"), 0.0);
    par_ = new QDoubleSpinBox(parBox);
    par_->setObjectName(QStringLiteral("interpretPar"));
    par_->setRange(0.1, 10);
    par_->setDecimals(4);
    par_->setSingleStep(0.01);
    par_->setValue(now.par > 0 ? now.par : 1.0);
    auto* parRow = new QHBoxLayout;
    parRow->addWidget(conformPar_);
    parRow->addWidget(parPreset_, 1);
    parRow->addWidget(par_);
    parLay->addWidget(filePar_);
    parLay->addLayout(parRow);
    auto* pars = new QButtonGroup(this);
    pars->addButton(filePar_);
    pars->addButton(conformPar_);
    (now.par > 0 ? conformPar_ : filePar_)->setChecked(true);
    int preset = parPreset_->count() - 1;
    for (int k = 0; k + 1 < parPreset_->count(); ++k)
        if (std::fabs(parPreset_->itemData(k).toDouble() - par_->value()) < 1e-4) {
            preset = k;
            break;
        }
    parPreset_->setCurrentIndex(preset);
    connect(parPreset_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int k) {
        if (const double v = parPreset_->itemData(k).toDouble(); v > 0) par_->setValue(v);
    });
    auto syncPar = [this] {
        parPreset_->setEnabled(conformPar_->isChecked());
        par_->setEnabled(conformPar_->isChecked());
    };
    connect(conformPar_, &QRadioButton::toggled, this, syncPar);
    syncPar();
    lay->addWidget(parBox);

    // Alpha and fields.
    auto* form = new QFormLayout;
    alpha_ = new QComboBox(this);
    alpha_->setObjectName(QStringLiteral("interpretAlpha"));
    alpha_->addItem(tr("As in the file (straight)"), QString());
    alpha_->addItem(tr("Premultiplied"), QStringLiteral("premultiplied"));
    alpha_->addItem(tr("Ignore alpha"), QStringLiteral("ignore"));
    alpha_->addItem(tr("Invert alpha"), QStringLiteral("invert"));
    alpha_->setCurrentIndex(std::max(0, alpha_->findData(QString::fromStdString(now.alpha))));
    fields_ = new QComboBox(this);
    fields_->setObjectName(QStringLiteral("interpretFields"));
    fields_->addItem(tr("As the frames are flagged"), QString());
    fields_->addItem(tr("No fields (progressive)"), QStringLiteral("progressive"));
    fields_->addItem(tr("Upper field first"), QStringLiteral("upper"));
    fields_->addItem(tr("Lower field first"), QStringLiteral("lower"));
    fields_->setCurrentIndex(std::max(0, fields_->findData(QString::fromStdString(now.fields))));
    fields_->setEnabled(video);
    form->addRow(tr("Alpha:"), alpha_);
    form->addRow(tr("Field order:"), fields_);
    // Stereoscopic 3D: how the two eyes are packed in each frame, for files whose metadata says nothing (or is wrong).
    stereo_ = new QComboBox(this);
    stereo_->setObjectName(QStringLiteral("interpretStereo"));
    const QString detected = first.stereo.empty() || !now.stereo.empty() ? QString() : QStringLiteral(" (%1)").arg(tr("detected"));
    stereo_->addItem(tr("As the file says") + detected, QString());
    stereo_->addItem(tr("Not stereo (one picture)"), QStringLiteral("none"));
    stereo_->addItem(tr("Side by side, full width eyes"), QStringLiteral("sbs"));
    stereo_->addItem(tr("Side by side, squeezed (half width)"), QStringLiteral("sbs_half"));
    stereo_->addItem(tr("Top and bottom, full height eyes"), QStringLiteral("tb"));
    stereo_->addItem(tr("Top and bottom, squeezed (half height)"), QStringLiteral("tb_half"));
    stereo_->setCurrentIndex(std::max(0, stereo_->findData(QString::fromStdString(now.stereo))));
    stereo_->setToolTip(tr("In a stereoscopic 3D sequence each eye reads its own half of the frame; elsewhere the left eye is shown"));
    stereo_->setEnabled(video);
    swapEyes_ = new QCheckBox(tr("Swap left and right eyes"), this);
    swapEyes_->setObjectName(QStringLiteral("interpretSwapEyes"));
    swapEyes_->setChecked(now.swapEyes);
    swapEyes_->setEnabled(video);
    form->addRow(tr("Stereo 3D:"), stereo_);
    form->addRow(QString(), swapEyes_);
    lay->addLayout(form);

    // Camera RAW: how the sensor data is developed.
    auto* rawBox = new QGroupBox(tr("Camera RAW"), this);
    rawBox->setObjectName(QStringLiteral("interpretRawBox"));
    auto* rawForm = new QFormLayout(rawBox);
    exposure_ = new QDoubleSpinBox(rawBox);
    exposure_->setObjectName(QStringLiteral("interpretRawExposure"));
    exposure_->setRange(-5, 5);
    exposure_->setSingleStep(0.1);
    exposure_->setDecimals(2);
    exposure_->setSuffix(tr(" stops"));
    exposure_->setValue(now.rawExposure);
    whiteBalance_ = new QComboBox(rawBox);
    whiteBalance_->setObjectName(QStringLiteral("interpretRawWhiteBalance"));
    whiteBalance_->addItem(tr("As Shot"), 0.0);
    whiteBalance_->addItem(tr("Tungsten (3200 K)"), 3200.0);
    whiteBalance_->addItem(tr("Fluorescent (4000 K)"), 4000.0);
    whiteBalance_->addItem(tr("Daylight (5500 K)"), 5500.0);
    whiteBalance_->addItem(tr("Cloudy (6500 K)"), 6500.0);
    whiteBalance_->addItem(tr("Shade (7500 K)"), 7500.0);
    whiteBalance_->addItem(tr("Custom"), -1.0);
    temperature_ = new QDoubleSpinBox(rawBox);
    temperature_->setObjectName(QStringLiteral("interpretRawTemperature"));
    temperature_->setRange(2000, 25000);
    temperature_->setSingleStep(50);
    temperature_->setDecimals(0);
    temperature_->setSuffix(tr(" K"));
    temperature_->setValue(now.rawTemperature > 0 ? now.rawTemperature : 5500);
    tint_ = new QDoubleSpinBox(rawBox);
    tint_->setObjectName(QStringLiteral("interpretRawTint"));
    tint_->setRange(-150, 150);
    tint_->setDecimals(0);
    tint_->setValue(now.rawTint);
    int wb = now.rawTemperature > 0 ? whiteBalance_->count() - 1 : 0;
    for (int k = 1; k + 1 < whiteBalance_->count() && now.rawTemperature > 0; ++k)
        if (std::fabs(whiteBalance_->itemData(k).toDouble() - now.rawTemperature) < 0.5) wb = k;
    whiteBalance_->setCurrentIndex(wb);
    auto syncWb = [this] {
        const double v = whiteBalance_->currentData().toDouble();
        if (v > 0) temperature_->setValue(v);
        temperature_->setEnabled(v != 0);
    };
    connect(whiteBalance_, qOverload<int>(&QComboBox::currentIndexChanged), this, syncWb);
    syncWb();
    highlights_ = new QComboBox(rawBox);
    highlights_->setObjectName(QStringLiteral("interpretRawHighlights"));
    highlights_->addItem(tr("Clip"), QString());
    highlights_->addItem(tr("Blend"), QStringLiteral("blend"));
    highlights_->addItem(tr("Rebuild"), QStringLiteral("rebuild"));
    highlights_->setCurrentIndex(std::max(0, highlights_->findData(QString::fromStdString(now.rawHighlights))));
    half_ = new QCheckBox(tr("Decode at half size (faster playback)"), rawBox);
    half_->setObjectName(QStringLiteral("interpretRawHalf"));
    half_->setChecked(now.rawHalf);
    rawForm->addRow(tr("Exposure:"), exposure_);
    rawForm->addRow(tr("White balance:"), whiteBalance_);
    rawForm->addRow(tr("Temperature:"), temperature_);
    rawForm->addRow(tr("Tint:"), tint_);
    rawForm->addRow(tr("Highlights:"), highlights_);
    rawForm->addRow(QString(), half_);
    raw_ = isRawMedia(first);
    rawBox->setVisible(raw_);
    lay->addWidget(rawBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);
}

Interpretation InterpretFootageDialog::interpretation() const {
    Interpretation i;
    if (assumeRate_->isChecked()) {
        bool ok = false;
        const double fps = rate_->currentText().trimmed().toDouble(&ok);
        if (ok && fps > 0) i.fps = rateFor(fps);
    }
    i.keepPitch = keepPitch_->isChecked();
    if (conformPar_->isChecked()) i.par = par_->value();
    i.alpha = alpha_->currentData().toString().toStdString();
    i.fields = fields_->currentData().toString().toStdString();
    i.stereo = stereo_->currentData().toString().toStdString();
    i.swapEyes = swapEyes_->isChecked();
    if (raw_) {
        i.rawExposure = exposure_->value();
        i.rawTemperature = whiteBalance_->currentData().toDouble() == 0 ? 0 : temperature_->value();
        i.rawTint = tint_->value();
        i.rawHighlights = highlights_->currentData().toString().toStdString();
        i.rawHalf = half_->isChecked();
    }
    return i;
}

}  // namespace montage
