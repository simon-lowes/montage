// Montage — video scopes for the program monitor: luma waveform, RGB parade,
// vectorscope and histogram, one at a time or all four together (Resolve's quad view),
// analysed at a throttled rate on a downsampled frame.
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QTimer>
#include <QWidget>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "core/Model.h"
#include "render/LightLevel.h"

class QComboBox;
class QLabel;

namespace montage {

class ScopesWidget : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Waveform = 0, Parade, Vectorscope, Histogram, Quad };

    explicit ScopesWidget(QWidget* parent = nullptr);

    Mode mode() const { return mode_; }
    void setMode(Mode m);

    bool hasSignal() const { return haveData_; }
    // What the scopes measure in: the colour space of the frames' code values (ColorSpace.h id). HDR (PQ, HLG)
    // levels are scaled and labelled in nits, with the frame's brightest pixel and average light read out.
    QString signalSpace() const { return space_; }
    bool hdr() const;
    double peakNits() const { return peakNits_; }        // the frame's brightest channel (HDR), -1 otherwise
    double averageNits() const { return averageNits_; }  // its average light, as MaxFALL measures it (HDR), -1 otherwise
    // The top of the waveform and parade for PQ, in nits (10 000, 4 000, 2 000 or 1 000).
    double nitsRange() const { return nitsTop_; }
    void setNitsRange(double nits);
    // The trace's rows (levels shown, bottom to top), for tests.
    int traceRows() const { return trace_.height(); }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    // Latest program frame (any format), in Rec.709. Analysis happens at most ~15 Hz.
    void setFrame(const QImage& image, montage::FrameTime t);
    // Latest frame as delivered: code values in `space` (16-bit images keep HDR precision), with the sequence's
    // mastering peak (marked on the scale and the readout warning above it).
    void setSignal(const QImage& image, montage::FrameTime t, const QString& space, double masteringPeakNits);
    // Drops the current frame and shows the empty graticule.
    void clear();

protected:
    void paintEvent(QPaintEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    void scheduleAnalysis();
    void analyze();          // downsamples the pending frame, then computeTrace()
    void computeTrace();     // per-mode analysis of small_
    void analyzeWaveform(bool parade);
    void analyzeVectorscope();
    void analyzeHistogram();
    QRect scopeArea() const;
    // trace_ resampled (area-averaged) to `size`, cached until the trace or size changes.
    const QImage& scaledTrace(const QSize& size);

    void paintWaveform(QPainter& p, const QRect& area, bool parade);
    void paintVectorscope(QPainter& p, const QRect& area);
    void paintHistogram(QPainter& p, const QRect& area);
    void paintLevelGraticule(QPainter& p, const QRectF& plot, bool labels);
    // HDR scale marks: nits and where they sit (0..1 of the plot), the mastering peak's place or -1.
    std::vector<std::pair<double, double>> nitsMarks(bool fullRange, double& peakAt) const;
    void updateHdrControls();
    int luma(int r, int g, int b) const { return (lumaR_ * r + lumaG_ * g + lumaB_ * b + 32768) >> 16; }

    QComboBox* modeBox_ = nullptr;
    QComboBox* rangeBox_ = nullptr;
    QLabel* nitsLabel_ = nullptr;
    QString space_ = QStringLiteral("rec709"), pendingSpace_ = QStringLiteral("rec709");
    double masterPeak_ = 1000, pendingPeak_ = 1000;
    double nitsTop_ = 10000;
    double peakNits_ = -1, averageNits_ = -1;
    int lumaR_ = 13933, lumaG_ = 46871, lumaB_ = 4732;  // Rec.709 weights summing to 65536
    std::unique_ptr<LightMeter> meter_;
    Mode mode_ = Mode::Waveform;

    QImage pending_;        // latest frame as received
    QImage small_;          // downsampled RGB32 copy of the last analysed frame
    bool dirty_ = false;    // pending_ not yet analysed
    FrameTime frameTime_ = -1;
    QTimer throttle_;
    QElapsedTimer sinceAnalysis_;

    QImage trace_;                                     // waveform / parade / vectorscope trace
    std::array<QImage, 3> quad_;                       // Quad: the waveform, parade and vectorscope traces
    QImage traceScaled_;
    qint64 traceScaledKey_ = 0;
    std::array<std::array<uint32_t, 256>, 4> hist_{};  // R, G, B, luma
    bool haveData_ = false;
};

}  // namespace montage
