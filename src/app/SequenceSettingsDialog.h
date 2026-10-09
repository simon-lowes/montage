// Montage — sequence settings: name, frame size, frame rate, audio sample
// rate and colour space, for editing the active sequence or creating a new one.
#pragma once

#include <QDialog>
#include <QSpinBox>
#include <QString>
#include <optional>

#include "core/Model.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace montage {

class EditorState;

struct NewSequenceSpec {
    QString name;
    int width = 1920;
    int height = 1080;
    Rational fps{30, 1};
    int sampleRate = 48000;
    std::string colorSpace = "rec709";
    double hdrPeakNits = 1000;
    std::string audioLayout = "stereo";  // "stereo", "5.1" or "7.1"
    bool spherical = false;              // a 360° sequence (Sequence::spherical)
};

// Spin box that only accepts even values (most encoders need even frame sizes).
// Odd input is rounded up on commit (down at the maximum).
class EvenSpinBox : public QSpinBox {
public:
    explicit EvenSpinBox(QWidget* parent = nullptr);

protected:
    QValidator::State validate(QString& input, int& pos) const override;
    void fixup(QString& input) const override;
};

class SequenceSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SequenceSettingsDialog(QWidget* parent = nullptr);

    void setSpec(const NewSequenceSpec& spec);
    NewSequenceSpec spec() const;
    // Frame rate cannot change once a sequence holds clips (positions are frames).
    void setFrameRateLocked(bool locked, const QString& reason = QString());

    // Shows the dialog for the active sequence and applies changes as one undo
    // step. Returns true if the sequence was changed.
    static bool editActive(EditorState* state, QWidget* parent);
    // Asks for the settings of a new sequence; defaults to the last values used.
    static std::optional<NewSequenceSpec> askNew(QWidget* parent, const QString& defaultName = QString());

private:
    void selectPresetForSize();
    void applyPreset();
    void selectFrameRate(Rational fps);
    void selectSampleRate(int rate);
    void updateSummary();

    QLineEdit* name_ = nullptr;
    QComboBox* sizePreset_ = nullptr;
    EvenSpinBox* width_ = nullptr;
    EvenSpinBox* height_ = nullptr;
    QComboBox* frameRate_ = nullptr;
    QComboBox* sampleRate_ = nullptr;
    QComboBox* colorSpace_ = nullptr;
    QSpinBox* hdrPeak_ = nullptr;
    QComboBox* audioLayout_ = nullptr;
    QCheckBox* spherical_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* okButton_ = nullptr;
    bool updating_ = false;
};

}  // namespace montage
