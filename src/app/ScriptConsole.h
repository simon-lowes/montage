// Montage — scripts in the app (automation/ScriptHost.h): the Script Console (write, open, save and run Lua against
// the open project, its output below) and the scripts folder the Tools › Scripts menu lists. A script's tools work on
// the open project through the agent link (app/LiveLink.h), and a whole run is one undo step, "Script: <name>". A
// script that calls montage.panel ends there and its panel opens (app/ScriptPanelWidget.h, through the PanelHost);
// a button on the panel runs it again with what the panel holds.
#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <optional>

#include "automation/ScriptPanel.h"

class QEventLoop;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace montage {

class EditorState;
class LiveLink;

// $MONTAGE_SCRIPTS_DIR, else "Scripts" in the app's data folder.
QString scriptsDirectory();
// The .lua files in it, by name.
QStringList listScripts();

// Where scripts' panels live (the main window's docks). A panel is known by its key: its script's file, or
// "console" for the console's unsaved script.
struct PanelHost {
    // A script's panel described (`code` is the script, to run again from the console; a file is read afresh).
    std::function<void(const QString& key, const QString& name, const QString& code, const PanelSpec& spec)> described;
    // montage.set_panel: values shown on the panel; false with the error.
    std::function<bool(const QString& key, const QJsonObject& values, QString* error)> set;
    // A button's run ended: what to show on the panel (its last line of output, or the error).
    std::function<void(const QString& key, const QString& message, bool error)> finished;
};

class ScriptConsole : public QWidget {
    Q_OBJECT
public:
    ScriptConsole(EditorState* state, LiveLink* link, QWidget* parent = nullptr);
    void setPanelHost(PanelHost host) { panels_ = std::move(host); }

    // Runs `code` (called `name`) on the open project and returns when it has ended (the window stays responsive,
    // and Stop works); false with the error in the output.
    bool run(const QString& code, const QString& name, const QStringList& args = {});
    bool runFile(const QString& path, const QStringList& args = {});
    // A panel's button pressed: the script run with `values` (its controls and "pressed") as montage.panel's answer.
    bool runPanel(const QString& key, const QString& code, const QString& name, const QJsonObject& values);
    // A panel opened again (at start-up): its script run until it describes the panel, its tools refused meanwhile.
    bool setUpPanel(const QString& path);
    void stop();  // the script stops at its next step; a tool running is not waited for, and its result not applied
    bool running() const { return running_; }

    bool open(const QString& path);  // into the editor
    bool save(const QString& path);  // the editor's script
    QPlainTextEdit* editor() const { return editor_; }
    QString output() const;
    void clearOutput();

signals:
    void finished(bool ok);

private:
    void print(const QString& text, bool error = false);
    void updateButtons();
    // A run: `key` names its panel, `values` answer montage.panel (a button pressed), `setUp` refuses tools.
    bool runScriptAs(const QString& code, const QString& name, const QStringList& args, const QString& key,
                     const std::optional<QJsonObject>& values, bool setUp);
    PanelHost panels_;

    EditorState* state_;
    LiveLink* link_;
    QPlainTextEdit* editor_;
    QPlainTextEdit* output_;
    QPushButton* run_;
    QPushButton* stop_;
    QLabel* file_;
    QString path_;  // the editor's script on disk ("" until saved)
    QString group_;  // the running script's undo group (its calls, for cancelling)
    QEventLoop* waiting_ = nullptr;  // the wait for a tool's answer, while there is one
    bool running_ = false, stopping_ = false;
    int runs_ = 0, calls_ = 0;
};

}  // namespace montage
