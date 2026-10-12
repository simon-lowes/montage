#include "PythonHost.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <algorithm>

#include "ScriptHost.h"

namespace montage {

namespace {

// Read by the child from its first line of input: it takes over the pipes (the protocol on copies of them, standard
// output pointed at standard error and standard input at nothing, so programs it runs cannot break the protocol), says
// it is ready, reads the script and what goes with it, and runs it.
const char* kBoot = R"py(
import json, linecache, os, sys, threading, types, traceback

_in = open(os.dup(0), "rb")
_out = open(os.dup(1), "wb")
_null = os.open(os.devnull, os.O_RDONLY)
os.dup2(_null, 0)
os.dup2(2, 1)
# One message and its answer at a time, whichever thread the script calls from.
_lock = threading.RLock()


def _send(msg):
    with _lock:
        _out.write((json.dumps(msg, ensure_ascii=True, allow_nan=False) + "\n").encode("ascii"))
        _out.flush()


def _recv():
    line = _in.readline()
    if not line:
        os._exit(3)
    return json.loads(line)


def _ask(msg):
    with _lock:
        _send(msg)
        return _recv()


class _Output:
    encoding = "utf-8"
    errors = "strict"

    def __init__(self, err):
        self._err, self._buf = err, ""

    def write(self, s):
        if not isinstance(s, str):
            raise TypeError("write() argument must be str, not " + type(s).__name__)
        with _lock:
            # (A line ends at "\n", "\r\n" or a lone "\r", as progress bars write.)
            self._buf += s
            while True:
                i = min((self._buf.find(c) for c in "\r\n" if c in self._buf), default=-1)
                if i < 0 or (self._buf[i] == "\r" and i == len(self._buf) - 1):
                    break  # (a "\r" at the end may be followed by "\n")
                line = self._buf[:i]
                self._buf = self._buf[i + 2:] if self._buf.startswith("\r\n", i) else self._buf[i + 1:]
                _send({"t": "print", "text": line, "err": self._err})
        return len(s)

    def writelines(self, lines):
        for line in lines:
            self.write(line)

    def flush(self):
        # What is held is shown now (a prompt or progress written without a line end).
        with _lock:
            text = self._buf[:-1] if self._buf.endswith("\r") else self._buf
            if text:
                _send({"t": "print", "text": text, "err": self._err})
            self._buf = self._buf[len(text):]

    def end(self):
        with _lock:
            if self._buf.rstrip("\r"):
                _send({"t": "print", "text": self._buf.rstrip("\r"), "err": self._err})
            self._buf = ""

    def fileno(self):
        return 2 if self._err else 1  # (both are the host's output, for programs given them)

    def isatty(self):
        return False

    def writable(self):
        return True


class _NoInput:
    encoding = "utf-8"

    def read(self, *args):
        raise OSError("scripts have no standard input to read")

    readline = readlines = read

    def __iter__(self):
        return self

    def __next__(self):
        self.read()

    def fileno(self):
        return 0

    def isatty(self):
        return False

    def readable(self):
        return True


_send({"t": "ready"})
_init = _recv()
_stdout, _stderr = _Output(False), _Output(True)
sys.stdout, sys.stderr, sys.stdin = _stdout, _stderr, _NoInput()
_name = _init["name"]
_file = _init.get("file") or _name  # (tracebacks and __file__ name the file when there is one)
_tools = list(_init["tools"])


class ToolError(Exception):
    """A tool's error (or montage.panel's)."""


def _plain(v, depth=0, count=None):
    # What a tool's argument can be in JSON: an error rather than a different value.
    count = count if count is not None else [0]
    count[0] += 1
    if count[0] > 1000000:
        raise ValueError("a tool's argument is too large (a million values)")
    if depth > 64:
        raise ValueError("an argument nested too deeply (or holding itself)")
    if v is None or isinstance(v, (bool, str)):
        return v
    if isinstance(v, int):
        if abs(v) > 2 ** 53:
            raise ValueError("%d is too large for a tool's argument" % v)
        return v
    if isinstance(v, float):
        if v != v or v in (float("inf"), float("-inf")):
            raise ValueError("a tool's argument cannot be %s" % ("NaN" if v != v else "infinite"))
        return v
    if isinstance(v, dict):
        out = {}
        for k, x in v.items():
            if not isinstance(k, (str, int, float)) or isinstance(k, bool):
                raise ValueError("a key in a tool's argument must be a string or a number")
            out[str(k)] = _plain(x, depth + 1, count)
        return out
    if isinstance(v, (list, tuple)):
        return [_plain(x, depth + 1, count) for x in v]
    raise ValueError("a tool's argument cannot be a " + type(v).__name__)


def _args(args, kw):
    a = {}
    if args is not None:
        if not isinstance(args, dict):
            raise TypeError("a tool's arguments are a dict or keywords")
        a.update(args)
    a.update(kw)
    return _plain(a)


def call(name, args=None, /, **kw):
    """Runs tool `name` (montage_...) with `args` (a dict) or keywords (any name: `name` and `args` too); its result as a
    dict."""
    r = _ask({"t": "call", "name": str(name), "args": _args(args, kw)})
    if "error" in r:
        raise ToolError(r["error"])
    return r["result"]


def tools():
    """The tools' names."""
    return list(_tools)


def to_json(value):
    """A value as JSON text, to print a result."""
    return json.dumps(_plain(value), indent=4, ensure_ascii=False)


def _end(ok, error=None):
    _stdout.end()
    _stderr.end()
    _send({"t": "done"} if ok else {"t": "failed", "error": error})
    os._exit(0)


def panel(description=None, /, **kw):
    """The script's panel: described (the script ends here) or answered with what it holds."""
    with _lock:
        r = _ask({"t": "panel", "description": _args(description, kw)})
        if r.get("end"):
            _end(True)
    if "error" in r:
        raise ToolError(r["error"])
    return r["values"]


def set_panel(values=None, /, **kw):
    """Values shown on the script's panel (a label's value is its text)."""
    r = _ask({"t": "set_panel", "values": _args(values, kw)})
    if "error" in r:
        raise ToolError(r["error"])


_m = types.ModuleType("montage", "Montage's tools, as the MCP server's (montage.tools() lists them).")
_m.ToolError, _m.call, _m.tools, _m.json, _m.panel, _m.set_panel = ToolError, call, tools, to_json, panel, set_panel
_m.args, _m.project, _m.null = list(_init["args"]), _init["project"], None


def _tool(attr):
    name = attr if attr.startswith("montage_") else "montage_" + attr
    if attr.startswith("__") or name not in _tools:
        raise AttributeError("no tool montage.%s (montage.tools() lists them)" % attr)
    return lambda args=None, /, **kw: call(name, args, **kw)


_m.__getattr__ = _tool
sys.modules["montage"] = _m
sys.argv = [_file] + list(_init["args"])
if _init.get("file"):
    sys.path.insert(0, os.path.dirname(_file))  # (as `python script.py`: modules beside it)

if _init.get("quiet"):
    # A panel opening again by itself: nothing run, written, moved or removed (the user did not ask). (A guard against
    # a script doing so unawares, as Lua's: audit hooks are no sandbox.)
    def _quiet_hook():
        write = os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC | os.O_APPEND
        blocked = frozenset({
            "subprocess.Popen", "os.system", "os.posix_spawn", "os.fork", "os.forkpty", "os.remove", "os.rmdir",
            "os.rename", "os.mkdir", "os.chmod", "os.chown", "os.truncate", "os.link", "os.symlink", "os.utime",
            "os.startfile", "os.kill", "shutil.rmtree", "shutil.move", "shutil.copyfile", "shutil.copytree",
            "shutil.chown", "_winapi.CreateProcess", "winreg.SetValue", "winreg.SetValueEx", "winreg.CreateKey",
            "winreg.DeleteKey", "winreg.DeleteValue"})

        def hook(event, args):
            if event in blocked or event.startswith(("os.exec", "os.spawn", "ctypes.", "_ctypes", "socket.")):
                raise PermissionError(event + " is not available while a panel opens")
            if event == "open":
                mode, flags = args[1], args[2]
                if (mode is not None and any(c in str(mode) for c in "wax+")) or (mode is None and flags & write):
                    raise PermissionError("open: only reading while a panel opens")
            if event == "sqlite3.connect" and str(args[0]) not in (":memory:", ""):
                raise PermissionError("sqlite3.connect: only in memory while a panel opens")

        return hook

    sys.addaudithook(_quiet_hook())
    del _quiet_hook
    try:
        import _posixsubprocess  # (multiprocessing starts its Python through it, with no audit event)

        def _refused(*args, **kw):
            raise PermissionError("starting programs is not available while a panel opens")

        _posixsubprocess.fork_exec = _refused
    except ImportError:
        pass

_code = _init["code"]
if _code.startswith("\ufeff"):
    _code = _code[1:]
# (Its traceback shows the lines that ran, even when the file has changed since or there is none.)
linecache.cache[_file] = (len(_code), None, _code.splitlines(True), _file)
# (The script's own __main__, as `python script.py` gives it.)
_main = types.ModuleType("__main__")
_main.__file__, _main.montage = _file, _m
sys.modules["__main__"] = _main
try:
    exec(compile(_code, _file, "exec"), _main.__dict__)
except SystemExit as e:
    if e.code is None or e.code == 0:
        _end(True)
    _end(False, "%s: exited with %s" % (_name, e.code))
except BaseException as e:
    # Where in the script it went wrong: its last line there (or the syntax error's), and its own lines of the
    # traceback (Montage's are no help to the script's author).
    frames = [f for f in traceback.extract_tb(e.__traceback__) if f.filename == _file]
    syntax = isinstance(e, SyntaxError) and e.filename == _file
    line = e.lineno if syntax else (frames[-1].lineno if frames else None)
    if len(frames) > 1:
        sys.stderr.write("Traceback (most recent call last):\n" + "".join(traceback.format_list(frames)))
    # (The whole message, however many lines; a syntax error's as Python words it.)
    text = str(e)
    what = (type(e).__name__ + ": " + e.msg if syntax else type(e).__name__ + (": " + text if text else ""))
    _end(False, ("%s:%d: " % (_name, line) if line else _name + ": ") + what)
_end(True)
)py";

QByteArray line(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n'; }

// The interpreter `exe` runs, when it is Python 3.8 or later (a launcher's or shim's own Python; else empty).
QString usablePython(const QString& exe) {
    QProcess p;
    p.start(exe, {QStringLiteral("-c"), QStringLiteral("import sys; print(sys.version_info >= (3, 8)); print(sys.executable)")});
    if (!p.waitForFinished(5000) || p.exitStatus() != QProcess::NormalExit) {
        p.kill();
        p.waitForFinished(1000);
        return {};
    }
    const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    if (lines.value(0).trimmed() != QLatin1String("True")) return {};
    const QString real = lines.value(1).trimmed();
    return !real.isEmpty() && QFileInfo(real).isExecutable() ? real : exe;
}

}  // namespace

QString pythonInterpreter() {
    static const QString found = [] {
        // (One named is the one used: not another behind the user's back when it is not usable.)
        const QString env = qEnvironmentVariable("MONTAGE_PYTHON");
        if (!env.isEmpty()) return usablePython(QFileInfo(env).isAbsolute() ? env : QStandardPaths::findExecutable(env));
        QStringList names{QStringLiteral("python3"), QStringLiteral("python")};
#ifdef Q_OS_WIN
        names << QStringLiteral("py");  // (the launcher, when python.exe is only the Store's alias)
#endif
        for (const QString& n : names) {
#ifdef Q_OS_MACOS
            // (An app opened from the Finder has a short PATH: Homebrew's and python.org's Python before the system's
            // /usr/bin/python3, which may only offer to install the developer tools.)
            QString exe = QStandardPaths::findExecutable(n, {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")});
            if (exe.isEmpty()) exe = QStandardPaths::findExecutable(n);
#else
            const QString exe = QStandardPaths::findExecutable(n);
#endif
            if (exe.isEmpty()) continue;
            if (const QString real = usablePython(exe); !real.isEmpty()) return real;
        }
        return QString();
    }();
    return found;
}

bool pythonAvailable() { return !pythonInterpreter().isEmpty(); }

bool isPythonScript(const QString& name) { return name.endsWith(QLatin1String(".py"), Qt::CaseInsensitive); }

bool runPythonScript(const QString& code, const QString& name, const ScriptOptions& options, QString* error) {
    auto fail = [error](const QString& e) {
        if (error) *error = e;
        return false;
    };
    const QString exe = pythonInterpreter();
    if (exe.isEmpty()) return fail(QStringLiteral("No Python 3.8 or later was found: install one, or set MONTAGE_PYTHON to it"));
    QProcess proc;
    // (-u: nothing held back; -X utf8: text as UTF-8 whatever the locale)
    proc.start(exe, {QStringLiteral("-u"), QStringLiteral("-X"), QStringLiteral("utf8"), QStringLiteral("-c"),
                     QStringLiteral("import sys\n"
                                    // (Not the working directory's modules in place of the standard ones.)
                                    "if sys.path[:1] == ['']: del sys.path[0]\n"
                                    "import json\n"
                                    "exec(json.loads(sys.stdin.buffer.readline()))")});
    if (!proc.waitForStarted(10000)) return fail(QStringLiteral("Python could not start: %1").arg(proc.errorString()));
    proc.write(QJsonDocument(QJsonArray{QString::fromUtf8(kBoot)}).toJson(QJsonDocument::Compact).mid(1).chopped(1) + '\n');
    auto stop = [&] {
        proc.kill();
        proc.waitForFinished(5000);
        return fail(QStringLiteral("Stopped"));
    };
    QStringList tools = options.tools;
    QJsonArray args;
    for (const QString& a : options.args) args.append(a);
    const QJsonObject init{{"code", code}, {"name", name}, {"args", args}, {"project", options.project},
                           {"tools", QJsonArray::fromStringList(tools)}, {"quiet", options.quiet}, {"file", options.file}};
    QByteArray out, err;
    QString lastErr;  // the end of what it wrote to standard error (an interpreter that dies says why there)
    bool ended = false, ok = false, initSent = false;
    QString failure;
    auto print = [&](const QString& text) {
        if (options.print) options.print(text);
    };
    // One message from the script.
    auto handle = [&](const QByteArray& raw) {
        const QJsonObject m = QJsonDocument::fromJson(raw).object();
        const QString t = m.value("t").toString();
        if (t == QLatin1String("ready")) {
            if (!initSent) proc.write(line(init));
            initSent = true;
        } else if (t == QLatin1String("print")) {
            print(m.value("text").toString());
        } else if (t == QLatin1String("call")) {
            const QString tool = m.value("name").toString();
            if (options.stopped && options.stopped()) return false;  // (before the tool: a stopped script changes nothing)
            QString e;
            const QJsonObject result = options.call ? options.call(tool, m.value("args").toObject(), &e) : QJsonObject();
            if (options.stopped && options.stopped()) return false;
            QString text;
            for (const QJsonValue& c : result.value("content").toArray())
                if (c.toObject().value("type").toString() == QLatin1String("text")) text += c.toObject().value("text").toString();
            if (result.isEmpty()) {
                proc.write(line({{"error", QStringLiteral("%1: %2").arg(tool, e.isEmpty() ? QStringLiteral("no answer") : e)}}));
            } else if (result.value("isError").toBool()) {
                proc.write(line({{"error", QStringLiteral("%1: %2").arg(tool, text)}}));
            } else if (result.contains("structuredContent")) {
                QJsonObject r = result.value("structuredContent").toObject();
                if (!text.isEmpty()) r["message"] = text;
                proc.write(line({{"result", r}}));
            } else {
                proc.write(line({{"result", QJsonObject{{"text", text}}}}));
            }
        } else if (t == QLatin1String("panel")) {
            if (!options.panel) {
                proc.write(line({{"error", QStringLiteral("montage.panel: panels open in the Montage app (montage-cli script --press <button> answers one)")}}));
                return true;
            }
            if (options.stopped && options.stopped()) return false;
            QString e;
            const std::optional<QJsonObject> values = options.panel(m.value("description").toObject(), &e);
            if (!e.isEmpty()) proc.write(line({{"error", QStringLiteral("montage.panel: %1").arg(e)}}));
            else if (!values) proc.write(line({{"end", true}}));  // described: it runs again when a button is pressed
            else proc.write(line({{"values", *values}}));
        } else if (t == QLatin1String("set_panel")) {
            QString e;
            if (!options.setPanel) proc.write(line({{"error", QStringLiteral("montage.set_panel: this script has no panel open")}}));
            else if (!options.setPanel(m.value("values").toObject(), &e)) proc.write(line({{"error", QStringLiteral("montage.set_panel: %1").arg(e)}}));
            else proc.write(line({{"ok", true}}));
        } else if (t == QLatin1String("done")) {
            ended = ok = true;
        } else if (t == QLatin1String("failed")) {
            ended = true;
            failure = m.value("error").toString();
        } else if (!raw.trimmed().isEmpty()) {
            print(QString::fromUtf8(raw));  // (not a message: shown as it is)
        }
        return true;
    };
    auto drain = [&] {
        out += proc.readAllStandardOutput();
        err += proc.readAllStandardError();
        for (;;) {
            // What programs it runs (or Python itself) write: the script's output too, a line at each "\n", "\r\n" or
            // lone "\r" (progress written over itself).
            const qsizetype n = err.indexOf('\n'), r = err.indexOf('\r');
            const qsizetype nl = n < 0 ? r : r < 0 ? n : std::min(n, r);
            if (nl < 0 || (err[nl] == '\r' && nl == err.size() - 1)) break;
            const QString text = QString::fromUtf8(err.left(nl));
            err.remove(0, err[nl] == '\r' && nl + 1 < err.size() && err[nl + 1] == '\n' ? nl + 2 : nl + 1);
            print(text);
            lastErr = (lastErr + '\n' + text).right(2000);
        }
        for (qsizetype nl; (nl = out.indexOf('\n')) >= 0;) {
            const QByteArray raw = out.left(nl);
            out.remove(0, nl + 1);
            if (!handle(raw)) return false;
        }
        return true;
    };
    for (;;) {
        if (options.stopped && options.stopped()) return stop();
        if (options.tick) options.tick();
        proc.waitForReadyRead(20);
        if (!drain()) return stop();
        if (proc.state() == QProcess::NotRunning) {
            if (!drain()) return stop();
            break;
        }
    }
    if (err.endsWith('\r')) err.chop(1);
    if (!err.isEmpty()) print(QString::fromUtf8(err)), lastErr += QString::fromUtf8(err);
    if (ended) return ok || fail(failure);
    if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) return true;  // (os._exit(0))
    const QString why = lastErr.trimmed().section('\n', -1);
    return fail(QStringLiteral("%1: Python ended (exit code %2)%3").arg(name).arg(proc.exitCode()).arg(why.isEmpty() ? QString() : ": " + why));
}

}  // namespace montage
