// Montage — automatic trims from the HDR master (the shot-by-shot analysis HDR delivery tools run before a colourist
// trims): each shot's light is measured from the master (its log-average, 1st and 99.9th percentiles and peak, in
// nits), and a starting trim for each target worked out from it. A shot's highlights are rolled off from its own
// brightest light rather than the master's peak (into SDR to just short of white), shadows SDR would crush are lifted,
// saturation makes up for the roll-off, and gamma keeps the shot's mid-grey where Montage's HDR-to-SDR mapping puts it
// (BT.2408's 203-nit reference white at SDR white). Shots of one colour group are pulled towards the group's median
// mid-grey, so a cut within a scene does not jump. Analyse Trims fills only shots that have no trim of their own.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

struct ShotLight {
    Id clip = 0;
    FrameTime from = 0, to = 0;  // the first frame it is the shot at and one past the last (sequence frames)
    double logAverage = 0;       // nits: the geometric mean of its luminance (its mid-grey)
    double low = 0;              // nits: the 1st percentile of luminance
    double high = 0;             // nits: the 99.9th percentile of the brightest channel
    double peak = 0;             // nits: the brightest channel's brightest
    int frames = 0;              // frames measured
};

// Measures each shot (render/TrimPasses trimShot) from `from` to `to` (exclusive; the whole sequence when `to` is not
// after `from`): up to `framesPerShot` frames across it, rendered from the master at most `width` pixels wide. False
// with `error` when the sequence is not HDR (PQ or HLG), has no shots, or is cancelled.
bool measureShotLight(const Project& p, const Sequence& s, FrameTime from, FrameTime to, std::vector<ShotLight>& out,
                      std::string* error = nullptr, const std::function<void(double)>& progress = {},
                      const std::atomic<bool>* cancel = nullptr, int framesPerShot = 9, int width = 480);

struct AutoTrim {
    Id clip = 0;
    std::string target;
    std::map<std::string, double> values;  // the trim's controls that differ from their defaults
    double midGrey = 0;                    // nits: the shot's own mid-grey (its log-average)
    double aimedGrey = 0;                  // nits: where it is aimed (pulled towards its colour group's median)
    double greyCode = 0;                   // the target's code value the aimed mid-grey has with no trim
};

// The trims for `target` (an id of the sequence's targets) from the shots' light.
std::vector<AutoTrim> solveAutoTrims(const Sequence& s, const std::string& target, const std::vector<ShotLight>& shots);
// The code value a neutral grey of `nits` in the master has on `target` with `trim` (null: none).
double trimmedGrey(const Sequence& s, const TrimTarget& target, const Effect* trim, double nits);
// Whether a clip has a trim of its own for a target: one with keyframes or a control off its default.
bool hasOwnTrim(const Clip& c, const std::string& target);

namespace edit {
// Puts the trims on their clips where they have none of their own (`filled` counted in Result::applied); fails when
// none could be.
Result fillAutoTrims(Sequence& s, const std::vector<AutoTrim>& trims);
}  // namespace edit

}  // namespace montage
