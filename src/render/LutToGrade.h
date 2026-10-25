// Montage — a LUT rebuilt as an editable grade: a show LUT, or a look from elsewhere, turned into Montage's own
// controls, fitted to it (Levenberg–Marquardt with Huber weights over its lattice): an ASC CDL (slope, offset, power and
// saturation), an RGB Mixer (crosstalk between the channels), a monotone tone curve per channel (Curves) and Hue vs Hue
// and Hue vs Saturation (Hue Curves). A stage is kept only when it brings the fit closer (mean CIEDE2000 through
// Rec.709, skin tones weighted up). Whatever they still miss can follow as a small residual LUT that can be switched
// off. The fit happens in the space the LUT works in ("Apply In"), so a LUT made for a camera log is rebuilt there,
// between two Colour Space Transforms.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"
#include "render/Processing.h"

namespace montage {

struct ColorSpace;

struct LutGrade {
    std::vector<Effect> effects;      // the kept stages in order (ids 0), in the LUT's own space
    std::vector<std::string> stages;  // their effect types
    double meanDeltaE = 0, maxDeltaE = 0;  // the stages against the LUT over its lattice
    Lut3D residual;                        // 17³ after the stages: the rest of the LUT
    double residualMeanDeltaE = 0, residualMaxDeltaE = 0;  // the stages and the residual against the LUT
    // A LUT that clips (holds a range of colours flat at its top, along grey or a primary) is said to, with where: the
    // controls may clip as it does (a CDL does), or the residual carries it.
    bool clips = false;
    std::string clipNote;
};

// Fits the grade to `lut`, a transform on colours in 0..1 of `space` (how it is applied: the values it takes and gives).
// False with `error` when cancelled.
bool fitLutGrade(const std::function<void(float[3])>& lut, const ColorSpace& space, LutGrade& out, std::string* error = nullptr,
                 const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

namespace edit {
// The LUT a clip's LUT effect (`lutEffect`, 0: its first) applies and the space it applies it in, for fitLutGrade
// (under the sequence's working space); false with `error` when it cannot be rebuilt.
// `resolved`: the LUT effect's id. It is refused when off, masked, at part strength or animated.
bool lutGradeSource(const Sequence& s, Id clip, Id lutEffect, std::function<void(float[3])>& lut, const ColorSpace*& space,
                    std::string* error = nullptr, Id* resolved = nullptr);
// Rebuilds a clip's LUT effect (`lutEffect`, 0: its first) as a grade in its place: the fitted stages, then the residual
// LUT written to `residualPath` (none when empty, or when the stages miss nothing; refused when the LUT or another
// effect of the project reads that file), between Colour Space Transforms when the LUT is applied in another space. The
// grade as it was is kept as a grade version ("With LUT" when it was the clip's only one).
Result lutToGrade(Project& p, Sequence& s, Id clip, Id lutEffect, const std::string& residualPath, LutGrade* fit = nullptr,
                  const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);
// The second half of lutToGrade: puts a grade already fitted to the LUT in its place.
Result applyLutGrade(Project& p, Sequence& s, Id clip, Id lutEffect, const std::string& residualPath, const LutGrade& fit);
}  // namespace edit

}  // namespace montage
