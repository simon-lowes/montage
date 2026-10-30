// Montage — video scopes for the program monitor: luma waveform, RGB parade,
// vectorscope and histogram, one at a time or all four together (Resolve's quad view),
// and a CIE chromaticity scope (1931 xy or 1976 u'v') with the Rec.709, P3 and Rec.2020
// gamuts drawn on it, analysed at a throttled rate on a downsampled frame.
#pragma once

#include <QElapsedTimer>
#include <QPointF>
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
    enum class Mode { Waveform = 0, Parade, Vectorscope, Histogram, Quad, Chromaticity };

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
    // The chromaticity scope: 1976 u'v' (else 1931 xy).
    bool cieUv() const { return cieUv_; }
    void setCieUv(bool uv);
    // Read the working space's values (a sequence graded scene-referred: what its grade works on) rather than the output
    // as delivered; the viewer is told by readWorkingChanged.
    bool readsWorking() const { return readsWorking_; }
    void setReadsWorking(bool on);
    // Of the frame's lit pixels (chromaticity scope), the share outside Rec.709 and outside P3, 0..1 (-1: no data).
    double outsideRec709() const { return outside709_; }
    double outsideP3() const { return outsideP3_; }
    // The chromaticity scope's samples near a point (xy, or u'v' when cieUv()), for tests.
    int cieSamplesNear(double a, double b, double radius) const;
    // Primaries a Crosstalk moves, drawn on the chromaticity scope as arrows from where they were (CIE 1931 xy: each
    // pair's first point) to where it takes them; none, nothing drawn.
    void setPrimaryMoves(const std::vector<std::pair<QPointF, QPointF>>& xy);
    const std::vector<std::pair<QPointF, QPointF>>& primaryMoves() const { return primaryMoves_; }
    // The frame in words, for a screen reader: "Waveform: levels 4 % to 96 %, average 41 %; saturation 18 %, leaning
    // blue" (with the HDR light levels, and in the chromaticity scope what lies outside Rec.709).
    QString summary() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void readWorkingChanged(bool on);

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
    void analyzeChromaticity();
    QRect scopeArea() const;
    // trace_ resampled (area-averaged) to `size`, cached until the trace or size changes.
    const QImage& scaledTrace(const QSize& size);

    void paintWaveform(QPainter& p, const QRect& area, bool parade);
    void paintVectorscope(QPainter& p, const QRect& area);
    void paintHistogram(QPainter& p, const QRect& area);
    void paintChromaticity(QPainter& p, const QRect& area);
    void paintLevelGraticule(QPainter& p, const QRectF& plot, bool labels);
    // HDR scale marks: nits and where they sit (0..1 of the plot), the mastering peak's place or -1.
    std::vector<std::pair<double, double>> nitsMarks(bool fullRange, double& peakAt) const;
    void updateHdrControls();
    int luma(int r, int g, int b) const { return (lumaR_ * r + lumaG_ * g + lumaB_ * b + 32768) >> 16; }

    QComboBox* modeBox_ = nullptr;
    QComboBox* readBox_ = nullptr;
    bool readsWorking_ = false;
    QComboBox* rangeBox_ = nullptr;
    QComboBox* cieBox_ = nullptr;
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
    Image codes_;           // the same frame's code values (the chromaticity scope)
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
    bool cieUv_ = false;
    std::vector<uint32_t> cieCounts_;  // the chromaticity scope's bins (kCieBins square)
    double outside709_ = -1, outsideP3_ = -1;
    std::vector<std::pair<QPointF, QPointF>> primaryMoves_;
};

}  // namespace montage
