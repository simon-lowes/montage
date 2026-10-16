// Montage — parameter expressions (After Effects' expressions, Fusion's expressions, Motion's behaviours): a short Lua
// formula on any number setting of a clip that gives its value at each frame from the keyed one (`value`), the time,
// other clips' settings and the sound. Param::expr holds it; Param::at runs it (render/Expressions.cpp). Each render
// thread has its own sandboxed Lua state (no files, programs or clocks; noise and random numbers seeded by the setting),
// so a frame renders the same wherever and whenever it is drawn. A failing expression gives the keyed value and its
// error (expressionError) for the Inspector.
//
// In an expression (a Lua expression, or statements ending in `return`):
//   value         the setting's keyed (or still) value at this frame
//   time, frame   seconds and frames from the clip's start; fps the sequence's frame rate; duration the clip's, in seconds
//   wiggle(freq, amount [, octaves, falloff, seed])  value plus smooth noise: `freq` wobbles a second, about ±amount
//   noise(x [, seed])   smooth noise from -1 to 1;  random([lo [, hi]])  a new number each frame (0..1 without bounds)
//   loopOut([kind])     the keys played again after the last one: "cycle", "pingpong", "offset" or "continue"
//   param([clip,] name) another setting's value at this moment: a clip by name or id (this clip when left out); the
//                       setting by name ("pos_x"), or "effect.name" for an effect's ("gaussian_blur.radius")
//   audio([track [, band]])  an audio track's level (1-based, 0 or none = all tracks), 0..1, in "low", "mid", "high"
//   linear(t, t0, t1, v0, v1), ease(...)  ramps (ease is smooth at both ends); clamp(x, lo, hi); Lua's math functions
//                                         by name (sin, abs, pi...) as well as in math
#pragma once

#include <functional>
#include <string>

#include "core/Model.h"

namespace montage {

struct Project;
struct Sequence;
struct Clip;

// The value of `p` at clip frame `t` from its keyed value `keyed` (Param::at calls it when it has an expression).
double evaluateExpression(const Param& p, FrameTime t, double keyed);

// What expressions evaluated on this thread belong to, while it lives: the project and sequence, and the clip when it
// is known (else the clip whose setting it is is looked up). Scopes nest. Renders set one; without any, the thread's
// fallback (setExpressionFallback) is asked, and failing that `param` and `audio` give 0 and the frame rate is 25.
class ExpressionScope {
public:
    ExpressionScope(const Project* p, const Sequence* s, const Clip* c = nullptr);
    ~ExpressionScope();
    ExpressionScope(const ExpressionScope&) = delete;
    ExpressionScope& operator=(const ExpressionScope&) = delete;

    const Project* project;
    const Sequence* sequence;
    const Clip* clip;
    ExpressionScope* previous;
};

struct ExpressionContext {
    const Project* project = nullptr;
    const Sequence* sequence = nullptr;
    const Clip* clip = nullptr;
};
// Where this thread's expressions belong now (its scope, else its fallback), for work it hands to other threads.
ExpressionContext currentExpressionContext();
// For this thread: where expressions evaluated outside any scope belong (the app's window: its open project).
void setExpressionFallback(std::function<ExpressionContext()> fallback);

bool expressionsAvailable();  // built with Lua
// Whether `expr` compiles; false with the error.
bool checkExpression(const std::string& expr, std::string* error = nullptr);
// The error `expr` last gave when it ran (on any thread), "" when it last ran without one.
std::string expressionError(const std::string& expr);

}  // namespace montage
