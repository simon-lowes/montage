#include "SpectralRepairDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "core/EditOps.h"
#include "media/MediaPool.h"

namespace montage {

namespace {

// Level (dB, -100..0) as colour: black through purple and orange to pale yellow.
QRgb heat(float db) {
    static const QColor stops[] = {QColor(0, 0, 4), QColor(59, 15, 112), QColor(140, 41, 129), QColor(254, 159, 109), QColor(252, 253, 191)};
    const double t = std::clamp((double(db) + 100.0) / 100.0, 0.0, 1.0) * 4;
    const int i = std::min(3, int(t));
    const double f = t - i;
    const QColor& a = stops[i];
    const QColor& b = stops[i + 1];
    return qRgb(int(a.red() + (b.red() - a.red()) * f), int(a.green() + (b.green() - a.green()) * f), int(a.blue() + (b.blue() - a.blue()) * f));
}

constexpr double kLongest = 60;  // seconds shown at once

}  // namespace

// ---- The spectrogram ----------------------------------------------------------------------------------------------

SpectrogramView::SpectrogramView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("spectrogram"));
    setMinimumSize(480, 220);
    setMouseTracking(false);
}

void SpectrogramView::setSpectrogram(const Spectrogram& g) {
    g_ = g;
    const int rows = 256;
    image_ = QImage(std::max(1, g.columns), rows, QImage::Format_RGB32);
    image_.fill(Qt::black);
    if (g.columns > 0 && g.bins > 1) {
        minHz_ = std::min(40.0, g.nyquist / 4);
        // Each row shows the loudest of the bins it spans, so a pure tone never falls between rows.
        auto binOf = [&](double hz) { return std::clamp(int(std::lround(hz / g.nyquist * (g.bins - 1))), 0, g.bins - 1); };
        auto hzOfRow = [&](double r) { return minHz_ * std::pow(g.nyquist / minHz_, 1.0 - r / (rows - 1)); };
        for (int r = 0; r < rows; ++r) {
            const int lo = binOf(hzOfRow(r + 0.5)), hi = std::max(lo, binOf(hzOfRow(r - 0.5)));
            auto* line = reinterpret_cast<QRgb*>(image_.scanLine(r));
            for (int c = 0; c < g.columns; ++c) {
                float db = -200;
                for (int k = lo; k <= hi; ++k) db = std::max(db, g.at(c, k));
                line[c] = heat(db);
            }
        }
    }
    update();
}

QRectF SpectrogramView::plot() const { return QRectF(52, 6, std::max(10, width() - 60), std::max(10, height() - 30)); }

double SpectrogramView::timeAt(double x) const {
    const QRectF p = plot();
    return g_.start + std::clamp((x - p.left()) / p.width(), 0.0, 1.0) * (g_.end - g_.start);
}
double SpectrogramView::xOf(double t) const {
    const QRectF p = plot();
    return g_.end > g_.start ? p.left() + (t - g_.start) / (g_.end - g_.start) * p.width() : p.left();
}
double SpectrogramView::hzAt(double y) const {
    const QRectF p = plot();
    const double f = std::clamp((p.bottom() - y) / p.height(), 0.0, 1.0);
    return g_.nyquist > 0 ? minHz_ * std::pow(g_.nyquist / minHz_, f) : 0;
}
double SpectrogramView::yOf(double hz) const {
    const QRectF p = plot();
    if (g_.nyquist <= 0) return p.bottom();
    hz = std::clamp(hz, minHz_, g_.nyquist);
    return p.bottom() - std::log(hz / minHz_) / std::log(g_.nyquist / minHz_) * p.height();
}

void SpectrogramView::setSelection(double start, double end, double low, double high) {
    selStart_ = std::min(start, end), selEnd_ = std::max(start, end);
    selLow_ = std::max(0.0, std::min(low, high > 0 ? high : low)), selHigh_ = high > 0 ? std::max(low, high) : 0;
    update();
    emit selectionChanged();
}

void SpectrogramView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    const QRectF area = plot();
    p.drawImage(area, image_);
    p.setPen(palette().color(QPalette::WindowText));
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.85));
    p.setFont(small);
    // Frequencies up the side, times along the bottom.
    for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
        if (hz < minHz_ || hz > g_.nyquist) continue;
        const double y = yOf(hz);
        p.drawLine(QPointF(area.left() - 4, y), QPointF(area.left(), y));
        p.drawText(QRectF(0, y - 8, area.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                   hz >= 1000 ? QStringLiteral("%1k").arg(hz / 1000) : QString::number(hz));
    }
    const double span = g_.end - g_.start;
    if (span > 0) {
        const double step = span > 30 ? 10 : span > 10 ? 2 : span > 4 ? 1 : span > 1.5 ? 0.5 : 0.1;
        for (double t = std::ceil(g_.start / step) * step; t <= g_.end + 1e-9; t += step) {
            const double x = xOf(t);
            p.drawLine(QPointF(x, area.bottom()), QPointF(x, area.bottom() + 4));
            p.drawText(QRectF(x - 30, area.bottom() + 4, 60, 18), Qt::AlignHCenter | Qt::AlignTop, QString::number(t, 'f', step < 1 ? 1 : 0) + "s");
        }
    }
    p.setClipRect(area);
    auto box = [&](double t0, double t1, double lo, double hi) {
        const double top = hi > 0 ? yOf(hi) : area.top(), bottom = lo > minHz_ ? yOf(lo) : area.bottom();
        return QRectF(QPointF(xOf(t0), top), QPointF(xOf(t1), bottom));
    };
    for (const SpectralRegion& r : regions_) {
        p.setPen(QPen(r.mode == "heal" ? QColor(80, 220, 230) : QColor(255, 170, 60), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawRect(box(r.start, r.end, r.low, r.high));
    }
    if (hasSelection()) {
        p.setPen(QPen(Qt::white, 1.5, Qt::DashLine));
        p.setBrush(QColor(255, 255, 255, 40));
        p.drawRect(box(selStart_, selEnd_, selLow_, selHigh_));
    }
}

void SpectrogramView::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !plot().contains(e->position())) return;
    anchor_ = e->position();
    dragging_ = true;
}

void SpectrogramView::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    const QPointF at = e->position();
    const double topHz = hzAt(std::min(anchor_.y(), at.y())), lowHz = hzAt(std::max(anchor_.y(), at.y()));
    setSelection(timeAt(anchor_.x()), timeAt(at.x()), lowHz <= minHz_ * 1.001 ? 0 : lowHz, topHz >= g_.nyquist * 0.999 ? 0 : topHz);
}

void SpectrogramView::mouseReleaseEvent(QMouseEvent* e) {
    if (!dragging_) return;
    dragging_ = false;
    if ((e->position() - anchor_).manhattanLength() < 3) setSelection(0, 0, 0, 0);  // a click clears it
}

// ---- The dialog ---------------------------------------------------------------------------------------------------

SpectralRepairDialog::SpectralRepairDialog(EditorState* state, Id clipId, QWidget* parent) : QDialog(parent), state_(state), clip_(clipId) {
    setObjectName(QStringLiteral("spectralRepairDialog"));
    setWindowTitle(tr("Spectral Repair"));
    auto* lay = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Drag a box around the sound to take out (a phone, a squeak, a whistle), then Heal it to fill it in "
                                "from the sound either side, or Attenuate it. Find Frequencies fits the box to what stands out."),
                             this);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    view_ = new SpectrogramView(this);
    lay->addWidget(view_, 1);
    auto* row = new QHBoxLayout;
    all_ = new QCheckBox(tr("All frequencies"), this);
    all_->setObjectName(QStringLiteral("spectralAll"));
    find_ = new QPushButton(tr("Find Frequencies"), this);
    find_->setObjectName(QStringLiteral("spectralFind"));
    heal_ = new QPushButton(tr("Heal"), this);
    heal_->setObjectName(QStringLiteral("spectralHeal"));
    attenuate_ = new QPushButton(tr("Attenuate"), this);
    attenuate_->setObjectName(QStringLiteral("spectralAttenuate"));
    gain_ = new QDoubleSpinBox(this);
    gain_->setObjectName(QStringLiteral("spectralGain"));
    gain_->setRange(-96, -1);
    gain_->setValue(-20);
    gain_->setSuffix(tr(" dB"));
    undoLast_ = new QPushButton(tr("Remove Last"), this);
    undoLast_->setObjectName(QStringLiteral("spectralRemoveLast"));
    clear_ = new QPushButton(tr("Clear All"), this);
    clear_->setObjectName(QStringLiteral("spectralClear"));
    row->addWidget(all_);
    row->addWidget(find_);
    row->addSpacing(12);
    row->addWidget(heal_);
    row->addWidget(attenuate_);
    row->addWidget(gain_);
    row->addStretch(1);
    row->addWidget(undoLast_);
    row->addWidget(clear_);
    lay->addLayout(row);
    info_ = new QLabel(this);
    info_->setObjectName(QStringLiteral("spectralInfo"));
    lay->addWidget(info_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(buttons);

    connect(view_, &SpectrogramView::selectionChanged, this, [this] { selectionText(); });
    connect(all_, &QCheckBox::toggled, this, [this](bool) { selectionText(); });
    connect(find_, &QPushButton::clicked, this, [this] { findFrequencies(); });
    connect(heal_, &QPushButton::clicked, this, [this] { heal(); });
    connect(attenuate_, &QPushButton::clicked, this, [this] { attenuate(gain_->value()); });
    connect(undoLast_, &QPushButton::clicked, this, [this] { removeLast(); });
    connect(clear_, &QPushButton::clicked, this, [this] { clearAll(); });
    // Undo and redo show here too.
    connect(state_, &EditorState::projectChanged, this, [this] {
        if (clip() && regions() != shown_) refresh();
    });
    // Another sequence or project: this clip's sound is no longer what is open.
    connect(state_, &EditorState::sequenceSwitched, this, &QDialog::close);

    // The clip's own sound (its channel mapping), over the part of the source it plays (at most a minute, from the
    // playhead when it is longer).
    const Clip* c = clip();
    const Sequence* s = state_->sequence();
    const MediaItem* m = c ? state_->project().findMedia(c->mediaId) : nullptr;
    if (c && s && m && m->hasAudio && !m->path.empty()) {
        source_ = MediaPool::instance().audio(audioKey(m->path, c->channels), 48000);
        const double fps = s->fpsValue();
        double a = clipSourceSeconds(*s, *c, double(c->start) / fps), b = clipSourceSeconds(*s, *c, double(c->end()) / fps);
        if (a > b) std::swap(a, b);
        if (b - a > kLongest) {
            const double now = c->contains(state_->playhead()) ? clipSourceSeconds(*s, *c, double(state_->playhead()) / fps) : a;
            a = std::clamp(now, a, b - kLongest);
            b = a + kLongest;
        }
        const double length = source_ ? double(source_->frames()) / source_->sampleRate : b;
        start_ = std::max(0.0, a - 0.1), end_ = std::min(length, b + 0.1);
    }
    if (!source_) info_->setText(tr("This clip has no sound to repair."));
    for (QWidget* w : std::initializer_list<QWidget*>{find_, heal_, attenuate_, undoLast_, clear_}) w->setEnabled(source_ != nullptr);
    refresh();
    selectionText();
    resize(sizeHint().expandedTo(QSize(940, 520)));
}

const Clip* SpectralRepairDialog::clip() const {
    const Sequence* s = state_->sequence();
    return s ? edit::clipById(*s, clip_) : nullptr;
}

std::vector<SpectralRegion> SpectralRepairDialog::regions() const {
    const Clip* c = clip();
    return c ? spectralRegionsOf(*c) : std::vector<SpectralRegion>{};
}

void SpectralRepairDialog::setSelection(double start, double end, double low, double high) { view_->setSelection(start, end, low, high); }
void SpectralRepairDialog::setAllFrequencies(bool all) { all_->setChecked(all); }
QString SpectralRepairDialog::status() const { return info_->text(); }

void SpectralRepairDialog::selectionText() {
    if (!source_) return;
    if (!view_->hasSelection()) {
        info_->setText(tr("%n repair(s) on this clip. Drag on the spectrogram to choose a sound.", "", int(regions().size())));
        return;
    }
    auto hz = [](double v) { return v >= 1000 ? QStringLiteral("%1 kHz").arg(v / 1000, 0, 'f', 2) : QStringLiteral("%1 Hz").arg(v, 0, 'f', 0); };
    const bool all = all_->isChecked() || (view_->selectionLow() <= 0 && view_->selectionHigh() <= 0);
    info_->setText(tr("%1 to %2 s, %3").arg(view_->selectionStart(), 0, 'f', 2).arg(view_->selectionEnd(), 0, 'f', 2)
                       .arg(all ? tr("all frequencies")
                                : tr("%1 to %2").arg(hz(view_->selectionLow()), view_->selectionHigh() > 0 ? hz(view_->selectionHigh()) : tr("the top"))));
}

void SpectralRepairDialog::refresh() {
    if (!source_ || end_ <= start_) return;
    // The repaired sound around what is shown (a little either side, so healing has its surroundings).
    const int rate = source_->sampleRate;
    const double from = std::max(0.0, start_ - 1.0), to = std::min(double(source_->frames()) / rate, end_ + 1.0);
    AudioBuffer part;
    part.sampleRate = rate;
    const int64_t a = int64_t(from * rate), b = int64_t(to * rate);
    part.samples.assign(source_->samples.begin() + a * 2, source_->samples.begin() + b * 2);
    std::vector<SpectralRegion> shifted = regions();
    for (SpectralRegion& r : shifted) r.start -= double(a) / rate, r.end -= double(a) / rate;
    AudioBuffer repaired;
    spectralRepair(part, repaired, shifted);
    const int columns = std::clamp(int((end_ - start_) * 100), 240, 1400);
    Spectrogram g = computeSpectrogram(repaired, start_ - double(a) / rate, end_ - double(a) / rate, columns);
    g.start = start_, g.end = end_;
    view_->setSpectrogram(g);
    shown_ = regions();
    view_->setRegions(shown_);
    selectionText();
}

bool SpectralRepairDialog::apply(const QString& label, const std::vector<SpectralRegion>& rs) {
    const Id id = clip_;
    const bool done = state_->edit(label, [&](Project& p, Sequence& s) {
        Clip* c = edit::clipById(s, id);
        if (!c) return false;
        if (rs.empty()) {
            const size_t before = c->effects.size();
            c->effects.erase(std::remove_if(c->effects.begin(), c->effects.end(), [](const Effect& e) { return e.type == "spectral_repair"; }),
                             c->effects.end());
            return c->effects.size() != before;
        }
        spectralRepairEffect(p, *c, true)->strings["regions"] = spectralRegionsToString(rs);
        return true;
    });
    if (done && regions() != shown_) refresh();  // (normally already redrawn when the project changed)
    return done;
}

bool SpectralRepairDialog::add(const std::string& mode, double db) {
    if (!source_ || !view_->hasSelection()) {
        info_->setText(tr("Drag on the spectrogram to choose the sound first."));
        return false;
    }
    SpectralRegion r;
    r.start = view_->selectionStart(), r.end = view_->selectionEnd();
    const bool all = all_->isChecked();
    r.low = all ? 0 : view_->selectionLow();
    r.high = all ? 0 : view_->selectionHigh();
    r.mode = mode;
    r.gainDb = db;
    std::vector<SpectralRegion> rs = regions();
    rs.push_back(r);
    const bool done = apply(mode == "heal" ? tr("Spectral Heal") : tr("Spectral Attenuate"), rs);
    if (done) view_->setSelection(0, 0, 0, 0);
    return done;
}

bool SpectralRepairDialog::heal() { return add("heal", 0); }
bool SpectralRepairDialog::attenuate(double db) { return add("attenuate", db); }

bool SpectralRepairDialog::removeLast() {
    std::vector<SpectralRegion> rs = regions();
    if (rs.empty()) return false;
    rs.pop_back();
    return apply(tr("Remove Spectral Repair"), rs);
}

bool SpectralRepairDialog::clearAll() {
    if (regions().empty()) return false;
    return apply(tr("Clear Spectral Repairs"), {});
}

bool SpectralRepairDialog::findFrequencies() {
    if (!source_ || !view_->hasSelection()) {
        info_->setText(tr("Drag across the moment of the sound first."));
        return false;
    }
    const std::vector<SpectralBand> bands = prominentBands(*source_, view_->selectionStart(), view_->selectionEnd(), 1);
    if (bands.empty()) {
        info_->setText(tr("Nothing stands out from the sound around it there."));
        return false;
    }
    all_->setChecked(false);
    view_->setSelection(view_->selectionStart(), view_->selectionEnd(), bands[0].low, bands[0].high);
    return true;
}

}  // namespace montage
