// Montage — Python scripts (Resolve's and Avid's Python scripting), next to Lua's (automation/ScriptHost.h): run by
// the Python 3 the computer has ($MONTAGE_PYTHON, else python3 or python on the PATH; 3.8 or later), so the user's
// own packages are there, as a child process that asks Montage for each tool. The same tools and the same `montage`
// module as Lua's table: `montage.add_title(text="Hello", at=2)` (keywords or a dict) calls montage_add_title,
// `montage.call("montage_...", {...})` any tool, results are dicts (a tool's structured result, its text as
// "message"; else {"text": ...}), and a tool's error raises montage.ToolError. montage.tools(), montage.args (also
// sys.argv[1:]), montage.project, montage.json(value), montage.panel(...) and montage.set_panel(...) as in Lua; print()
// and sys.stderr write to the script's output. Scripts have no standard input. Stop ends the process. A panel opening
// again by itself (quiet) runs with Python's audit hooks refusing programs and changes to files.
#pragma once

#include <QString>

namespace montage {

struct ScriptOptions;

// The Python found: $MONTAGE_PYTHON when set (only it), else python3 or python on the PATH; cached; empty with none
// of 3.8 or later.
QString pythonInterpreter();
bool pythonAvailable();
// Whether a script's file name makes it Python (".py").
bool isPythonScript(const QString& name);

// Runs Python `code` (named `name` in errors and tracebacks); false with the error ("<name>:<line>: <Type>: <message>",
// or "Stopped") when it fails or is stopped. The traceback's lines from the script itself go to its output first.
bool runPythonScript(const QString& code, const QString& name, const ScriptOptions& options, QString* error = nullptr);

}  // namespace montage
