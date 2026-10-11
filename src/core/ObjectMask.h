// Montage — AI object masks: the clicks that pick an object out of a clip and
// the per-frame segmentation tracked from them (media/Segmenter.h runs the
// model). Effects reach it through Effect::object when their mask shape is
// "Object"; the data is immutable and shared, so undo snapshots stay small.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace montage {

// A prompt on one frame, in fractions of the media frame.
struct ObjectPoint {
    double x = 0, y = 0;
    int label = 1;  // 1 = the object, 0 = not the object, 2 / 3 = box top-left / bottom-right
    bool operator==(const ObjectPoint&) const = default;
};

// Mask logits on the model's grid: kObjectGrid x kObjectGrid values spanning
// the whole frame (row-major, top row first); > 0 is inside the object.
constexpr int kObjectGrid = 256;

struct ObjectMask {
    double fps = 0;  // the media's frame rate: frame n is on screen from n / fps
    std::map<int64_t, std::vector<ObjectPoint>> prompts;  // frames with clicks
    std::map<int64_t, std::string> frames;                // segmented frames (packObjectLogits)
    std::string name;       // its name ("" shows as "Object N")
    bool subtract = false;  // cut out of the mask (a further object; the first always adds)
    // Further objects in the same mask (Premiere 26.5's and Resolve's several objects in one mask): each clicked and
    // tracked on its own, at the same frame rate. Added ones join the mask, subtracted ones are cut out of it.
    std::vector<ObjectMask> more;

    int64_t frameAt(double seconds) const;
    // This object's logits on `frame` (kObjectGrid² values); false if it was not segmented.
    bool logits(int64_t frame, std::vector<float>& out) const;
    // The segmented frame on screen at `seconds`.
    bool logitsAt(double seconds, std::vector<float>& out) const;
    // The objects: 0 is this one, then `more` (an index out of range clamps).
    int count() const { return 1 + int(more.size()); }
    ObjectMask& object(int i) { return i <= 0 || more.empty() ? *this : more[size_t(std::min<int>(i, int(more.size())) - 1)]; }
    const ObjectMask& object(int i) const { return const_cast<ObjectMask*>(this)->object(i); }
    // The whole mask's logits on `frame` (or at `seconds`): the added objects joined (the largest logit at each point),
    // the subtracted ones cut out; false when no added object was segmented there.
    bool maskLogits(int64_t frame, std::vector<float>& out) const;
    bool maskLogitsAt(double seconds, std::vector<float>& out) const;
    bool operator==(const ObjectMask&) const = default;
};

// Logits quantised to 1/8 (clamped to ±15.9, where the sigmoid is saturated) and deflated.
std::string packObjectLogits(const float* logits);
bool unpackObjectLogits(const std::string& packed, std::vector<float>& out);

// Fraction of the grid inside the object.
double objectCoverage(const std::vector<float>& logits);

}  // namespace montage
