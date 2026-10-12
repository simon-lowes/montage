// Montage — Clip › Correct Lens… (Premiere's and Resolve's lens correction, Lightroom's profiles): the camera and lens
// a clip was shot with, from lensfun's profiles (the camera suggested by the clip's Camera Model, a fixed lens with
// it), the focal length and aperture; Apply puts a Lens Correction first on the clip (or on every clip of its media).
// The Inspector's Choose Lens… opens it for a Lens Correction already there.
#pragma once

#include <QDialog>
#include <functional>
#include <memory>
#include <vector>

#include "core/Model.h"
#include "media/LensDatabase.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace montage {

class EditorState;

class LensCorrectionDialog : public QDialog {
    Q_OBJECT
public:
    LensCorrectionDialog(std::shared_ptr<const lens::Database> db, const LensChoice& start, const QString& clipName,
                         QWidget* parent = nullptr);
    LensChoice choice() const;
    bool sameMedia() const;
    // By name as the lists show them ("" for any camera); false when there is none.
    bool setCamera(const QString& name);
    bool setLens(const QString& name);
    QString cameraText() const;
    QString lensText() const;

private:
    void cameraChanged();
    void lensChanged();
    const lens::Camera* currentCamera() const;
    const lens::Lens* currentLens() const;
    std::shared_ptr<const lens::Database> db_;
    std::vector<const lens::Camera*> cameras_;
    std::vector<const lens::Lens*> lenses_;
    QComboBox* camera_;
    QComboBox* lens_;
    QDoubleSpinBox* focal_;
    QDoubleSpinBox* aperture_;
    QLabel* about_;
    QCheckBox* sameMedia_;
    QPushButton* apply_;
};

// The dialog for a picture clip, its lens profile database fetched first if need be (and asked for); applied on OK.
// True when a Lens Correction was put on.
bool correctLens(QWidget* parent, EditorState* state, Id clip);
// Tests: given each dialog before it runs.
std::function<void(LensCorrectionDialog*)>& lensDialogHook();

}  // namespace montage
