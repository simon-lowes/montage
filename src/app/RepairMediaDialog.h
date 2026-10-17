// Montage — Media bin › Repair Media: a recording the camera or recorder never finished (a flat battery, a pulled
// card) rebuilt into a new file beside it (media/Recover.h), and imported labelled as recovered. An MP4 or MOV needs a
// good clip from the same camera, which is guessed from the files beside it.
#pragma once

#include <QDialog>

#include "core/Model.h"
#include "media/Recover.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace montage {

class EditorState;

class RepairMediaDialog : public QDialog {
    Q_OBJECT
public:
    RepairMediaDialog(EditorState* state, QWidget* parent = nullptr);

    // The damaged file: what is wrong with it shows, and a reference clip is guessed when one is needed.
    void setDamaged(const QString& file);
    void setReference(const QString& file);
    void setBin(const QString& bin) { bin_ = bin; }
    // Repairs (with a progress dialog) and, when asked, imports the result into the bin, labelled as recovered.
    // Returns whether it was repaired; the report shows in the dialog. `imported` is the new media item (0 = none).
    bool run();
    Id imported() const { return imported_; }
    const QString& report() const { return reportText_; }

private:
    void check();

    EditorState* state_;
    QLineEdit* damaged_;
    QLabel* problem_;
    QLineEdit* reference_;
    QPushButton* chooseReference_;
    QLineEdit* output_;
    QCheckBox* import_;
    QPlainTextEdit* report_;
    QPushButton* start_ = nullptr;
    QString bin_, reportText_;
    QString replace_;  // an existing file the save dialog was told to replace
    RepairCheck checked_;
    Id imported_ = 0;
};

// A good clip from the same camera for `damaged`: the newest other file beside it with the same extension that
// opens; "" when there is none.
QString guessReferenceClip(const QString& damaged);

}  // namespace montage
