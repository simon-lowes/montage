// Montage — Clip › Edit Sound by Samples…: an audio clip's sound moved, its start or end trimmed, or what it plays
// slipped, by samples (core/EditOps.h editSoundSamples), with where it starts and ends shown as timecode and samples.
#pragma once

#include <QDialog>

#include "core/Model.h"

class QLabel;
class QSpinBox;

namespace montage {

class EditorState;

class SoundSamplesDialog : public QDialog {
    Q_OBJECT
public:
    SoundSamplesDialog(EditorState* state, Id clip, QWidget* parent = nullptr);
    // Applies the fields as one undo step and sets them back to 0. False (with the reason shown) when an edit cannot
    // be made, and nothing changes.
    bool apply();
    QString span() const;  // "<start> to <end>" as shown
    QString error() const;

private:
    void refresh();
    EditorState* state_;
    Id clip_;
    QLabel* span_;
    QLabel* error_;
    QSpinBox* move_;
    QSpinBox* trimStart_;
    QSpinBox* trimEnd_;
    QSpinBox* slip_;
};

}  // namespace montage
