// Montage — Sequence › Build Cut from Script…: paste or open a script, see
// which lines were found in the transcribed takes, and build a sequence with
// the best reading of each line in order and the other readings above it
// (core/ScriptCut.h).
#pragma once

#include <QDialog>
#include <vector>

#include "core/ScriptCut.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTimer;
class QTreeWidget;

namespace montage {

class EditorState;

class ScriptCutDialog : public QDialog {
    Q_OBJECT
public:
    // `selected`: media chosen in the bin, offered as the takes to search.
    ScriptCutDialog(EditorState* state, const std::vector<Id>& selected, QWidget* parent = nullptr);

    void setScript(const QString& text);
    QString script() const;
    bool loadScript(const QString& path, QString* error = nullptr);  // text, Fountain, Markdown or Final Draft (.fdx)
    ScriptCutOptions options() const;
    QString sequenceName() const;
    // The lines and their readings as the dialog shows them (refreshed as the script changes).
    const std::vector<ScriptMatch>& matches();

    // Builds the cut as one undo step and opens it.
    static ScriptCutResult build(EditorState* state, const QString& script, const ScriptCutOptions& o, const QString& name);

private:
    void refresh();

    EditorState* state_;
    std::vector<Id> selected_;
    QPlainTextEdit* text_;
    QComboBox* scope_;
    QDoubleSpinBox* coverage_;
    QSpinBox* alternates_;
    QDoubleSpinBox* handle_;
    QCheckBox* markers_;
    QLineEdit* name_;
    QTreeWidget* preview_;
    QLabel* summary_;
    QTimer* debounce_;
    std::vector<ScriptMatch> matches_;
    bool stale_ = true;
};

}  // namespace montage
