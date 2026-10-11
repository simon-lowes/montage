// Montage — a script's own panel in the app (automation/ScriptPanel.h; Resolve's Workflow Integrations, Premiere's
// UXP panels): the controls its montage.panel{...} describes, docked like any panel. A button runs the script again
// with what the controls hold (one undo step, "Script: <name>"), and montage.set_panel shows values back on it.
#pragma once

#include <QJsonObject>
#include <QWidget>
#include <map>

#include "automation/ScriptPanel.h"

class QLabel;

namespace montage {

class ScriptPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit ScriptPanelWidget(const PanelSpec& spec, QWidget* parent = nullptr);

    const PanelSpec& spec() const { return spec_; }
    // What its controls hold, by id, with the button pressed as "pressed" ("" for none).
    QJsonObject values(const QString& pressed = {}) const;
    // Shows values (montage.set_panel); false with the error, changing nothing, if any is wrong.
    bool set(const QJsonObject& values, QString* error = nullptr);
    void setBusy(bool busy);              // its buttons wait while its script runs
    void showMessage(const QString& text, bool error = false);  // the script's output, below the controls
    QWidget* control(const QString& id) const;  // (for tests)

signals:
    void pressed(const QString& id);

private:
    void show(const PanelControl& c);  // puts a control's value in its widget
    PanelSpec spec_;
    std::map<QString, QWidget*> widgets_;
    QLabel* message_;
};

}  // namespace montage
