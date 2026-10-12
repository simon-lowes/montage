#include "ScriptConsole.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "EditorState.h"
#include "LiveLink.h"
#include "Theme.h"
#include "automation/PythonHost.h"
#include "automation/ScriptHost.h"

namespace montage {

QString scriptsDirectory() {
    const QString env = qEnvironmentVariable("MONTAGE_SCRIPTS_DIR");
    return !env.isEmpty() ? env : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Scripts");
}

QStringList listScripts() {
    QStringList out;
    const QDir dir(scriptsDirectory());
    for (const QString& f : dir.entryList({QStringLiteral("*.lua"), QStringLiteral("*.py")}, QDir::Files, QDir::Name | QDir::IgnoreCase)) out << dir.filePath(f);
    return out;
}

namespace {

const char* kExample =
    "-- A Lua script on the open project: each tool is montage.<name>{...} (montage.tools() lists them).\n"
    "-- A whole run is one undo step.\n"
    "local here = montage.live_context{}\n"
    "print(\"Playhead at \" .. here.playhead)\n"
    "-- montage.add_marker{at = here.playhead, name = \"From a script\"}\n";

const char* kPythonExample =
    "# A Python script on the open project: each tool is montage.<name>(...) (montage.tools() lists them).\n"
    "# A whole run is one undo step.\n"
    "here = montage.live_context()\n"
    "print(\"Playhead at\", here[\"playhead\"])\n"
    "# montage.add_marker(at=here[\"playhead\"], name=\"From a script\")\n";

QString noRunner(bool python) {
    return python ? ScriptConsole::tr("No Python 3.8 or later was found (set MONTAGE_PYTHON to one), so Python scripts cannot run.")
                  : ScriptConsole::tr("This build of Montage has no Lua, so Lua scripts cannot run.");
}

QByteArray compact(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

}  // namespace

ScriptConsole::ScriptConsole(EditorState* state, LiveLink* link, QWidget* parent) : QWidget(parent), state_(state), link_(link) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    auto* bar = new QHBoxLayout;
    run_ = new QPushButton(tr("Run"), this);
    run_->setObjectName(QStringLiteral("runScript"));
    run_->setToolTip(tr("Run the script on the open project (Ctrl+Return); one undo step"));
    // (Only in the console: the button's own shortcut would take Ctrl+Return from the whole window.)
    auto* runKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
    connect(runKey, &QShortcut::activated, run_, &QPushButton::click);
    stop_ = new QPushButton(tr("Stop"), this);
    stop_->setObjectName(QStringLiteral("stopScript"));
    language_ = new QComboBox(this);
    language_->setObjectName(QStringLiteral("scriptLanguage"));
    language_->addItems({QStringLiteral("Lua"), QStringLiteral("Python")});
    language_->setToolTip(tr("The script's language (a script opened or saved follows its file's)"));
    auto* openBtn = new QPushButton(tr("Open…"), this);
    auto* saveBtn = new QPushButton(tr("Save…"), this);
    auto* clearBtn = new QPushButton(tr("Clear Output"), this);
    file_ = new QLabel(this);
    file_->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
    for (QPushButton* b : {run_, stop_}) bar->addWidget(b);
    bar->addWidget(language_);
    for (QPushButton* b : {openBtn, saveBtn, clearBtn}) bar->addWidget(b);
    bar->addWidget(file_, 1);
    v->addLayout(bar);
    auto* split = new QSplitter(Qt::Vertical, this);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    editor_ = new QPlainTextEdit(split);
    editor_->setObjectName(QStringLiteral("scriptEditor"));
    editor_->setFont(mono);
    editor_->setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(QLatin1Char(' ')) * 4);
    editor_->setPlainText(QString::fromLatin1(kExample));
    output_ = new QPlainTextEdit(split);
    output_->setObjectName(QStringLiteral("scriptOutput"));
    output_->setFont(mono);
    output_->setReadOnly(true);
    output_->setMaximumBlockCount(5000);
    split->addWidget(editor_);
    split->addWidget(output_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);
    if (!scriptingAvailable()) print(noRunner(false), true);

    connect(language_, &QComboBox::currentIndexChanged, this, [this](int i) {
        const QString was = QString::fromLatin1(i == 1 ? kExample : kPythonExample);
        if (editor_->toPlainText() == was) editor_->setPlainText(QString::fromLatin1(i == 1 ? kPythonExample : kExample));
        if (i == 1 && !pythonAvailable()) print(noRunner(true), true);
        updateButtons();
    });
    connect(run_, &QPushButton::clicked, this, [this] {
        // (The console's panel runs the console's script: the file on disk may differ from what is in the editor. The
        // name says the language: the file's, unless another one is picked.)
        const bool file = !path_.isEmpty() && isPythonScript(path_) == python();
        const QString name = file ? QFileInfo(path_).fileName() : python() ? tr("Console") + QStringLiteral(".py") : tr("Console");
        runScriptAs(editor_->toPlainText(), name, {}, QStringLiteral("console"), std::nullopt, false);
    });
    connect(stop_, &QPushButton::clicked, this, &ScriptConsole::stop);
    connect(openBtn, &QPushButton::clicked, this, [this] {
        QDir().mkpath(scriptsDirectory());
        const QString f = QFileDialog::getOpenFileName(this, tr("Open Script"), path_.isEmpty() ? scriptsDirectory() : path_,
                                                       tr("Scripts (*.lua *.py);;Lua scripts (*.lua);;Python scripts (*.py);;All files (*)"));
        if (!f.isEmpty() && !open(f)) print(tr("Cannot read %1").arg(QDir::toNativeSeparators(f)), true);
    });
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        QDir().mkpath(scriptsDirectory());
        // (A file of the other language is offered again with this one's extension.)
        const QString ext = python() ? QStringLiteral(".py") : QStringLiteral(".lua");
        const QString proposed = path_.isEmpty() ? scriptsDirectory() + QStringLiteral("/script") + ext
                                 : isPythonScript(path_) == python() ? path_
                                                                     : QFileInfo(path_).path() + '/' + QFileInfo(path_).completeBaseName() + ext;
        QString f = QFileDialog::getSaveFileName(this, tr("Save Script"), proposed,
                                                 python() ? tr("Python scripts (*.py)") : tr("Lua scripts (*.lua)"));
        if (f.isEmpty()) return;
        if (QFileInfo(f).suffix().isEmpty()) f += ext;
        if (!save(f)) print(tr("Cannot write %1").arg(QDir::toNativeSeparators(f)), true);
    });
    connect(clearBtn, &QPushButton::clicked, this, &ScriptConsole::clearOutput);
    updateButtons();
}

void ScriptConsole::updateButtons() {
    // (Python's is looked for when it is picked, not here.)
    run_->setEnabled(!running_ && (python() || scriptingAvailable()));
    stop_->setEnabled(running_);
    file_->setText(path_.isEmpty() ? tr("Not saved") : QDir::toNativeSeparators(path_));
}

void ScriptConsole::print(const QString& text, bool error) {
    if (error) output_->appendHtml(QStringLiteral("<span style=\"color:%1\">%2</span>").arg(theme::kMeterRed.name(), text.toHtmlEscaped().replace('\n', "<br>")));
    else output_->appendPlainText(text);
}

QString ScriptConsole::output() const { return output_->toPlainText(); }
void ScriptConsole::clearOutput() { output_->clear(); }

bool ScriptConsole::python() const { return language_->currentIndex() == 1; }
void ScriptConsole::setPython(bool on) { language_->setCurrentIndex(on ? 1 : 0); }

void ScriptConsole::followFile() {
    // (What is in the editor stays: no example swapped in.)
    const QSignalBlocker block(language_);
    language_->setCurrentIndex(isPythonScript(path_) ? 1 : 0);
}

bool ScriptConsole::open(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    editor_->setPlainText(QString::fromUtf8(f.readAll()));
    path_ = QFileInfo(path).absoluteFilePath();
    followFile();
    updateButtons();
    return true;
}

bool ScriptConsole::save(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(editor_->toPlainText().toUtf8()) < 0) return false;
    path_ = QFileInfo(path).absoluteFilePath();
    followFile();
    updateButtons();
    return true;
}

bool ScriptConsole::runFile(const QString& path, const QStringList& args) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        print(tr("Cannot read %1").arg(QDir::toNativeSeparators(path)), true);
        return false;
    }
    return runScriptAs(QString::fromUtf8(f.readAll()), QFileInfo(path).fileName(), args, QFileInfo(path).absoluteFilePath(), std::nullopt, false);
}

bool ScriptConsole::runPanel(const QString& key, const QString& code, const QString& name, const QJsonObject& values) {
    return runScriptAs(code, name, {}, key, values, false);
}

bool ScriptConsole::setUpPanel(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        print(tr("Cannot read %1").arg(QDir::toNativeSeparators(path)), true);
        return false;
    }
    runScriptAs(QString::fromUtf8(f.readAll()), QFileInfo(path).fileName(), {}, QFileInfo(path).absoluteFilePath(), std::nullopt, true);
    return lastSetUpDescribed_;
}

void ScriptConsole::stop() {
    if (!running_) return;
    stopping_ = true;
    // A tool still running is not waited for (nor applied).
    link_->cancelScript(group_);
    if (waiting_) waiting_->quit();
}

bool ScriptConsole::run(const QString& code, const QString& name, const QStringList& args) {
    return runScriptAs(code, name, args, QStringLiteral("console"), std::nullopt, false);
}

bool ScriptConsole::runScriptAs(const QString& code, const QString& name, const QStringList& args, const QString& key,
                                const std::optional<QJsonObject>& values, bool setUp) {
    if (running_) return false;
    if (!scriptAvailable(name)) {
        print(noRunner(isPythonScript(name)), true);
        lastSetUpDescribed_ = false;
        return false;
    }
    running_ = true;
    stopping_ = false;
    updateButtons();
    const QString label = tr("Script: %1").arg(name), group = QStringLiteral("script-run-%1").arg(++runs_);
    group_ = group;
    state_->setUndoGroup(group);
    const uint64_t generation = state_->projectGeneration();  // the project it runs on
    print(tr("Running %1…").arg(name));

    ScriptOptions o;
    // The tools, as the agent link offers them (on the open project unless a script names another file).
    QByteArray list;
    link_->handle(compact(QJsonObject{{"jsonrpc", "2.0"}, {"id", ++calls_}, {"method", "tools/list"}}), [&list](QByteArray a) { list = std::move(a); });
    for (const QJsonValue& t : QJsonDocument::fromJson(list).object().value("result").toObject().value("tools").toArray())
        o.tools << t.toObject().value("name").toString();
    o.project = state_->filePath();
    if (key != QLatin1String("console")) o.file = key;  // (a file's key is its path)
    o.args = args;
    o.noStdin = true;
    QString lastLine;  // (a panel shows its run's last line)
    o.print = [this, &lastLine](const QString& line) {
        print(line);
        lastLine = line.section(QLatin1Char('\n'), -1);
    };
    // Its panel: described (the script ends there and the panel opens), or answered with what it holds.
    std::optional<PanelSpec> described;
    o.panel = [&](const QJsonObject& d, QString* error) -> std::optional<QJsonObject> {
        std::optional<PanelSpec> spec = parsePanel(d, error);
        if (!spec) return std::nullopt;
        if (values) {
            // The script as it is now may describe another panel than the one pressed (edited since): what the panel
            // holds goes into the new one where a control of that id and kind is still there, new controls bring their
            // defaults, and the panel is rebuilt to match.
            const std::optional<PanelSpec> open = panels_.current ? panels_.current(key) : std::nullopt;
            if (open && samePanelLayout(*open, *spec)) return *values;
            PanelSpec next = *spec;
            for (auto it = values->begin(); it != values->end(); ++it)
                if (const PanelControl* c = next.find(it.key()); c && (!open || !open->find(it.key()) || open->find(it.key())->type == c->type))
                    setPanelValue(next, it.key(), it.value());  // (a value that no longer fits keeps the default)
            const QString pressed = values->value("pressed").toString();
            const PanelControl* button = next.find(pressed);
            if (!button || button->type != QLatin1String("button")) {
                *error = tr("the panel has changed: it has no button \"%1\" now (pressed again, it is rebuilt)").arg(pressed);
                if (panels_.described) panels_.described(key, name, code, next);
                return std::nullopt;
            }
            if (panels_.described) panels_.described(key, name, code, next);
            QJsonObject merged = panelValues(next);
            merged["pressed"] = pressed;
            return merged;
        }
        if (!panels_.described) {
            *error = tr("panels open in the main window");
            return std::nullopt;
        }
        described = std::move(spec);
        return std::nullopt;
    };
    o.setPanel = [&](const QJsonObject& v, QString* error) {
        if (!values || !panels_.set) {
            *error = tr("call it after montage.panel, from a press of the panel's button");
            return false;
        }
        return panels_.set(key, v, error);
    };
    QElapsedTimer started;
    started.start();
    // A panel opening by itself is given five seconds (a script that runs on without describing it is ended).
    o.stopped = [this, setUp, &started] { return stopping_ || (setUp && started.elapsed() > 5000); };
    o.quiet = setUp;
    QElapsedTimer sinceEvents;
    sinceEvents.start();
    o.tick = [&sinceEvents] {
        if (sinceEvents.elapsed() < 30) return;
        QCoreApplication::processEvents();
        sinceEvents.restart();
    };
    o.call = [this, label, group, generation, setUp](const QString& tool, const QJsonObject& a, QString* error) -> QJsonObject {
        // A panel being opened again: the script runs only as far as montage.panel, so nothing changes.
        if (setUp) {
            if (error) *error = tr("a panel opening again runs no tools: call montage.panel before any");
            return {};
        }
        // Another project opened since it started: its calls would edit that one, so none runs.
        if (state_->projectGeneration() != generation) {
            if (error) *error = tr("the project the script was running on was closed");
            return {};
        }
        const QJsonObject msg{{"jsonrpc", "2.0"}, {"id", ++calls_}, {"method", "tools/call"}, {"params", QJsonObject{{"name", tool}, {"arguments", a}}}};
        // (Shared: Stop leaves the wait before the answer comes, and the answer may still come later.)
        struct Pending {
            QByteArray answer;
            bool answered = false;
            QEventLoop* loop = nullptr;
        };
        auto pending = std::make_shared<Pending>();
        QEventLoop wait;
        pending->loop = &wait;
        link_->handleScript(compact(msg), [pending](QByteArray r) {
            pending->answer = std::move(r);
            pending->answered = true;
            if (pending->loop) pending->loop->quit();
        }, label, group);
        if (!pending->answered && !stopping_) {
            waiting_ = &wait;
            wait.exec();
            waiting_ = nullptr;
        }
        pending->loop = nullptr;
        if (!pending->answered) return {};  // stopped meanwhile (the host raises "Stopped")
        const QJsonObject reply = QJsonDocument::fromJson(pending->answer).object();
        if (reply.contains("error")) {
            if (error) *error = reply.value("error").toObject().value("message").toString();
            return {};
        }
        return reply.value("result").toObject();
    };
    QString err;
    const bool ok = runScript(code, name, o, &err);
    state_->setUndoGroup({});
    group_.clear();
    if (ok && described) print(tr("Panel \"%1\" open.").arg(described->title));
    else if (ok) print(tr("Done."));
    else print(err, true);
    if (ok && described) panels_.described(key, name, code, *described);
    lastSetUpDescribed_ = !setUp || (ok && described);
    if (values && panels_.finished) panels_.finished(key, ok ? lastLine : err, !ok);
    running_ = stopping_ = false;
    updateButtons();
    emit finished(ok);
    return ok;
}

}  // namespace montage
