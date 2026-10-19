#include "LensCalibrationDialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <cmath>

#include "EditorState.h"
#include "core/EditOps.h"
#include "core/Interpretation.h"
#include "media/Decoder.h"
#include "media/LensDatabase.h"
#include "render/ColorChart.h"

namespace montage {

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }
}  // namespace

// The frame with lines drawn on it: a click adds a point to the line being drawn, a double click (or a right click)
// ends it; each line ended is snapped to the picture's edges, its snapped points shown.
class LineCanvas : public QWidget {
public:
    LineCanvas(const QImage& frame, const GrayImage* gray, QWidget* parent) : QWidget(parent), frame_(frame), gray_(gray) {
        setMinimumSize(480, 270);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::CrossCursor);
    }
    // Lines in the frame's pixels: as drawn, and snapped.
    std::vector<std::vector<Point2>> drawn, snapped;
    std::vector<Point2> current;
    std::function<void()> changed;

    void endLine() {
        if (current.size() >= 2) {
            drawn.push_back(current);
            snapped.push_back(snapLineToEdge(*gray_, current));
        }
        current.clear();
        update();
        if (changed) changed();
    }
    void clear() {
        drawn.clear(), snapped.clear(), current.clear();
        update();
        if (changed) changed();
    }
    QSize sizeHint() const override { return {800, 450}; }

protected:
    // Where the frame is drawn in the widget.
    QRectF placed() const {
        if (frame_.isNull()) return QRectF(rect());
        const double s = std::min(width() / double(frame_.width()), height() / double(frame_.height()));
        const double w = frame_.width() * s, h = frame_.height() * s;
        return {(width() - w) / 2, (height() - h) / 2, w, h};
    }
    QPointF toWidget(Point2 p) const {
        const QRectF r = placed();
        return {r.left() + p.x / std::max(1, frame_.width()) * r.width(), r.top() + p.y / std::max(1, frame_.height()) * r.height()};
    }
    Point2 toFrame(QPointF p) const {
        const QRectF r = placed();
        return {(p.x() - r.left()) / r.width() * frame_.width(), (p.y() - r.top()) / r.height() * frame_.height()};
    }
    void paintEvent(QPaintEvent*) override {
        QPainter g(this);
        g.fillRect(rect(), Qt::black);
        g.setRenderHint(QPainter::SmoothPixmapTransform);
        g.drawImage(placed(), frame_);
        g.setRenderHint(QPainter::Antialiasing);
        auto polyline = [&](const std::vector<Point2>& pts, const QColor& c) {
            if (pts.empty()) return;
            QPainterPath path(toWidget(pts[0]));
            for (size_t i = 1; i < pts.size(); ++i) path.lineTo(toWidget(pts[i]));
            g.setPen(QPen(c, 1.5));
            g.drawPath(path);
            for (const Point2& p : pts) g.drawEllipse(toWidget(p), 3, 3);
        };
        for (const auto& l : drawn) polyline(l, QColor(255, 210, 0));
        polyline(current, QColor(255, 120, 0));
        g.setPen(Qt::NoPen);
        g.setBrush(QColor(60, 230, 90));
        for (const auto& l : snapped)
            for (const Point2& p : l) g.drawEllipse(toWidget(p), 1.8, 1.8);
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::RightButton) return endLine();
        if (e->button() != Qt::LeftButton || !placed().contains(e->position())) return;
        current.push_back(toFrame(e->position()));
        update();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) endLine();
    }
    void keyPressEvent(QKeyEvent* e) override {
        if ((e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) && !current.empty()) {
            current.pop_back();
            update();
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) return endLine();
        QWidget::keyPressEvent(e);
    }

private:
    QImage frame_;
    const GrayImage* gray_;
};

std::function<void(LensCalibrationDialog*)>& lensCalibrationDialogHook() {
    static std::function<void(LensCalibrationDialog*)> hook;
    return hook;
}

LensCalibrationDialog::LensCalibrationDialog(Source source, QWidget* parent) : QDialog(parent), src_(std::move(source)) {
    setWindowTitle(tr("Calibrate Lens: %1").arg(src_.clipName));
    auto* v = new QVBoxLayout(this);
    auto* modes = new QHBoxLayout;
    boardMode_ = new QRadioButton(tr("Checkerboard in the clip"), this);
    boardMode_->setObjectName(QStringLiteral("calibrateBoardMode"));
    linesMode_ = new QRadioButton(tr("Lines that should be straight"), this);
    linesMode_->setObjectName(QStringLiteral("calibrateLinesMode"));
    auto* group = new QButtonGroup(this);
    group->addButton(boardMode_);
    group->addButton(linesMode_);
    boardMode_->setChecked(true);
    modes->addWidget(boardMode_);
    modes->addWidget(linesMode_);
    modes->addStretch();
    v->addLayout(modes);

    boardBox_ = new QWidget(this);
    auto* bf = new QFormLayout(boardBox_);
    auto* corners = new QHBoxLayout;
    cols_ = new QSpinBox(boardBox_);
    cols_->setObjectName(QStringLiteral("calibrateCols"));
    cols_->setRange(2, 60);
    cols_->setValue(9);
    rows_ = new QSpinBox(boardBox_);
    rows_->setObjectName(QStringLiteral("calibrateRows"));
    rows_->setRange(2, 60);
    rows_->setValue(6);
    corners->addWidget(cols_);
    corners->addWidget(new QLabel(tr("across by"), boardBox_));
    corners->addWidget(rows_);
    corners->addWidget(new QLabel(tr("down"), boardBox_));
    corners->addStretch();
    bf->addRow(tr("Inner corners:"), corners);
    auto* boardHelp = new QLabel(tr("Film the whole board from many angles, reaching into the frame's corners: up to 40 "
                                    "frames where it has moved are used (a board of 10 by 7 squares has 9 by 6 inner corners)."),
                                 boardBox_);
    boardHelp->setWordWrap(true);
    bf->addRow(boardHelp);
    v->addWidget(boardBox_);

    linesBox_ = new QWidget(this);
    auto* lv = new QVBoxLayout(linesBox_);
    lv->setContentsMargins(0, 0, 0, 0);
    auto* linesHelp = new QLabel(tr("Click along an edge that should be straight (a doorframe, a horizon), double-click to end "
                                    "it; lines far from the middle in several directions work best. Each is snapped to the edge."),
                                 linesBox_);
    linesHelp->setWordWrap(true);
    lv->addWidget(linesHelp);
    canvas_ = new LineCanvas(src_.frame, &src_.gray, linesBox_);
    canvas_->setObjectName(QStringLiteral("calibrateCanvas"));
    canvas_->setFocusPolicy(Qt::ClickFocus);
    lv->addWidget(canvas_, 1);
    auto* lineButtons = new QHBoxLayout;
    auto* endLine = new QPushButton(tr("End Line"), linesBox_);
    auto* clearLines = new QPushButton(tr("Clear Lines"), linesBox_);
    clearLines->setObjectName(QStringLiteral("calibrateClearLines"));
    lineButtons->addWidget(endLine);
    lineButtons->addWidget(clearLines);
    lineButtons->addStretch();
    lv->addLayout(lineButtons);
    connect(endLine, &QPushButton::clicked, this, [this] { canvas_->endLine(); });
    connect(clearLines, &QPushButton::clicked, this, [this] { canvas_->clear(); });
    v->addWidget(linesBox_, 1);

    auto* form = new QFormLayout;
    model_ = new QComboBox(this);
    model_->setObjectName(QStringLiteral("calibrateModel"));
    model_->addItem(tr("Poly 5 (two terms)"), int(lens::Distortion::Poly5));
    model_->addItem(tr("Poly 3 (one term)"), int(lens::Distortion::Poly3));
    model_->addItem(tr("Cubic (three terms)"), int(lens::Distortion::PtLens));
    form->addRow(tr("Distortion model:"), model_);
    anamorphic_ = new QCheckBox(tr("Anamorphic: fit the squeeze"), this);
    anamorphic_->setObjectName(QStringLiteral("calibrateAnamorphic"));
    anamorphic_->setChecked(src_.anamorphic);
    centre_ = new QCheckBox(tr("Fit the lens's centre"), this);
    centre_->setObjectName(QStringLiteral("calibrateCentre"));
    form->addRow(anamorphic_);
    form->addRow(centre_);
    maker_ = new QLineEdit(src_.maker, this);
    maker_->setObjectName(QStringLiteral("calibrateMaker"));
    name_ = new QLineEdit(src_.model, this);
    autoName_ = src_.model;
    name_->setObjectName(QStringLiteral("calibrateName"));
    focal_ = new QDoubleSpinBox(this);
    focal_->setObjectName(QStringLiteral("calibrateFocal"));
    focal_->setRange(0, 2000);
    focal_->setDecimals(1);
    focal_->setSuffix(tr(" mm"));
    focal_->setSpecialValueText(tr("As found"));
    form->addRow(tr("Lens maker:"), maker_);
    form->addRow(tr("Lens name:"), name_);
    form->addRow(tr("Focal length:"), focal_);
    v->addLayout(form);

    auto* run = new QPushButton(tr("Calibrate"), this);
    run->setObjectName(QStringLiteral("calibrateRun"));
    result_ = new QLabel(this);
    result_->setObjectName(QStringLiteral("calibrateResult"));
    result_->setWordWrap(true);
    result_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* runRow = new QHBoxLayout;
    runRow->addWidget(run);
    runRow->addWidget(result_, 1);
    v->addLayout(runRow);
    sameMedia_ = new QCheckBox(tr("Correct every clip of this media"), this);
    sameMedia_->setChecked(true);
    v->addWidget(sameMedia_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    save_ = buttons->addButton(tr("Save and Apply"), QDialogButtonBox::AcceptRole);
    save_->setObjectName(QStringLiteral("calibrateSave"));
    save_->setEnabled(false);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(run, &QPushButton::clicked, this, [this] { calibrate(); });
    connect(boardMode_, &QRadioButton::toggled, this, [this] { modeChanged(); });
    // (A change to what is fitted makes the fit stale.)
    auto stale = [this] {
        fitted_ = false;
        updateResult();
    };
    connect(cols_, qOverload<int>(&QSpinBox::valueChanged), this, stale);
    connect(rows_, qOverload<int>(&QSpinBox::valueChanged), this, stale);
    connect(model_, qOverload<int>(&QComboBox::currentIndexChanged), this, stale);
    connect(anamorphic_, &QCheckBox::toggled, this, stale);
    connect(centre_, &QCheckBox::toggled, this, stale);
    canvas_->changed = stale;
    modeChanged();
    resize(860, 720);
}

void LensCalibrationDialog::modeChanged() {
    const bool lines = linesMode_->isChecked();
    boardBox_->setVisible(!lines);
    linesBox_->setVisible(lines);
    // (A board fixes the centre well; a few lines seldom do.)
    centre_->setChecked(!lines);
    fitted_ = false;
    updateResult();
}

void LensCalibrationDialog::setLinesMode(bool lines) { (lines ? linesMode_ : boardMode_)->setChecked(true); }

void LensCalibrationDialog::setBoard(int cols, int rows) {
    cols_->setValue(cols);
    rows_->setValue(rows);
}

void LensCalibrationDialog::addLine(const std::vector<QPointF>& fractions) {
    canvas_->current.clear();
    for (const QPointF& f : fractions) canvas_->current.push_back({f.x() * src_.frame.width(), f.y() * src_.frame.height()});
    canvas_->endLine();
}

int LensCalibrationDialog::lineCount() const { return int(canvas_->snapped.size()); }

bool LensCalibrationDialog::calibrate() {
    LensFitOptions o;
    o.model = lens::Distortion::Model(model_->currentData().toInt());
    o.fitSqueeze = anamorphic_->isChecked();
    o.fitCenter = centre_->isChecked();
    std::string error;
    fitted_ = false;
    if (linesMode_->isChecked()) {
        fitWidth_ = src_.frame.width(), fitHeight_ = src_.frame.height();
        fitted_ = calibrateFromLines(canvas_->snapped, fitWidth_, fitHeight_, o, fit_, &error);
    } else {
        QProgressDialog progress(tr("Finding the board…"), tr("Cancel"), 0, 1000, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(300);
        std::atomic<bool> cancel{false};
        ClipCalibration cc;
        fitted_ = calibrateClip(src_.path, src_.from, src_.to, cols_->value(), rows_->value(), o, cc, &error,
                                [&](double f) {
                                    progress.setValue(int(f * 1000));
                                    QCoreApplication::processEvents();
                                    if (progress.wasCanceled()) cancel = true;
                                },
                                &cancel);
        fit_ = cc.fit;
        fitWidth_ = cc.width, fitHeight_ = cc.height;
    }
    updateResult();
    if (!fitted_) result_->setText(qs(error));
    return fitted_;
}

QString LensCalibrationDialog::resultText() const { return result_->text(); }

void LensCalibrationDialog::updateResult() {
    save_->setEnabled(fitted_);
    if (!fitted_) {
        result_->setText(linesMode_->isChecked() ? tr("%n line(s) drawn", "", int(canvas_->snapped.size())) : QString());
        return;
    }
    QString text = linesMode_->isChecked()
                       ? tr("%n line(s) straight within %1 px (rms %2)", "", fit_.views).arg(fit_.worst, 0, 'f', 2).arg(fit_.rms, 0, 'f', 3)
                       : tr("Corners from %n frame(s) within %1 px (rms %2)", "", fit_.views).arg(fit_.worst, 0, 'f', 2).arg(fit_.rms, 0, 'f', 3);
    const double mm = fitFocalMm(fit_, fitWidth_, fitHeight_, src_.cropFactor);
    if (mm > 0) text += tr("; focal length %1 mm").arg(mm, 0, 'f', 1);
    // (The name, unless one was typed, with the focal length: so lenses calibrated on one camera keep their own files.)
    if (name_->text() == autoName_) {
        const double named = focal_->value() > 0 ? focal_->value() : mm;
        autoName_ = (src_.device.isEmpty() ? QString() : src_.device + " ") +
                    (named > 0 ? QString::number(named, 'f', 0) + "mm " : QString()) + tr("(calibrated)");
        if (src_.device.isEmpty() && !(named > 0)) autoName_ = tr("Lens (calibrated)");
        name_->setText(autoName_);
    }
    if (fit_.centerX != 0 || fit_.centerY != 0) text += tr("; centre %1, %2").arg(fit_.centerX, 0, 'f', 4).arg(fit_.centerY, 0, 'f', 4);
    if (fit_.squeeze != 1) text += tr("; squeeze %1").arg(fit_.squeeze, 0, 'f', 3);
    result_->setText(text);
}

lens::Lens LensCalibrationDialog::lens() const {
    LensIdentity id;
    id.maker = maker_->text().trimmed().toStdString();
    id.model = name_->text().trimmed().toStdString();
    id.focalMm = focal_->value();
    id.cropFactor = src_.cropFactor;
    id.mount = src_.mount;
    return calibratedLens(fit_, fitWidth_, fitHeight_, id);
}

bool LensCalibrationDialog::sameMedia() const { return sameMedia_->isChecked(); }

bool calibrateLens(QWidget* parent, EditorState* state, Id clipId) {
    const Sequence* s = state->sequence();
    const auto loc = s ? edit::locate(*s, clipId) : std::nullopt;
    const Clip* clip = loc && loc->track.kind == TrackKind::Video ? edit::clipById(*s, clipId) : nullptr;
    const MediaItem* media = clip && clip->mediaId ? state->project().findMedia(clip->mediaId) : nullptr;
    if (!media || !media->hasVideo) {
        state->message(QObject::tr("Select the video clip to calibrate"));
        return false;
    }
    LensCalibrationDialog::Source src;
    src.path = media->path;
    src.clipName = qs(clip->name);
    const double fps = s->fpsValue();
    src.from = std::min(clip->sourceAt(0), clip->sourceAt(double(clip->duration))) / fps;
    src.to = std::max(clip->sourceAt(0), clip->sourceAt(double(clip->duration))) / fps;
    // The frame at the playhead (the clip's middle when it is not on it), as recorded.
    const FrameTime at = clip->contains(state->playhead()) ? state->playhead() : clip->start + clip->duration / 2;
    VideoDecoder dec;
    std::string error;
    if (!dec.open(media->path, &error)) {
        state->message(qs(error));
        return false;
    }
    const Frame16Ptr f = dec.frameAt(clip->sourceAt(double(at - clip->start)) / fps, dec.displayWidth(), dec.displayHeight(), true);
    if (!f) {
        state->message(QObject::tr("The clip's frame could not be read"));
        return false;
    }
    src.frame = QImage(reinterpret_cast<const uchar*>(f->px.data()), f->width, f->height, f->width * 8, QImage::Format_RGBA64)
                    .convertToFormat(QImage::Format_RGB32);
    src.gray = toGray(*f);
    // Named after the camera when it is known.
    const std::shared_ptr<const lens::Database> db = lensDatabase();
    const lens::Camera* camera = db ? suggestLens(*db, *media).camera : nullptr;
    const auto device = media->metadata.find("device");
    src.maker = camera ? qs(camera->maker) : QObject::tr("Calibrated");
    src.device = device != media->metadata.end() ? qs(device->second) : QString();
    src.model = (src.device.isEmpty() ? QString() : src.device + " ") + QObject::tr("(calibrated)");
    src.cropFactor = camera ? camera->cropFactor : 0;
    src.mount = camera ? camera->mount : std::string();
    const double par = interpretationOf(*media).par;
    src.anamorphic = par > 0 && std::fabs(par - 1) > 0.01;
    LensCalibrationDialog dlg(src, parent);
    if (lensCalibrationDialogHook()) lensCalibrationDialogHook()(&dlg);
    if (dlg.exec() != QDialog::Accepted || !dlg.fitted()) return false;
    const lens::Lens made = dlg.lens();
    // (Put on the clip even when it cannot be kept: the effect holds the profile itself.)
    std::string file;
    const bool saved = saveUserLens(made, &file, &error);
    LensChoice c;
    c.lens = &made;
    c.camera = camera;
    c.focal = made.minFocal;
    c.aperture = 0;
    int n = 0;
    const bool all = dlg.sameMedia();
    state->edit(QObject::tr("Calibrate Lens"), [&](Project& p, Sequence& sq) {
        n = applyLensCorrection(p, sq, all ? clipsOfSameMedia(sq, clipId) : std::vector<Id>{clipId}, c);
        return n > 0;
    });
    if (n && saved) state->message(QObject::tr("%1 saved and corrected on %n clip(s)", "", n).arg(qs(lens::lensName(made))), 5000);
    else if (n) state->message(QObject::tr("%1 corrected on %n clip(s), not saved: %2", "", n).arg(qs(lens::lensName(made)), qs(error)), 8000);
    else if (!saved) state->message(qs(error));
    return n > 0;
}

}  // namespace montage
