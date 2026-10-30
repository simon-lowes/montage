// Montage — video scopes widget.
#include "ScopesWidget.h"
#include "Language.h"

#include "Accessibility.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <vector>

#include "Theme.h"
#include "render/ColorSpace.h"
#include "render/Gamut.h"

namespace montage {

namespace {

constexpr int kAnalysisHz = 15;
constexpr int kMaxAnalysisWidth = 480;
constexpr int kMaxAnalysisHeight = 320;
constexpr int kVectorBins = 256;
constexpr float kVectorRange = 0.6f;  // Cb/Cr magnitude at the edge of the vectorscope circle
constexpr double kPi = 3.14159265358979323846;
constexpr int kCieBins = 256;
// The chromaticity scope's square, from 0 on both axes: the whole spectral locus fits.
double cieRange(bool uv) { return uv ? 0.7 : 0.9; }

struct CbCr {
    float cb, cr;  // -0.5..0.5
};

inline CbCr cbcr709(float r, float g, float b) {
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return {(b - y) / 1.8556f, (r - y) / 1.5748f};
}

// The frame at analysis size, in its own format (16-bit frames keep their precision).
QImage pointSample(const QImage& src) {
    QSize size = src.size();
    if (size.width() > kMaxAnalysisWidth || size.height() > kMaxAnalysisHeight)
        size = size.scaled(kMaxAnalysisWidth, kMaxAnalysisHeight, Qt::KeepAspectRatio);
    size = size.expandedTo(QSize(1, 1));
    // Point sampling keeps real pixel values (smoothing would hide outliers).
    return size == src.size() ? src : src.scaled(size, Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

// Code values 0..1 of a frame (any format) for the light meter.
Image toCodeValues(const QImage& src) {
    const QImage img = src.convertToFormat(QImage::Format_RGBA64);
    Image out(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y) {
        const auto* in = reinterpret_cast<const quint16*>(img.constScanLine(y));
        float* o = out.row(y);
        for (int x = 0; x < img.width() * 4; ++x) o[x] = in[x] / 65535.0f;
    }
    return out;
}

// The scale's labels: whole nits, thousands as K.
QString nitsLabel(double n) { return n >= 1000 ? QStringLiteral("%1K").arg(n / 1000) : QString::number(n); }

// Phosphor-like shade: tint scales with density, the densest areas bloom towards white.
// Returns a premultiplied ARGB value.
QRgb phosphor(const QColor& base, float s) {
    const float hot = std::clamp((s - 0.8f) / 0.2f, 0.0f, 1.0f) * 0.35f;
    auto ch = [&](int v) { return int((v + (255 - v) * hot) * s); };
    return qRgba(ch(base.red()), ch(base.green()), ch(base.blue()), int(255 * s));
}

// The colour of every chromaticity bin (its xy or u'v' shown in Rec.709 at full brightness, lifted like the
// vectorscope's), so the trace shows the colours it plots.
const std::vector<QRgb>& chromaticityColors(bool uv) {
    static const std::array<std::vector<QRgb>, 2> tables = [] {
        double fromXyz[9], toXyz[9];
        primariesToXyz(Primaries::Bt709, toXyz);
        // (the inverse, by hand)
        const double* m = toXyz;
        const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
        fromXyz[0] = (m[4] * m[8] - m[5] * m[7]) / det, fromXyz[1] = (m[2] * m[7] - m[1] * m[8]) / det, fromXyz[2] = (m[1] * m[5] - m[2] * m[4]) / det;
        fromXyz[3] = (m[5] * m[6] - m[3] * m[8]) / det, fromXyz[4] = (m[0] * m[8] - m[2] * m[6]) / det, fromXyz[5] = (m[2] * m[3] - m[0] * m[5]) / det;
        fromXyz[6] = (m[3] * m[7] - m[4] * m[6]) / det, fromXyz[7] = (m[1] * m[6] - m[0] * m[7]) / det, fromXyz[8] = (m[0] * m[4] - m[1] * m[3]) / det;
        std::vector<QRgb> out[2];
        for (int t = 0; t < 2; ++t) {
            const double range = cieRange(t == 1);
            out[t].resize(size_t(kCieBins) * kCieBins);
            for (int by = 0; by < kCieBins; ++by)
                for (int bx = 0; bx < kCieBins; ++bx) {
                    double a = (bx + 0.5) / kCieBins * range, b = (1 - (by + 0.5) / kCieBins) * range;
                    double x = a, y = b;
                    if (t == 1) {  // u'v' back to xy
                        const double d = 6 * a - 16 * b + 12;
                        x = 9 * a / d, y = 4 * b / d;
                    }
                    double rgb[3] = {0.5, 0.5, 0.5};
                    if (y > 1e-6) {
                        const double X = x / y, Z = (1 - x - y) / y;
                        for (int i = 0; i < 3; ++i) rgb[i] = fromXyz[i * 3] * X + fromXyz[i * 3 + 1] + fromXyz[i * 3 + 2] * Z;
                        const double mx = std::max({rgb[0], rgb[1], rgb[2], 1e-6});
                        for (double& v : rgb) v = std::pow(std::clamp(v / mx, 0.0, 1.0), 1 / 2.2);
                    }
                    auto fix = [](double v) { return int(255 * (0.35 + 0.65 * v)); };
                    out[t][size_t(by) * kCieBins + bx] = qRgb(fix(rgb[0]), fix(rgb[1]), fix(rgb[2]));
                }
        }
        return std::array<std::vector<QRgb>, 2>{out[0], out[1]};
    }();
    return tables[uv ? 1u : 0u];
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
    access::describe(this, {QAccessible::Graphic, [this] { return tr("Scopes"); }, {}, [this] { return summary(); }});

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
    modeBox_->addItem(tr("CIE Chromaticity"));
    modeBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    modeBox_->setToolTip(tr("Scope type"));
    row->addWidget(modeBox_);
    // What is read: the output, or the working space's values (graded scene-referred).
    readBox_ = new QComboBox(this);
    readBox_->setObjectName(QStringLiteral("scopeRead"));
    readBox_->addItem(tr("Output"));
    readBox_->addItem(tr("Working Space"));
    readBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    readBox_->setToolTip(tr("Measure the picture as delivered, or (in a sequence graded scene-referred) the values its grade\n"
                            "works on, in its working space; read when the playhead stops"));
    row->addWidget(readBox_);
    connect(readBox_, &QComboBox::currentIndexChanged, this, [this](int i) { setReadsWorking(i == 1); });
    // HDR: the waveform's top in nits, and the frame's light.
    rangeBox_ = new QComboBox(this);
    rangeBox_->setObjectName(QStringLiteral("scopeRange"));
    for (int n : {10000, 4000, 2000, 1000}) rangeBox_->addItem(tr("%1 nits").arg(QLocale().toString(n)), n);
    rangeBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    rangeBox_->setToolTip(tr("The top of the waveform and parade"));
    rangeBox_->hide();
    row->addWidget(rangeBox_);
    // The chromaticity scope's diagram.
    cieBox_ = new QComboBox(this);
    cieBox_->setObjectName(QStringLiteral("scopeCie"));
    cieBox_->addItem(tr("CIE 1931 xy"));
    cieBox_->addItem(tr("CIE 1976 u′v′"));
    cieBox_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    cieBox_->setToolTip(tr("The chromaticity diagram: 1931 xy, or 1976 u'v' (its distances closer to how different colours look)"));
    cieBox_->hide();
    row->addWidget(cieBox_);
    connect(cieBox_, &QComboBox::currentIndexChanged, this, [this](int i) { setCieUv(i == 1); });
    connect(rangeBox_, &QComboBox::currentIndexChanged, this, [this](int i) { setNitsRange(rangeBox_->itemData(i).toDouble()); });
    row->addStretch(1);
    nitsLabel_ = new QLabel(this);
    nitsLabel_->setObjectName(QStringLiteral("scopeNits"));
    nitsLabel_->hide();
    row->addWidget(nitsLabel_);
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
    cieBox_->setVisible(m == Mode::Chromaticity);
    if (m == mode_)
        return;
    mode_ = m;
    computeTrace();
    update();
}

void ScopesWidget::setCieUv(bool uv) {
    if (cieBox_->currentIndex() != (uv ? 1 : 0)) {
        QSignalBlocker block(cieBox_);
        cieBox_->setCurrentIndex(uv ? 1 : 0);
    }
    if (uv == cieUv_) return;
    cieUv_ = uv;
    computeTrace();
    update();
}

void ScopesWidget::setPrimaryMoves(const std::vector<std::pair<QPointF, QPointF>>& xy) {
    if (xy == primaryMoves_) return;
    primaryMoves_ = xy;
    if (mode_ == Mode::Chromaticity) update();
}

int ScopesWidget::cieSamplesNear(double a, double b, double radius) const {
    if (cieCounts_.empty()) return 0;
    const double range = cieRange(cieUv_);
    int n = 0;
    for (int by = 0; by < kCieBins; ++by)
        for (int bx = 0; bx < kCieBins; ++bx) {
            const double ca = (bx + 0.5) / kCieBins * range, cb = (1 - (by + 0.5) / kCieBins) * range;
            if (std::hypot(ca - a, cb - b) <= radius) n += int(cieCounts_[size_t(by) * kCieBins + bx]);
        }
    return n;
}

void ScopesWidget::setReadsWorking(bool on) {
    if (readBox_->currentIndex() != (on ? 1 : 0)) {
        QSignalBlocker block(readBox_);
        readBox_->setCurrentIndex(on ? 1 : 0);
    }
    if (readsWorking_ == on) return;
    readsWorking_ = on;
    emit readWorkingChanged(on);
}

void ScopesWidget::setFrame(const QImage& image, FrameTime t) { setSignal(image, t, QStringLiteral("rec709"), 1000); }

void ScopesWidget::setSignal(const QImage& image, FrameTime t, const QString& space, double masteringPeakNits) {
    if (image.isNull()) {
        clear();
        return;
    }
    frameTime_ = t;
    pending_ = image;  // implicitly shared, no copy
    pendingSpace_ = space;
    pendingPeak_ = masteringPeakNits;
    dirty_ = true;
    scheduleAnalysis();
}

bool ScopesWidget::hdr() const {
    const ColorSpace* cs = findColorSpace(space_.toStdString());
    return cs && cs->hdr();
}

void ScopesWidget::setNitsRange(double nits) {
    nitsTop_ = std::clamp(nits, 100.0, 10000.0);
    const int i = rangeBox_->findData(int(std::lround(nitsTop_)));
    if (i >= 0 && i != rangeBox_->currentIndex()) {
        QSignalBlocker block(rangeBox_);
        rangeBox_->setCurrentIndex(i);
    }
    computeTrace();
    update();
}

void ScopesWidget::updateHdrControls() {
    const ColorSpace* cs = findColorSpace(space_.toStdString());
    const bool h = cs && cs->hdr();
    rangeBox_->setVisible(h && cs->transfer == Transfer::Pq);
    nitsLabel_->setVisible(h && haveData_);
    if (!h || !haveData_) return;
    const QLocale loc;
    nitsLabel_->setText(tr("Peak %1 · Avg %2 nits").arg(loc.toString(qRound(peakNits_)), loc.toString(qRound(averageNits_))));
    const bool over = cs->transfer == Transfer::Pq && peakNits_ > masterPeak_ + 0.5;
    nitsLabel_->setStyleSheet(over ? QStringLiteral("color: #ff6b5e;") : QString());
    nitsLabel_->setToolTip(over ? tr("Brighter than the %1-nit mastering peak").arg(loc.toString(qRound(masterPeak_)))
                                : tr("The frame's brightest pixel and its average light (MaxCLL and MaxFALL measures)"));
}

void ScopesWidget::clear() {
    throttle_.stop();
    pending_ = QImage();
    small_ = QImage();
    codes_ = Image();
    cieCounts_.clear();
    outside709_ = outsideP3_ = -1;
    trace_ = QImage();
    traceScaled_ = QImage();
    dirty_ = false;
    haveData_ = false;
    frameTime_ = -1;
    peakNits_ = averageNits_ = -1;
    updateHdrControls();
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
    const QImage sampled = pointSample(pending_);
    if (!meter_ || pendingSpace_ != space_) {
        space_ = pendingSpace_;
        const ColorSpace* cs = findColorSpace(space_.toStdString());
        if (!cs) cs = &rec709Space();
        meter_ = cs->hdr() ? std::make_unique<LightMeter>(*cs) : nullptr;
        // Luma as the space weighs its primaries (Rec.709 and BT.2020 differ).
        double m[9];
        primariesToXyz(cs->primaries, m);
        lumaR_ = int(std::lround(m[3] * 65536)), lumaG_ = int(std::lround(m[4] * 65536)), lumaB_ = 65536 - lumaR_ - lumaG_;
    }
    masterPeak_ = pendingPeak_;
    peakNits_ = averageNits_ = -1;
    codes_ = toCodeValues(sampled);
    if (meter_) meter_->measure(codes_, peakNits_, averageNits_);
    small_ = sampled.convertToFormat(QImage::Format_RGB32);
    pending_ = QImage();
    dirty_ = false;
    sinceAnalysis_.restart();
    computeTrace();
    updateHdrControls();
    update();
}

QString ScopesWidget::summary() const {
    if (!haveData_ || small_.isNull()) return tr("No picture");
    std::array<uint64_t, 256> hist{};
    double cb = 0, cr = 0, saturation = 0, sum = 0;
    const int n = small_.width() * small_.height();
    for (int y = 0; y < small_.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(small_.constScanLine(y));
        for (int x = 0; x < small_.width(); ++x) {
            const int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
            const int l = std::clamp(luma(r, g, b), 0, 255);
            ++hist[size_t(l)];
            sum += l;
            const double pb = (b - l) / 1.8556 / 255, pr = (r - l) / 1.5748 / 255;
            cb += pb, cr += pr;
            saturation += std::hypot(pb, pr);
        }
    }
    if (n <= 0) return tr("No picture");
    auto percentile = [&](double share) {
        uint64_t seen = 0;
        for (int i = 0; i < 256; ++i)
            if ((seen += hist[size_t(i)]) >= uint64_t(share * n)) return i;
        return 255;
    };
    auto percent = [](double code) { return int(std::lround(code / 2.55)); };
    QString text = tr("%1: levels %2 % to %3 %, average %4 %; saturation %5 %")
                       .arg(modeBox_ ? modeBox_->currentText() : tr("Scopes"))
                       .arg(percent(percentile(0.02)))
                       .arg(percent(percentile(0.98)))
                       .arg(percent(sum / n))
                       .arg(int(std::lround(saturation / n * 200)));  // (0.5, a primary, as 100 %)
    // A cast: the average colour's hue, when it is far enough from grey.
    cb /= n, cr /= n;
    if (std::hypot(cb, cr) > 0.02) {
        static const std::pair<double, const char*> kHues[] = {{103, QT_TR_NOOP("red")},   {175, QT_TR_NOOP("yellow")},
                                                                {230, QT_TR_NOOP("green")}, {283, QT_TR_NOOP("cyan")},
                                                                {355, QT_TR_NOOP("blue")},  {50, QT_TR_NOOP("magenta")}};
        const double angle = std::fmod(std::atan2(cr, cb) * 180 / M_PI + 360, 360);
        const char* hue = kHues[0].second;
        double best = 360;
        for (const auto& [at, name] : kHues)
            if (const double d = std::min(std::fabs(angle - at), 360 - std::fabs(angle - at)); d < best) best = d, hue = name;
        text += tr(", leaning %1").arg(tr(hue));
    } else {
        text += tr(", neutral");
    }
    if (peakNits_ > 0) text += tr("; peak %1 nits, average %2 nits").arg(std::lround(peakNits_)).arg(std::lround(averageNits_));
    if (mode_ == Mode::Chromaticity && outside709_ >= 0)
        text += tr("; %1 % outside Rec.709, %2 % outside P3").arg(std::lround(outside709_ * 100)).arg(std::lround(outsideP3_ * 100));
    return text;
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
    case Mode::Chromaticity: analyzeChromaticity(); break;
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
                ++counts[size_t(255 - luma(r, g, b)) * stride + x];
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
    // PQ shown to a lower top: only the levels up to it (brighter ones are off the scale).
    const ColorSpace* cs = findColorSpace(space_.toStdString());
    if (cs && cs->transfer == Transfer::Pq && nitsTop_ < 10000) {
        const int top = std::clamp(int(std::lround(nitsToCode(*cs, nitsTop_) * 255)), 1, 255);
        trace_ = trace_.copy(0, 255 - top, stride, top + 1);
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
            ++hist_[3][luma(r, g, b)];
        }
    }
}

void ScopesWidget::analyzeChromaticity() {
    const ColorSpace* cs = findColorSpace(space_.toStdString());
    if (!cs) cs = &rec709Space();
    const bool uv = cieUv_;
    const double range = cieRange(uv);
    cieCounts_.assign(size_t(kCieBins) * kCieBins, 0);
    const GamutCheck in709(*cs, Primaries::Bt709), inP3(*cs, Primaries::P3D65);
    // (The codes came from 16-bit pictures: their light by table.)
    const std::vector<float>& lin = linearTable16(cs->transfer);
    double toXyz[9];
    primariesToXyz(cs->primaries, toXyz);
    int lit = 0, out709 = 0, outP3 = 0;
    for (int y = 0; y < codes_.height; ++y) {
        const float* row = codes_.row(y);
        for (int x = 0; x < codes_.width; ++x) {
            const float* px = row + x * 4;
            // (Near black a colour's chromaticity is noise: left out.)
            if (std::max({px[0], px[1], px[2]}) < 0.02f) continue;
            double l[3], xyz[3];
            for (int i = 0; i < 3; ++i) l[i] = lin[size_t(std::clamp(int(px[i] * 65535.0f + 0.5f), 0, 65535))];
            for (int r = 0; r < 3; ++r) xyz[r] = toXyz[r * 3] * l[0] + toXyz[r * 3 + 1] * l[1] + toXyz[r * 3 + 2] * l[2];
            const double sum = xyz[0] + xyz[1] + xyz[2];
            if (!(sum > 1e-9)) continue;
            const Xy c{xyz[0] / sum, xyz[1] / sum};
            ++lit;
            out709 += in709.possible() && in709.outsideLinear(l);
            outP3 += inP3.possible() && inP3.outsideLinear(l);
            const Xy at = uv ? xyToUv(c) : c;
            const int bx = int(at.x / range * kCieBins), by = int((1 - at.y / range) * kCieBins);
            if (bx >= 0 && bx < kCieBins && by >= 0 && by < kCieBins) ++cieCounts_[size_t(by) * kCieBins + bx];
        }
    }
    outside709_ = lit ? double(out709) / lit : 0;
    outsideP3_ = lit ? double(outP3) / lit : 0;
    const std::vector<QRgb>& colors = chromaticityColors(uv);
    const float ref = std::log1p(std::max(4.0f, float(codes_.width) * codes_.height / 600.0f));
    trace_ = QImage(kCieBins, kCieBins, QImage::Format_ARGB32_Premultiplied);
    for (int row = 0; row < kCieBins; ++row) {
        auto* out = reinterpret_cast<QRgb*>(trace_.scanLine(row));
        for (int x = 0; x < kCieBins; ++x) {
            const size_t i = size_t(row) * kCieBins + x;
            const uint32_t c = cieCounts_[i];
            out[x] = c ? phosphor(QColor(colors[i]), std::min(1.0f, 0.2f + 0.8f * std::log1p(float(c)) / ref)) : 0;
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
    case Mode::Chromaticity: paintChromaticity(p, area); break;
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

std::vector<std::pair<double, double>> ScopesWidget::nitsMarks(bool fullRange, double& peakAt) const {
    std::vector<std::pair<double, double>> marks;
    peakAt = -1;
    const ColorSpace* cs = findColorSpace(space_.toStdString());
    if (!cs || !cs->hdr()) return marks;
    const bool pq = cs->transfer == Transfer::Pq;
    const double top = pq && !fullRange ? nitsToCode(*cs, nitsTop_) : 1.0;
    for (double n : {0.0, 10.0, 100.0, 203.0, 1000.0, 4000.0, 10000.0}) {
        const double at = nitsToCode(*cs, n) / top;
        if (at <= 1 + 1e-6 && (pq || n <= 1000)) marks.push_back({n, std::min(at, 1.0)});
    }
    if (pq && masterPeak_ < (fullRange ? 10000 : nitsTop_) - 0.5) peakAt = nitsToCode(*cs, masterPeak_) / top;
    return marks;
}

void ScopesWidget::paintLevelGraticule(QPainter& p, const QRectF& plot, bool labels) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    const QFontMetrics fm(p.font());
    double peakAt = -1;
    const auto marks = nitsMarks(false, peakAt);
    if (!marks.empty()) {
        // HDR: the levels in nits where they fall on the signal, reference white (203) and the mastering peak marked.
        for (const auto& [nits, at] : marks) {
            const double y = std::round(plot.bottom() - plot.height() * at) + 0.5;
            QPen pen(graticuleColor(nits == 203 ? 110 : 70));
            if (nits != 0 && nits != 203 && nits != 1000 && nits != 10000) pen.setStyle(Qt::DotLine);
            p.setPen(pen);
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            if (labels) {
                p.setPen(theme::kTextDim);
                p.drawText(QRectF(plot.left() - 40, y - fm.height() / 2.0, 36, fm.height()), Qt::AlignRight | Qt::AlignVCenter, nitsLabel(nits));
            }
        }
        if (peakAt > 0) {
            const double y = std::round(plot.bottom() - plot.height() * peakAt) + 0.5;
            p.setPen(QPen(QColor(255, 150, 60, 170), 1, Qt::DashLine));
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
        p.restore();
        return;
    }
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
    // Each level is a bin centred on its graticule position (an HDR range shows fewer than all 256).
    const int rows = trace_.isNull() ? 256 : std::max(2, trace_.height());
    const double binH = plot.height() / double(rows - 1);
    // Pre-scaled with area averaging: drawImage() would point-sample a shrinking trace into dotted lines.
    const QImage scaled = haveData_ ? scaledTrace(QSize(panelW * panels, int(std::lround(binH * rows)))) : QImage();

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

    // Vertical graticule at 0/25/50/75/100% (HDR: at the nits marks over the whole signal).
    p.save();
    double peakAt = -1;
    for (const auto& [nits, at] : nitsMarks(true, peakAt)) {
        const double x = std::round(plot.left() + plot.width() * at) + 0.5;
        p.setPen(QPen(graticuleColor(nits == 203 ? 110 : 70), 1, nits == 0 || nits == 203 || nits == 1000 || nits == 10000 ? Qt::SolidLine : Qt::DotLine));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setPen(theme::kTextDim);
        p.drawText(QRectF(x - 20, plot.bottom() + 2, 40, fm.height()), Qt::AlignHCenter | Qt::AlignTop, nitsLabel(nits));
    }
    for (int pct = 0; pct <= 100 && !hdr(); pct += 25) {
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

void ScopesWidget::paintChromaticity(QPainter& p, const QRect& area) {
    const QFontMetrics fm(p.font());
    const int labelW = fm.horizontalAdvance(QStringLiteral("0.0")) + 6;
    const int side = std::min(area.width() - labelW - 4, area.height() - fm.height() - 6);
    if (side < 40) return;
    const QRectF plot(area.left() + labelW + (area.width() - labelW - side) / 2.0, area.top() + 2, side, side);
    const bool uv = cieUv_;
    const double range = cieRange(uv);
    auto at = [&](Xy c) {
        const Xy d = uv ? xyToUv(c) : c;
        return QPointF(plot.left() + d.x / range * plot.width(), plot.bottom() - d.y / range * plot.height());
    };
    p.save();
    p.fillRect(plot, theme::kWindow);
    // Grid a tenth apart, labelled.
    p.setPen(QPen(graticuleColor(45), 1, Qt::DotLine));
    for (int i = 1; i * 0.1 < range + 1e-9; ++i) {
        const double f = i * 0.1 / range;
        p.drawLine(QPointF(plot.left() + f * plot.width(), plot.top()), QPointF(plot.left() + f * plot.width(), plot.bottom()));
        p.drawLine(QPointF(plot.left(), plot.bottom() - f * plot.height()), QPointF(plot.right(), plot.bottom() - f * plot.height()));
    }
    p.setPen(theme::kTextDim);
    for (int i = 0; i * 0.1 < range + 1e-9; i += 2) {
        const double f = i * 0.1 / range;
        const QString label = QString::number(i * 0.1, 'f', 1);
        p.drawText(QRectF(plot.left() - labelW - 2, plot.bottom() - f * plot.height() - fm.height() / 2.0, labelW - 2, fm.height()),
                   Qt::AlignRight | Qt::AlignVCenter, label);
        p.drawText(QRectF(plot.left() + f * plot.width() - 20, plot.bottom() + 2, 40, fm.height()), Qt::AlignHCenter | Qt::AlignTop, label);
    }
    p.drawText(QRectF(plot.right() - 40, plot.bottom() - fm.height() - 2, 38, fm.height()), Qt::AlignRight | Qt::AlignBottom,
               uv ? QStringLiteral("u′") : QStringLiteral("x"));
    p.drawText(QRectF(plot.left() + 3, plot.top() + 2, 40, fm.height()), Qt::AlignLeft | Qt::AlignTop, uv ? QStringLiteral("v′") : QStringLiteral("y"));

    p.setRenderHint(QPainter::Antialiasing);
    // The spectral locus, closed by the line of purples.
    QPainterPath locus;
    const std::vector<Xy>& points = spectralLocus();
    for (size_t i = 0; i < points.size(); ++i) {
        if (i == 0) locus.moveTo(at(points[i]));
        else locus.lineTo(at(points[i]));
    }
    locus.closeSubpath();
    p.fillPath(locus, QColor(255, 255, 255, 10));
    p.setPen(QPen(graticuleColor(140), 1));
    p.drawPath(locus);
    // Wavelengths along it.
    p.setPen(theme::kTextDim);
    for (int nm : {460, 480, 500, 520, 540, 560, 580, 600, 620}) {
        const QPointF pt = at(points[size_t(nm - 380) / 5]);
        const QPointF c = at(Xy{0.3127, 0.3290});
        QPointF dir = pt - c;
        const double len = std::hypot(dir.x(), dir.y());
        if (len > 0) dir /= len;
        p.drawLine(pt, pt + dir * 3);
        const QPointF lp = pt + dir * (fm.height() * 0.9);
        p.drawText(QRectF(lp.x() - 14, lp.y() - fm.height() / 2.0, 28, fm.height()), Qt::AlignCenter, QString::number(nm));
    }

    if (haveData_ && !trace_.isNull()) {
        p.save();
        p.setClipRect(plot);
        p.setCompositionMode(QPainter::CompositionMode_Plus);
        p.drawImage(plot, scaledTrace(plot.size().toSize()));
        p.restore();
    }

    // The gamuts: Rec.709, P3 and Rec.2020, and the signal's own when it is another.
    struct Gamut {
        Primaries primaries;
        QString name;
        QColor color;
    };
    std::vector<Gamut> gamuts{{Primaries::Bt709, tr("Rec.709"), QColor(235, 235, 235, 200)},
                              {Primaries::P3D65, tr("P3"), QColor(255, 180, 70, 200)},
                              {Primaries::Bt2020, tr("Rec.2020"), QColor(90, 200, 255, 200)}};
    const ColorSpace* cs = findColorSpace(space_.toStdString());
    if (cs && cs->primaries != Primaries::Bt709 && cs->primaries != Primaries::P3D65 && cs->primaries != Primaries::Bt2020)
        gamuts.push_back({cs->primaries, language::engine(cs->label), QColor(200, 130, 255, 200)});
    for (size_t g = 0; g < gamuts.size(); ++g) {
        const auto c = gamutChromaticities(gamuts[g].primaries);
        QPolygonF tri;
        for (int i = 0; i < 3; ++i) tri << at(c[size_t(i)]);
        p.setPen(QPen(gamuts[g].color, 1, g == 3 ? Qt::DashLine : Qt::SolidLine));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(tri);
        // Named beside its red corner, one below another.
        p.setPen(gamuts[g].color);
        const QPointF red = tri[0];
        p.drawText(QRectF(red.x() + 4, red.y() - fm.height() / 2.0 + double(g) * fm.height() - fm.height(), 120, fm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, gamuts[g].name);
    }
    // A Crosstalk's primaries: an arrow from each one's place to where it is taken, and the triangle they make.
    if (!primaryMoves_.empty()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        const QColor tint[3] = {QColor(255, 90, 90), QColor(90, 230, 110), QColor(110, 150, 255)};
        QPolygonF moved;
        for (size_t i = 0; i < primaryMoves_.size(); ++i) {
            const QPointF a = at(Xy{primaryMoves_[i].first.x(), primaryMoves_[i].first.y()});
            const QPointF b = at(Xy{primaryMoves_[i].second.x(), primaryMoves_[i].second.y()});
            moved << b;
            p.setPen(QPen(tint[i % 3], 1.5));
            p.drawLine(a, b);
            p.setBrush(tint[i % 3]);
            p.drawEllipse(b, 3.5, 3.5);
        }
        if (moved.size() == 3) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(255, 255, 255, 160), 1, Qt::DotLine));
            p.drawPolygon(moved);
        }
        p.restore();
    }
    // D65.
    const QPointF white = at(Xy{0.3127, 0.3290});
    p.setPen(QPen(graticuleColor(200), 1));
    p.drawLine(white - QPointF(4, 0), white + QPointF(4, 0));
    p.drawLine(white - QPointF(0, 4), white + QPointF(0, 4));
    // How much of the picture is outside the smaller gamuts.
    if (haveData_ && outside709_ >= 0) {
        const QLocale loc;
        auto pct = [&](double f) { return f > 0 && f < 0.001 ? QStringLiteral("<0.1") : loc.toString(f * 100, 'f', 1); };
        p.setPen(outside709_ > 0 ? QColor(255, 150, 90) : theme::kTextDim);
        p.drawText(QRectF(plot.right() - 200, plot.top() + 3, 196, fm.height()), Qt::AlignRight | Qt::AlignTop,
                   tr("Outside Rec.709 %1 % · P3 %2 %").arg(pct(outside709_), pct(outsideP3_)));
    }
    p.restore();
}

}  // namespace montage
