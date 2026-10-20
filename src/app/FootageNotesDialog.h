// Montage — Sequence › Summary and Notes: a summary, show notes or story notes written from the sequence's transcript
// by the language model on this computer (render/FootageWords.h), to read, correct, copy or save.
#pragma once

#include <QDialog>

#include "render/FootageWords.h"

class QComboBox;
class QLabel;
class QPlainTextEdit;

namespace montage {

class EditorState;

class FootageNotesDialog : public QDialog {
    Q_OBJECT
public:
    FootageNotesDialog(EditorState* state, QWidget* parent = nullptr);
    // Writes `what` into the text box (downloading the model first if asked). False with `error` when it cannot.
    bool write(FootageWriting what, QString* error = nullptr);
    QString text() const;

private:
    EditorState* state_;
    QComboBox* kind_;
    QPlainTextEdit* text_;
    QLabel* status_;
};

}  // namespace montage
