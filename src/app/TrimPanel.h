// Montage — the Trims panel (Resolve's MultiMaster Trim Manager): the sequence's trim targets (render/TrimPasses.h),
// the one the Program monitor and reference output show, and the shot at the playhead's trim for it (lift, gamma,
// gain, saturation, highlight roll-off, mid-tone detail; a control with keyframes is set at the playhead). Copy puts
// the shot's trim on the selected clips; Queue Passes renders every target into a folder.
#pragma once

#include <QWidget>
#include <map>

#include "core/Model.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QToolButton;

namespace montage {

class EditorState;

class TrimPanel : public QWidget {
    Q_OBJECT
public:
    explicit TrimPanel(EditorState* state, QWidget* parent = nullptr);

    // The target shown ("" the master).
    QString shown() const;
    void showTarget(const QString& target);
    // The edits; false (with a message) when they cannot be made.
    bool addTarget(const QString& preset);
    bool removeShown();
    bool setControl(const QString& name, double value);
    bool resetShot();
    bool copyToSelection();
    // The shot being trimmed (0: none at the playhead).
    Id shot() const;
    void refresh();
    void refreshShot();  // the shot at the playhead and its controls

signals:
    void shownChanged(const QString& target);
    void queueRequested();

private:
    EditorState* state_;
    QComboBox* targets_;
    QToolButton* add_;
    QToolButton* remove_;
    QLabel* shotLabel_;
    std::map<std::string, QDoubleSpinBox*> controls_;
    QToolButton* key_;
    QPushButton* reset_;
    QPushButton* copy_;
    QPushButton* queue_;
    bool updating_ = false;
};

}  // namespace montage
