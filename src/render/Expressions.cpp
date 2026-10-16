// Parameter expressions (core/Expression.h): Lua 5.4, one sandboxed state per thread.
#include "core/Expression.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/Automation.h"
#include "core/Effects.h"
#include "render/Expressions.h"
#include "render/AudioReactive.h"

#ifdef MONTAGE_WITH_LUA
// (Lua is compiled as C++ here: its headers without lua.hpp's extern "C", and its errors are C++ exceptions.)
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#endif

namespace montage {

namespace {

thread_local ExpressionScope* tScope = nullptr;
thread_local std::function<ExpressionContext()> tFallback;

std::mutex gErrorMutex;
std::map<std::string, std::string>& errors() {
    static std::map<std::string, std::string> e;
    return e;
}
void noteError(const std::string& expr, const std::string& error) {
    std::lock_guard<std::mutex> lock(gErrorMutex);
    if (error.empty()) errors().erase(expr);
    else errors()[expr] = error;
}

}  // namespace

ExpressionScope::ExpressionScope(const Project* p, const Sequence* s, const Clip* c) : project(p), sequence(s), clip(c), previous(tScope) {
    tScope = this;
}
ExpressionScope::~ExpressionScope() { tScope = previous; }

void setExpressionFallback(std::function<ExpressionContext()> fallback) { tFallback = std::move(fallback); }

ExpressionContext currentExpressionContext() {
    if (tScope) return {tScope->project, tScope->sequence, tScope->clip};
    return tFallback ? tFallback() : ExpressionContext{};
}

std::string expressionError(const std::string& expr) {
    std::lock_guard<std::mutex> lock(gErrorMutex);
    auto it = errors().find(expr);
    return it == errors().end() ? std::string() : it->second;
}

#ifndef MONTAGE_WITH_LUA

bool expressionsAvailable() { return false; }
bool checkExpression(const std::string&, std::string* error) {
    if (error) *error = "Expressions need Montage built with Lua";
    return false;
}
double evaluateExpression(const Param& p, FrameTime, double keyed) {
    noteError(p.expr, "Expressions need Montage built with Lua");
    return keyed;
}

#else

namespace {

// ---- Where a setting is ------------------------------------------------------------------------------------------

struct Owner {
    const Clip* clip = nullptr;
    const Effect* effect = nullptr;
    std::string name;
};

// The effects of a clip that hold its settings.
template <typename F>
bool eachEffect(const Clip& c, F&& f) {
    for (const Effect* e : {&c.motion, &c.generator, &c.audio, &c.timing})
        if (f(*e)) return true;
    for (const Effect& e : c.effects)
        if (f(e)) return true;
    for (const auto& [n, e] : c.trims)
        if (f(e)) return true;
    return false;
}

bool findOwner(const Sequence& s, const Clip* only, const Param* p, Owner& out) {
    auto inClip = [&](const Clip& c) {
        return eachEffect(c, [&](const Effect& e) {
            for (const auto& [n, q] : e.params)
                if (&q == p) {
                    out = {&c, &e, n};
                    return true;
                }
            return false;
        });
    };
    if (only) return inClip(*only);
    for (const auto* tracks : {&s.videoTracks, &s.audioTracks})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips)
                if (inClip(c)) return true;
    return false;
}

const Clip* findClip(const Sequence& s, const std::string& name, Id id) {
    for (const auto* tracks : {&s.videoTracks, &s.audioTracks})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips)
                if (id ? c.id == id : c.name == name) return &c;
    return nullptr;
}

// A clip's setting by name ("pos_x"), or by effect and name ("gaussian_blur.radius").
const Param* findSetting(const Clip& c, const std::string& name, const Effect** in) {
    std::string type, key = name;
    if (const size_t dot = name.find('.'); dot != std::string::npos) type = name.substr(0, dot), key = name.substr(dot + 1);
    const Param* found = nullptr;
    eachEffect(c, [&](const Effect& e) {
        if (!type.empty() && e.type != type) return false;
        auto it = e.params.find(key);
        if (it == e.params.end()) return false;
        found = &it->second;
        *in = &e;
        return true;
    });
    return found;
}

// ---- Seeded noise --------------------------------------------------------------------------------------------------

uint64_t mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
uint64_t hashText(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char ch : s) h = (h ^ ch) * 1099511628211ull;
    return h;
}
double unit(uint64_t h) { return double(h >> 11) * (1.0 / 9007199254740992.0); }  // 0..1

// Smooth value noise, -1..1: random values at whole x, eased between.
double noise1(double x, uint64_t seed) {
    const double f = std::floor(x);
    const double u = x - f;
    const int64_t i = int64_t(f);
    const double a = unit(mix64(seed ^ mix64(uint64_t(i)))) * 2 - 1, b = unit(mix64(seed ^ mix64(uint64_t(i + 1)))) * 2 - 1;
    const double s = u * u * u * (u * (u * 6 - 15) + 10);
    return a + (b - a) * s;
}

// ---- The state on each thread ----------------------------------------------------------------------------------

// What the expression running now is evaluated for (innermost last: param() runs others').
struct Eval {
    const Param* param = nullptr;
    FrameTime t = 0;
    double keyed = 0;
    double fps = 25;
    const Project* project = nullptr;
    const Sequence* sequence = nullptr;
    Owner owner;
    uint64_t seed = 0;
    int randoms = 0;
};
thread_local std::vector<Eval> tEval;
thread_local bool tLooped = false;  // an expression this one reads went round in a loop

constexpr int kMaxDepth = 8;          // param() into param() ...
constexpr int kBudget = 2000;         // x 1000 instructions an evaluation
constexpr size_t kMemory = 64 << 20;  // bytes a state
constexpr size_t kCompiled = 512;     // expressions kept compiled a thread

struct State {
    lua_State* L = nullptr;
    size_t used = 0;
    int ticks = 0;
    int base = LUA_NOREF;  // the read-only globals expressions see
    std::unordered_map<std::string, int> compiled;  // expression -> its function (registry ref)
    std::unordered_map<std::string, std::string> bad;  // expression -> why it does not compile
    ~State() {
        if (L) lua_close(L);
    }
};

void* allocate(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* st = static_cast<State*>(ud);
    if (!ptr) osize = 0;
    if (nsize == 0) {
        st->used -= osize;
        std::free(ptr);
        return nullptr;
    }
    if (nsize > osize && st->used + (nsize - osize) > kMemory) return nullptr;
    void* p = std::realloc(ptr, nsize);
    if (p) st->used += nsize - osize;
    return p;
}

State* state();

void budget(lua_State* L, lua_Debug*) {
    if (++state()->ticks > kBudget) luaL_error(L, "the expression took too long");
}

Eval& current() { return tEval.back(); }

double numberArg(lua_State* L, int i, double def) { return lua_isnoneornil(L, i) ? def : luaL_checknumber(L, i); }

int luaWiggle(lua_State* L) {
    const Eval& e = current();
    const double freq = luaL_checknumber(L, 1), amount = luaL_checknumber(L, 2);
    const int octaves = std::clamp(int(numberArg(L, 3, 1)), 1, 10);
    const double falloff = numberArg(L, 4, 0.5);
    const uint64_t seed = e.seed ^ mix64(uint64_t(int64_t(numberArg(L, 5, 0))));
    const double x = double(e.t) / e.fps * freq;
    double sum = 0, a = 1, norm = 0, f = 1;
    for (int o = 0; o < octaves; ++o) {
        sum += a * noise1(x * f, mix64(seed + uint64_t(o)));
        norm += a;
        a *= falloff;
        f *= 2;
    }
    lua_pushnumber(L, e.keyed + amount * sum / std::max(1e-9, norm));
    return 1;
}

int luaNoise(lua_State* L) {
    const double x = luaL_checknumber(L, 1);
    lua_pushnumber(L, noise1(x, current().seed ^ mix64(uint64_t(int64_t(numberArg(L, 2, 0))) + 7)));
    return 1;
}

int luaRandom(lua_State* L) {
    Eval& e = current();
    const double u = unit(mix64(e.seed ^ mix64(uint64_t(e.t)) ^ mix64(uint64_t(++e.randoms) << 32)));
    if (lua_isnoneornil(L, 1)) lua_pushnumber(L, u);
    else if (lua_isnoneornil(L, 2)) lua_pushnumber(L, u * luaL_checknumber(L, 1));
    else {
        const double lo = luaL_checknumber(L, 1), hi = luaL_checknumber(L, 2);
        lua_pushnumber(L, lo + (hi - lo) * u);
    }
    return 1;
}

int luaLoopOut(lua_State* L) {
    const Eval& e = current();
    const std::string kind = luaL_optstring(L, 1, "cycle");
    Param keys = *e.param;
    keys.expr.clear();
    if (kind == "continue") {
        double v = keys.keyedAt(e.t);
        if (keys.keys.size() >= 2 && e.t > keys.keys.back().t) {
            const FrameTime last = keys.keys.back().t;
            v = keys.keys.back().v + double(e.t - last) * (keys.keys.back().v - keys.keyedAt(last - 1));
        }
        lua_pushnumber(L, v);
        return 1;
    }
    if (kind == "cycle") keys.repeat = Repeat::Loop;
    else if (kind == "pingpong") keys.repeat = Repeat::PingPong;
    else if (kind == "offset") keys.repeat = Repeat::Offset;
    else return luaL_error(L, "loopOut: \"%s\" is not cycle, pingpong, offset or continue", kind.c_str());
    lua_pushnumber(L, keys.keyedAt(e.t));
    return 1;
}

int luaParam(lua_State* L) {
    const Eval& e = current();
    const Clip* c = e.owner.clip;
    std::string name;
    if (lua_gettop(L) >= 2) {
        if (!e.sequence) return luaL_error(L, "param: no sequence to find the clip in");
        if (lua_type(L, 1) == LUA_TNUMBER) c = findClip(*e.sequence, {}, Id(lua_tointeger(L, 1)));
        else c = findClip(*e.sequence, luaL_checkstring(L, 1), 0);
        if (!c) return luaL_error(L, "param: no clip %s", luaL_tolstring(L, 1, nullptr));
        name = luaL_checkstring(L, 2);
    } else {
        name = luaL_checkstring(L, 1);
    }
    if (!c) return luaL_error(L, "param: the clip this setting belongs to is not known here");
    const Effect* in = nullptr;
    const Param* p = findSetting(*c, name, &in);
    // At the same moment on the timeline (a setting without a value of its own is at its default).
    const FrameTime t = (e.owner.clip ? e.owner.clip->start : 0) + e.t - c->start;
    if (!p) {
        const std::string type = name.find('.') == std::string::npos ? std::string("transform") : name.substr(0, name.find('.'));
        const std::string key = name.substr(name.find('.') == std::string::npos ? 0 : name.find('.') + 1);
        const EffectInfo* info = findEffectInfo(type);
        const ParamInfo* pi = nullptr;
        if (info)
            for (const ParamInfo& x : info->params)
                if (x.name == key) pi = &x;
        if (!pi) return luaL_error(L, "param: %s has no setting \"%s\"", c->name.c_str(), name.c_str());
        lua_pushnumber(L, pi->def);
        return 1;
    }
    const double v = p->at(t);
    if (tLooped) return luaL_error(L, "param: %s's %s reads expressions that read each other in a loop", c->name.c_str(), name.c_str());
    lua_pushnumber(L, v);
    return 1;
}

int luaAudio(lua_State* L) {
    const Eval& e = current();
    const int track = int(numberArg(L, 1, 0));
    const std::string band = luaL_optstring(L, 2, "all");
    if (!e.project || !e.sequence) {
        lua_pushnumber(L, 0);
        return 1;
    }
    if (track < 0 || track > int(e.sequence->audioTracks.size())) return luaL_error(L, "audio: no audio track %d", track);
    const double frame = double((e.owner.clip ? e.owner.clip->start : 0) + e.t);
    constexpr int kRate = 48000;
    if (band == "all") {
        // Loudness over a frame: the root mean square of its samples (a full-scale sine is about 0.71).
        const std::vector<float> x = trackSoundAt(*e.project, *e.sequence, track, frame, std::max(64, int(kRate / e.fps)), kRate);
        double sum = 0;
        for (float v : x) sum += double(v) * v;
        lua_pushnumber(L, x.empty() ? 0.0 : std::sqrt(sum / double(x.size())));
        return 1;
    }
    double f0 = 0, f1 = 0;
    if (band == "low") f0 = 20, f1 = 250;
    else if (band == "mid") f0 = 250, f1 = 4000;
    else if (band == "high") f0 = 4000, f1 = 20000;
    else return luaL_error(L, "audio: band \"%s\" is not all, low, mid or high", band.c_str());
    // The band's share of the frame's level, through a 2048-sample spectrum (as Animate to Audio measures it).
    const size_t n = 2048;
    const std::vector<float> mag = spectrum(trackSoundAt(*e.project, *e.sequence, track, frame, int(n), kRate));
    const double binHz = double(kRate) / double(n);
    double energy = 0;
    for (size_t i = size_t(f0 / binHz); i < mag.size() && double(i) * binHz < f1; ++i) energy += double(mag[i]) * mag[i];
    // (A sine's bins through the Hann window are 1, 1/2 and 1/2 its amplitude: scaled so a sine in the band reads its
    // root mean square, as "all" does.)
    lua_pushnumber(L, std::sqrt(energy / 3.0));
    return 1;
}

int luaClamp(lua_State* L) {
    lua_pushnumber(L, std::clamp(luaL_checknumber(L, 1), luaL_checknumber(L, 2), std::max(luaL_checknumber(L, 2), luaL_checknumber(L, 3))));
    return 1;
}

// linear(t, t0, t1, v0, v1) or linear(t, v0, v1) for t from 0 to 1; ease the same, smooth at both ends.
int ramp(lua_State* L, bool smooth) {
    const double t = luaL_checknumber(L, 1);
    double t0 = 0, t1 = 1, v0, v1;
    if (lua_gettop(L) >= 5) t0 = luaL_checknumber(L, 2), t1 = luaL_checknumber(L, 3), v0 = luaL_checknumber(L, 4), v1 = luaL_checknumber(L, 5);
    else v0 = luaL_checknumber(L, 2), v1 = luaL_checknumber(L, 3);
    double u = t1 == t0 ? (t >= t1 ? 1.0 : 0.0) : std::clamp((t - t0) / (t1 - t0), 0.0, 1.0);
    if (smooth) u = u * u * (3 - 2 * u);
    lua_pushnumber(L, v0 + (v1 - v0) * u);
    return 1;
}
int luaLinear(lua_State* L) { return ramp(L, false); }
int luaEase(lua_State* L) { return ramp(L, true); }

State* state() {
    thread_local std::unique_ptr<State> st;
    if (st) return st.get();
    st = std::make_unique<State>();
    lua_State* L = lua_newstate(allocate, st.get());
    st->L = L;
    // Only what computes: no files, programs, clocks, module loading or loading code from strings.
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 4);
    lua_newtable(L);  // the globals expressions read
    const int base = lua_gettop(L);
    for (const char* name : {"assert", "error", "ipairs", "pairs", "next", "pcall", "select", "tonumber", "tostring", "type", "xpcall",
                             "rawequal", "rawget", "rawlen", "getmetatable", "setmetatable"}) {
        lua_getglobal(L, name);
        lua_setfield(L, base, name);
    }
    for (const char* lib : {LUA_MATHLIBNAME, LUA_STRLIBNAME, LUA_TABLIBNAME}) {
        lua_getglobal(L, lib);
        lua_setfield(L, base, lib);
    }
    // math without its clock-seeded random numbers (random() is seeded by the setting); its functions by name too.
    lua_getfield(L, base, LUA_MATHLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "random");
    lua_pushnil(L);
    lua_setfield(L, -2, "randomseed");
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_settable(L, base);
    }
    lua_pop(L, 1);
    const luaL_Reg helpers[] = {{"wiggle", luaWiggle}, {"noise", luaNoise},   {"random", luaRandom}, {"loopOut", luaLoopOut},
                                {"param", luaParam},   {"audio", luaAudio},   {"clamp", luaClamp},   {"linear", luaLinear},
                                {"ease", luaEase},     {nullptr, nullptr}};
    lua_pushvalue(L, base);
    luaL_setfuncs(L, helpers, 0);
    lua_pop(L, 1);
    st->base = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_sethook(L, budget, LUA_MASKCOUNT, 1000);
    return st.get();
}

// Pushes the compiled function for `expr`; false with the error.
bool compiled(State& st, const std::string& expr, std::string* error) {
    lua_State* L = st.L;
    if (auto it = st.compiled.find(expr); it != st.compiled.end()) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
        return true;
    }
    if (auto it = st.bad.find(expr); it != st.bad.end()) {
        if (error) *error = it->second;
        return false;
    }
    if (st.compiled.size() + st.bad.size() >= kCompiled) {
        for (const auto& [x, ref] : st.compiled) luaL_unref(L, LUA_REGISTRYINDEX, ref);
        st.compiled.clear();
        st.bad.clear();
    }
    // A formula, or statements that return the value; its globals are the table it is called with (so the same
    // expression can run inside itself, through param(), each with its own).
    std::string why;
    const std::string asExpression = "local _ENV = ...; return " + expr, asStatements = "local _ENV = ...; " + expr;
    if (luaL_loadbufferx(L, asExpression.data(), asExpression.size(), "=expression", "t") != LUA_OK) {
        lua_pop(L, 1);
        if (luaL_loadbufferx(L, asStatements.data(), asStatements.size(), "=expression", "t") != LUA_OK) {
            why = lua_tostring(L, -1) ? lua_tostring(L, -1) : "does not compile";
            lua_pop(L, 1);
            st.bad[expr] = why;
            if (error) *error = why;
            return false;
        }
    }
    lua_pushvalue(L, -1);
    st.compiled[expr] = luaL_ref(L, LUA_REGISTRYINDEX);
    return true;
}

}  // namespace

bool expressionsAvailable() { return true; }

bool checkExpression(const std::string& expr, std::string* error) {
    State& st = *state();
    if (!compiled(st, expr, error)) return false;
    lua_pop(st.L, 1);
    return true;
}

double evaluateExpression(const Param& p, FrameTime t, double keyed) {
    if (tEval.empty()) tLooped = false;
    if (int(tEval.size()) >= kMaxDepth) {
        tLooped = true;
        noteError(p.expr, "Expressions read each other in a loop");
        return keyed;
    }
    Eval e;
    e.param = &p;
    e.t = t;
    e.keyed = keyed;
    const ExpressionContext where = currentExpressionContext();
    const Clip* clip = where.clip;
    e.project = where.project, e.sequence = where.sequence;
    if (e.sequence) {
        e.fps = std::max(1.0, e.sequence->fpsValue());
        if (!findOwner(*e.sequence, clip, &p, e.owner) && clip && !findOwner(*e.sequence, nullptr, &p, e.owner))
            e.owner.clip = clip;  // (a setting not in the sequence, such as a copy being tried out: the scope's clip)
    }
    e.seed = e.owner.effect ? mix64(uint64_t(e.owner.clip ? e.owner.clip->id : 0) * 1000003u + uint64_t(e.owner.effect->id)) ^ hashText(e.owner.name)
                            : hashText(p.expr);
    State& st = *state();
    lua_State* L = st.L;
    const int top = lua_gettop(L);
    std::string error;
    double out = keyed;
    tEval.push_back(e);
    if (compiled(st, p.expr, &error)) {
        // Its own globals each time, reading the shared ones: what one evaluation sets does not leak into the next.
        lua_newtable(L);
        lua_newtable(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, st.base);
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, -2);
        lua_pushnumber(L, keyed);
        lua_setfield(L, -2, "value");
        lua_pushnumber(L, double(t) / e.fps);
        lua_setfield(L, -2, "time");
        lua_pushinteger(L, lua_Integer(t));
        lua_setfield(L, -2, "frame");
        lua_pushnumber(L, e.fps);
        lua_setfield(L, -2, "fps");
        lua_pushnumber(L, e.owner.clip ? double(e.owner.clip->duration) / e.fps : 0.0);
        lua_setfield(L, -2, "duration");
        if (tEval.size() == 1) st.ticks = 0;  // (the ones it reads through param() share its time)
        try {
            if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
                error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "failed";
            } else if (lua_isboolean(L, -1)) {
                out = lua_toboolean(L, -1) ? 1.0 : 0.0;
            } else if (lua_type(L, -1) != LUA_TNUMBER) {
                error = std::string("gave ") + luaL_typename(L, -1) + ", not a number";
            } else if (const double v = lua_tonumber(L, -1); !std::isfinite(v)) {
                error = "gave a number that is not finite";
            } else {
                out = v;
            }
        } catch (const std::exception& x) {  // (out of memory and the like)
            error = x.what();
        }
    }
    tEval.pop_back();
    lua_settop(L, top);
    noteError(p.expr, error);
    return error.empty() ? out : keyed;
}

#endif

namespace edit {

namespace {

// The effect of a clip that `effect` names (0 = its Transform, made if missing), with the setting's description.
Effect* settingOf(Project& p, Clip& c, Id effect, const std::string& param, ParamInfo& info, std::string* error) {
    Effect* e = nullptr;
    if (effect == 0) {
        if (c.motion.empty()) c.motion = makeEffect(p, "transform");
        e = &c.motion;
    } else {
        for (Effect& x : c.effects)
            if (x.id == effect) e = &x;
        for (Effect* x : {&c.motion, &c.generator, &c.audio, &c.timing})
            if (!x->empty() && x->id == effect) e = x;
    }
    if (!e) {
        *error = "No such effect on the clip";
        return nullptr;
    }
    // (Plug-ins' settings too: effectParams describes them from the effect.)
    const std::vector<ParamInfo> params = effectParams(*e);
    auto it = std::find_if(params.begin(), params.end(), [&](const ParamInfo& pi) { return pi.name == param; });
    if (it == params.end()) {
        const EffectInfo* ei = findEffectInfo(e->type);
        *error = "\"" + param + "\" is not a setting of " + (ei ? ei->displayName : e->type);
        return nullptr;
    }
    if (!it->keyframeable) {
        *error = it->label + " cannot change over time";
        return nullptr;
    }
    info = *it;
    return e;
}

}  // namespace

Result setExpression(Project& p, Sequence& s, Id clipId, Id effect, const std::string& param, const std::string& expr) {
    Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("No such clip");
    std::string error;
    if (!expr.empty() && !checkExpression(expr, &error)) return Result::fail("The expression does not work: " + error);
    ParamInfo info;
    Effect* e = settingOf(p, *c, effect, param, info, &error);
    if (!e) return Result::fail(error);
    auto it = e->params.find(param);
    if (it == e->params.end()) {
        if (expr.empty()) return {};
        it = e->params.emplace(param, Param(info.def)).first;
    }
    it->second.expr = expr;
    return {};
}

Result bakeExpression(Project& p, Sequence& s, Id clipId, Id effect, const std::string& param) {
    Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("No such clip");
    std::string error;
    ParamInfo info;
    Effect* e = settingOf(p, *c, effect, param, info, &error);
    if (!e) return Result::fail(error);
    auto it = e->params.find(param);
    if (it == e->params.end() || it->second.expr.empty()) return Result::fail(info.label + " has no expression");
    const Param& prm = it->second;
    std::vector<Keyframe> keys;
    {
        ExpressionScope scope(&p, &s, c);
        for (FrameTime f = 0; f < c->duration; ++f) {
            const double v = prm.at(f);
            if (const std::string why = expressionError(prm.expr); !why.empty()) return Result::fail("The expression fails at frame " + std::to_string(f) + ": " + why);
            keys.push_back({f, v});
        }
    }
    const bool held = info.kind == ParamKind::Bool || info.kind == ParamKind::Choice;
    if (held) {
        for (Keyframe& k : keys) k.interp = Interp::Hold;
        std::vector<Keyframe> changes;  // only where it changes
        for (const Keyframe& k : keys)
            if (changes.empty() || k.v != changes.back().v) changes.push_back(k);
        keys = std::move(changes);
    } else {
        thinKeys(keys, std::max(1e-9, (info.max - info.min) * 0.001));
    }
    Param& out = it->second;
    out.keys = std::move(keys);
    out.expr.clear();
    out.repeat = Repeat::Hold;
    if (!out.keys.empty()) out.value = out.keys.front().v;
    return {};
}

}  // namespace edit

}  // namespace montage
