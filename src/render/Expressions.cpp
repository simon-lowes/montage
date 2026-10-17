// Parameter expressions (core/Expression.h): Lua 5.4, one sandboxed state per thread.
#include "core/Expression.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
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

// Where a setting is, and its range.
struct Owner {
    const Clip* clip = nullptr;
    const Effect* effect = nullptr;
    std::string name;
    bool described = false;  // (kind and range known)
    ParamKind kind = ParamKind::Number;
    double min = 0, max = 0;
    bool noted = false;      // (its error, as last recorded from this scope)
    std::string lastError;
};

// The errors expressions last gave, by clip, effect and setting. (Kept to the end: render threads may still note one
// while the program exits.)
using ErrorKey = std::tuple<Id, Id, std::string>;
std::mutex gErrorMutex;
std::atomic<int> gErrors{0};
std::map<ErrorKey, std::string>& errors() {
    static auto* e = new std::map<ErrorKey, std::string>;
    return *e;
}
void noteError(Owner& o, const std::string& error) {
    if (!o.effect || !o.clip) return;
    if (o.noted && o.lastError == error) return;
    o.noted = true;
    o.lastError = error;
    if (error.empty() && gErrors.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard<std::mutex> lock(gErrorMutex);
    const ErrorKey key{o.clip->id, o.effect->id, o.name};
    if (error.empty()) {
        if (errors().erase(key)) --gErrors;
    } else if (errors().insert_or_assign(key, error).second) {
        ++gErrors;
    }
}

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

}  // namespace

struct ExpressionScope::Owners {
    std::unordered_map<const Param*, Owner> found;
};

ExpressionScope::ExpressionScope(const Project* p, const Sequence* s, const Clip* c) : project(p), sequence(s), clip(c), previous(tScope) {
    tScope = this;
}
ExpressionScope::~ExpressionScope() { tScope = previous; }

void setExpressionFallback(std::function<ExpressionContext()> fallback) { tFallback = std::move(fallback); }

ExpressionContext currentExpressionContext() {
    if (tScope) return {tScope->project, tScope->sequence, tScope->clip};
    return tFallback ? tFallback() : ExpressionContext{};
}

std::string expressionError(Id clip, Id effect, const std::string& param) {
    if (gErrors.load(std::memory_order_relaxed) == 0) return {};
    std::lock_guard<std::mutex> lock(gErrorMutex);
    auto it = errors().find({clip, effect, param});
    return it == errors().end() ? std::string() : it->second;
}

bool usesExpressions(const Effect& e) {
    return std::any_of(e.params.begin(), e.params.end(), [](const auto& kv) { return !kv.second.expr.empty(); });
}

bool usesExpressions(const Project& p) {
    for (const Sequence& s : p.sequences)
        for (const auto* tracks : {&s.videoTracks, &s.audioTracks})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips) {
                    for (const Effect* e : {&c.motion, &c.generator, &c.audio, &c.timing})
                        if (usesExpressions(*e)) return true;
                    for (const Effect& e : c.effects)
                        if (usesExpressions(e)) return true;
                }
    return false;
}

uint32_t expressionSeed(Id clip, Id effect, const std::string& param) {
    const auto s = uint32_t(mix64(clip * 1000003u + effect) ^ hashText(param));
    return s ? s : 1;
}

#ifndef MONTAGE_WITH_LUA

bool expressionsAvailable() { return false; }
bool checkExpression(const std::string&, std::string* error) {
    if (error) *error = "Expressions need Montage built with Lua";
    return false;
}
double evaluateExpression(const Param&, FrameTime, double keyed, std::string* error) {
    if (error) *error = "Expressions need Montage built with Lua";
    return keyed;
}

#else

namespace {

// ---- Where a setting is ------------------------------------------------------------------------------------------

// The effects of a clip whose settings expressions read.
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
                    out.clip = &c, out.effect = &e, out.name = n;
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

// Where `p` is in `where` (with its range when it is an effect's setting).
Owner locate(const ExpressionContext& where, const Param& p) {
    Owner o;
    if (!where.sequence) return o;
    if (!findOwner(*where.sequence, where.clip, &p, o) && where.clip && !findOwner(*where.sequence, nullptr, &p, o))
        o.clip = where.clip;  // (a setting not in the sequence, such as a copy being tried out: the scope's clip)
    if (o.effect) {
        for (const ParamInfo& pi : effectParams(*o.effect))
            if (pi.name == o.name) {
                o.described = true, o.kind = pi.kind, o.min = pi.min, o.max = pi.max;
                break;
            }
    }
    return o;
}

// The owner of `p`: found once a scope (the scope's project is not edited while it lives), else every time.
Owner& ownerOf(const ExpressionContext& where, const Param& p, Owner& scratch) {
    if (tScope && tScope->sequence == where.sequence) {
        if (!tScope->owners) tScope->owners = std::make_unique<ExpressionScope::Owners>();
        auto [it, added] = tScope->owners->found.try_emplace(&p);
        Owner& o = it->second;
        if (!added && o.effect) {  // (still there: not a setting gone, its place taken by another)
            auto at = o.effect->params.find(o.name);
            if (at == o.effect->params.end() || &at->second != &p) added = true;
        }
        if (added) o = locate(where, p);
        return o;
    }
    scratch = locate(where, p);
    return scratch;
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

// A number from Lua as a whole number within lo..hi (0 for not-a-number).
int64_t whole(double v, int64_t lo, int64_t hi) {
    if (!std::isfinite(v)) return v > 0 ? hi : v < 0 ? lo : std::clamp<int64_t>(0, lo, hi);
    return int64_t(std::clamp(std::floor(v), double(lo), double(hi)));
}

// ---- The state on each thread ----------------------------------------------------------------------------------

// What the expression running now is evaluated for (innermost last: param() runs others').
struct Eval {
    const Param* param = nullptr;
    FrameTime t = 0;       // clip frame (the keys')
    FrameTime local = 0;   // frames since the expression's start (Param::exprFrom)
    double keyed = 0;
    double fps = 25;
    const Project* project = nullptr;
    const Sequence* sequence = nullptr;
    const Clip* clip = nullptr;
    uint64_t seed = 0;
    int randoms = 0;
};
thread_local std::vector<Eval> tEval;
thread_local bool tLooped = false;  // an expression this one reads went round in a loop

struct Evaluating {
    explicit Evaluating(const Eval& e) { tEval.push_back(e); }
    ~Evaluating() { tEval.pop_back(); }
    Evaluating(const Evaluating&) = delete;
    Evaluating& operator=(const Evaluating&) = delete;
};

constexpr int kMaxDepth = 16;         // param() into param() ...
constexpr int kBudget = 2000;         // x 1000 instructions an evaluation
constexpr size_t kMemory = 64 << 20;  // bytes a state
// The longest string. Copying, comparing or changing the case of a string runs in C as one instruction, out of the
// time limit's sight; a megabyte at most keeps a thousand of them (between two looks at the clock) to a moment.
constexpr size_t kMaxString = 1 << 20;
constexpr double kSeconds = 1.0;  // the time limit on the clock, whatever the instructions were
constexpr size_t kCompiled = 512;     // expressions kept compiled a thread

struct State {
    lua_State* L = nullptr;
    size_t used = 0;
    int ticks = 0;
    std::chrono::steady_clock::time_point started;  // of the outermost evaluation
    int base = LUA_NOREF;  // the read-only globals expressions see
    std::unordered_map<std::string, int> compiled;  // expression -> its function (registry ref)
    std::unordered_map<std::string, std::string> bad;  // expression -> why it does not compile
    ~State() {
        if (L) lua_close(L);
    }
};
thread_local std::unique_ptr<State> tState;

// A Lua error with nothing to catch it (memory running out while setting up a call): the state cannot be trusted
// after it, so it is thrown out to the outermost evaluation, which starts the thread's state again.
struct Panic {};
int panic(lua_State*) { throw Panic{}; }

void* allocate(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* st = static_cast<State*>(ud);
    const bool newString = !ptr && osize == LUA_TSTRING;  // (a new object's "old size" is its type)
    if (!ptr) osize = 0;
    if (nsize == 0) {
        st->used -= osize;
        std::free(ptr);
        return nullptr;
    }
    if (nsize > osize && st->used + (nsize - osize) > kMemory) return nullptr;
    if (newString && nsize > kMaxString) return nullptr;
    void* p = std::realloc(ptr, nsize);
    if (p) {
        st->used += nsize - osize;
        if (nsize > osize) st->ticks += int((nsize - osize) >> 16);  // memory made counts as time too (64 kB a tick)
    }
    return p;
}

void budget(lua_State* L, lua_Debug*) {
    if (++tState->ticks > kBudget ||
        std::chrono::duration<double>(std::chrono::steady_clock::now() - tState->started).count() > kSeconds)
        luaL_error(L, "the expression took too long");
}

Eval& current(lua_State* L) {
    if (tEval.empty()) luaL_error(L, "expression helpers only work while an expression runs");
    return tEval.back();
}

double numberArg(lua_State* L, int i, double def) { return lua_isnoneornil(L, i) ? def : luaL_checknumber(L, i); }

int luaWiggle(lua_State* L) {
    const Eval& e = current(L);
    const double freq = luaL_checknumber(L, 1), amount = luaL_checknumber(L, 2);
    const int octaves = int(whole(numberArg(L, 3, 1), 1, 10));
    const double falloff = numberArg(L, 4, 0.5);
    const uint64_t seed = e.seed ^ mix64(uint64_t(whole(numberArg(L, 5, 0), INT32_MIN, INT32_MAX)));
    const double x = double(e.local) / e.fps * freq;
    if (!std::isfinite(x) || std::abs(x) * std::ldexp(1.0, octaves - 1) > 9e15) return luaL_error(L, "wiggle: %f is too fast", freq);
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
    if (!std::isfinite(x) || std::abs(x) > 9e15) return luaL_error(L, "noise: %f is too far", x);
    lua_pushnumber(L, noise1(x, current(L).seed ^ mix64(uint64_t(whole(numberArg(L, 2, 0), INT32_MIN, INT32_MAX)) + 7)));
    return 1;
}

int luaRandom(lua_State* L) {
    Eval& e = current(L);
    const double u = unit(mix64(e.seed ^ mix64(uint64_t(e.local)) ^ mix64(uint64_t(++e.randoms) << 32)));
    if (lua_isnoneornil(L, 1)) lua_pushnumber(L, u);
    else if (lua_isnoneornil(L, 2)) lua_pushnumber(L, u * luaL_checknumber(L, 1));
    else {
        const double lo = luaL_checknumber(L, 1), hi = luaL_checknumber(L, 2);
        lua_pushnumber(L, lo + (hi - lo) * u);
    }
    return 1;
}

int luaLoopOut(lua_State* L) {
    const Eval& e = current(L);
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
    const Eval& e = current(L);
    const Clip* c = e.clip;
    std::string name;
    if (lua_gettop(L) >= 2) {
        if (!e.sequence) return luaL_error(L, "param: no sequence to find the clip in");
        if (lua_type(L, 1) == LUA_TNUMBER) {
            int whole = 0;
            const lua_Integer id = lua_tointegerx(L, 1, &whole);
            if (!whole || id <= 0) return luaL_error(L, "param: %s is not a clip's id", luaL_tolstring(L, 1, nullptr));
            c = findClip(*e.sequence, {}, Id(id));
        } else {
            c = findClip(*e.sequence, luaL_checkstring(L, 1), 0);
        }
        if (!c) return luaL_error(L, "param: no clip %s", luaL_tolstring(L, 1, nullptr));
        name = luaL_checkstring(L, 2);
    } else {
        name = luaL_checkstring(L, 1);
    }
    if (!c) return luaL_error(L, "param: the clip this setting belongs to is not known here");
    const Effect* in = nullptr;
    const Param* p = findSetting(*c, name, &in);
    // At the same moment on the timeline (a setting without a value of its own is at its default).
    const FrameTime t = (e.clip ? e.clip->start : 0) + e.t - c->start;
    if (!p) {
        const size_t dot = name.find('.');
        const std::string type = dot == std::string::npos ? std::string("transform") : name.substr(0, dot);
        const std::string key = dot == std::string::npos ? name : name.substr(dot + 1);
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
    const Eval& e = current(L);
    const int track = int(whole(numberArg(L, 1, 0), -1, 1 << 20));
    const std::string band = luaL_optstring(L, 2, "all");
    if (!e.project || !e.sequence) {
        lua_pushnumber(L, 0);
        return 1;
    }
    if (track < 0 || track > int(e.sequence->audioTracks.size())) return luaL_error(L, "audio: no audio track %d", track);
    const double frame = double((e.clip ? e.clip->start : 0) + e.t);
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
    const double x = luaL_checknumber(L, 1), lo = luaL_checknumber(L, 2), hi = luaL_checknumber(L, 3);
    lua_pushnumber(L, std::isnan(x) ? lo : std::clamp(x, lo, std::max(lo, hi)));
    return 1;
}

// linear(t, t0, t1, v0, v1) or linear(t, v0, v1) for t from 0 to 1; ease the same, smooth at both ends.
int ramp(lua_State* L, bool smooth) {
    const double t = luaL_checknumber(L, 1);
    double t0 = 0, t1 = 1, v0, v1;
    if (lua_gettop(L) >= 5) t0 = luaL_checknumber(L, 2), t1 = luaL_checknumber(L, 3), v0 = luaL_checknumber(L, 4), v1 = luaL_checknumber(L, 5);
    else v0 = luaL_checknumber(L, 2), v1 = luaL_checknumber(L, 3);
    double u = t1 == t0 ? (t >= t1 ? 1.0 : 0.0) : std::clamp((t - t0) / (t1 - t0), 0.0, 1.0);
    if (std::isnan(u)) u = 0;
    if (smooth) u = u * u * (3 - 2 * u);
    lua_pushnumber(L, v0 + (v1 - v0) * u);
    return 1;
}
int luaLinear(lua_State* L) { return ramp(L, false); }
int luaEase(lua_State* L) { return ramp(L, true); }

int readOnly(lua_State* L) { return luaL_error(L, "%s cannot be changed", luaL_tolstring(L, 2, nullptr)); }

// Replaces the library table at `index` with one expressions can read but not change.
void protect(lua_State* L, int base, const char* name) {
    lua_newtable(L);
    lua_newtable(L);
    lua_getfield(L, base, name);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, readOnly);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, -2);
    lua_setfield(L, base, name);
}

State* state() {
    if (tState) return tState.get();
    tState = std::make_unique<State>();
    State* st = tState.get();
    lua_State* L = lua_newstate(allocate, st);
    if (!L) throw std::bad_alloc();
    st->L = L;
    lua_atpanic(L, panic);
    // Only what computes: no files, programs, clocks, module loading or loading code from strings; nothing that catches
    // errors (the time limit could not stop it) or reaches metatables (shared by every evaluation); no string matching,
    // repeating or splitting into bytes, and no moving or unpacking tables (they run in C, where the time limit is not
    // looked at).
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 4);
    lua_getglobal(L, LUA_STRLIBNAME);  // (the strings' own metatable reads it, so they go from the table itself)
    for (const char* name : {"find", "match", "gmatch", "gsub", "rep", "pack", "packsize", "unpack", "dump", "byte"}) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
    lua_getglobal(L, LUA_TABLIBNAME);  // (moving or unpacking a whole table is one instruction too)
    for (const char* name : {"move", "unpack"}) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
    lua_getglobal(L, LUA_MATHLIBNAME);  // (without its clock-seeded random numbers: random() is seeded by the setting)
    lua_pushnil(L);
    lua_setfield(L, -2, "random");
    lua_pushnil(L);
    lua_setfield(L, -2, "randomseed");
    lua_pop(L, 1);
    lua_newtable(L);  // the globals expressions read
    const int base = lua_gettop(L);
    for (const char* name : {"assert", "error", "ipairs", "pairs", "next", "select", "tonumber", "tostring", "type", "rawequal", "rawget", "rawlen"}) {
        lua_getglobal(L, name);
        lua_setfield(L, base, name);
    }
    for (const char* lib : {LUA_MATHLIBNAME, LUA_STRLIBNAME, LUA_TABLIBNAME}) {
        lua_getglobal(L, lib);
        lua_setfield(L, base, lib);
    }
    const luaL_Reg helpers[] = {{"wiggle", luaWiggle}, {"noise", luaNoise},   {"random", luaRandom}, {"loopOut", luaLoopOut},
                                {"param", luaParam},   {"audio", luaAudio},   {"clamp", luaClamp},   {"linear", luaLinear},
                                {"ease", luaEase},     {nullptr, nullptr}};
    lua_pushvalue(L, base);
    luaL_setfuncs(L, helpers, 0);
    lua_pop(L, 1);
    // math's functions by name too (not over the ones already there: math.type is not type).
    lua_getfield(L, base, LUA_MATHLIBNAME);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        lua_pushvalue(L, -2);
        if (lua_rawget(L, base) == LUA_TNIL) {
            lua_pop(L, 1);
            lua_pushvalue(L, -2);
            lua_insert(L, -2);
            lua_rawset(L, base);
        } else {
            lua_pop(L, 2);
        }
    }
    lua_pop(L, 1);
    for (const char* lib : {LUA_MATHLIBNAME, LUA_STRLIBNAME, LUA_TABLIBNAME}) protect(L, base, lib);
    st->base = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_sethook(L, budget, LUA_MASKCOUNT, 1000);
    return st;
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
    const std::string asExpression = "local _ENV = ...; return " + expr, asStatements = "local _ENV = ...; " + expr;
    if (luaL_loadbufferx(L, asExpression.data(), asExpression.size(), "=expression", "t") != LUA_OK) {
        lua_pop(L, 1);
        if (const int status = luaL_loadbufferx(L, asStatements.data(), asStatements.size(), "=expression", "t"); status != LUA_OK) {
            const std::string why = lua_tostring(L, -1) ? lua_tostring(L, -1) : "does not compile";
            lua_pop(L, 1);
            if (status == LUA_ERRSYNTAX) st.bad[expr] = why;  // (not running out of memory: it may compile next time)
            if (error) *error = why;
            return false;
        }
    }
    lua_pushvalue(L, -1);
    st.compiled[expr] = luaL_ref(L, LUA_REGISTRYINDEX);
    return true;
}

// Runs the compiled expression (its one argument) with its own globals for this evaluation, reading the shared ones:
// what one evaluation sets does not leak into the next. (A C function called protected: making the table can fail.)
int run(lua_State* L) {
    const Eval& e = current(L);
    lua_newtable(L);
    lua_newtable(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, tState->base);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pushnumber(L, e.keyed);
    lua_setfield(L, -2, "value");
    lua_pushnumber(L, double(e.local) / e.fps);
    lua_setfield(L, -2, "time");
    lua_pushinteger(L, lua_Integer(e.local));
    lua_setfield(L, -2, "frame");
    lua_pushnumber(L, e.fps);
    lua_setfield(L, -2, "fps");
    lua_pushnumber(L, e.clip ? double(e.clip->duration) / e.fps : 0.0);
    lua_setfield(L, -2, "duration");
    lua_call(L, 1, 1);
    return 1;
}

}  // namespace

bool expressionsAvailable() { return true; }

bool checkExpression(const std::string& expr, std::string* error) {
    try {
        State& st = *state();
        if (!compiled(st, expr, error)) return false;
        lua_pop(st.L, 1);
        return true;
    } catch (...) {
        if (!tEval.empty()) throw;  // (inside an expression: Lua's own error, for the call around it)
        tState.reset();
        if (error) *error = "out of memory";
        return false;
    }
}

double evaluateExpression(const Param& p, FrameTime t, double keyed, std::string* errorOut) {
    const bool outermost = tEval.empty();
    if (outermost) tLooped = false;
    const ExpressionContext where = currentExpressionContext();
    Owner scratch;
    Owner& owner = ownerOf(where, p, scratch);
    std::string error;
    double out = keyed;
    auto finish = [&]() {
        noteError(owner, error);
        if (errorOut) *errorOut = error;
        if (!error.empty()) return keyed;
        // Within the setting's range (angles turn on past a turn); switches and choices whole.
        if (owner.described) {
            if (owner.kind == ParamKind::Bool) out = out >= 0.5 ? 1.0 : 0.0;
            else if (owner.kind == ParamKind::Choice) out = std::round(out);
            if (owner.kind != ParamKind::Angle && owner.max > owner.min) out = std::clamp(out, owner.min, owner.max);
        }
        return out;
    };
    if (int(tEval.size()) >= kMaxDepth) {
        tLooped = true;
        error = "Expressions read each other in a loop";
        return finish();
    }
    Eval e;
    e.param = &p;
    e.t = t;
    e.local = t - p.exprFrom;
    e.keyed = keyed;
    e.project = where.project, e.sequence = where.sequence;
    e.clip = owner.clip;
    if (e.sequence) e.fps = std::max(1.0, e.sequence->fpsValue());
    e.seed = p.exprSeed                    ? mix64(p.exprSeed)
             : owner.effect && owner.clip ? mix64(expressionSeed(owner.clip->id, owner.effect->id, owner.name))
                                          : hashText(p.expr);
    try {
        State& st = *state();
        lua_State* L = st.L;
        const int top = lua_gettop(L);
        Evaluating evaluating(e);
        lua_pushcfunction(L, run);
        if (compiled(st, p.expr, &error)) {
            if (outermost) st.ticks = 0, st.started = std::chrono::steady_clock::now();  // (param()'s share its time)
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
        }
        lua_settop(L, top);
    } catch (...) {
        if (!outermost) throw;  // (Lua's own error, unwinding to the call around this one)
        tState.reset();        // (a panic: start again)
        error = "out of memory";
    }
    return finish();
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
    if (e == &c.timing) {  // (the clip's length follows its speed: it cannot come from a formula run a frame at a time)
        *error = "Speed cannot have an expression";
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
    Param& prm = it->second;
    prm.expr = expr;
    if (expr.empty()) prm.exprFrom = 0, prm.exprSeed = 0;
    else if (prm.exprSeed == 0) prm.exprSeed = expressionSeed(c->id, e->id, param);
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
            std::string why;
            const double v = evaluateExpression(prm, f, prm.keyedAt(f), &why);
            if (!why.empty()) return Result::fail("The expression fails at frame " + std::to_string(f) + ": " + why);
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
        // (Within a thousandth of what it spans: the setting's whole range could be far wider than its movement.)
        double lo = keys.empty() ? 0.0 : keys.front().v, hi = lo;
        for (const Keyframe& k : keys) lo = std::min(lo, k.v), hi = std::max(hi, k.v);
        thinKeys(keys, std::max(1e-9, (hi - lo) * 0.001));
    }
    Param& out = it->second;
    out.keys = std::move(keys);
    out.expr.clear();
    out.exprFrom = 0;
    out.exprSeed = 0;
    out.repeat = Repeat::Hold;
    if (!out.keys.empty()) out.value = out.keys.front().v;
    return {};
}

}  // namespace edit

}  // namespace montage
