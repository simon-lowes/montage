// Montage — Clip › Calibrate Lens…: a lens profile made from the clip itself (media/LensCalibration.h). Either the
// clip shows a checkerboard (its inner corners across and down given), found in its frames and fitted; or lines that
// should be straight are drawn on its frame at the playhead (as recorded, before any correction), snapped to the
// picture's edges and straightened. Save and Apply keeps the profile in the user's lens folder (so Correct Lens offers
// it) and puts it in a Lens Correction first on the clip (or every clip of its media).
#pragma once

#include <QDialog>
#include <QImage>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/LensCalibration.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;

namespace montage {

class EditorState;
class LineCanvas;

class LensCalibrationDialog : public QDialog {
    Q_OBJECT
public:
    struct Source {
        std::string path;           // the media
        double from = 0, to = 0;    // the clip's part of it (seconds)
        QImage frame;               // the frame lines are drawn on, as recorded
        GrayImage gray;             // the same, for snapping
        QString clipName, maker, model;
        QString device;             // the camera's name, as the clip gives it
        double cropFactor = 0;      // the camera's, when known
        std::string mount;
        bool anamorphic = false;    // the media is (a squeeze worth fitting)
    };
    explicit LensCalibrationDialog(Source source, QWidget* parent = nullptr);

    void setLinesMode(bool lines);
    void setBoard(int cols, int rows);
    // A line drawn on the frame (points in fractions of it), snapped as the canvas snaps one.
    void addLine(const std::vector<QPointF>& fractions);
    int lineCount() const;
    // Calibrate: false (and why, shown) when it could not fit.
    bool calibrate();
    bool fitted() const { return fitted_; }
    const LensFit& fit() const { return fit_; }
    QString resultText() const;
    // The fit as a lens with the names given.
    lens::Lens lens() const;
    bool sameMedia() const;

private:
    void modeChanged();
    void updateResult();
    Source src_;
    LensFit fit_;
    int fitWidth_ = 0, fitHeight_ = 0;
    bool fitted_ = false;
    QString autoName_;  // the name given while it is ours (not typed)
    QRadioButton* boardMode_;
    QRadioButton* linesMode_;
    QWidget* boardBox_;
    QWidget* linesBox_;
    QSpinBox* cols_;
    QSpinBox* rows_;
    LineCanvas* canvas_;
    QComboBox* model_;
    QCheckBox* anamorphic_;
    QCheckBox* centre_;
    QLineEdit* maker_;
    QLineEdit* name_;
    QDoubleSpinBox* focal_;
    QLabel* result_;
    QCheckBox* sameMedia_;
    QPushButton* save_;
};

// The dialog for a picture clip; on Save and Apply the lens is saved and applied. True when a Lens Correction was put on.
bool calibrateLens(QWidget* parent, EditorState* state, Id clip);
// Tests: given each dialog before it runs.
std::function<void(LensCalibrationDialog*)>& lensCalibrationDialogHook();

}  // namespace montage
