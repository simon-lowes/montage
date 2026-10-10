// Montage — File › Offload Card: a camera card copied to one or more drives with every file checksummed and each copy
// read back, an ASC MHL hash list written with each copy, and the copied media imported into a bin named after the
// card (media/Offload.h). Verify Media Hash List checks any folder against its ASC MHL history.
#pragma once

#include <QDialog>

#include "media/Offload.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

namespace montage {

class EditorState;

class OffloadDialog : public QDialog {
    Q_OBJECT
public:
    OffloadDialog(EditorState* state, QWidget* parent = nullptr);

    void setSource(const QString& folder);
    void addDestination(const QString& folder);
    // Runs the offload (with a progress dialog) and, when asked, imports the first copy's media into a bin named after
    // the card. Returns the result; the report shows in the dialog.
    OffloadResult run();
    const QString& report() const { return reportText_; }

private:
    EditorState* state_;
    QLineEdit* source_;
    QListWidget* destinations_;
    QCheckBox* verify_;
    QCheckBox* mhl_;
    QCheckBox* import_;
    QLineEdit* author_;
    QPlainTextEdit* report_;
    QPushButton* start_ = nullptr;
    QString reportText_;
    bool running_ = false;
};

// Checks `folder` against its ASC MHL history (optionally adding a generation) with a progress dialog; the report is a
// sentence or two for a message box.
MhlVerifyResult verifyMhlWithProgress(QWidget* parent, const QString& folder, bool writeGeneration, QString* report);

}  // namespace montage
