#include "AudioAnalysisWidget.h"
#include "Settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace montage {

namespace {

constexpr double kLow = 20, kHigh = 20000;  // the spectrum's frequency span
constexpr double kTopDb = 0, kBottomDb = -90;
const QColor kGreen(0x3f, 0xb9, 0x50), kAmber(0xd2, 0x99, 0x22), kRed(0xf8, 0x51, 0x49);

QColor correlationColour(double c) { return c >= 0 ? kGreen : c >= -0.5 ? kAmber : kRed; }

}  // namespace

// The bar, the goniometer and the spectrum, drawn from the widget's latest analysis.
class AnalysisCanvas : public QWidget {
public:
    explicit AnalysisCanvas(AudioAnalysisWidget* owner) : QWidget(owner), owner_(owner) {
        setMinimumHeight(110);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(24, 24, 28));
        const AudioAnalysis* a = owner_->analysis_.get();
        const QColor grid(70, 70, 78), label(150, 150, 160);
        QFont small = font();
        small.setPointSizeF(std::max(6.0, small.pointSizeF() * 0.75));
        p.setFont(small);

        // Correlation: -1 at the left, +1 at the right, filled from the middle.
        const QRectF bar(4, 3, width() - 8, 8);
        p.fillRect(bar, QColor(40, 40, 46));
        p.setPen(grid);
        p.drawLine(QPointF(bar.center().x(), bar.top() - 1), QPointF(bar.center().x(), bar.bottom() + 1));
        if (a) {
            const double c = std::clamp(a->correlation, -1.0, 1.0);
            const double mid = bar.center().x(), x = mid + c * bar.width() / 2;
            p.fillRect(QRectF(std::min(mid, x), bar.top(), std::fabs(x - mid), bar.height()), correlationColour(c));
        }

        const double top = bar.bottom() + 4;
        const double side = std::max(20.0, std::min(height() - top - 2, width() * 0.38));
        // Goniometer: mid up, side across; the L and R axes on the diagonals.
        const QRectF g(4, top, side, side);
        p.fillRect(g, QColor(18, 18, 22));
        p.setPen(grid);
        p.drawLine(g.topLeft(), g.bottomRight());
        p.drawLine(g.topRight(), g.bottomLeft());
        p.drawLine(QPointF(g.center().x(), g.top()), QPointF(g.center().x(), g.bottom()));
        p.setPen(label);
        p.drawText(g.adjusted(2, 1, 0, 0), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("L"));
        p.drawText(g.adjusted(0, 1, -2, 0), Qt::AlignRight | Qt::AlignTop, QStringLiteral("R"));
        if (a && !a->points.empty()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(120, 220, 160, 110));
            const double r = side / 2;
            for (const auto& [s, m] : a->points) {
                const double x = g.center().x() + std::clamp(double(s), -1.0, 1.0) * r;
                const double y = g.center().y() - std::clamp(double(m), -1.0, 1.0) * r;
                p.drawRect(QRectF(x, y, 1.2, 1.2));
            }
        }

        // Spectrum, on a log frequency axis.
        const QRectF sp(g.right() + 6, top, width() - g.right() - 10, side);
        if (sp.width() < 20) return;
        p.fillRect(sp, QColor(18, 18, 22));
        const auto xOf = [&](double hz) { return sp.left() + sp.width() * std::log(std::clamp(hz, kLow, kHigh) / kLow) / std::log(kHigh / kLow); };
        const auto yOf = [&](double db) { return sp.top() + sp.height() * (kTopDb - std::clamp(db, kBottomDb, kTopDb)) / (kTopDb - kBottomDb); };
        p.setPen(grid);
        for (double db = -20; db > kBottomDb; db -= 20) p.drawLine(QPointF(sp.left(), yOf(db)), QPointF(sp.right(), yOf(db)));
        for (double hz : {100.0, 1000.0, 10000.0}) {
            p.setPen(grid);
            p.drawLine(QPointF(xOf(hz), sp.top()), QPointF(xOf(hz), sp.bottom()));
            p.setPen(label);
            p.drawText(QPointF(xOf(hz) + 2, sp.bottom() - 2), hz >= 1000 ? QStringLiteral("%1k").arg(hz / 1000) : QString::number(hz));
        }
        if (!a) return;
        const bool hold = owner_->peakHold();
        if (owner_->mode() == AudioAnalysisWidget::SpectrumMode::Bands) {
            const auto& centres = SpectrumAnalyser::bandCentres();
            for (size_t b = 0; b < centres.size() && b < a->bands.size(); ++b) {
                const double x0 = xOf(centres[b] * std::pow(2.0, -1.0 / 6)) + 1, x1 = xOf(centres[b] * std::pow(2.0, 1.0 / 6)) - 1;
                p.fillRect(QRectF(QPointF(x0, yOf(a->bands[b])), QPointF(x1, sp.bottom())), QColor(80, 160, 230));
                if (hold && b < a->bandPeaks.size()) p.fillRect(QRectF(x0, yOf(a->bandPeaks[b]) - 1, x1 - x0, 2), QColor(230, 230, 240));
            }
        } else {
            const double binHz = double(a->rate) / SpectrumAnalyser::kFft;
            const auto line = [&](const std::vector<double>& v, const QColor& c) {
                // Each pixel column the loudest bin under it.
                QPainterPath path;
                bool started = false;
                size_t k = 1;
                for (int px = int(sp.left()); px <= int(sp.right()) && k < v.size(); ++px) {
                    const double hz = kLow * std::pow(kHigh / kLow, (px + 1 - sp.left()) / sp.width());
                    double most = kBottomDb;
                    bool any = false;
                    while (k < v.size() && k * binHz < hz) most = std::max(most, v[k++]), any = true;
                    if (!any) {
                        if (k >= v.size()) break;
                        most = v[k];  // a bin wider than the pixel
                    }
                    const QPointF pt(px, yOf(most));
                    if (!started) path.moveTo(pt), started = true;
                    else path.lineTo(pt);
                }
                p.setPen(QPen(c, 1));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            };
            if (hold) line(a->binPeaks, QColor(230, 230, 240, 120));
            line(a->bins, QColor(80, 160, 230));
        }
        if (a->kWeighted) {
            p.setPen(label);
            p.drawText(sp.adjusted(0, 1, -3, 0), Qt::AlignRight | Qt::AlignTop, QStringLiteral("K"));
        }
    }

private:
    AudioAnalysisWidget* owner_;
};

AudioAnalysisWidget::AudioAnalysisWidget(QWidget* parent) : QWidget(parent) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 2, 4, 2);
    v->setSpacing(2);
    auto* top = new QHBoxLayout;
    auto reading = [&](const QString& name, const QString& tip, QLabel*& out, const char* objectName) {
        auto* l = new QLabel(name, this);
        l->setToolTip(tip);
        out = new QLabel(QStringLiteral("—"), this);
        out->setObjectName(QString::fromLatin1(objectName));
        out->setToolTip(tip);
        QFont f = out->font();
        f.setFamily(QStringLiteral("Monospace"));
        f.setStyleHint(QFont::TypeWriter);
        out->setFont(f);
        top->addWidget(l);
        top->addWidget(out);
    };
    reading(tr("CORR"), tr("Phase correlation of left and right over the last 300 ms: +1 the same (mono), 0 unrelated, "
                           "under 0 out of phase (lost when folded down to mono)"),
            corr_, "phaseCorrelation");
    top->addSpacing(8);
    reading(tr("MONO"), tr("How much quieter a mono fold-down would be than the stereo (dB): 0 for mono, about 3 for wide "
                           "stereo, more when left and right cancel"),
            mono_, "monoLoss");
    top->addStretch();
    v->addLayout(top);
    canvas_ = new AnalysisCanvas(this);
    canvas_->setToolTip(tr("Phase correlation (top), goniometer (left: mono is upright, out of phase lies flat) and spectrum"));
    v->addWidget(canvas_, 1);
    auto* controls = new QHBoxLayout;
    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("spectrumMode"));
    mode_->addItems({tr("Third octaves"), tr("FFT")});
    mode_->setToolTip(tr("The spectrum in 31 third-octave bands or 4096 FFT bins"));
    kWeight_ = new QCheckBox(tr("K-weighted"), this);
    kWeight_->setObjectName(QStringLiteral("spectrumKWeighting"));
    kWeight_->setToolTip(tr("Weight the spectrum as loudness is measured (ITU-R BS.1770): low frequencies count less"));
    hold_ = new QCheckBox(tr("Peaks"), this);
    hold_->setObjectName(QStringLiteral("spectrumPeakHold"));
    hold_->setToolTip(tr("Hold each band's peak, falling 20 dB a second"));
    controls->addWidget(mode_);
    controls->addWidget(kWeight_);
    controls->addWidget(hold_);
    controls->addStretch();
    v->addLayout(controls);

    QSettings s = appSettings();
    mode_->setCurrentIndex(std::clamp(s.value(QStringLiteral("meters/spectrumMode"), 0).toInt(), 0, 1));
    kWeight_->setChecked(s.value(QStringLiteral("meters/kWeighting"), false).toBool());
    hold_->setChecked(s.value(QStringLiteral("meters/peakHold"), true).toBool());
    connect(mode_, &QComboBox::currentIndexChanged, this, [this](int i) {
        appSettings().setValue(QStringLiteral("meters/spectrumMode"), i);
        canvas_->update();
    });
    connect(kWeight_, &QCheckBox::toggled, this, [this](bool on) {
        appSettings().setValue(QStringLiteral("meters/kWeighting"), on);
        emit kWeightingChanged(on);
    });
    connect(hold_, &QCheckBox::toggled, this, [this](bool on) {
        appSettings().setValue(QStringLiteral("meters/peakHold"), on);
        canvas_->update();
    });
}

AudioAnalysisWidget::SpectrumMode AudioAnalysisWidget::mode() const {
    return mode_->currentIndex() == 1 ? SpectrumMode::Bins : SpectrumMode::Bands;
}

void AudioAnalysisWidget::setMode(SpectrumMode m) { mode_->setCurrentIndex(m == SpectrumMode::Bins ? 1 : 0); }

bool AudioAnalysisWidget::kWeighting() const { return kWeight_->isChecked(); }

void AudioAnalysisWidget::setKWeighting(bool on) { kWeight_->setChecked(on); }

bool AudioAnalysisWidget::peakHold() const { return hold_->isChecked(); }

void AudioAnalysisWidget::setPeakHold(bool on) { hold_->setChecked(on); }

QString AudioAnalysisWidget::text(const char* field) const { return (QString::fromLatin1(field) == "MONO" ? mono_ : corr_)->text(); }

double AudioAnalysisWidget::levelAt(double hz, bool peak) const {
    if (!analysis_) return -120;
    const AudioAnalysis& a = *analysis_;
    if (mode() == SpectrumMode::Bands) {
        const auto& c = SpectrumAnalyser::bandCentres();
        size_t best = 0;
        for (size_t b = 1; b < c.size(); ++b)
            if (std::fabs(std::log(c[b] / hz)) < std::fabs(std::log(c[best] / hz))) best = b;
        const auto& v = peak ? a.bandPeaks : a.bands;
        return best < v.size() ? v[best] : -120;
    }
    const auto& v = peak ? a.binPeaks : a.bins;
    const size_t k = size_t(std::lround(hz * SpectrumAnalyser::kFft / std::max(1, a.rate)));
    return k < v.size() ? v[k] : -120;
}

void AudioAnalysisWidget::setAnalysis(AudioAnalysisPtr analysis) {
    analysis_ = std::move(analysis);
    if (analysis_) {
        const double c = analysis_->correlation;
        corr_->setText(QStringLiteral("%1%2").arg(c >= 0 ? QStringLiteral("+") : QString()).arg(c, 0, 'f', 2));
        corr_->setStyleSheet(QStringLiteral("color: %1;").arg(correlationColour(c).name()));
        mono_->setText(QString::number(analysis_->foldDownLossDb > 0.05 ? -analysis_->foldDownLossDb : 0.0, 'f', 1));
        mono_->setStyleSheet(analysis_->foldDownLossDb >= 6 ? QStringLiteral("color: %1;").arg(kRed.name()) : QString());
    }
    canvas_->update();
}

void AudioAnalysisWidget::clear() {
    analysis_.reset();
    for (QLabel* l : {corr_, mono_}) {
        l->setText(QStringLiteral("—"));
        l->setStyleSheet(QString());
    }
    canvas_->update();
}

}  // namespace montage
