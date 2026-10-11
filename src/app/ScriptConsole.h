// Montage — scripts in the app (automation/ScriptHost.h): the Script Console (write, open, save and run Lua against
// the open project, its output below) and the scripts folder the Tools › Scripts menu lists. A script's tools work on
// the open project through the agent link (app/LiveLink.h), and a whole run is one undo step, "Script: <name>".
#pragma once

#include <QStringList>
#include <QWidget>

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

class ScriptConsole : public QWidget {
    Q_OBJECT
public:
    ScriptConsole(EditorState* state, LiveLink* link, QWidget* parent = nullptr);

    // Runs `code` (called `name`) on the open project and returns when it has ended (the window stays responsive,
    // and Stop works); false with the error in the output.
    bool run(const QString& code, const QString& name, const QStringList& args = {});
    bool runFile(const QString& path, const QStringList& args = {});
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
