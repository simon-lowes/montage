// Montage — Group Shots: a cut's shots grouped by camera angle and by scene, for grading (core/ColorGroups.h). Each
// shot is described without a model of its own: CIE L*a*b* histograms of its middle frame and of its median frame (each
// pixel's median over the shot, so what moves through it counts less), a 32 x 18 L*a*b* thumbnail of its layout, its
// camera metadata (camera, reel, lens, ISO, white balance), its source file and time, the faces in it (YuNet, when
// installed) and, for media Find Shots has indexed, its visual embedding. Shots are clustered by average linkage, and a
// sensitivity sets where the tree is cut: the clusters are angle groups. Angle groups the cut goes back and forth
// between make a scene, unless the media's scene metadata names the scenes.
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/ColorGroups.h"
#include "core/Model.h"

namespace montage {

constexpr int kShotHistBins = 16;          // per L*, a*, b* channel
constexpr int kShotLayoutW = 32, kShotLayoutH = 18;

struct ShotTraits {
    Id clip = 0, media = 0;
    FrameTime start = 0;              // in the sequence
    double sourceSeconds = 0;         // the middle frame's, in its media
    std::array<float, 3 * kShotHistBins> histogram{}, medianHistogram{};  // each channel's sums to 1
    std::vector<float> layout;        // kShotLayoutW x kShotLayoutH x (L*, a*, b*)
    std::map<std::string, std::string> camera;  // "camera", "tape", "lens", "iso", "wb" (where the media has them)
    std::string scene;                // the media's scene metadata ("" none)
    int faces = -1;                   // -1 not looked for
    std::vector<float> facePlaces;    // each face's centre x, y and height, as fractions of the picture
    std::vector<float> embedding;     // the visual index's, unit length (empty: none)
    std::string embeddingModel;       // the model it comes from (only embeddings of one model are compared)
    bool seen = true;                 // its frames were read (an offline shot is compared by what else it has)
};

// Each picture clip's traits (all the sequence's video clips from media when `clips` is empty), in timeline order.
// `visualIndex` adds the embeddings Find Shots made, where the media has them; faces are looked for when the face
// model is installed.
std::vector<ShotTraits> shotTraits(const Project& p, const Sequence& s, const std::vector<Id>& clips = {},
                                           bool visualIndex = false, const std::function<void(double)>& progress = {},
                                           const std::atomic<bool>* cancel = nullptr);

// How unalike two shots are, 0 (the same angle beyond doubt) to 1: a weighted mean of what both have.
double shotDistance(const ShotTraits& a, const ShotTraits& b);

// Average-linkage clustering of the shots by `distance` (n x n, symmetric), cut at `threshold`: each shot's cluster
// (numbered by first appearance). A lower threshold only ever splits clusters.
std::vector<int> clusterShots(const std::vector<std::vector<double>>& distance, double threshold);
// The cut for a sensitivity from 0 (few, broad groups) to 1 (many, close groups); it falls as the sensitivity rises.
double groupingThreshold(double sensitivity);

// The scene of each angle group (numbered by first appearance), from the angle of each shot in cut order: two angles
// the cut goes between at least twice share a scene, and an angle seen once between two shots of one scene joins it.
std::vector<int> scenesOfAngles(const std::vector<int>& angleInCutOrder, int angles);

// The adjusted Rand index of two labellings of the same items (1: the same grouping; about 0: chance).
double adjustedRandIndex(const std::vector<int>& a, const std::vector<int>& b);

struct GroupShotsOptions {
    double sensitivity = 0.5;
    bool scenes = true;        // scene groups around the angle groups
    bool visualIndex = false;  // use Find Shots' embeddings where there are some
};

// The clips Group Shots leaves where they are: in a group made by hand, or a locked one (or inside a locked one).
bool keptOutOfGrouping(const Sequence& s, const Clip& c);

// The grouping of the shots described (in timeline order). `kept`: the shots of locked angle groups (or angles inside a
// locked scene), as (start, their angle group), which take part in finding the scenes; a scene with a kept angle in a
// scene group keeps that scene group (unless it is locked). keptShots gives them; shots in groups made by hand are left
// out, as such a group (a look used across the film) says nothing of where scenes are.
ShotGrouping planShotGroups(const std::vector<ShotTraits>& shots, const GroupShotsOptions& o,
                            const std::vector<std::pair<FrameTime, Id>>& kept = {}, const Sequence* s = nullptr);
std::vector<std::pair<FrameTime, Id>> keptShots(const Sequence& s);

}  // namespace montage
