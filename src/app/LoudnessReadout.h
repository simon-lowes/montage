// Montage — the loudness readout under the audio meters: momentary,
// short-term and integrated loudness (LUFS), loudness range (LU) and true
// peak (dBTP) of what is playing, coloured against a delivery target; and the
// dialogue's loudness (render/DialogueLoudness.h) with the share that is speech,
// which Netflix's target is measured on.
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

    // Targets: EBU R128 (-23 LUFS), ATSC A/85 (-24), streaming (-14), Apple and podcasts (-16), Netflix (-27 LKFS
    // of dialogue ±2, peaks under -2 dBTP).
    double target() const;
    bool dialogueTarget() const;  // Netflix's: the dialogue's loudness counts (the programme's under 15 % speech)
    void setTargetIndex(int index);
    // Where the loudness that counts stands against the target: 0 on it (within 1 LU, 2 for Netflix), 1 near (1 LU
    // more), 2 off.
    int integratedStatus() const { return status_; }
    QString text(const char* field) const;  // "M", "S", "I", "LRA", "TP", "D", "SP" or "INT" as shown

public slots:
    void setReading(double momentary, double shortTerm, double integrated, double range, double truePeak);
    // Over speech only (LKFS, -200 for none yet) and the share of the sound that is speech (-1: none is marked).
    void setDialogue(double integrated, double speechShare);
    // Dialogue intelligibility (render/Intelligibility.h) where the playhead is, -1 for none; status 0 green, 1 amber, 2 red.
    void setIntelligibility(double index, int status);
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
    QLabel* d_;
    QLabel* sp_;
    QLabel* int_;
    double integrated_ = -200;
    double truePeak_ = -200;
    double dialogue_ = -200;
    double share_ = -1;
    int status_ = 2;
    void colour();
};

}  // namespace montage
