// Montage — Clip › Convert Voice… (DaVinci Resolve 20's Voice Convert, ElevenLabs' speech to speech): a clip's speech
// said again in another person's voice, its words, timing and delivery kept (media/VoiceClone.h convertVoice), for ADR
// read by someone else, or a temp line made to sound like the actor. The voice comes from another clip or a recording,
// once the editor confirms they have its owner's consent. The converted sound (with a second of handle either side)
// becomes a take of the clip, picked, so the original is one take away; its file says in its own metadata, and in the
// bin, what it was converted from and whose voice it now has.
#pragma once

#include <QDialog>
#include <QString>
#include <string>

#include "SpeechDialog.h"
#include "core/Model.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

namespace montage {

class EditorState;

// The audio clip a selection converts: the first selected clip on an audio track, else the sound linked to the first
// selected picture; 0 for none.
Id voiceConversionClip(const Project& p, const Sequence& s, const std::vector<Id>& selection);
// What a converted file says about itself.
std::string convertedVoiceNote(const std::string& speechFrom, const ClonedVoice& voice);

// Converts audio clip `clip`'s sound into `voice` (the caller has its owner's consent), with progress, and adds it to
// the clip as a take, picked (Convert Voice: one undo step after the file's import). False if cancelled or failed.
bool convertClipVoice(EditorState* state, Id clip, const ClonedVoice& voice, QWidget* parent, QString* error = nullptr,
                      double handles = 1);

class VoiceConvertDialog : public QDialog {
    Q_OBJECT
public:
    // Converts `clip`; the voice offered first is `voiceClip`'s (else the first other clip with speech).
    VoiceConvertDialog(EditorState* state, Id clip, Id voiceClip = 0, QWidget* parent = nullptr);
    bool convert();
    void setRecording(const QString& path);  // the voice from a recording (as picking one does)

private:
    void voiceChanged();

    EditorState* state_;
    Id clip_;
    QComboBox* voice_;
    QCheckBox* consent_;
    QLabel* status_;
    QPushButton* go_ = nullptr;
    QString recording_;
};

}  // namespace montage
