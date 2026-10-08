// Montage — video scopes widget.
#include "ScopesWidget.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <vector>

#include "Theme.h"

namespace montage {

namespace {

constexpr int kAnalysisHz = 15;
constexpr int kMaxAnalysisWidth = 480;
constexpr int kMaxAnalysisHeight = 320;
constexpr int kVectorBins = 256;
constexpr float kVectorRange = 0.6f;  // Cb/Cr magnitude at the edge of the vectorscope circle
constexpr double kPi = 3.14159265358979323846;

// Rec.709 luma on 8-bit values (weights sum to 65536).
inline int luma709(int r, int g, int b) { return (13933 * r + 46871 * g + 4732 * b + 32768) >> 16; }

struct CbCr {
    float cb, cr;  // -0.5..0.5
};

inline CbCr cbcr709(float r, float g, float b) {
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return {(b - y) / 1.8556f, (r - y) / 1.5748f};
}

QImage downsample(const QImage& src) {
    QSize size = src.size();
    if (size.width() > kMaxAnalysisWidth || size.height() > kMaxAnalysisHeight)
        size = size.scaled(kMaxAnalysisWidth, kMaxAnalysisHeight, Qt::KeepAspectRatio);
    size = size.expandedTo(QSize(1, 1));
    // Point sampling keeps real pixel values (smoothing would hide outliers).
    const QImage img = size == src.size() ? src : src.scaled(size, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    return img.convertToFormat(QImage::Format_RGB32);
}

// Phosphor-like shade: tint scales with density, the densest areas bloom towards white.
// Returns a premultiplied ARGB value.
QRgb phosphor(const QColor& base, float s) {
    const float hot = std::clamp((s - 0.8f) / 0.2f, 0.0f, 1.0f) * 0.35f;
    auto ch = [&](int v) { return int((v + (255 - v) * hot) * s); };
    return qRgba(ch(base.red()), ch(base.green()), ch(base.blue()), int(255 * s));
}

// Hue of every vectorscope bin, so the trace shows the colours it represents.
const std::vector<QRgb>& vectorscopeColors() {
    static const std::vector<QRgb> table = [] {
        std::vector<QRgb> t(size_t(kVectorBins) * kVectorBins);
        const float half = kVectorBins / 2.0f;
        for (int by = 0; by < kVectorBins; ++by) {
            for (int bx = 0; bx < kVectorBins; ++bx) {
                const float cb = (bx + 0.5f - half) / half * kVectorRange;
                const float cr = -(by + 0.5f - half) / half * kVectorRange;
                const float y = 0.6f;
                float r = y + 1.5748f * cr;
                float g = y - 0.1873f * cb - 0.4681f * cr;
                float b = y + 1.8556f * cb;
                // Lift towards white so dark hues stay visible on the dark background.
                auto fix = [](float v) { return int(255 * (0.35f + 0.65f * std::clamp(v, 0.0f, 1.0f))); };
                t[size_t(by) * kVectorBins + bx] = qRgb(fix(r), fix(g), fix(b));
            }
        }
        return t;
    }();
    return table;
}

QColor graticuleColor(int alpha) {
    QColor c = theme::kTextDim;
    c.setAlpha(alpha);
    return c;
}

QFont scopeFont(const QFont& base) {
    QFont f = base;
    f.setPixelSize(9);
    return f;
}

}  // namespace

ScopesWidget::ScopesWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(0);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    modeBox_ = new QComboBox(this);
    modeBox_->addItem(tr("Waveform"));
    modeBox_->addItem(tr("RGB Parade"));
    modeBox_->addItem(tr("Vectorscope"));
    modeBox_->addItem(tr("Histogram"));
    modeBox_->addItem(tr("All Four"));
    modeBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    modeBox_->setToolTip(tr("Scope type"));
    row->addWidget(modeBox_);
    row->addStretch(1);
    layout->addLayout(row);
    layout->addStretch(1);
    connect(modeBox_, &QComboBox::currentIndexChanged, this, [this](int i) { setMode(Mode(i)); });

    throttle_.setSingleShot(true);
    connect(&throttle_, &QTimer::timeout, this, &ScopesWidget::analyze);
}

QSize ScopesWidget::sizeHint() const { return {360, 260}; }

QSize ScopesWidget::minimumSizeHint() const { return {160, 120}; }

void ScopesWidget::setMode(Mode m) {
    if (modeBox_->currentIndex() != int(m)) {
        QSignalBlocker block(modeBox_);
        modeBox_->setCurrentIndex(int(m));
    }
    if (m == mode_)
        return;
    mode_ = m;
    computeTrace();
    update();
}

void ScopesWidget::setFrame(const QImage& image, FrameTime t) {
    if (image.isNull()) {
        clear();
        return;
    }
    frameTime_ = t;
    pending_ = image;  // implicitly shared, no copy
    dirty_ = true;
    scheduleAnalysis();
}

void ScopesWidget::clear() {
    throttle_.stop();
    pending_ = QImage();
    small_ = QImage();
    trace_ = QImage();
    traceScaled_ = QImage();
    dirty_ = false;
    haveData_ = false;
    frameTime_ = -1;
    update();
}

void ScopesWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (dirty_)
        scheduleAnalysis();
}

void ScopesWidget::scheduleAnalysis() {
    if (throttle_.isActive())
        return;
    const qint64 interval = 1000 / kAnalysisHz;
    const qint64 elapsed = sinceAnalysis_.isValid() ? sinceAnalysis_.elapsed() : interval;
    throttle_.start(int(std::max<qint64>(0, interval - elapsed)));
}

void ScopesWidget::analyze() {
    if (!dirty_ || !isVisible())
        return;  // a hidden scope catches up in showEvent()
    small_ = downsample(pending_);
    pending_ = QImage();
    dirty_ = false;
    sinceAnalysis_.restart();
    computeTrace();
    update();
}

void ScopesWidget::computeTrace() {
    trace_ = QImage();
    haveData_ = !small_.isNull();
    if (!haveData_)
        return;
    switch (mode_) {
    case Mode::Waveform: analyzeWaveform(false); break;
    case Mode::Parade: analyzeWaveform(true); break;
    case Mode::Vectorscope: analyzeVectorscope(); break;
    case Mode::Histogram: analyzeHistogram(); break;
    case Mode::Quad:
        analyzeWaveform(false);
        quad_[0] = trace_;
        analyzeWaveform(true);
        quad_[1] = trace_;
        analyzeVectorscope();
        quad_[2] = trace_;
        analyzeHistogram();
        break;
    }
}

void ScopesWidget::analyzeWaveform(bool parade) {
    const int w = small_.width(), h = small_.height();
    const int panels = parade ? 3 : 1;
    const int stride = w * panels;
    // Laid out like the trace image: row = 255 - level, column = panel * w + x.
    std::vector<uint32_t> counts(size_t(stride) * 256, 0);
    for (int y = 0; y < h; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(small_.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb c = line[x];
            const int r = qRed(c), g = qGreen(c), b = qBlue(c);
            if (parade) {
                ++counts[size_t(255 - r) * stride + x];
                ++counts[size_t(255 - g) * stride + w + x];
                ++counts[size_t(255 - b) * stride + 2 * w + x];
            } else {
                ++counts[size_t(255 - luma709(r, g, b)) * stride + x];
            }
        }
    }

    // A cell can hold at most h samples (one column); log scaling against a
    // fraction of that keeps sparse detail visible while dense areas saturate.
    static const QColor kLuma{110, 255, 140};
    static const QColor kParade[3] = {{255, 85, 75}, {85, 255, 115}, {95, 145, 255}};
    const float ref = std::log1p(std::max(4.0f, h / 5.0f));
    std::vector<QRgb> lut[3];
    for (int p = 0; p < panels; ++p) {
        lut[p].resize(size_t(h) + 1);
        for (int c = 0; c <= h; ++c)
            lut[p][c] = phosphor(parade ? kParade[p] : kLuma, std::min(1.0f, std::log1p(float(c)) / ref));
    }

    trace_ = QImage(stride, 256, QImage::Format_ARGB32_Premultiplied);
    for (int row = 0; row < 256; ++row) {
        auto* out = reinterpret_cast<QRgb*>(trace_.scanLine(row));
        const uint32_t* in = counts.data() + size_t(row) * stride;
        for (int p = 0; p < panels; ++p) {
            const QRgb* l = lut[p].data();
            for (int x = 0; x < w; ++x)
                out[p * w + x] = l[in[p * w + x]];
        }
    }
}

void ScopesWidget::analyzeVectorscope() {
    const int w = small_.width(), h = small_.height();
    constexpr int n = kVectorBins;
    std::vector<uint32_t> counts(size_t(n) * n, 0);
    const float half = n / 2.0f;
    const float k = half / kVectorRange / 255.0f;
    for (int y = 0; y < h; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(small_.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb c = line[x];
            const CbCr v = cbcr709(float(qRed(c)), float(qGreen(c)), float(qBlue(c)));
            const int bx = int(half + v.cb * k);
            const int by = int(half - v.cr * k);
            if (bx >= 0 && bx < n && by >= 0 && by < n)
                ++counts[size_t(by) * n + bx];
        }
    }

    const std::vector<QRgb>& colors = vectorscopeColors();
    const float ref = std::log1p(std::max(4.0f, float(w) * h / 600.0f));
    trace_ = QImage(n, n, QImage::Format_ARGB32_Premultiplied);
    for (int row = 0; row < n; ++row) {
        auto* out = reinterpret_cast<QRgb*>(trace_.scanLine(row));
        for (int x = 0; x < n; ++x) {
            const size_t i = size_t(row) * n + x;
            const uint32_t c = counts[i];
            if (!c) {
                out[x] = 0;
                continue;
            }
            const float s = std::min(1.0f, 0.15f + 0.85f * std::log1p(float(c)) / ref);
            out[x] = phosphor(QColor(colors[i]), s);
        }
    }
}

void ScopesWidget::analyzeHistogram() {
    for (auto& h : hist_)
        h.fill(0);
    const int w = small_.width(), h = small_.height();
    for (int y = 0; y < h; ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(small_.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb c = line[x];
            const int r = qRed(c), g = qGreen(c), b = qBlue(c);
            ++hist_[0][r];
            ++hist_[1][g];
            ++hist_[2][b];
            ++hist_[3][luma709(r, g, b)];
        }
    }
}

// ---------------------------------------------------------------------------
// Painting

QRect ScopesWidget::scopeArea() const {
    const int top = modeBox_->geometry().bottom() + 6;
    return QRect(4, top, width() - 8, height() - top - 4);
}

const QImage& ScopesWidget::scaledTrace(const QSize& size) {
    if (traceScaledKey_ != trace_.cacheKey() || traceScaled_.size() != size) {
        traceScaled_ = trace_.isNull() || size.isEmpty()
                           ? QImage()
                           : trace_.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        traceScaledKey_ = trace_.cacheKey();
    }
    return traceScaled_;
}

void ScopesWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), theme::kPanel);
    const QRect area = scopeArea();
    if (area.width() < 40 || area.height() < 40)
        return;
    p.setFont(scopeFont(font()));
    switch (mode_) {
    case Mode::Waveform: paintWaveform(p, area, false); break;
    case Mode::Parade: paintWaveform(p, area, true); break;
    case Mode::Vectorscope: paintVectorscope(p, area); break;
    case Mode::Histogram: paintHistogram(p, area); break;
    case Mode::Quad: {
        // Waveform and parade above, vectorscope and histogram below.
        const int w2 = area.width() / 2, h2 = area.height() / 2;
        const QRect cells[4] = {QRect(area.left(), area.top(), w2 - 3, h2 - 3), QRect(area.left() + w2 + 3, area.top(), area.width() - w2 - 3, h2 - 3),
                                QRect(area.left(), area.top() + h2 + 3, w2 - 3, area.height() - h2 - 3),
                                QRect(area.left() + w2 + 3, area.top() + h2 + 3, area.width() - w2 - 3, area.height() - h2 - 3)};
        const QImage single = trace_;
        for (int i = 0; i < 3; ++i) {
            trace_ = quad_[size_t(i)];
            if (i == 2) paintVectorscope(p, cells[2]);
            else paintWaveform(p, cells[i], i == 1);
        }
        trace_ = single;
        paintHistogram(p, cells[3]);
        break;
    }
    }
    if (!haveData_) {
        p.setPen(theme::kTextDim);
        p.drawText(area, Qt::AlignCenter, tr("No signal"));
    }
}

void ScopesWidget::paintLevelGraticule(QPainter& p, const QRectF& plot, bool labels) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    const QFontMetrics fm(p.font());
    for (int pct = 0; pct <= 100; pct += 25) {
        const double y = std::round(plot.bottom() - plot.height() * pct / 100.0) + 0.5;
        QPen pen(graticuleColor(pct % 50 == 0 ? 95 : 60));
        if (pct % 50 != 0)
            pen.setStyle(Qt::DotLine);
        p.setPen(pen);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        if (labels) {
            p.setPen(theme::kTextDim);
            const QRectF box(plot.left() - 40, y - fm.height() / 2.0, 36, fm.height());
            p.drawText(box, Qt::AlignRight | Qt::AlignVCenter, QString::number(pct));
        }
    }
    p.restore();
}

void ScopesWidget::paintWaveform(QPainter& p, const QRect& area, bool parade) {
    const QFontMetrics fm(p.font());
    const int labelW = fm.horizontalAdvance(QStringLiteral("100")) + 6;
    const int pad = fm.height() / 2 + 1;
    const QRectF plot(area.left() + labelW, area.top() + pad, area.width() - labelW - 2, area.height() - 2 * pad);
    if (plot.width() < 10 || plot.height() < 10)
        return;

    const int panels = parade ? 3 : 1;
    const int gap = parade ? 6 : 0;
    const int panelW = (int(plot.width()) - gap * (panels - 1)) / panels;
    // Each level is a bin centred on its graticule position.
    const double binH = plot.height() / 255.0;
    // Pre-scaled with area averaging: drawImage() would point-sample a shrinking trace into dotted lines.
    const QImage scaled = haveData_ ? scaledTrace(QSize(panelW * panels, int(std::lround(binH * 256)))) : QImage();

    static const QColor kPanelTint[3] = {{255, 90, 80}, {90, 230, 120}, {100, 150, 255}};
    for (int i = 0; i < panels; ++i) {
        const QRectF panel(plot.left() + i * (panelW + gap), plot.top(), panelW, plot.height());
        p.fillRect(panel, theme::kWindow);
        if (!scaled.isNull()) {
            p.save();
            p.setClipRect(panel);
            p.setCompositionMode(QPainter::CompositionMode_Plus);
            p.drawImage(QPointF(panel.left(), std::round(panel.top() - binH / 2)), scaled,
                        QRect(i * panelW, 0, panelW, scaled.height()));
            p.restore();
        }
        paintLevelGraticule(p, panel, i == 0);
        if (parade) {
            QColor c = kPanelTint[i];
            c.setAlpha(170);
            p.setPen(c);
            static const char* kNames[3] = {"R", "G", "B"};
            p.drawText(panel.adjusted(4, 2, -4, 0), Qt::AlignRight | Qt::AlignTop, QString::fromLatin1(kNames[i]));
        }
    }
}

void ScopesWidget::paintVectorscope(QPainter& p, const QRect& area) {
    const QFontMetrics fm(p.font());
    const double labelPad = fm.height() + 4;
    const int side = int(std::min(area.width(), area.height()) - 2 * labelPad);
    if (side < 30)
        return;
    const QPointF c(std::round(area.left() + area.width() / 2.0), std::round(area.top() + area.height() / 2.0));
    const double r = side / 2.0;
    const QPointF topLeft(c.x() - side / 2, c.y() - side / 2);

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::kWindow);
    p.drawEllipse(c, r, r);

    if (haveData_ && !trace_.isNull()) {
        p.save();
        QPainterPath disc;
        disc.addEllipse(c, r, r);
        p.setClipPath(disc);
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        p.drawImage(topLeft, scaledTrace(QSize(side, side)));
        p.restore();
    }

    // Graticule: rings, cross hair and 10-degree ticks.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(graticuleColor(120), 1));
    p.drawEllipse(c, r, r);
    p.setPen(QPen(graticuleColor(50), 1, Qt::DotLine));
    p.drawEllipse(c, r * 0.5, r * 0.5);
    p.setPen(QPen(graticuleColor(60), 1));
    p.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - r), QPointF(c.x(), c.y() + r));
    p.setPen(QPen(graticuleColor(100), 1));
    for (int deg = 0; deg < 360; deg += 10) {
        const double a = deg * kPi / 180.0;
        const double len = deg % 30 == 0 ? std::max(4.0, r * 0.06) : std::max(2.0, r * 0.03);
        const QPointF dir(std::cos(a), -std::sin(a));
        p.drawLine(c + dir * r, c + dir * (r - len));
    }

    // Skin-tone (flesh) line.
    {
        const double a = 123.0 * kPi / 180.0;
        const QPointF dir(std::cos(a), -std::sin(a));
        p.setPen(QPen(QColor(230, 175, 135, 150), 1, Qt::DashLine));
        p.drawLine(c, c + dir * r);
    }

    // 75% colour targets.
    struct Target {
        const char* name;
        float r, g, b;
    };
    static const Target kTargets[] = {{"R", 1, 0, 0}, {"Mg", 1, 0, 1}, {"B", 0, 0, 1},
                                      {"Cy", 0, 1, 1}, {"G", 0, 1, 0}, {"Yl", 1, 1, 0}};
    const double box = std::clamp(r * 0.08, 5.0, 14.0);
    for (const Target& t : kTargets) {
        const CbCr v = cbcr709(0.75f * t.r, 0.75f * t.g, 0.75f * t.b);
        const QPointF pos(c.x() + v.cb / kVectorRange * r, c.y() - v.cr / kVectorRange * r);
        QColor col = QColor::fromRgbF(0.35f + 0.65f * t.r, 0.35f + 0.65f * t.g, 0.35f + 0.65f * t.b);
        col.setAlpha(190);
        p.setPen(QPen(col, 1));
        p.drawRect(QRectF(pos.x() - box / 2, pos.y() - box / 2, box, box));
        // Label just outside the box, pointing away from the centre.
        QPointF dir = pos - c;
        const double len = std::hypot(dir.x(), dir.y());
        if (len > 0)
            dir /= len;
        const QPointF lp = pos + dir * (box / 2 + fm.height() * 0.75);
        p.setPen(theme::kTextDim);
        p.drawText(QRectF(lp.x() - 12, lp.y() - fm.height() / 2.0, 24, fm.height()), Qt::AlignCenter,
                   QString::fromLatin1(t.name));
    }
    p.restore();
}

void ScopesWidget::paintHistogram(QPainter& p, const QRect& area) {
    const QFontMetrics fm(p.font());
    const QRectF plot(area.left() + 8, area.top() + 4, area.width() - 16, area.height() - fm.height() - 8);
    if (plot.width() < 10 || plot.height() < 10)
        return;
    p.fillRect(plot, theme::kWindow);

    // Vertical graticule at 0/25/50/75/100%.
    p.save();
    for (int pct = 0; pct <= 100; pct += 25) {
        const double x = std::round(plot.left() + plot.width() * pct / 100.0) + 0.5;
        QPen pen(graticuleColor(pct % 50 == 0 ? 95 : 60));
        if (pct % 50 != 0)
            pen.setStyle(Qt::DotLine);
        p.setPen(pen);
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setPen(theme::kTextDim);
        p.drawText(QRectF(x - 20, plot.bottom() + 2, 40, fm.height()), Qt::AlignHCenter | Qt::AlignTop,
                   QString::number(pct));
    }
    p.restore();

    if (!haveData_)
        return;

    // Scale to the tallest bin, ignoring the clipped extremes so one spike does not flatten the rest.
    uint32_t peak = 1;
    for (const auto& h : hist_)
        for (int i = 1; i < 255; ++i)
            peak = std::max(peak, h[i]);

    struct Channel {
        int index;
        QColor color;
    };
    static const Channel kOrder[] = {{3, {200, 200, 200}}, {0, {235, 70, 70}}, {1, {70, 210, 100}}, {2, {80, 130, 255}}};

    p.save();
    p.setClipRect(plot);
    p.setRenderHint(QPainter::Antialiasing);
    for (const Channel& ch : kOrder) {
        const auto& h = hist_[ch.index];
        QPainterPath line;
        for (int i = 0; i < 256; ++i) {
            const double x = plot.left() + (i + 0.5) / 256.0 * plot.width();
            const double y = plot.bottom() - std::min(1.0, double(h[i]) / peak) * plot.height();
            if (i == 0)
                line.moveTo(x, y);
            else
                line.lineTo(x, y);
        }
        QPainterPath fill = line;
        fill.lineTo(plot.right(), plot.bottom());
        fill.lineTo(plot.left(), plot.bottom());
        fill.closeSubpath();

        QColor fc = ch.color;
        fc.setAlpha(ch.index == 3 ? 45 : 70);
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        p.fillPath(fill, fc);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        QColor lc = ch.color;
        lc.setAlpha(ch.index == 3 ? 150 : 210);
        p.setPen(QPen(lc, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);
    }
    p.restore();
}

}  // namespace montage
