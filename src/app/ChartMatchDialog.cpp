#include "ChartMatchDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

namespace montage {

namespace {

QColor toColor(const ChartColor& c) {
    return QColor::fromRgbF(float(std::clamp(c[0], 0.0, 1.0)), float(std::clamp(c[1], 0.0, 1.0)), float(std::clamp(c[2], 0.0, 1.0)));
}

QImage toQImage(const Image& img) {
    QImage q(img.width, img.height, QImage::Format_RGB32);
    for (int y = 0; y < img.height; ++y) {
        auto* line = reinterpret_cast<QRgb*>(q.scanLine(y));
        for (int x = 0; x < img.width; ++x) {
            const float* p = img.at(x, y);
            auto c = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
            line[x] = qRgb(c(p[0]), c(p[1]), c(p[2]));
        }
    }
    return q;
}

}  // namespace

ChartView::ChartView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 180);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Chart outline"));
    setAccessibleDescription(tr("Press 1 to 4 (or [ and ]) to pick a corner, the arrow keys to move it, Shift for larger steps"));
    quad_.p[0] = {0.3, 0.3}, quad_.p[1] = {0.7, 0.3}, quad_.p[2] = {0.7, 0.7}, quad_.p[3] = {0.3, 0.7};
}

void ChartView::setPicture(const QImage& img) {
    picture_ = img;
    update();
}

void ChartView::setQuad(const TrackQuad& q) {
    quad_ = q;
    update();
    emit quadChanged();
}

QRectF ChartView::pictureRect() const {
    if (picture_.isNull()) return rect();
    const QSizeF s = QSizeF(picture_.size()).scaled(size(), Qt::KeepAspectRatio);
    return {(width() - s.width()) / 2, (height() - s.height()) / 2, s.width(), s.height()};
}

QPointF ChartView::toWidget(const Point2& p) const {
    const QRectF r = pictureRect();
    return {r.left() + p.x * r.width(), r.top() + p.y * r.height()};
}

void ChartView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    p.setRenderHint(QPainter::Antialiasing);
    if (!picture_.isNull()) p.drawImage(pictureRect(), picture_);
    // The patch grid through the outline (each patch's own cell), and the corners.
    Homography h;
    fitHomography({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, {quad_.p[0], quad_.p[1], quad_.p[2], quad_.p[3]}, h);
    auto at = [&](double u, double v) { return toWidget(h.apply({u, v})); };
    p.setPen(QPen(QColor(255, 255, 255, 200), 1));
    for (int c = 0; c <= 6; ++c) p.drawLine(at(c / 6.0, 0), at(c / 6.0, 1));
    for (int r = 0; r <= 4; ++r) p.drawLine(at(0, r / 4.0), at(1, r / 4.0));
    p.setPen(QPen(QColor(255, 210, 0), 2));
    for (int i = 0; i < 4; ++i) p.drawLine(toWidget(quad_.p[i]), toWidget(quad_.p[(i + 1) % 4]));
    for (int i = 0; i < 4; ++i) {
        p.setBrush(i == 0 ? QColor(255, 210, 0) : QColor(0, 0, 0, 160));
        p.drawEllipse(toWidget(quad_.p[i]), 6, 6);
    }
    if (hasFocus()) {  // the corner the keys move
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::white, 2, Qt::DotLine));
        p.drawEllipse(toWidget(quad_.p[corner_]), 10, 10);
    }
    p.setPen(Qt::white);
    p.drawText(toWidget(quad_.p[0]) + QPointF(9, -8), tr("Dark Skin"));
}

void ChartView::mousePressEvent(QMouseEvent* e) {
    dragging_ = -1;
    double best = 14;
    for (int i = 0; i < 4; ++i) {
        const double d = QLineF(toWidget(quad_.p[i]), e->position()).length();
        if (d < best) best = d, dragging_ = i;
    }
    if (dragging_ >= 0) corner_ = dragging_;
}

void ChartView::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_ < 0 || !(e->buttons() & Qt::LeftButton)) return;
    const QRectF r = pictureRect();
    quad_.p[dragging_] = {std::clamp((e->position().x() - r.left()) / r.width(), 0.0, 1.0), std::clamp((e->position().y() - r.top()) / r.height(), 0.0, 1.0)};
    update();
    emit quadChanged();
    emit quadEdited();
}

void ChartView::mouseReleaseEvent(QMouseEvent*) { dragging_ = -1; }

bool ChartView::ownKey(int k) {
    return (k >= Qt::Key_1 && k <= Qt::Key_4) || k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight || k == Qt::Key_Left ||
           k == Qt::Key_Right || k == Qt::Key_Up || k == Qt::Key_Down;
}

bool ChartView::event(QEvent* e) {
    if (e->type() == QEvent::ShortcutOverride && ownKey(static_cast<QKeyEvent*>(e)->key())) {
        e->accept();
        return true;
    }
    return QWidget::event(e);
}

void ChartView::keyPressEvent(QKeyEvent* e) {
    const int k = e->key();
    if (k >= Qt::Key_1 && k <= Qt::Key_4) {
        corner_ = k - Qt::Key_1;
    } else if (k == Qt::Key_BracketRight || k == Qt::Key_BracketLeft) {
        corner_ = (corner_ + (k == Qt::Key_BracketRight ? 1 : 3)) % 4;
    } else if (k == Qt::Key_Left || k == Qt::Key_Right || k == Qt::Key_Up || k == Qt::Key_Down) {
        const double n = e->modifiers() & Qt::ShiftModifier ? 10 : 1;
        const double dx = n / std::max(1, picture_.isNull() ? width() : picture_.width()),
                     dy = n / std::max(1, picture_.isNull() ? height() : picture_.height());
        Point2& p = quad_.p[corner_];
        if (k == Qt::Key_Left) p.x -= dx;
        if (k == Qt::Key_Right) p.x += dx;
        if (k == Qt::Key_Up) p.y -= dy;
        if (k == Qt::Key_Down) p.y += dy;
        p.x = std::clamp(p.x, 0.0, 1.0), p.y = std::clamp(p.y, 0.0, 1.0);
        emit quadChanged();
        emit quadEdited();
    } else {
        QWidget::keyPressEvent(e);
        return;
    }
    update();
}

ChartSwatches::ChartSwatches(QWidget* parent) : QWidget(parent) { setMinimumSize(300, 120); }

void ChartSwatches::setColors(const ChartColors& shot, const ChartColors& matched) {
    shot_ = shot, matched_ = matched;
    update();
}

void ChartSwatches::paintEvent(QPaintEvent*) {
    // Each patch in three bands: as shot, as matched, and as the chart has it.
    QPainter p(this);
    const ChartColors& ref = colorChecker24();
    const double cw = width() / 6.0, ch = height() / 4.0;
    for (int i = 0; i < 24; ++i) {
        const QRectF cell((i % 6) * cw + 2, (i / 6) * ch + 2, cw - 4, ch - 4);
        const double band = cell.height() / 3;
        p.fillRect(QRectF(cell.left(), cell.top(), cell.width(), band), toColor(shot_[size_t(i)]));
        p.fillRect(QRectF(cell.left(), cell.top() + band, cell.width(), band), toColor(matched_[size_t(i)]));
        p.fillRect(QRectF(cell.left(), cell.top() + 2 * band, cell.width(), cell.height() - 2 * band), toColor(ref[size_t(i)]));
    }
}

ChartMatchDialog::ChartMatchDialog(const Image& frame, const Image& view, const ColorSpace& space, const QString& clipName, QWidget* parent)
    : QDialog(parent), frame_(frame), view_image_(view), space_(space) {
    setObjectName(QStringLiteral("chartMatchDialog"));
    setWindowTitle(tr("Match Colour Chart"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Outline the ColorChecker's 24 patches in %1 (Find Chart, or drag the corners: the yellow one by Dark Skin; "
                                "or press 1 to 4 and the arrow keys). Each patch shows as shot, as matched and as the chart has it.")
                                 .arg(clipName),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* row = new QHBoxLayout;
    view_ = new ChartView(this);
    view_->setObjectName(QStringLiteral("chartView"));
    view_->setPicture(toQImage(view_image_));
    row->addWidget(view_, 3);
    auto* side = new QVBoxLayout;
    swatches_ = new ChartSwatches(this);
    swatches_->setObjectName(QStringLiteral("chartSwatches"));
    side->addWidget(swatches_);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("chartStatus"));
    status_->setWordWrap(true);
    side->addWidget(status_);
    find_ = new QPushButton(tr("Find Chart"), this);
    find_->setObjectName(QStringLiteral("chartFind"));
    connect(find_, &QPushButton::clicked, this, [this] { findChartLater(); });
    side->addWidget(find_);
    sameMedia_ = new QCheckBox(tr("Every clip of this media"), this);
    sameMedia_->setObjectName(QStringLiteral("chartSameMedia"));
    side->addWidget(sameMedia_);
    crosstalk_ = new QCheckBox(tr("As Crosstalk (neutrals kept, no offset)"), this);
    crosstalk_->setObjectName(QStringLiteral("chartAsCrosstalk"));
    crosstalk_->setToolTip(tr("Fit the camera's crosstalk, as under LED light, as a Crosstalk effect whose primaries can then be moved by hand"));
    side->addWidget(crosstalk_);
    side->addStretch(1);
    row->addLayout(side, 2);
    layout->addLayout(row, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    apply_ = buttons->button(QDialogButtonBox::Ok);
    apply_->setText(tr("Apply"));
    apply_->setObjectName(QStringLiteral("chartApply"));
    apply_->setEnabled(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(view_, &ChartView::quadChanged, this, &ChartMatchDialog::measure);
    connect(view_, &ChartView::quadEdited, this, [this] {
        placed_ = true;
        if (searching_) editedWhileSearching_ = true;
        measure();
    });
    connect(&watcher_, &QFutureWatcherBase::finished, this, [this] {
        const auto [ok, q] = watcher_.result();
        found(ok, q);
    });
    resize(980, 520);
    measure();
    findChartLater();
}

bool ChartMatchDialog::findChart() {
    TrackQuad q;
    const bool ok = detectChart(view_image_, q);
    found(ok, q);
    return ok;
}

void ChartMatchDialog::findChartLater() {
    if (searching_) return;
    searching_ = true;
    editedWhileSearching_ = false;
    find_->setEnabled(false);
    apply_->setEnabled(false);
    status_->setText(tr("Looking for the chart…"));
    const Image img = view_image_;
    watcher_.setFuture(QtConcurrent::run([img] {
        TrackQuad q;
        const bool ok = detectChart(img, q);
        return std::make_pair(ok, q);
    }));
}

void ChartMatchDialog::found(bool ok, const TrackQuad& q) {
    searching_ = false;
    find_->setEnabled(true);
    if (ok && !editedWhileSearching_) {
        placed_ = true;
        view_->setQuad(q);  // (measured again by quadChanged)
        return;
    }
    measure();
    if (editedWhileSearching_) {
        editedWhileSearching_ = false;
        return;  // (placed by hand: measured as it is)
    }
    status_->setText(placed_ ? tr("No chart found: the outline stays where it was.") : tr("No chart found: drag the corners onto its patches."));
}

void ChartMatchDialog::setQuad(const TrackQuad& q) {
    placed_ = true;
    view_->setQuad(q);
}

TrackQuad ChartMatchDialog::quad() const { return view_->quad(); }

bool ChartMatchDialog::sameMedia() const { return sameMedia_->isChecked(); }
bool ChartMatchDialog::asCrosstalk() const { return crosstalk_->isChecked(); }

void ChartMatchDialog::measure() {
    bool complete = false;
    const ChartColors shot = sampleChart(frame_, view_->quad(), space_, &complete);
    fit_ = complete ? fitChart(shot) : ChartFit{};
    ChartColors matched{};
    for (size_t i = 0; i < 24; ++i) matched[i] = fit_.ok ? applyChartFit(fit_, shot[i]) : shot[i];
    swatches_->setColors(shot, matched);
    ready_ = fit_.ok && placed_;
    apply_->setEnabled(ready_ && !searching_);
    if (searching_) return;  // ("Looking for the chart…" stays)
    status_->setText(!complete  ? tr("The outline must lie inside the picture, its corners apart: put it on the chart.")
                     : !fit_.ok ? tr("The patches cannot be matched: put the outline on the chart.")
                     : !placed_ ? tr("Find Chart, or drag the corners onto the patches.")
                                : tr("Delta E %1 as shot, %2 matched (worst patch %3)")
                                      .arg(fit_.deltaEBefore, 0, 'f', 1)
                                      .arg(fit_.deltaEAfter, 0, 'f', 1)
                                      .arg(fit_.deltaEMax, 0, 'f', 1));
}

}  // namespace montage
