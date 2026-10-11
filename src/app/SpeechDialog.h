// Montage — Sequence › Generate Voiceover: text spoken by an AI voice
// (media/TextToSpeech.h) and placed on an audio track at the playhead, or a
// caption track spoken cue by cue at each cue's time (sped up a little where
// a cue is too short for its words). The sound files go in the Voiceover
// folder beside the project and the bin's Voiceover folder. The voice can
// also be cloned from a clip or a recording (media/VoiceClone.h), in 23
// languages, once the editor confirms they have the speaker's consent; each
// file made says in its own metadata, and in the bin, that it is generated
// and whose voice it was cloned from.
#pragma once

#include <QDialog>
#include <string>
#include <vector>

#include "core/Model.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace montage {

class EditorState;

struct SpeechLine {
    std::string text;
    FrameTime at = 0;
    FrameTime fit = -1;  // the room it has, in frames (-1: any)
};

// A voice cloned from a recording.
struct ClonedVoice {
    std::vector<float> sample;  // mono at kCloneSampleRate (media/VoiceClone.h cloneReference: at most ten seconds)
    std::string language = "en";
    std::string from;  // where the sample came from, noted in every file spoken with it
};
// The voice in a clip: its sound as the clip plays it (its channels), from its source In, at most `seconds` of it.
bool clipVoice(const Project& p, const Sequence& s, Id clip, ClonedVoice& voice, QString* error = nullptr, double seconds = 30);
// The voice heard at timeline frame `at`: the clip with sound there on the first audio track that has one.
bool voiceAt(const Project& p, const Sequence& s, FrameTime at, ClonedVoice& voice, QString* error = nullptr);
// The voice in a recording, from its start (at most thirty seconds of it).
bool fileVoice(const QString& path, ClonedVoice& voice, QString* error = nullptr);
// What each file spoken in a cloned voice says about itself.
std::string clonedVoiceNote(const ClonedVoice& voice);

// Speaks the lines (offering the model download first, with progress) and places each at its time
// on audio track `track` (-1: a new one), in one undo step. Returns the new clips; empty if cancelled or failed.
// With `clone`, in that voice (the caller has the speaker's consent) instead of `voice`, sped up by time-stretching.
std::vector<Id> generateSpeech(EditorState* state, const std::vector<SpeechLine>& lines, const std::string& voice, double speed,
                               int track, QWidget* parent, QString* error = nullptr, const ClonedVoice* clone = nullptr);

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
    void voiceChanged();
    bool cloning() const;

    EditorState* state_;
    QComboBox* source_;
    QPlainTextEdit* text_;
    QComboBox* voice_;
    QComboBox* language_;
    QLabel* sample_;
    QCheckBox* consent_;
    QDoubleSpinBox* speed_;
    QComboBox* track_;
    QLabel* status_;
    QPushButton* go_ = nullptr;
    QString sampleFile_;  // "Clone a voice from a recording…": the file picked
};

}  // namespace montage
