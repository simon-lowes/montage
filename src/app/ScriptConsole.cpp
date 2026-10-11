#include "ScriptConsole.h"

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
#include <QSplitter>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "EditorState.h"
#include "LiveLink.h"
#include "Theme.h"
#include "automation/ScriptHost.h"

namespace montage {

QString scriptsDirectory() {
    const QString env = qEnvironmentVariable("MONTAGE_SCRIPTS_DIR");
    return !env.isEmpty() ? env : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Scripts");
}

QStringList listScripts() {
    QStringList out;
    const QDir dir(scriptsDirectory());
    for (const QString& f : dir.entryList({QStringLiteral("*.lua")}, QDir::Files, QDir::Name | QDir::IgnoreCase)) out << dir.filePath(f);
    return out;
}

namespace {

const char* kExample =
    "-- A Lua script on the open project: each tool is montage.<name>{...} (montage.tools() lists them).\n"
    "-- A whole run is one undo step.\n"
    "local here = montage.live_context{}\n"
    "print(\"Playhead at \" .. here.playhead)\n"
    "-- montage.add_marker{at = here.playhead, name = \"From a script\"}\n";

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
    auto* openBtn = new QPushButton(tr("Open…"), this);
    auto* saveBtn = new QPushButton(tr("Save…"), this);
    auto* clearBtn = new QPushButton(tr("Clear Output"), this);
    file_ = new QLabel(this);
    file_->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
    for (QPushButton* b : {run_, stop_, openBtn, saveBtn, clearBtn}) bar->addWidget(b);
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
    if (!scriptingAvailable()) print(tr("This build of Montage has no Lua, so scripts cannot run."), true);

    connect(run_, &QPushButton::clicked, this, [this] {
        runScriptAs(editor_->toPlainText(), path_.isEmpty() ? tr("Console") : QFileInfo(path_).fileName(), {},
                    path_.isEmpty() ? QStringLiteral("console") : path_, std::nullopt, false);
    });
    connect(stop_, &QPushButton::clicked, this, &ScriptConsole::stop);
    connect(openBtn, &QPushButton::clicked, this, [this] {
        QDir().mkpath(scriptsDirectory());
        const QString f = QFileDialog::getOpenFileName(this, tr("Open Script"), path_.isEmpty() ? scriptsDirectory() : path_,
                                                       tr("Lua scripts (*.lua);;All files (*)"));
        if (!f.isEmpty() && !open(f)) print(tr("Cannot read %1").arg(QDir::toNativeSeparators(f)), true);
    });
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        QDir().mkpath(scriptsDirectory());
        QString f = QFileDialog::getSaveFileName(this, tr("Save Script"), path_.isEmpty() ? scriptsDirectory() + QStringLiteral("/script.lua") : path_,
                                                 tr("Lua scripts (*.lua)"));
        if (f.isEmpty()) return;
        if (QFileInfo(f).suffix().isEmpty()) f += QStringLiteral(".lua");
        if (!save(f)) print(tr("Cannot write %1").arg(QDir::toNativeSeparators(f)), true);
    });
    connect(clearBtn, &QPushButton::clicked, this, &ScriptConsole::clearOutput);
    updateButtons();
}

void ScriptConsole::updateButtons() {
    run_->setEnabled(!running_ && scriptingAvailable());
    stop_->setEnabled(running_);
    file_->setText(path_.isEmpty() ? tr("Not saved") : QDir::toNativeSeparators(path_));
}

void ScriptConsole::print(const QString& text, bool error) {
    if (error) output_->appendHtml(QStringLiteral("<span style=\"color:%1\">%2</span>").arg(theme::kMeterRed.name(), text.toHtmlEscaped().replace('\n', "<br>")));
    else output_->appendPlainText(text);
}

QString ScriptConsole::output() const { return output_->toPlainText(); }
void ScriptConsole::clearOutput() { output_->clear(); }

bool ScriptConsole::open(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    editor_->setPlainText(QString::fromUtf8(f.readAll()));
    path_ = QFileInfo(path).absoluteFilePath();
    updateButtons();
    return true;
}

bool ScriptConsole::save(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(editor_->toPlainText().toUtf8()) < 0) return false;
    path_ = QFileInfo(path).absoluteFilePath();
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
    return runScriptAs(QString::fromUtf8(f.readAll()), QFileInfo(path).fileName(), {}, QFileInfo(path).absoluteFilePath(), std::nullopt, true);
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
    if (!scriptingAvailable()) {
        print(tr("This build of Montage has no Lua, so scripts cannot run."), true);
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
        if (values) return *values;
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
    o.stopped = [this] { return stopping_; };
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
    if (values && panels_.finished) panels_.finished(key, ok ? lastLine : err, !ok);
    running_ = stopping_ = false;
    updateButtons();
    emit finished(ok);
    return ok;
}

}  // namespace montage
