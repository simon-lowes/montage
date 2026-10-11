#include "ScriptHost.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <cmath>

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
const char* kStopped = "Stopped";

const ScriptOptions& host(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kHostKey);
    const auto* o = static_cast<const ScriptOptions*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *o;
}

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
                push(L, a[i]);
                lua_rawseti(L, -2, lua_Integer(i + 1));
            }
            break;
        }
        case QJsonValue::Object: pushObject(L, v.toObject()); break;
        default: lua_pushnil(L);
    }
}

// A Lua value as JSON: a table whose keys are 1..n is an array, any other table an object (keys as strings).
QJsonValue toJson(lua_State* L, int index, int depth = 0) {
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
        case LUA_TBOOLEAN: return bool(lua_toboolean(L, index));
        case LUA_TNUMBER: return lua_isinteger(L, index) ? QJsonValue(double(lua_tointeger(L, index))) : QJsonValue(lua_tonumber(L, index));
        case LUA_TSTRING: {
            size_t n = 0;
            const char* s = lua_tolstring(L, index, &n);
            return QString::fromUtf8(s, qsizetype(n));
        }
        case LUA_TTABLE: {
            if (depth > 64) luaL_error(L, "a table nested too deeply (or holding itself)");
            luaL_checkstack(L, 4, "a table nested too deeply");  // each level holds a key, a value and a copy
            const lua_Integer n = lua_Integer(lua_rawlen(L, index));
            lua_Integer keys = 0;
            bool sequence = true;
            lua_pushnil(L);
            while (lua_next(L, index)) {
                ++keys;
                if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_tointeger(L, -2) > n) sequence = false;
                lua_pop(L, 1);
            }
            if (sequence && keys == n && n > 0) {
                QJsonArray a;
                for (lua_Integer i = 1; i <= n; ++i) {
                    lua_rawgeti(L, index, i);
                    a.append(toJson(L, -1, depth + 1));
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
                o[key] = toJson(L, -1, depth + 1);
                lua_pop(L, 1);
            }
            return o;
        }
        default: return QJsonValue();
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
    QString error;
    const QJsonObject result = o.call ? o.call(name, args, &error) : QJsonObject();
    if (o.stopped && o.stopped()) return luaL_error(L, kStopped);
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

void hook(lua_State* L, lua_Debug*) {
    const ScriptOptions& o = host(L);
    if (o.tick) o.tick();
    if (o.stopped && o.stopped()) luaL_error(L, kStopped);
}

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
    lua_createtable(L, int(options.args.size()), 0);
    for (qsizetype i = 0; i < options.args.size(); ++i) {
        lua_pushstring(L, options.args[i].toUtf8().constData());
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    lua_setfield(L, -2, "args");
    lua_pushstring(L, options.project.toUtf8().constData());
    lua_setfield(L, -2, "project");
    lua_newtable(L);  // its metatable
    lua_pushcfunction(L, montageIndex);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "montage");
    lua_sethook(L, hook, LUA_MASKCOUNT, 20000);
    const QByteArray src = code.toUtf8(), chunk = ("=" + name).toUtf8();
    bool ok = luaL_loadbuffer(L, src.constData(), size_t(src.size()), chunk.constData()) == LUA_OK && lua_pcall(L, 0, 0, 0) == LUA_OK;
    if (!ok && error) {
        *error = QString::fromUtf8(lua_tostring(L, -1));
        if (error->endsWith(QLatin1String(kStopped))) *error = QString::fromLatin1(kStopped);
    }
    lua_close(L);
    return ok;
}

#endif

}  // namespace montage
