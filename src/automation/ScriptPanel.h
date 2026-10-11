// Montage — a script's own panel (Resolve's Workflow Integrations, Premiere's UXP panels): what montage.panel{...}
// describes (automation/ScriptHost.h), checked, and the values its controls hold. The app builds the panel from this
// (app/ScriptPanelWidget.h), and montage-cli prints it or answers it from the command line.
//
//   local ui = montage.panel{title = "Rename Shots", controls = {
//     {type = "text", id = "prefix", label = "Prefix", value = "Shot_"},
//     {type = "number", id = "start", label = "Start at", value = 10, min = 0, max = 9999, step = 10},
//     {type = "check", id = "selected", label = "Selected clips only", value = true},
//     {type = "choice", id = "track", label = "Track", choices = {"V1", "V2"}},
//     {type = "slider", id = "amount", label = "Amount", min = 0, max = 100, value = 50},
//     {type = "label", id = "status", text = "Renames clips in timeline order"},
//     {type = "separator"},
//     {type = "button", id = "go", label = "Rename"},
//   }}
//   if ui.pressed == "go" then ... ui.prefix ... montage.set_panel{status = "Done"} end
#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

namespace montage {

struct PanelControl {
    QString type;   // text, number, check, choice, slider, label, separator, button
    QString id;     // what the script reads it by ("" for a label or separator without one)
    QString label;  // shown beside it (a button's caption)
    QString text;   // a label's text
    QJsonValue value;  // its value now: text and choice a string, number a number, slider an integer, check a boolean
    double min = 0, max = 100, step = 1;  // number and slider
    QStringList choices;                  // choice
    bool hasValue() const { return type != QLatin1String("label") && type != QLatin1String("separator") && type != QLatin1String("button"); }
};

struct PanelSpec {
    QString title;
    std::vector<PanelControl> controls;
    const PanelControl* find(const QString& id) const;
};

// What montage.panel was given, checked (types, unique ids, choices, ranges) with every default filled in; nothing
// with the error otherwise.
std::optional<PanelSpec> parsePanel(const QJsonObject& description, QString* error = nullptr);
// Its controls' values by id (labels by their text, where they have an id).
QJsonObject panelValues(const PanelSpec& spec);
// A value for control `id` (from a script's montage.set_panel, or text from the command line), made its control's
// type (numbers clamped to the range, a choice one of its own); false with the error for an unknown id, a button or a
// value of another kind. A label's value is its text.
bool setPanelValue(PanelSpec& spec, const QString& id, const QJsonValue& value, QString* error = nullptr);
// The panel as JSON (montage-cli prints it): its title and controls, every default filled in.
QJsonObject describePanel(const PanelSpec& spec);

}  // namespace montage
