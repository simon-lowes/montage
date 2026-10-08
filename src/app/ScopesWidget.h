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

#include "core/Model.h"

class QComboBox;

namespace montage {

class ScopesWidget : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Waveform = 0, Parade, Vectorscope, Histogram, Quad };

    explicit ScopesWidget(QWidget* parent = nullptr);

    Mode mode() const { return mode_; }
    void setMode(Mode m);

    bool hasSignal() const { return haveData_; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    // Latest program frame (any format). Analysis happens at most ~15 Hz.
    void setFrame(const QImage& image, montage::FrameTime t);
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

    QComboBox* modeBox_ = nullptr;
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
