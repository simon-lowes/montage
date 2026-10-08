// Montage — Sequence › Generate Voiceover: text spoken by an AI voice
// (media/TextToSpeech.h) and placed on an audio track at the playhead, or a
// caption track spoken cue by cue at each cue's time (sped up a little where
// a cue is too short for its words). The sound files go in the Voiceover
// folder beside the project and the bin's Voiceover folder.
#pragma once

#include <QDialog>
#include <string>
#include <vector>

#include "core/Model.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;

namespace montage {

class EditorState;

struct SpeechLine {
    std::string text;
    FrameTime at = 0;
    FrameTime fit = -1;  // the room it has, in frames (-1: any)
};

// Speaks the lines (offering the model download first, with progress) and places each at its time
// on audio track `track`, in one undo step. Returns the new clips; empty if cancelled or failed.
std::vector<Id> generateSpeech(EditorState* state, const std::vector<SpeechLine>& lines, const std::string& voice, double speed,
                               int track, QWidget* parent, QString* error = nullptr);

// A caption track's cues as lines to speak.
std::vector<SpeechLine> captionLines(const Sequence& s, int captionTrack);

class SpeechDialog : public QDialog {
    Q_OBJECT
public:
    explicit SpeechDialog(EditorState* state, QWidget* parent = nullptr);
    // Generates what the dialog holds; returns the new clips.
    std::vector<Id> generate();

private:
    void sourceChanged();

    EditorState* state_;
    QComboBox* source_;
    QPlainTextEdit* text_;
    QComboBox* voice_;
    QDoubleSpinBox* speed_;
    QComboBox* track_;
    QLabel* status_;
};

}  // namespace montage
