// Montage — the Meters dock's analysis of what is heard (media/StereoAnalysis.h): the phase correlation bar (+1 mono,
// 0 wide, under 0 out of phase) with what a mono fold-down would lose, the goniometer (left and right as mid up and
// side across), and the spectrum in third-octave bands or FFT bins, K-weighted if asked, with held peaks.
#pragma once

#include <QWidget>

#include "media/StereoAnalysis.h"

class QCheckBox;
class QComboBox;
class QLabel;

namespace montage {

class AudioAnalysisWidget : public QWidget {
    Q_OBJECT
public:
    enum class SpectrumMode { Bands, Bins };
    explicit AudioAnalysisWidget(QWidget* parent = nullptr);

    SpectrumMode mode() const;
    void setMode(SpectrumMode m);
    bool kWeighting() const;
    void setKWeighting(bool on);
    bool peakHold() const;
    void setPeakHold(bool on);
    const AudioAnalysisPtr& analysis() const { return analysis_; }
    QString text(const char* field) const;  // "CORR" or "MONO" as shown
    // The level drawn at a frequency (dB; the band it falls in, or the nearest bin), -120 for none.
    double levelAt(double hz, bool peak = false) const;

public slots:
    void setAnalysis(montage::AudioAnalysisPtr analysis);
    void clear();

signals:
    void kWeightingChanged(bool on);

private:
    friend class AnalysisCanvas;
    AudioAnalysisPtr analysis_;
    QLabel* corr_;
    QLabel* mono_;
    QComboBox* mode_;
    QCheckBox* kWeight_;
    QCheckBox* hold_;
    QWidget* canvas_;
};

}  // namespace montage
