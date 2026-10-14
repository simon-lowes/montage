// Montage — Clip › Separate Stems: the sound of the selected audio clips split into vocals, drums, bass (and piano)
// and the rest (media/StemSeparation.h), as Logic's Stem Splitter and Resolve's Music Remixer do. Each stem is a WAV in
// the Stems folder beside the project and the bin's Stems folder, played by a copy of the clip on a track named for it,
// linked with the clip; the clip itself is muted (disabled) so the stems replace it, and can be turned back on.
#pragma once

#include <QDialog>
#include <vector>

#include "core/Model.h"

class QCheckBox;
class QComboBox;

namespace montage {

class EditorState;

struct StemOptions {
    int stems = 4;              // 2, 3 (dialogue, music, effects), 4 or 5
    bool muteOriginal = true;   // disable the clip the stems came from
    double handles = 2.0;       // seconds of source separated either side of each clip, for trimming
};

// Separates each clip (offering the model download first, with progress) and places its stems in sync, in one undo
// step. Returns the new clips; empty if cancelled or failed (with `error`).
std::vector<Id> separateStems(EditorState* state, const std::vector<Id>& clips, const StemOptions& o, QWidget* parent,
                              QString* error = nullptr);

class StemsDialog : public QDialog {
    Q_OBJECT
public:
    explicit StemsDialog(QWidget* parent = nullptr);
    StemOptions options() const;

private:
    QComboBox* stems_;
    QCheckBox* mute_;
};

}  // namespace montage
