#include "ScriptHost.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <cmath>
#include <cstdlib>

#include "automation/McpServer.h"

#ifdef MONTAGE_WITH_LUA
// (Lua is compiled as C++ here: its headers without lua.hpp's extern "C", and its errors are C++ exceptions.)
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#endif

namespace montage {

ScriptOptions toolScriptOptions(McpServer& server, const QString& project) {
    ScriptOptions o;
    o.project = project;
    auto request = [&server](const QString& method, const QJsonObject& params) {
        static int id = 0;
        QJsonObject msg{{"jsonrpc", "2.0"}, {"id", ++id}, {"method", method}};
        if (!params.isEmpty()) msg["params"] = params;
        const std::vector<std::string> lines = server.handle(QJsonDocument(msg).toJson(QJsonDocument::Compact).toStdString());
        return lines.empty() ? QJsonObject() : QJsonDocument::fromJson(QByteArray::fromStdString(lines.back())).object();
    };
    auto takesProject = std::make_shared<QStringList>();
    for (const QJsonValue& v : request("tools/list", {}).value("result").toObject().value("tools").toArray()) {
        const QJsonObject t = v.toObject();
        o.tools << t.value("name").toString();
        if (t.value("inputSchema").toObject().value("properties").toObject().contains("project")) *takesProject << t.value("name").toString();
    }
    o.call = [request, takesProject, project](const QString& name, const QJsonObject& args, QString* error) {
        QJsonObject a = args;
        if (!project.isEmpty() && !a.contains("project") && takesProject->contains(name)) a["project"] = project;
        const QJsonObject reply = request("tools/call", QJsonObject{{"name", name}, {"arguments", a}});
        if (reply.contains("error")) {
            if (error) *error = reply.value("error").toObject().value("message").toString();
            return QJsonObject();
        }
        return reply.value("result").toObject();
    };
    return o;
}

#ifndef MONTAGE_WITH_LUA

bool scriptingAvailable() { return false; }
bool runScript(const QString&, const QString&, const ScriptOptions&, QString* error) {
    if (error) *error = QStringLiteral("This build of Montage has no Lua");
    return false;
}

#else

bool scriptingAvailable() { return true; }

namespace {

const char* kHostKey = "montage.host";
const char* kStopKey = "montage.stop";  // the error a stopped script is ended with (no pcall keeps it)
const char* kDoneKey = "montage.done";  // and a script that has described its panel (it ends there, successfully)
const char* kEndingKey = "montage.ending";  // which of the two is ending the script (the hook raises it again)
const char* kStopped = "Stopped";

const ScriptOptions& host(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kHostKey);
    const auto* o = static_cast<const ScriptOptions*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *o;
}

void hook(lua_State* L, lua_Debug*);

// Ends the script with the value under `key` (stopped, or done), which pcall, xpcall and coroutine.resume pass on (see
// kPrelude), and the hook from now on at every instruction raising it again, so a script that catches it anyway
// cannot go on for long.
[[noreturn]] void endScript(lua_State* L, const char* key) {
    lua_getfield(L, LUA_REGISTRYINDEX, key);
    lua_setfield(L, LUA_REGISTRYINDEX, kEndingKey);
    lua_sethook(L, hook, LUA_MASKCOUNT, 1);
    // The main thread too (raised in a coroutine, it could be caught where the prelude does not reach).
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    if (lua_State* main = lua_tothread(L, -1); main && main != L) lua_sethook(main, hook, LUA_MASKCOUNT, 1);
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, key);
    lua_error(L);
    std::abort();  // (lua_error does not return)
}

[[noreturn]] void stopScript(lua_State* L) { endScript(L, kStopKey); }

// JSON null in a result's arrays and as montage.null (a NULL light userdata, as lua-cjson's).
bool isNull(lua_State* L, int index) { return lua_islightuserdata(L, index) && !lua_touserdata(L, index); }

void push(lua_State* L, const QJsonValue& v);

void pushObject(lua_State* L, const QJsonObject& o) {
    luaL_checkstack(L, 3, "a result nested too deeply");  // (a C function is promised only 20 slots)
    lua_createtable(L, 0, int(o.size()));
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (it.value().isNull() || it.value().isUndefined()) continue;
        push(L, it.value());
        lua_setfield(L, -2, it.key().toUtf8().constData());
    }
}

void push(lua_State* L, const QJsonValue& v) {
    switch (v.type()) {
        case QJsonValue::Bool: lua_pushboolean(L, v.toBool()); break;
        case QJsonValue::Double: {
            const double d = v.toDouble();
            if (d == std::floor(d) && std::fabs(d) < 9.0e15) lua_pushinteger(L, lua_Integer(d));
            else lua_pushnumber(L, d);
            break;
        }
        case QJsonValue::String: {
            const QByteArray s = v.toString().toUtf8();
            lua_pushlstring(L, s.constData(), size_t(s.size()));
            break;
        }
        case QJsonValue::Array: {
            const QJsonArray a = v.toArray();
            luaL_checkstack(L, 3, "a result nested too deeply");
            lua_createtable(L, int(a.size()), 0);
            for (qsizetype i = 0; i < a.size(); ++i) {
                if (a[i].isNull()) lua_pushlightuserdata(L, nullptr);  // (nil would leave a hole that ends # and ipairs)
                else push(L, a[i]);
                lua_rawseti(L, -2, lua_Integer(i + 1));
            }
            break;
        }
        case QJsonValue::Object: pushObject(L, v.toObject()); break;
        default: lua_pushnil(L);
    }
}

// A Lua value as JSON: a table whose keys are whole numbers from 1 is an array (gaps as null), any other an object
// (keys strings or numbers); montage.null is null. What JSON cannot hold (NaN, infinity, integers past 2^53, a function,
// a key that is neither string nor number) is an error rather than a different value.
QJsonValue toJson(lua_State* L, int index, int depth = 0, int64_t* values = nullptr) {
    // (a table held many times over is copied each time: {t, t} nested 60 deep would be 2^60 values)
    int64_t count = 0;
    if (!values) values = &count;
    if (++*values > 1000000) luaL_error(L, "a tool's argument is too large (a million values, or a table held many times over)");
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
        case LUA_TBOOLEAN: return bool(lua_toboolean(L, index));
        case LUA_TNUMBER: {
            if (lua_isinteger(L, index)) {
                const lua_Integer i = lua_tointeger(L, index);
                if (i > (lua_Integer(1) << 53) || i < -(lua_Integer(1) << 53)) luaL_error(L, "%I is too large for a tool's argument", i);
                return double(i);
            }
            const double d = lua_tonumber(L, index);
            if (!std::isfinite(d)) luaL_error(L, "a tool's argument cannot be %s", std::isnan(d) ? "NaN" : "infinite");
            return d;
        }
        case LUA_TLIGHTUSERDATA:
            if (isNull(L, index)) return QJsonValue(QJsonValue::Null);
            luaL_error(L, "a tool's argument cannot be a userdata");
            return {};
        case LUA_TSTRING: {
            size_t n = 0;
            const char* s = lua_tolstring(L, index, &n);
            return QString::fromUtf8(s, qsizetype(n));
        }
        case LUA_TTABLE: {
            if (depth > 64) luaL_error(L, "a table nested too deeply (or holding itself)");
            luaL_checkstack(L, 4, "a table nested too deeply");  // each level holds a key, a value and a copy
            lua_Integer keys = 0, last = 0;
            bool sequence = true;
            lua_pushnil(L);
            while (lua_next(L, index)) {
                ++keys;
                const int kt = lua_type(L, -2);
                if (kt != LUA_TSTRING && kt != LUA_TNUMBER) luaL_error(L, "a table key in a tool's argument must be a string or a number");
                if (lua_isinteger(L, -2) && lua_tointeger(L, -2) >= 1) last = std::max(last, lua_tointeger(L, -2));
                else sequence = false;
                lua_pop(L, 1);
            }
            // An array (with null for each gap, unless the gaps outnumber what is there).
            if (sequence && keys > 0 && last <= 2 * keys + 16) {
                QJsonArray a;
                for (lua_Integer i = 1; i <= last; ++i) {
                    lua_rawgeti(L, index, i);
                    a.append(lua_isnil(L, -1) ? QJsonValue(QJsonValue::Null) : toJson(L, -1, depth + 1, values));
                    lua_pop(L, 1);
                }
                return a;
            }
            QJsonObject o;
            lua_pushnil(L);
            while (lua_next(L, index)) {
                lua_pushvalue(L, -2);  // (a copy: tostring on the key itself would confuse lua_next)
                const QString key = QString::fromUtf8(lua_tostring(L, -1));
                lua_pop(L, 1);
                o[key] = toJson(L, -1, depth + 1, values);
                lua_pop(L, 1);
            }
            return o;
        }
        case LUA_TNIL:
        case LUA_TNONE: return QJsonValue();
        default:
            luaL_error(L, "a tool's argument cannot be a %s", luaL_typename(L, index));
            return {};
    }
}

// A tool's answer as Lua: its structured result, else its text; its error raised.
int callTool(lua_State* L, const QString& name, int argsIndex) {
    const ScriptOptions& o = host(L);
    QJsonObject args;
    if (!lua_isnoneornil(L, argsIndex)) {
        luaL_checktype(L, argsIndex, LUA_TTABLE);
        const QJsonValue v = toJson(L, argsIndex);
        args = v.toObject();
    }
    if (o.stopped && o.stopped()) stopScript(L);  // (before the tool: a stopped script changes nothing more)
    QString error;
    const QJsonObject result = o.call ? o.call(name, args, &error) : QJsonObject();
    if (o.stopped && o.stopped()) stopScript(L);
    if (result.isEmpty()) return luaL_error(L, "%s: %s", name.toUtf8().constData(), (error.isEmpty() ? QStringLiteral("no answer") : error).toUtf8().constData());
    QString text;
    for (const QJsonValue& c : result.value("content").toArray())
        if (c.toObject().value("type").toString() == QLatin1String("text")) text += c.toObject().value("text").toString();
    if (result.value("isError").toBool()) return luaL_error(L, "%s: %s", name.toUtf8().constData(), text.toUtf8().constData());
    if (result.contains("structuredContent")) {
        pushObject(L, result.value("structuredContent").toObject());
        if (!text.isEmpty()) {
            const QByteArray t = text.toUtf8();
            lua_pushlstring(L, t.constData(), size_t(t.size()));
            lua_setfield(L, -2, "message");
        }
    } else {
        lua_createtable(L, 0, 1);
        const QByteArray t = text.toUtf8();
        lua_pushlstring(L, t.constData(), size_t(t.size()));
        lua_setfield(L, -2, "text");
    }
    return 1;
}

int montageCall(lua_State* L) { return callTool(L, QString::fromUtf8(luaL_checkstring(L, 1)), 2); }

// montage.add_title{...}: the tool by its name without "montage_".
int montageShorthand(lua_State* L) {
    const QString name = QString::fromUtf8(lua_tostring(L, lua_upvalueindex(1)));
    // Called as montage.x{...} or montage:x{...}.
    const int argsIndex = lua_gettop(L) >= 2 && lua_istable(L, 1) && lua_rawequal(L, 1, lua_upvalueindex(2)) ? 2 : 1;
    return callTool(L, name, argsIndex);
}

int montageIndex(lua_State* L) {
    const QString key = QString::fromUtf8(luaL_checkstring(L, 2));
    const QString name = key.startsWith(QLatin1String("montage_")) ? key : QStringLiteral("montage_") + key;
    if (!host(L).tools.contains(name)) return luaL_error(L, "no tool montage.%s (montage.tools() lists them)", key.toUtf8().constData());
    lua_pushstring(L, name.toUtf8().constData());
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, montageShorthand, 2);
    return 1;
}

int montageTools(lua_State* L) {
    const QStringList& tools = host(L).tools;
    lua_createtable(L, int(tools.size()), 0);
    for (qsizetype i = 0; i < tools.size(); ++i) {
        lua_pushstring(L, tools[i].toUtf8().constData());
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    return 1;
}

int montagePrint(lua_State* L) {
    QStringList parts;
    for (int i = 1; i <= lua_gettop(L); ++i) parts << QString::fromUtf8(luaL_tolstring(L, i, nullptr)), lua_pop(L, 1);
    if (const ScriptOptions& o = host(L); o.print) o.print(parts.join(QLatin1Char('\t')));
    return 0;
}

// montage.json(value): a value as JSON text, to print a result.
int montageJson(lua_State* L) {
    const QJsonValue v = toJson(L, 1);
    const QByteArray s = v.isObject() ? QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented)
                         : v.isArray() ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented)
                                       : QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
    lua_pushlstring(L, s.constData(), size_t(s.size()));
    return 1;
}

// montage.panel{...}: the script's panel described to the host, which answers with what it holds (or ends the script).
int montagePanel(lua_State* L) {
    const ScriptOptions& o = host(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    const QJsonValue description = toJson(L, 1);
    if (!o.panel) return luaL_error(L, "montage.panel: panels open in the Montage app (montage-cli script --press <button> answers one)");
    if (o.stopped && o.stopped()) stopScript(L);
    QString error;
    const std::optional<QJsonObject> values = o.panel(description.toObject(), &error);
    if (!error.isEmpty()) return luaL_error(L, "montage.panel: %s", error.toUtf8().constData());
    if (!values) endScript(L, kDoneKey);  // described: it runs again when a button is pressed
    pushObject(L, *values);
    return 1;
}

int montageSetPanel(lua_State* L) {
    const ScriptOptions& o = host(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    const QJsonValue values = toJson(L, 1);
    if (!o.setPanel) return luaL_error(L, "montage.set_panel: this script has no panel open");
    QString error;
    if (!o.setPanel(values.toObject(), &error)) return luaL_error(L, "montage.set_panel: %s", error.toUtf8().constData());
    return 0;
}

void hook(lua_State* L, lua_Debug*) {
    lua_getfield(L, LUA_REGISTRYINDEX, kEndingKey);
    if (!lua_isnil(L, -1)) lua_error(L);  // (ending: caught somewhere, raised again)
    lua_pop(L, 1);
    const ScriptOptions& o = host(L);
    if (o.tick) o.tick();
    if (o.stopped && o.stopped()) stopScript(L);
}

// Run before a script, with the stop value as its argument: the catching functions pass it on, so Stop (and Ctrl+C)
// end a script that catches errors too.
const char* kPrelude = R"lua(
local STOP, DONE = ...
local pcall0, xpcall0, error0 = pcall, xpcall, error
local function pass(ok, ...)
  if not ok and ((...) == STOP or (...) == DONE) then error0((...), 0) end
  return ok, ...
end
pcall = function(f, ...) return pass(pcall0(f, ...)) end
xpcall = function(f, handler, ...)
  return pass(xpcall0(f, function(e) if e == STOP or e == DONE then return e end return handler(e) end, ...))
end
local resume0, close0 = coroutine.resume, coroutine.close
coroutine.resume = function(co, ...) return pass(resume0(co, ...)) end
coroutine.close = function(co) return pass(close0(co)) end
local load0 = load
load = function(...)
  local f, e = load0(...)
  if f == nil and (e == STOP or e == DONE) then error0(e, 0) end
  return f, e
end
)lua";

}  // namespace

bool runScript(const QString& code, const QString& name, const ScriptOptions& options, QString* error) {
    lua_State* L = luaL_newstate();
    if (!L) {
        if (error) *error = QStringLiteral("Lua could not start");
        return false;
    }
    luaL_openlibs(L);
    lua_pushlightuserdata(L, const_cast<ScriptOptions*>(&options));
    lua_setfield(L, LUA_REGISTRYINDEX, kHostKey);
    lua_newtable(L);  // the stop value: a table only the host and the prelude hold
    lua_setfield(L, LUA_REGISTRYINDEX, kStopKey);
    lua_newtable(L);  // the panel described: another
    lua_setfield(L, LUA_REGISTRYINDEX, kDoneKey);
    // Nothing that takes the stop hook away, and no C modules (Lua is built into Montage, not a library to load).
    lua_getglobal(L, "debug");
    lua_pushnil(L);
    lua_setfield(L, -2, "sethook");
    lua_pop(L, 1);
    lua_getglobal(L, "package");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "cpath");
    lua_pushnil(L);
    lua_setfield(L, -2, "loadlib");
    lua_getfield(L, -1, "searchers");
    for (int i = 4; i >= 3; --i) lua_pushnil(L), lua_rawseti(L, -2, i);  // (the C and all-in-one searchers)
    lua_pop(L, 2);
    // A panel opening again at start-up runs with nothing that changes files or runs programs (the user did not ask).
    if (options.quiet) {
        luaL_dostring(L, "local function no(name) return function() error(name .. ' is not available while a panel opens', 2) end end\n"
                         "for _, n in ipairs{'execute', 'remove', 'rename', 'tmpname'} do os[n] = no('os.' .. n) end\n"
                         "io.popen = no('io.popen'); io.output = no('io.output'); io.write = no('io.write'); io.tmpfile = no('io.tmpfile')\n"
                         "local open0 = io.open\n"
                         "io.open = function(f, mode) if mode ~= nil and not tostring(mode):match('^rb?$') then error('io.open: only reading while a panel opens', 2) end return open0(f, mode) end\n"
                         // (debug reaches the functions replaced here, through their upvalues and the registry)
                         "debug = nil; package.loaded.debug = nil");
    }
    // Without a terminal (the app), reading standard input would wait for ever.
    if (options.noStdin) {
        luaL_dostring(L, "local lines0 = io.lines\n"
                         "io.read = function() error('io.read: scripts in the app have no input to read', 2) end\n"
                         "io.lines = function(f, ...) if f == nil then error('io.lines needs a file here', 2) end return lines0(f, ...) end\n"
                         "io.stdin = nil\n"
                         // (io.input() with nothing named is standard input, until a file is named)
                         "local input0, named = io.input, false\n"
                         "io.input = function(f) if f == nil and not named then error('io.input: scripts in the app have no input to read', 2) end if f ~= nil then named = true end return input0(f) end");
    }
    lua_pushcfunction(L, montagePrint);
    lua_setglobal(L, "print");
    // os.exit would end Montage itself (unsaved, in the app): a script ends with error() or by returning.
    lua_getglobal(L, "os");
    lua_pushcfunction(L, [](lua_State* L) -> int { return luaL_error(L, "os.exit is not available: end a script with error() or return"); });
    lua_setfield(L, -2, "exit");
    lua_pop(L, 1);
    // The montage table, its tools found by name.
    lua_newtable(L);
    lua_pushcfunction(L, montageCall);
    lua_setfield(L, -2, "call");
    lua_pushcfunction(L, montageTools);
    lua_setfield(L, -2, "tools");
    lua_pushcfunction(L, montageJson);
    lua_setfield(L, -2, "json");
    lua_pushcfunction(L, montagePanel);
    lua_setfield(L, -2, "panel");
    lua_pushcfunction(L, montageSetPanel);
    lua_setfield(L, -2, "set_panel");
    lua_createtable(L, int(options.args.size()), 0);
    for (qsizetype i = 0; i < options.args.size(); ++i) {
        lua_pushstring(L, options.args[i].toUtf8().constData());
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    lua_setfield(L, -2, "args");
    lua_pushstring(L, options.project.toUtf8().constData());
    lua_setfield(L, -2, "project");
    lua_pushlightuserdata(L, nullptr);
    lua_setfield(L, -2, "null");
    lua_newtable(L);  // its metatable
    lua_pushcfunction(L, montageIndex);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "montage");
    // The prelude, given the stop value.
    if (luaL_loadstring(L, kPrelude) != LUA_OK) {
        if (error) *error = QString::fromUtf8(lua_tostring(L, -1));
        lua_close(L);
        return false;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, kStopKey);
    lua_getfield(L, LUA_REGISTRYINDEX, kDoneKey);
    lua_call(L, 2, 0);
    lua_sethook(L, hook, LUA_MASKCOUNT, 20000);
    // As lua.c reads a file: a UTF-8 byte order mark and a first "#!" line skipped (the line kept, for line numbers).
    QByteArray src = code.toUtf8();
    if (src.startsWith("\xEF\xBB\xBF")) src.remove(0, 3);
    if (src.startsWith('#')) src.remove(0, src.indexOf('\n') < 0 ? src.size() : src.indexOf('\n'));
    const QByteArray chunk = ("=" + name).toUtf8();
    bool ok = luaL_loadbuffer(L, src.constData(), size_t(src.size()), chunk.constData()) == LUA_OK && lua_pcall(L, 0, 0, 0) == LUA_OK;
    if (!ok) {
        // Ended at montage.panel, its panel described: a success.
        lua_getfield(L, LUA_REGISTRYINDEX, kDoneKey);
        ok = lua_rawequal(L, -1, -2);
        lua_pop(L, 1);
    }
    if (!ok && error) {
        lua_getfield(L, LUA_REGISTRYINDEX, kStopKey);
        const bool stopped = lua_rawequal(L, -1, -2);
        lua_pop(L, 1);
        *error = stopped ? QString::fromLatin1(kStopped)
                 : lua_isstring(L, -1) ? QString::fromUtf8(lua_tostring(L, -1))
                                       : QStringLiteral("%1: the script raised a %2").arg(name, QString::fromUtf8(luaL_typename(L, -1)));
    }
    lua_close(L);
    return ok;
}

#endif

}  // namespace montage
