// Montage — Sequence › Auto Mix…: what each audio clip was recognised as
// (each can be changed), the levels to mix to, and one undoable step that
// writes clip volumes, rides dialogue and ducks music (render/AutoMix.h).
#pragma once

#include <QDialog>
#include <vector>

#include "render/AutoMix.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QTableWidget;

namespace montage {

class EditorState;

class AutoMixDialog : public QDialog {
    Q_OBJECT
public:
    AutoMixDialog(EditorState* state, std::vector<ClipMix> plan, QWidget* parent = nullptr);

    const std::vector<ClipMix>& plan() const { return plan_; }
    MixOptions options() const;
    // Sets a clip's role, as its row's menu does.
    void setRole(int row, AudioRole role);

    // Listens to the sequence's audio with a progress dialog, shows the
    // dialog, and applies the mix if accepted. Returns clips changed, or -1.
    static int run(EditorState* state, QWidget* parent);
    static int apply(EditorState* state, const std::vector<ClipMix>& plan, const MixOptions& o);

private:
    void replan();
    void fillTable();

    EditorState* state_;
    std::vector<ClipMix> plan_;
    QTableWidget* table_;
    QComboBox* preset_;
    QDoubleSpinBox* dialogue_;
    QDoubleSpinBox* music_;
    QDoubleSpinBox* effects_;
    QCheckBox* ride_;
    QCheckBox* duck_;
    QDoubleSpinBox* duckDb_;
};

}  // namespace montage
