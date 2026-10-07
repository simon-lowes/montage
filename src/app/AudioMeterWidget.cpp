// Montage — stereo peak meter.
#include "AudioMeterWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <iterator>

#include "Theme.h"

namespace montage {

namespace {

constexpr double kMinDb = -60.0;
constexpr double kMaxDb = 6.0;
constexpr double kSilentDb = -200.0;
constexpr double kFalloffDbPerSec = 24.0;
constexpr double kHoldFalloffDbPerSec = 24.0;
constexpr qint64 kHoldMs = 1500;
constexpr int kTickMs = 33;
constexpr double kGreenTopDb = -12.0;
constexpr double kYellowTopDb = -3.0;
constexpr int kTicks[] = {0, -6, -12, -18, -24, -36, -48, -60};

// Piecewise-linear scale with more resolution near 0 dBFS (slope grows with level).
struct ScalePoint {
    double db, pos;
};
constexpr ScalePoint kScale[] = {{-60, 0.0},   {-48, 0.06},  {-36, 0.16}, {-24, 0.32}, {-18, 0.43},
                                 {-12, 0.55},  {-6, 0.70},   {0, 0.85},   {6, 1.0}};

QColor zoneColor(double db) {
    if (db > kYellowTopDb)
        return theme::kMeterRed;
    if (db >= kGreenTopDb)
        return theme::kMeterYellow;
    return theme::kMeterGreen;
}

QColor withAlpha(QColor c, int a) {
    c.setAlpha(a);
    return c;
}

}  // namespace

AudioMeterWidget::AudioMeterWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setToolTip(tr("Peak level (click to reset clip indicators)"));
    timer_.setInterval(kTickMs);
    connect(&timer_, &QTimer::timeout, this, &AudioMeterWidget::tick);
    clock_.start();
}

double AudioMeterWidget::dbToPosition(double db) {
    if (db <= kScale[0].db)
        return 0.0;
    for (size_t i = 1; i < std::size(kScale); ++i) {
        if (db <= kScale[i].db) {
            const ScalePoint& a = kScale[i - 1];
            const ScalePoint& b = kScale[i];
            return a.pos + (db - a.db) / (b.db - a.db) * (b.pos - a.pos);
        }
    }
    return 1.0;
}

void AudioMeterWidget::setShowScale(bool show) {
    if (show == showScale_)
        return;
    showScale_ = show;
    updateGeometry();
    update();
}

void AudioMeterWidget::setCompact(bool compact) {
    if (compact == compact_)
        return;
    compact_ = compact;
    updateGeometry();
    update();
}

int AudioMeterWidget::scaleWidth() const {
    if (!showScale_)
        return 0;
    QFont f = font();
    f.setPixelSize(compact_ ? 8 : 9);
    return QFontMetrics(f).horizontalAdvance(QStringLiteral("-48")) + 6;
}

QSize AudioMeterWidget::sizeHint() const {
    const int margins = compact_ ? 2 : 4;
    int w = (compact_ ? 16 : 22) + margins + scaleWidth();
    if (showScale_ && !compact_)
        w = std::max(w, 48);
    return {w, 200};
}

QSize AudioMeterWidget::minimumSizeHint() const { return {18 + scaleWidth(), 60}; }

void AudioMeterWidget::setLevels(float left, float right) {
    const float in[2] = {left, right};
    const qint64 now = clock_.elapsed();
    bool active = false;
    for (int i = 0; i < 2; ++i) {
        Channel& c = ch_[i];
        const float v = std::isfinite(in[i]) ? std::max(0.0f, in[i]) : 0.0f;
        const double db = v > 1e-7f ? 20.0 * std::log10(double(v)) : kSilentDb;
        c.level = std::max(c.level, db);  // instant attack, timed release
        if (db >= c.hold) {
            c.hold = db;
            c.holdSince = now;
        }
        if (v >= 1.0f)
            c.clip = true;
        active = active || c.level > kMinDb || c.hold > kMinDb;
    }
    if (active && !timer_.isActive()) {
        lastTick_ = now;
        timer_.start();
    }
    update();
}

void AudioMeterWidget::resetClip() {
    for (Channel& c : ch_)
        c.clip = false;
    update();
}

void AudioMeterWidget::reset() {
    timer_.stop();
    ch_ = {};
    update();
}

void AudioMeterWidget::tick() {
    const qint64 now = clock_.elapsed();
    const double dt = (now - lastTick_) / 1000.0;
    lastTick_ = now;
    bool active = false;
    for (Channel& c : ch_) {
        if (c.level > kSilentDb)
            c.level = c.level - kFalloffDbPerSec * dt;
        if (c.level <= kMinDb)
            c.level = kSilentDb;
        if (now - c.holdSince > kHoldMs && c.hold > kSilentDb)
            c.hold = c.hold - kHoldFalloffDbPerSec * dt;
        if (c.hold <= kMinDb)
            c.hold = kSilentDb;
        active = active || c.level > kMinDb || c.hold > kMinDb;
    }
    if (!active)
        timer_.stop();
    update();
}

void AudioMeterWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        resetClip();
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void AudioMeterWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), theme::kPanel);

    QFont f = font();
    f.setPixelSize(compact_ ? 8 : 9);
    p.setFont(f);
    const QFontMetrics fm(f);

    const int m = compact_ ? 1 : 2;
    const int gap = compact_ ? 1 : 2;
    const int clipH = compact_ ? 4 : 6;
    const int sw = scaleWidth();
    const QRect inner = rect().adjusted(m, m, -m, -m);
    const int barW = std::max(2, (inner.width() - sw - gap) / 2);
    const int barTop = inner.top() + clipH + gap + 1;
    // Leave room for the half of the bottom label that hangs below the -60 tick.
    const int barBottom = inner.bottom() - (showScale_ ? fm.ascent() / 2 : 0);
    const int barH = barBottom - barTop + 1;
    if (barH < 8 || inner.width() < 4)
        return;

    auto yAt = [&](double db) { return barBottom + 1 - dbToPosition(std::min(db, kMaxDb)) * barH; };
    const double yGreen = yAt(kGreenTopDb);
    const double yYellow = yAt(kYellowTopDb);

    for (int i = 0; i < 2; ++i) {
        const Channel& c = ch_[i];
        const int x = inner.left() + i * (barW + gap);

        // Clip indicator.
        const QRect clipRect(x, inner.top(), barW, clipH);
        p.fillRect(clipRect, c.clip ? theme::kMeterRed : theme::kPanelAlt);

        // Unlit track showing the colour zones.
        const QRectF bar(x, barTop, barW, barH);
        p.fillRect(bar, theme::kWindow);
        p.fillRect(QRectF(x, bar.top(), barW, yYellow - bar.top()), withAlpha(theme::kMeterRed, 38));
        p.fillRect(QRectF(x, yYellow, barW, yGreen - yYellow), withAlpha(theme::kMeterYellow, 30));
        p.fillRect(QRectF(x, yGreen, barW, bar.bottom() - yGreen), withAlpha(theme::kMeterGreen, 26));

        // Lit level, split into zones.
        if (c.level > kMinDb) {
            const double top = yAt(c.level);
            const double bottom = bar.bottom();
            auto fillZone = [&](double zoneTop, double zoneBottom, const QColor& col) {
                const double t = std::max(top, zoneTop);
                if (t < zoneBottom)
                    p.fillRect(QRectF(x, t, barW, zoneBottom - t), col);
            };
            fillZone(yGreen, bottom, theme::kMeterGreen);
            fillZone(yYellow, yGreen, theme::kMeterYellow);
            fillZone(bar.top(), yYellow, theme::kMeterRed);
        }

        // Peak-hold marker.
        if (c.hold > kMinDb) {
            const double y = std::max(bar.top(), yAt(c.hold) - 1);
            p.fillRect(QRectF(x, y, barW, compact_ ? 1 : 2), zoneColor(c.hold).lighter(115));
        }
    }

    // Tick marks as thin dark notches across the bars.
    if (barH >= 60) {
        const QColor notch = withAlpha(theme::kWindow, 150);
        for (int db : kTicks) {
            if (db == kTicks[std::size(kTicks) - 1])
                continue;
            const int y = int(std::round(yAt(db)));
            p.fillRect(QRect(inner.left(), y, 2 * barW + gap, 1), notch);
        }
    }

    if (!showScale_)
        return;

    // dB scale beside the bars, skipping labels that would collide.
    const int sx = inner.left() + 2 * barW + gap + 3;
    double lastLabelBottom = -1e9;
    for (int db : kTicks) {
        const double y = std::round(yAt(db));
        const QRectF box(sx, y - fm.height() / 2.0, sw - 3, fm.height());
        if (box.top() >= lastLabelBottom) {
            p.setPen(theme::kTextDim);
            p.drawText(box, Qt::AlignLeft | Qt::AlignVCenter, QString::number(db));
            lastLabelBottom = box.bottom() - 2;
        }
    }
}

}  // namespace montage
