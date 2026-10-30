// Montage — keying uneven screens: a clean plate (the screen with nothing in front of it) for the Keyer to correct the
// screen against, pixel by pixel, and edges extended so no screen colour shows in hair and motion blur.
#pragma once

#include <memory>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

struct ColorSpace;

// Fills the holes in `v` (`channels` values a pixel, 1 to 4, w x h) where `known` is zero: pulled down a pyramid of
// half-size levels (the mean of the known values below each cell) and pushed back up, each level's unknown cells
// then relaxed towards the mean of their neighbours `sweeps` times (an eighth of that on the finest level, a quarter
// on the next): a smooth, harmonic fill, so a straight ramp across a hole comes back straight. False, with `v`
// unchanged, when nothing is known.
bool fillHoles(std::vector<float>& v, int channels, const std::vector<uint8_t>& known, int w, int h, int sweeps);

// How a clean plate is made from the footage (the parameters plate_* of the Keyer and of Clean Plate): the pixels
// whose screen colour dominates by at least `threshold` of the screen's own (`key`) are screen, shrunk by `erode` px
// so edges mixed with the subject are left out; over `frames` frames `spacing` apart (this one in the middle, the
// extra one of an even count after it) each pixel is the median of where it was screen; the rest is filled in; then
// blurred by `blur` px. The other frames are taken as the clip's source gives them, without the effects before the
// Keyer (so a plate over several frames is for a Keyer that comes first).
struct PlateSettings {
    float key[3] = {0.1f, 0.75f, 0.2f};
    float balance = 0.5f;
    float threshold = 0.5f;
    double erode = 3;
    int frames = 1;
    int spacing = 10;
    double blur = 0;
};
PlateSettings plateSettings(const Effect& e, FrameTime t);

// Which pixels of `img` (premultiplied) are screen, before shrinking.
std::vector<uint8_t> screenPixels(const Image& img, const PlateSettings& s);

// A clean plate from `img` (premultiplied, the picture at clip frame t; the effect's own colours), opaque, the same
// size. Other frames (for a median over time) come from the clip being rendered (render/Ofx.h currentFetch), in
// the colours the effect runs in. `found`, when given, is told which pixels were screen in this frame after
// shrinking. Empty when no screen was found.
Image makeCleanPlate(const PlateSettings& s, FrameTime t, const Image& img, double pixelScale, std::vector<uint8_t>* found = nullptr);

// The Clean Plate effect: the picture replaced with the clean plate made from it (or which pixels were screen).
void cleanPlate(const Effect& e, FrameTime t, Image& img, double pixelScale);

// The plate the Keyer `e` corrects the screen against for `img` (its Clean Plate setting): made from the picture, or
// the picture on its plate track (currentCleanPlate), in the effect's colours and resized to `img`. Empty for none.
Image keyerPlate(const Effect& e, FrameTime t, const Image& img, double pixelScale);

// Edge Extend: under semi-transparent edges (alpha below Solid), the colour of the solid foreground pushed outward (the
// solid pixels' own colours, filled outward by pull-push and premultiplied again by each edge pixel's alpha) in place of
// the colour there, by Amount, so the screen mixed into hair and motion blur goes; the alpha is kept.
void edgeExtend(const Effect& e, FrameTime t, Image& img);

// The track (index) the clip's first Keyer with a plate track (Clean Plate: Track) takes its plate from at sequence
// time t, or -1; with `hiddenOnly`, only when that track is to be hidden. (The clip must be one of seq's own.)
int cleanPlateTrack(const Sequence& seq, const Clip& c, FrameTime t, bool hiddenOnly = false);

// The picture of the track a clip's Keyer takes its clean plate from (as its source gives it: before the plate clip's
// own effects, after what runs on its source such as noise reduction), and the colour space the clip's effects start
// in: set by the compositor around the clip's effects.
struct CleanPlateContext {
    std::shared_ptr<const Image> plate;
    const ColorSpace* space = nullptr;  // (the space the clip's effects start in, which other frames are in too)
};
const CleanPlateContext& currentCleanPlate();
class CleanPlateScope {
public:
    explicit CleanPlateScope(CleanPlateContext context);
    ~CleanPlateScope();
    CleanPlateScope(const CleanPlateScope&) = delete;
    CleanPlateScope& operator=(const CleanPlateScope&) = delete;

private:
    CleanPlateContext previous_;
};

}  // namespace montage
