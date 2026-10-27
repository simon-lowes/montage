// Montage — a learned effect's examples taken from a clip (media/LearnedEffect.h): each frame as the clip's picture is
// before its effects and with them, and the network then given to clips as their Learned Effect.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/LearnedEffect.h"

namespace montage {

// The clip's picture at timeline frame t, alone, at its own size and unmoved: without its effects, and with them (its
// Paint, colour corrections, masks, mattes; those a Learned Effect on it stands in for on again, the Learned Effect
// left out). Its colour group's pre-clip grade is in both (it runs before the network); its grade's nodes and the
// group's post-clip grade in neither (they run after it). `out.frame` is t. False (with `error`) when the clip has no
// picture there.
bool learningPair(const Project& p, const Sequence& s, const Clip& c, FrameTime t, learn::Pair& out, std::string* error = nullptr);

// The most pixels each of `frames` examples keeps, so that together they stay within what training can hold (about a
// gigabyte: a few 4K frames whole, a minute's markers on 8K each a part).
int64_t learningPixelsEach(size_t frames);
// Cuts a pair larger than `maxPixels` down to the part of the frame (its shape, at full size, so the network still
// learns at the picture's scale) where before and after differ most: where it was worked on.
void fitLearningPair(learn::Pair& p, int64_t maxPixels);

// The frames the clip was worked on, to learn from: the sequence's markers on it, else where its effects (those a
// Learned Effect stands in for too) have keys, else the playhead (when on it). Timeline frames, in order.
std::vector<FrameTime> exampleFrames(const Sequence& s, const Clip& c, FrameTime playhead);

// The network a clip's Learned Effect runs ("" when it has none).
std::string learnedNetworkOf(const Clip& c);
// A new file for a clip's network, beside the project in "<project> Learned Effects/" (or in the system's temporary
// folder for a project never saved): one no clip of the project runs, so training again never changes what clips
// already show (an undo puts back the one they had), and no file is at, unless `reuse` (a saved project's network
// trained before and not given, to go on from).
std::string newLearnedNetworkPath(const std::string& projectPath, const Project& p, const Clip& c, bool reuse = false);
// A project saved for the first time keeps its networks: those its clips run from the temporary folder (made before it
// was saved) are copied beside it, as newLearnedNetworkPath() names them, and the clips pointed at the copies. Returns
// the networks copied, from where to where.
std::map<std::string, std::string> keepLearnedNetworks(Project& p, const std::string& projectPath);
// Points the clips running the networks `moved` (keepLearnedNetworks()) at their copies.
void repointLearnedNetworks(Project& p, const std::map<std::string, std::string>& moved);
// Whether a clip of the project runs that network.
bool networkInUse(const Project& p, const std::string& network);

// Gives each video clip a Learned Effect running `network`, first in its effects (or points the one it has at it).
// With `replaceExamples`, the effects on `examples` that made the examples are turned off (the network now does their
// work there too) and marked (`learned_off`) for learningPair. Returns how many clips were changed.
int applyLearnedNetwork(Project& p, Sequence& s, const std::vector<Id>& clips, const std::string& network, Id examples = 0,
                        bool replaceExamples = false);

}  // namespace montage
