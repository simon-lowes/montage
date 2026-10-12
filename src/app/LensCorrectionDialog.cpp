#include "LensCorrectionDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

#include "EditorState.h"
#include "ModelPacks.h"
#include "core/EditOps.h"
#include "render/ColorChart.h"

namespace montage {

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

// An editable list found by any part of a name.
QComboBox* searchableCombo(QWidget* parent) {
    auto* c = new QComboBox(parent);
    c->setEditable(true);
    c->setInsertPolicy(QComboBox::NoInsert);
    c->setMaxVisibleItems(20);
    c->completer()->setCompletionMode(QCompleter::PopupCompletion);
    c->completer()->setFilterMode(Qt::MatchContains);
    c->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    return c;
}

}  // namespace

std::function<void(LensCorrectionDialog*)>& lensDialogHook() {
    static std::function<void(LensCorrectionDialog*)> hook;
    return hook;
}

LensCorrectionDialog::LensCorrectionDialog(std::shared_ptr<const lens::Database> db, const LensChoice& start,
                                           const QString& clipName, QWidget* parent)
    : QDialog(parent), db_(std::move(db)) {
    setWindowTitle(tr("Correct Lens: %1").arg(clipName));
    auto* v = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    v->addLayout(form);
    camera_ = searchableCombo(this);
    camera_->setObjectName(QStringLiteral("lensCamera"));
    lens_ = searchableCombo(this);
    lens_->setObjectName(QStringLiteral("lensLens"));
    lens_->setMinimumContentsLength(40);
    focal_ = new QDoubleSpinBox(this);
    focal_->setObjectName(QStringLiteral("lensFocal"));
    focal_->setDecimals(1);
    focal_->setSuffix(tr(" mm"));
    aperture_ = new QDoubleSpinBox(this);
    aperture_->setObjectName(QStringLiteral("lensAperture"));
    aperture_->setDecimals(1);
    aperture_->setRange(0, 64);
    aperture_->setPrefix(tr("f/"));
    aperture_->setSpecialValueText(tr("Not known (no vignetting correction)"));
    aperture_->setToolTip(tr("The aperture the clip was shot at: vignetting depends on it"));
    form->addRow(tr("Camera:"), camera_);
    form->addRow(tr("Lens:"), lens_);
    form->addRow(tr("Focal length:"), focal_);
    form->addRow(tr("Aperture:"), aperture_);
    about_ = new QLabel(this);
    about_->setWordWrap(true);
    about_->setObjectName(QStringLiteral("lensAbout"));
    v->addWidget(about_);
    sameMedia_ = new QCheckBox(tr("Every clip of this media"), this);
    sameMedia_->setChecked(true);
    v->addWidget(sameMedia_);
    auto* credit = new QLabel(tr("Profiles from the lensfun database (CC BY-SA 3.0)."), this);
    credit->setEnabled(false);
    v->addWidget(credit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    apply_ = buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    apply_->setObjectName(QStringLiteral("lensApply"));
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    for (const lens::Camera& c : db_->cameras()) cameras_.push_back(&c);
    std::sort(cameras_.begin(), cameras_.end(), [](const lens::Camera* a, const lens::Camera* b) {
        return QString::compare(qs(lens::cameraName(*a)), qs(lens::cameraName(*b)), Qt::CaseInsensitive) < 0;
    });
    camera_->addItem(tr("Any camera"));
    for (const lens::Camera* c : cameras_) camera_->addItem(qs(lens::cameraName(*c)));
    if (start.camera) {
        const auto at = std::find(cameras_.begin(), cameras_.end(), start.camera);
        if (at != cameras_.end()) camera_->setCurrentIndex(int(at - cameras_.begin()) + 1);
    }
    connect(camera_, &QComboBox::currentIndexChanged, this, &LensCorrectionDialog::cameraChanged);
    connect(lens_, &QComboBox::currentIndexChanged, this, &LensCorrectionDialog::lensChanged);
    cameraChanged();
    if (start.lens) setLens(qs(lens::lensName(*start.lens)));
    if (start.focal > 0) focal_->setValue(start.focal);
    if (start.aperture > 0) aperture_->setValue(start.aperture);
}

const lens::Camera* LensCorrectionDialog::currentCamera() const {
    const int i = camera_->currentIndex();
    return i > 0 && i <= int(cameras_.size()) && camera_->currentText() == camera_->itemText(i) ? cameras_[size_t(i - 1)] : nullptr;
}

const lens::Lens* LensCorrectionDialog::currentLens() const {
    const int i = lens_->currentIndex();
    return i >= 0 && i < int(lenses_.size()) && lens_->currentText() == lens_->itemText(i) ? lenses_[size_t(i)] : nullptr;
}

void LensCorrectionDialog::cameraChanged() {
    const QString was = lens_->currentText();
    const lens::Camera* cam = currentCamera();
    lenses_ = db_->lensesFor(cam);
    {
        QSignalBlocker b(lens_);
        lens_->clear();
        for (const lens::Lens* l : lenses_) lens_->addItem(qs(lens::lensName(*l)));
        lens_->setCurrentIndex(-1);
        lens_->setCurrentText(QString());
        lens_->lineEdit()->setPlaceholderText(tr("Choose the lens (type to search)"));
    }
    // The lens chosen before when it fits this camera too; else a fixed-lens camera's lens.
    const auto fixed = std::find(lenses_.begin(), lenses_.end(), db_->fixedLens(cam));
    if (!was.isEmpty() && lens_->findText(was) >= 0) lens_->setCurrentIndex(lens_->findText(was));
    else if (fixed != lenses_.end()) lens_->setCurrentIndex(int(fixed - lenses_.begin()));
    lensChanged();
}

void LensCorrectionDialog::lensChanged() {
    const lens::Lens* l = currentLens();
    apply_->setEnabled(l != nullptr);
    if (!l) {
        about_->setText(lenses_.empty() ? tr("No lens profile fits this camera.") : tr("%n lens profile(s) fit this camera.", "", int(lenses_.size())));
        return;
    }
    const double lo = l->minFocal > 0 ? l->minFocal : 1, hi = std::max(lo, l->maxFocal);
    focal_->setRange(lo, hi);
    focal_->setValue(lo);
    focal_->setEnabled(hi > lo);
    aperture_->setValue(l->minAperture);
    bool distortion = false, tca = false, vignetting = false;
    for (const lens::Calibration& c : l->calibrations) {
        distortion |= !c.distortion.empty();
        tca |= !c.tca.empty();
        vignetting |= !c.vignetting.empty();
    }
    QStringList fixes;
    if (distortion) fixes << tr("distortion");
    if (tca) fixes << tr("colour fringes");
    if (vignetting) fixes << tr("vignetting");
    QString text = tr("Corrects %1.").arg(QLocale().createSeparatedList(fixes));
    if (l->type != lens::Projection::Rectilinear && l->type != lens::Projection::Unknown)
        text += " " + tr("A %1 lens: its picture is made rectilinear (Fisheye to Rectilinear in the Inspector).").arg(qs(lens::projectionName(l->type)).toLower());
    text += " " + tr("Measured on a sensor of crop factor %1.").arg(l->cropFactor, 0, 'g', 3);
    about_->setText(text);
}

LensChoice LensCorrectionDialog::choice() const {
    LensChoice c;
    c.camera = currentCamera();
    c.lens = currentLens();
    c.focal = focal_->value();
    c.aperture = aperture_->value();
    return c;
}

bool LensCorrectionDialog::sameMedia() const { return sameMedia_->isChecked(); }

bool LensCorrectionDialog::setCamera(const QString& name) {
    const int i = name.isEmpty() ? 0 : camera_->findText(name, Qt::MatchFixedString);
    if (i < 0) return false;
    camera_->setCurrentIndex(i);
    camera_->setCurrentText(camera_->itemText(i));
    return true;
}

bool LensCorrectionDialog::setLens(const QString& name) {
    const int i = lens_->findText(name, Qt::MatchFixedString);
    if (i < 0) return false;
    lens_->setCurrentIndex(i);
    lens_->setCurrentText(lens_->itemText(i));
    lensChanged();
    return true;
}

QString LensCorrectionDialog::cameraText() const { return camera_->currentText(); }
QString LensCorrectionDialog::lensText() const { return lens_->currentText(); }

bool correctLens(QWidget* parent, EditorState* state, Id clipId) {
    const Sequence* s = state->sequence();
    const Clip* clip = s ? edit::clipById(*s, clipId) : nullptr;
    const MediaItem* media = clip && clip->mediaId ? state->project().findMedia(clip->mediaId) : nullptr;
    if (!media) {
        state->message(QObject::tr("Select the video clip to correct"));
        return false;
    }
    if (!ensureEffectModel(parent, "lens_correction")) return false;
    const std::shared_ptr<const lens::Database> db = lensDatabase();
    if (!db) return false;
    // What its Lens Correction has, else what its camera suggests.
    LensChoice start = suggestLens(*db, *media);
    for (const Effect& e : clip->effects)
        if (e.type == "lens_correction" && !e.s("lens").empty()) {
            const LensChoice had = lensChoiceOf(*db, e);
            if (had.lens) start = had;
            break;
        }
    LensCorrectionDialog dlg(db, start, qs(clip->name), parent);
    if (lensDialogHook()) lensDialogHook()(&dlg);
    if (dlg.exec() != QDialog::Accepted) return false;
    const LensChoice c = dlg.choice();
    if (!c.lens) return false;
    int n = 0;
    const bool all = dlg.sameMedia();
    state->edit(QObject::tr("Correct Lens"), [&](Project& p, Sequence& sq) {
        n = applyLensCorrection(p, sq, all ? clipsOfSameMedia(sq, clipId) : std::vector<Id>{clipId}, c);
        return n > 0;
    });
    if (n) state->message(QObject::tr("%1 corrected on %n clip(s)", "", n).arg(qs(lens::lensName(*c.lens))), 5000);
    return n > 0;
}

}  // namespace montage
