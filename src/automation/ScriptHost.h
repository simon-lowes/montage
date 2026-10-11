// Montage — scripts (Resolve's Lua and Python scripting, Premiere's UXP scripts, Avid's scripting): Lua 5.4 programs
// that drive Montage through the same tools as its MCP server and AI agents. In a script `montage.add_title{text =
// "Hello", at = 2}` calls montage_add_title (any tool by its name without "montage_", or montage.call("montage_...",
// args)); arguments are tables, results come back as tables (a tool's structured result, else {text = ...}), and a
// tool's error stops the script with its message. `montage.tools()` lists them, `montage.args` holds the script's
// arguments, and print() writes to the script's output. The host decides what a call works on: montage-cli runs
// scripts on project files, the app on the project open in it (each change one undo step).
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

namespace montage {

struct ScriptOptions {
    // Runs tool `name` with `args`; the MCP result (content, structuredContent, isError), or empty with `error`.
    std::function<QJsonObject(const QString& name, const QJsonObject& args, QString* error)> call;
    QStringList tools;  // their names, for montage.tools() (and the shorthand names)
    std::function<void(const QString&)> print;
    std::function<bool()> stopped;  // asked now and then: true stops the script
    std::function<void()> tick;     // called now and then while the script runs (the app keeps its window alive)
    QStringList args;
    QString project;  // montage.project: the project the script works on ("" if none)
};

class McpServer;
// Calls go to `server`; a tool that takes a project gets `project` when the script names none.
ScriptOptions toolScriptOptions(McpServer& server, const QString& project = {});

bool scriptingAvailable();  // built with Lua

// Runs Lua `code` (named `name` in errors); false with the error (its line, or "Stopped") when it fails or is stopped.
bool runScript(const QString& code, const QString& name, const ScriptOptions& options, QString* error = nullptr);

}  // namespace montage
