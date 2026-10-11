// Montage — the loudness readout under the audio meters: momentary,
// short-term and integrated loudness (LUFS), loudness range (LU) and true
// peak (dBTP) of what is playing, coloured against a delivery target.
#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QToolButton;

namespace montage {

class LoudnessReadout : public QWidget {
    Q_OBJECT
public:
    explicit LoudnessReadout(QWidget* parent = nullptr);

    // Targets: EBU R128 (-23 LUFS), ATSC A/85 (-24), streaming (-14), Apple and podcasts (-16).
    double target() const;
    void setTargetIndex(int index);
    // Where the integrated loudness stands against the target: 0 on it (within 1 LU), 1 near (2 LU), 2 off.
    int integratedStatus() const { return status_; }
    QString text(const char* field) const;  // "M", "S", "I", "LRA" or "TP" as shown

public slots:
    void setReading(double momentary, double shortTerm, double integrated, double range, double truePeak);
    void clear();

signals:
    void resetRequested();

private:
    QComboBox* target_;
    QToolButton* reset_;
    QLabel* m_;
    QLabel* s_;
    QLabel* i_;
    QLabel* lra_;
    QLabel* tp_;
    double integrated_ = -200;
    int status_ = 2;
};

}  // namespace montage
