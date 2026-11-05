#include "EqDisplay.h"

#include <QAccessible>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "Accessibility.h"
#include "Theme.h"
#include "core/Effects.h"
#include "render/AudioFx.h"

namespace montage {

namespace {
constexpr double kLowHz = 20, kHighHz = 20000;
constexpr double kGrab = 9;          // px from a dot's centre that takes it
constexpr double kRate = 48000;      // the rate the curves are drawn at
constexpr double kFloorDb = -90;     // the spectrum's bottom
constexpr int kSpectrumMs = 400;     // a spectrum older than this is not drawn (playback stopped)

fx::ParametricEq eqOf(const std::vector<EqDisplay::Band>& bands, double outputDb) {
    std::vector<fx::EqBand> fb;
    for (const EqDisplay::Band& b : bands)
        fb.push_back({b.hz, eqTypeHasGain(b.type) ? b.db : 0.0, b.q, fx::EqShape(b.type), 12 * (b.slope + 1), b.on});
    fx::ParametricEq eq;
    eq.set(kRate, fb, outputDb);
    return eq;
}

// The type names as the interface shows them (core/Effects.h eqBandTypes, in that order).
QString typeName(int type) {
    static const char* const names[] = {QT_TRANSLATE_NOOP("montage::EqDisplay", "Bell"),     QT_TRANSLATE_NOOP("montage::EqDisplay", "Low Shelf"),
                                        QT_TRANSLATE_NOOP("montage::EqDisplay", "High Shelf"), QT_TRANSLATE_NOOP("montage::EqDisplay", "Low Cut"),
                                        QT_TRANSLATE_NOOP("montage::EqDisplay", "High Cut"),  QT_TRANSLATE_NOOP("montage::EqDisplay", "Notch"),
                                        QT_TRANSLATE_NOOP("montage::EqDisplay", "Band-Pass"), QT_TRANSLATE_NOOP("montage::EqDisplay", "Tilt")};
    return type >= 0 && type < int(std::size(names)) ? EqDisplay::tr(names[type]) : QString();
}
}  // namespace

EqDisplay::EqDisplay(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("eqDisplay"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("EQ display"));
    setToolTip(tr("Double-click to add a band; drag a dot to move it; scroll over a dot to change its Q; right-click a dot for more"));
    access::describe(this, {QAccessible::Graphic, [this] { return accessibleName(); },
                            [this] {
                                const int i = bandIndex(selected_);
                                return i < 0 ? tr("%n band(s)", "", int(bands_.size())) : describe(bands_[size_t(i)]);
                            },
                            {}});
}

QString EqDisplay::hzText(double hz) {
    return hz >= 1000 ? tr("%1 kHz").arg(hz / 1000, 0, 'f', hz >= 10000 ? 1 : 2) : tr("%1 Hz").arg(hz, 0, 'f', hz >= 100 ? 0 : 1);
}

QString EqDisplay::describe(const Band& b) {
    const QString name = b.name == "low" ? tr("Low band") : b.name == "high" ? tr("High band") : tr("Band %1").arg(QString::fromStdString(b.name.substr(1)));
    QStringList parts{name, typeName(b.type), hzText(b.hz)};
    if (eqTypeHasGain(b.type)) parts << tr("%1 dB").arg(QString::asprintf("%+.1f", b.db));
    parts << tr("Q %1").arg(b.q, 0, 'f', 2);
    if (eqTypeHasSlope(b.type)) parts << tr("%1 dB/oct").arg(12 * (b.slope + 1));
    if (!b.on) parts << tr("off");
    return parts.join(QStringLiteral(", "));
}

QColor EqDisplay::colour(int band) {
    const int n = theme::labelCount() - 1;
    return n > 0 ? theme::labelColor(1 + (band + 4) % n) : theme::kAccent;
}

void EqDisplay::setBands(const std::vector<Band>& bands, double outputDb) {
    if (bands == bands_ && outputDb == output_) return;
    bands_ = bands;
    output_ = outputDb;
    if (drag_ >= int(bands_.size())) drag_ = -1;
    update();
}

int EqDisplay::bandIndex(const std::string& name) const {
    for (size_t i = 0; i < bands_.size(); ++i)
        if (bands_[i].name == name) return int(i);
    return -1;
}

void EqDisplay::setSelected(const std::string& band) {
    if (band == selected_) return;
    selected_ = band;
    update();
}

void EqDisplay::select(int band) {
    const std::string name = band >= 0 && band < int(bands_.size()) ? bands_[size_t(band)].name : std::string();
    if (name == selected_) return;
    selected_ = name;
    update();
    announce();
    emit selectionChanged(name);
}

void EqDisplay::announce() {
    if (!QAccessible::isActive()) return;
    const int i = bandIndex(selected_);
    QAccessibleValueChangeEvent ev(this, i < 0 ? QString() : describe(bands_[size_t(i)]));
    QAccessible::updateAccessibility(&ev);
}

void EqDisplay::setRange(double db) {
    range_ = std::clamp(db, 3.0, 48.0);
    update();
}

void EqDisplay::setSpectrum(AudioAnalysisPtr analysis) {
    spectrum_ = std::move(analysis);
    if (!spectrum_) return update();
    spectrumAge_.start();
    if (showSpectrum_) {
        update();
        // (once playback stops, the last one goes)
        QTimer::singleShot(kSpectrumMs + 50, this, [this] {
            if (!spectrumShown()) update();
        });
    }
}

void EqDisplay::setShowSpectrum(bool on) {
    showSpectrum_ = on;
    update();
}

bool EqDisplay::spectrumShown() const {
    return showSpectrum_ && spectrum_ && !spectrum_->bins.empty() && spectrumAge_.isValid() && spectrumAge_.elapsed() < kSpectrumMs;
}

double EqDisplay::responseDb(double hz) const { return eqOf(bands_, output_).responseDb(hz); }

QRectF EqDisplay::plot() const {
    const QFontMetricsF fm(font());
    const double left = fm.horizontalAdvance(QStringLiteral("-30")) + 8, bottom = fm.height() + 4;
    return QRectF(rect()).adjusted(left, 6, -8, -bottom);
}

double EqDisplay::hzToX(double hz) const {
    const QRectF r = plot();
    return r.left() + std::log(std::clamp(hz, kLowHz, kHighHz) / kLowHz) / std::log(kHighHz / kLowHz) * r.width();
}

double EqDisplay::xToHz(double x) const {
    const QRectF r = plot();
    return std::clamp(kLowHz * std::pow(kHighHz / kLowHz, (x - r.left()) / std::max(1.0, r.width())), kLowHz, kHighHz);
}

double EqDisplay::dbToY(double db) const {
    const QRectF r = plot();
    return r.center().y() - db / range_ * r.height() / 2;
}

double EqDisplay::yToDb(double y) const {
    const QRectF r = plot();
    return (r.center().y() - y) / std::max(1.0, r.height() / 2) * range_;
}

QPointF EqDisplay::nodePos(int band) const {
    if (band < 0 || band >= int(bands_.size())) return {};
    const Band& b = bands_[size_t(band)];
    return {hzToX(b.hz), dbToY(eqTypeHasGain(b.type) ? std::clamp(b.db, -range_, range_) : 0.0)};
}

int EqDisplay::nodeAt(QPointF pos) const {
    int best = -1;
    double bestD = kGrab;
    // (the band picked first when dots overlap, then the later ones, drawn on top)
    for (int i = int(bands_.size()) - 1; i >= 0; --i) {
        const double d = std::hypot(nodePos(i).x() - pos.x(), nodePos(i).y() - pos.y());
        if (d < bestD || (d <= kGrab && bands_[size_t(i)].name == selected_)) best = i, bestD = d;
    }
    return best;
}

void EqDisplay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plot();
    p.fillRect(rect(), theme::kPanel);
    p.fillRect(r, theme::kWindow);
    // The grid: decades and their 2s and 5s labelled, the rest faint; gain every step.
    const QFontMetricsF fm(font());
    p.setFont(font());
    for (double decade = 10; decade <= kHighHz; decade *= 10)
        for (int m = 1; m <= 9; ++m) {
            const double hz = decade * m;
            if (hz < kLowHz || hz > kHighHz) continue;
            const bool major = m == 1 || m == 2 || m == 5;
            const double x = hzToX(hz);
            p.setPen(QPen(major ? theme::kBorder : QColor(theme::kBorder.red(), theme::kBorder.green(), theme::kBorder.blue(), 110), 1));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            if (major) {
                const QString label = hz >= 1000 ? QStringLiteral("%1k").arg(hz / 1000) : QString::number(hz);
                const double w = fm.horizontalAdvance(label);
                p.setPen(theme::kTextDim);
                p.drawText(QPointF(std::clamp(x - w / 2, r.left(), r.right() - w), r.bottom() + fm.ascent() + 2), label);
            }
        }
    const double step = range_ <= 6 ? 2 : range_ <= 12 ? 3 : range_ <= 24 ? 6 : 10;
    for (double db = -std::floor(range_ / step) * step; db <= range_ + 1e-9; db += step) {
        const double y = dbToY(db);
        p.setPen(QPen(std::fabs(db) < 1e-9 ? theme::kTextDim : theme::kBorder, 1));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        const QString label = db > 0 ? QStringLiteral("+%1").arg(db) : QString::number(db);
        p.setPen(theme::kTextDim);
        p.drawText(QPointF(r.left() - fm.horizontalAdvance(label) - 4, y + fm.ascent() / 2 - 1), label);
    }
    p.save();
    p.setClipRect(r);
    // The spectrum of what plays (0 dBFS at the top, kFloorDb at the bottom), behind everything.
    if (spectrumShown()) {
        const auto& bins = spectrum_->bins;
        const double binHz = spectrum_->rate / 2.0 / double(bins.size());
        QPainterPath area;
        area.moveTo(r.left(), r.bottom());
        for (int x = int(r.left()); x <= int(r.right()); ++x) {
            const double f0 = xToHz(x - 0.5), f1 = xToHz(x + 0.5);
            const size_t i0 = std::min(bins.size() - 1, size_t(std::max(1.0, f0 / binHz))),
                         i1 = std::min(bins.size() - 1, std::max(i0, size_t(f1 / binHz)));
            double db = kFloorDb;
            for (size_t i = i0; i <= i1; ++i) db = std::max(db, bins[i]);
            area.lineTo(x, r.bottom() - std::clamp((db - kFloorDb) / -kFloorDb, 0.0, 1.0) * r.height());
        }
        area.lineTo(r.right(), r.bottom());
        area.closeSubpath();
        QColor fill = theme::kTextDim;
        fill.setAlpha(55);
        p.fillPath(area, fill);
    }
    const fx::ParametricEq eq = eqOf(bands_, output_);
    const int columns = std::max(2, int(r.width()));
    auto curve = [&](auto&& db) {
        QPainterPath path;
        for (int i = 0; i <= columns; ++i) {
            const double x = r.left() + r.width() * i / columns;
            const QPointF pt(x, dbToY(std::clamp(db(xToHz(x)), -range_ * 1.5, range_ * 1.5)));
            if (i == 0) path.moveTo(pt);
            else path.lineTo(pt);
        }
        return path;
    };
    // Each band's own curve, faint (the picked one's area filled), then what they make together.
    for (int b = 0; b < int(bands_.size()); ++b) {
        QPainterPath path = curve([&](double hz) { return eq.bandResponseDb(b, hz); });
        QColor c = colour(b);
        const bool picked = bands_[size_t(b)].name == selected_;
        if (picked && bands_[size_t(b)].on) {
            QPainterPath fill = path;
            fill.lineTo(r.right(), dbToY(0));
            fill.lineTo(r.left(), dbToY(0));
            fill.closeSubpath();
            QColor f = c;
            f.setAlpha(40);
            p.fillPath(fill, f);
        }
        c.setAlpha(bands_[size_t(b)].on ? (picked ? 200 : 110) : 50);
        p.setPen(QPen(c, picked ? 1.5 : 1, bands_[size_t(b)].on ? Qt::SolidLine : Qt::DotLine));
        p.drawPath(path);
    }
    p.setPen(QPen(theme::kText, 2));
    p.drawPath(curve([&](double hz) { return eq.responseDb(hz); }));
    p.restore();
    // The dots, numbered.
    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() * 0.8));
    p.setFont(small);
    const QFontMetricsF sfm(small);
    for (int b = 0; b < int(bands_.size()); ++b) {
        const Band& band = bands_[size_t(b)];
        const QPointF at = nodePos(b);
        const bool picked = band.name == selected_;
        QColor c = colour(b);
        p.setPen(QPen(picked ? theme::kSelection : theme::kWindow, picked ? 2 : 1));
        p.setBrush(band.on ? c : theme::kPanelAlt);
        const double rad = picked ? 7 : 6;
        p.drawEllipse(at, rad, rad);
        if (!band.on) {
            p.setPen(QPen(c, 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(at, rad - 2, rad - 2);
        }
        const QString n = band.name == "low" ? QStringLiteral("L") : band.name == "high" ? QStringLiteral("H") : QString::fromStdString(band.name.substr(1));
        p.setPen(band.on ? theme::kWindow : theme::kText);
        p.drawText(QRectF(at.x() - rad, at.y() - rad, 2 * rad, 2 * rad), Qt::AlignCenter, n);
        if (picked && hasFocus()) {
            p.setPen(QPen(theme::kAccent, 1, Qt::DotLine));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(at, rad + 3, rad + 3);
        }
    }
    // What the pointer is on or moving.
    if (!readout_.isEmpty()) {
        p.setFont(font());
        const QRectF box = fm.boundingRect(readout_).adjusted(-4, -2, 4, 2);
        const QRectF at(r.left() + 4, r.top() + 4, box.width(), box.height());
        QColor bg = theme::kPanelAlt;
        bg.setAlpha(220);
        p.fillRect(at, bg);
        p.setPen(theme::kText);
        p.drawText(at, Qt::AlignCenter, readout_);
    }
}

void EqDisplay::updateReadout(QPointF pos) {
    QString text;
    const int i = drag_ >= 0 ? drag_ : nodeAt(pos);
    if (i >= 0) text = describe(bands_[size_t(i)]);
    else if (plot().contains(pos)) {
        const double hz = xToHz(pos.x());
        text = tr("%1, %2 dB").arg(hzText(hz), QString::asprintf("%+.1f", responseDb(hz)));
    }
    if (text != readout_) {
        readout_ = text;
        update();
    }
}

void EqDisplay::leaveEvent(QEvent* e) {
    if (drag_ < 0 && !readout_.isEmpty()) {
        readout_.clear();
        update();
    }
    QWidget::leaveEvent(e);
}

void EqDisplay::mousePressEvent(QMouseEvent* e) {
    setFocus(Qt::MouseFocusReason);
    const int i = nodeAt(e->position());
    if (e->button() != Qt::LeftButton) {
        if (i >= 0) select(i);
        return;
    }
    if (i < 0) return;
    select(i);
    drag_ = i;
    moved_ = false;
    pressAt_ = anchorAt_ = e->position();
    anchorHz_ = pressHz_ = bands_[size_t(i)].hz;
    anchorDb_ = pressDb_ = bands_[size_t(i)].db;
    anchorMods_ = e->modifiers();
    const std::string name = bands_[size_t(i)].name;
    emit dragStarted(name);
    if (e->modifiers() & Qt::AltModifier) {
        soloing_ = true;
        emit soloHeld(name, true);
    }
    updateReadout(e->position());
}

void EqDisplay::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ < 0) return updateReadout(e->position());
    Band& b = bands_[size_t(drag_)];
    const Qt::KeyboardModifiers mods = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    // Shift pressed or let go mid-drag: carry on from where the dot is, at the new pace.
    if ((mods & Qt::ShiftModifier) != (anchorMods_ & Qt::ShiftModifier)) {
        anchorAt_ = e->position();
        anchorHz_ = b.hz, anchorDb_ = b.db;
    }
    anchorMods_ = mods;
    const double pace = (mods & Qt::ShiftModifier) ? 0.1 : 1.0;
    const QRectF r = plot();
    const double octaves = std::log2(kHighHz / kLowHz) * (e->position().x() - anchorAt_.x()) / std::max(1.0, r.width()) * pace;
    double hz = std::clamp(anchorHz_ * std::pow(2.0, octaves), kLowHz, kHighHz);
    double db = std::clamp(anchorDb_ - (e->position().y() - anchorAt_.y()) / std::max(1.0, r.height() / 2) * range_ * pace, -30.0, 30.0);
    if (mods & Qt::ControlModifier) {
        // One direction only: whichever the pointer has gone further in since the dot was taken.
        if (std::fabs(e->position().x() - pressAt_.x()) >= std::fabs(e->position().y() - pressAt_.y())) db = pressDb_;
        else hz = pressHz_;
    }
    if (!eqTypeHasGain(b.type)) db = b.db;
    moved_ = moved_ || hz != b.hz || db != b.db;
    b.hz = hz, b.db = db;
    update();
    updateReadout(e->position());
    emit bandMoved(b.name, hz, db, false);
}

void EqDisplay::mouseReleaseEvent(QMouseEvent* e) {
    if (drag_ < 0 || e->button() != Qt::LeftButton) return;
    const Band b = bands_[size_t(drag_)];
    drag_ = -1;
    if (soloing_) {
        soloing_ = false;
        emit soloHeld(b.name, false);
    }
    emit bandMoved(b.name, b.hz, b.db, true);
    updateReadout(e->position());
}

void EqDisplay::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const int i = nodeAt(e->position());
    if (i >= 0) {
        // On a dot: its band off, or back on.
        emit onRequested(bands_[size_t(i)].name, !bands_[size_t(i)].on);
        return;
    }
    if (!plot().contains(e->position())) return;
    if (int(bands_.size()) >= kEqMaxBands) {
        readout_ = tr("An EQ has at most %1 bands").arg(kEqMaxBands);
        update();
        return;
    }
    // A bell where it was clicked; at the ends, a cut (gain does not apply there).
    const double hz = xToHz(e->position().x());
    const int type = hz < 40 ? kEqLowCut : hz > 15000 ? kEqHighCut : kEqBell;
    emit addRequested(hz, type == kEqBell ? std::clamp(yToDb(e->position().y()), -30.0, 30.0) : 0.0, type);
}

void EqDisplay::wheelEvent(QWheelEvent* e) {
    const int i = nodeAt(e->position());
    if (i < 0) return QWidget::wheelEvent(e);
    select(i);
    Band& b = bands_[size_t(i)];
    // (Shift turns a wheel sideways on some systems: either way counts)
    const QPoint d = e->angleDelta();
    const double steps = (d.y() != 0 ? d.y() : d.x()) / 120.0;
    const double q = std::clamp(b.q * std::pow(2.0, steps / ((e->modifiers() & Qt::ShiftModifier) ? 40.0 : 8.0)), 0.1, 30.0);
    if (q != b.q) {
        b.q = q;
        update();
        updateReadout(e->position());
        emit bandStepped(b.name, b.hz, b.db, q);
    }
    e->accept();
}

void EqDisplay::contextMenuEvent(QContextMenuEvent* e) {
    const int i = nodeAt(e->pos());
    if (i < 0) return;
    select(i);
    const Band b = bands_[size_t(i)];
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("eqBandMenu"));
    QMenu* types = menu.addMenu(tr("Type"));
    for (int t = 0; t < int(eqBandTypes().size()); ++t) {
        QAction* a = types->addAction(typeName(t), this, [this, name = b.name, t] { emit typeRequested(name, t); });
        a->setCheckable(true);
        a->setChecked(t == b.type);
    }
    if (eqTypeHasSlope(b.type)) {
        QMenu* slopes = menu.addMenu(tr("Slope"));
        for (int s = 0; s < int(eqSlopes().size()); ++s) {
            QAction* a = slopes->addAction(tr("%1 dB/oct").arg(12 * (s + 1)), this, [this, name = b.name, s] { emit slopeRequested(name, s); });
            a->setCheckable(true);
            a->setChecked(s == b.slope);
        }
    }
    QAction* on = menu.addAction(tr("Band On"), this, [this, name = b.name, now = b.on] { emit onRequested(name, !now); });
    on->setCheckable(true);
    on->setChecked(b.on);
    menu.addSeparator();
    menu.addAction(tr("Delete Band"), this, [this, name = b.name] { emit removeRequested(name); });
    menu.exec(e->globalPos());
}

void EqDisplay::focusInEvent(QFocusEvent* e) {
    if (bandIndex(selected_) < 0 && !bands_.empty()) select(e->reason() == Qt::BacktabFocusReason ? int(bands_.size()) - 1 : 0);
    QWidget::focusInEvent(e);
    update();
}

bool EqDisplay::focusNextPrevChild(bool next) {
    // Tab goes from band to band, then on out of the display.
    const int i = bandIndex(selected_), to = next ? i + 1 : i - 1;
    if (i >= 0 && to >= 0 && to < int(bands_.size())) {
        select(to);
        return true;
    }
    return QWidget::focusNextPrevChild(next);
}

// The keys the display uses go to it before the window's shortcuts (the arrows step frames there, Space plays).
bool EqDisplay::event(QEvent* e) {
    if (e->type() == QEvent::ShortcutOverride) {
        const auto* k = static_cast<QKeyEvent*>(e);
        const bool own = k->key() == Qt::Key_Insert || k->key() == Qt::Key_Plus ||
                         (!selected_.empty() && (k->key() == Qt::Key_Left || k->key() == Qt::Key_Right || k->key() == Qt::Key_Up ||
                                                 k->key() == Qt::Key_Down || k->key() == Qt::Key_PageUp || k->key() == Qt::Key_PageDown ||
                                                 k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace || k->key() == Qt::Key_Space));
        if (own && !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            e->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

void EqDisplay::keyPressEvent(QKeyEvent* e) {
    const int i = bandIndex(selected_);
    if (e->key() == Qt::Key_Insert || (e->key() == Qt::Key_Plus && !(e->modifiers() & Qt::ControlModifier))) {
        if (int(bands_.size()) < kEqMaxBands) emit addRequested(1000, 0, kEqBell);
        return;
    }
    if (i < 0) return QWidget::keyPressEvent(e);
    Band& b = bands_[size_t(i)];
    const bool fine = e->modifiers() & Qt::ShiftModifier;
    double hz = b.hz, db = b.db, q = b.q;
    switch (e->key()) {
        case Qt::Key_Left: hz /= std::pow(2.0, fine ? 1.0 / 96 : 1.0 / 12); break;
        case Qt::Key_Right: hz *= std::pow(2.0, fine ? 1.0 / 96 : 1.0 / 12); break;
        case Qt::Key_Up: db += fine ? 0.1 : 0.5; break;
        case Qt::Key_Down: db -= fine ? 0.1 : 0.5; break;
        case Qt::Key_PageUp: q *= std::pow(2.0, fine ? 1.0 / 16 : 0.25); break;
        case Qt::Key_PageDown: q /= std::pow(2.0, fine ? 1.0 / 16 : 0.25); break;
        case Qt::Key_Delete:
        case Qt::Key_Backspace: emit removeRequested(b.name); return;
        case Qt::Key_Space: emit onRequested(b.name, !b.on); return;
        default: return QWidget::keyPressEvent(e);
    }
    hz = std::clamp(hz, kLowHz, kHighHz);
    db = eqTypeHasGain(b.type) ? std::clamp(std::round(db * 10) / 10, -30.0, 30.0) : b.db;
    q = std::clamp(q, 0.1, 30.0);
    b.hz = hz, b.db = db, b.q = q;
    update();
    announce();
    emit bandStepped(b.name, hz, db, q);
}

}  // namespace montage
