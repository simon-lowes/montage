// Montage — image processing kernels for video effects, blending and transitions.
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"
#include "render/ColorSpace.h"
#include "render/VideoDenoise.h"

namespace montage {

// Applies one video filter effect in place. `t` is the clip-relative frame
// (for keyframes); `pixelScale` converts pixel-sized parameters (blur
// radius...) from sequence pixels to this image's pixels.
// `sourceSeconds` is the media time of the frame (for effects that follow
// the footage, such as Stabilize); -1 when there is none.
void applyVideoEffect(const Effect& e, FrameTime t, Image& img, double pixelScale, double sourceSeconds = -1);
// The values an effect is made for, in a sequence graded scene-referred: log (lift, gamma and gain, curves, the Colour
// Warper), light (blurs, lens effects, Relight), the picture as a display shows it (keys, stylising effects: run through
// the output transform and back), or whatever the working space holds. applyVideoEffect runs each in its own.
enum class EffectDomain { AsIs, Log, Linear, Display };
EffectDomain effectDomain(const std::string& type);

// Film Negative (render/FilmScan.h): the base found in the frame's rebate (or as set), the negative inverted in the
// picture's own curve `transfer`. Steady on Perforations: the frame moved so its perforations sit where they were on the
// reference frame. Both run where the clip's source frame is read, the lock first.
void applyFilmNegative(const Effect& e, FrameTime t, Image& img, Transfer transfer);
void applyPerfLock(const Effect& e, FrameTime t, Image& img);
// Dirt Removal and Dust Spot Removal (render/Restoration.h), which need the source frames either side, followed along the
// motion (either may be missing): run where the clip's source frame is read, through the effect's mask if it has one. Dirt
// Removal also takes the frames two away when it has them, for dirt that lasts two frames.
void applyDirtRemoval(const Effect& e, FrameTime lt, Image& src, const Image* prev, const Image* next, const MotionFn& toPrev,
                      const MotionFn& toNext, double pixelScale, double sourceSeconds = -1, const Image* prev2 = nullptr,
                      const Image* next2 = nullptr, const MotionFn& toPrev2 = {}, const MotionFn& toNext2 = {});
void applyDustSpots(const Effect& e, FrameTime lt, Image& src, const Image* prev, const Image* next, const MotionFn& toPrev,
                     const MotionFn& toNext, double pixelScale, double sourceSeconds = -1);
// A Paint effect's strokes at clip frame t, in the frame's pixels (each tracked one moved with the mask box).
struct PaintStrokeAt {
    bool heal = false;
    double dx = 0, dy = 0, radius = 1, softness = 0.5;
    std::vector<std::array<double, 2>> points;
};
std::vector<PaintStrokeAt> paintStrokesAt(const Effect& e, FrameTime t, int width, int height);

namespace gpu {
struct ColorOp;
}
// The GPU compositor's form of a colour effect at frame t (render/GpuCompositor.h): Color Correct, Curves, Hue Curves,
// .cube LUTs, ASC CDL, Colour Space Transform, Invert, Black & White, Hue/Saturation, Levels and the keyers (Chroma,
// Luma and Screen Key, its matte shrunk, grown or softened too), through a drawn mask and an HSL qualifier if it has them, appended to `out` (nothing when the effect is off or changes nothing). False for
// any other effect or mask (people, objects, depth, Refine Edges, Show Mask), which only the CPU draws. The mask's
// shape is drawn for a picture `width` by `height` (0: whether it could be, alone).
bool gpuColorOps(const Effect& e, FrameTime t, std::vector<gpu::ColorOp>& out, int width = 0, int height = 0, double pixelScale = 1);
// An effect's (or a grade node's) mask in that form, into `op`; false when only the CPU can make it. `showing`: Show
// Mask is the caller's to draw (a grade node's).
bool gpuMask(const Effect& e, FrameTime t, gpu::ColorOp& op, int width = 0, int height = 0, double pixelScale = 1,
             bool showing = false);

// The depth of the frame being worked on (media/DepthMap.h), for depth
// effects and depth qualifiers: set by the compositor for the effects of one
// clip on this thread, null when there is none.
struct DepthMap;
struct ValueMap;
const DepthMap* currentDepth();
// The depth Lens Blur (Depth) focuses at on frame t (`depth` W x H, 0 far .. 1 near): its Focus Depth, the depth at
// its Focus Point (between keys, the keys' depths eased from one to the other), or the nearest face's (currentFaces()).
double depthBlurFocus(const Effect& e, FrameTime t, const std::vector<float>& depth, int W, int H);
class DepthScope {
public:
    explicit DepthScope(std::shared_ptr<const DepthMap> depth);
    ~DepthScope();
    DepthScope(const DepthScope&) = delete;
    DepthScope& operator=(const DepthScope&) = delete;

private:
    std::shared_ptr<const DepthMap> previous_;
};
// The same for the people in the frame (media/Matting.h): Remove Background and People masks.
const ValueMap* currentPersonMatte();
class PersonScope {
public:
    explicit PersonScope(std::shared_ptr<const ValueMap> matte);
    ~PersonScope();
    PersonScope(const PersonScope&) = delete;
    PersonScope& operator=(const PersonScope&) = delete;

private:
    std::shared_ptr<const ValueMap> previous_;
};
// And the OpenEXR file the frame was read from, for Cryptomatte masks (its ID mattes; media/Exr.h); "" for any other.
const std::string& currentCryptomatteFile();
class CryptomatteScope {
public:
    explicit CryptomatteScope(std::string file);
    ~CryptomatteScope();
    CryptomatteScope(const CryptomatteScope&) = delete;
    CryptomatteScope& operator=(const CryptomatteScope&) = delete;

private:
    std::string previous_;
};
// How a frame being drawn takes effects that run for seconds a frame (a Learned Effect): `run` (deliveries, previews
// rendered ahead) runs them, else a result already made is used or the frame goes without, told to `issues`.
struct RenderIssues;
struct SlowEffects {
    bool run = true;
    const std::atomic<bool>* cancel = nullptr;
    RenderIssues* issues = nullptr;
};
const SlowEffects& currentSlowEffects();
class SlowEffectsScope {
public:
    explicit SlowEffectsScope(SlowEffects slow);
    ~SlowEffectsScope();
    SlowEffectsScope(const SlowEffectsScope&) = delete;
    SlowEffectsScope& operator=(const SlowEffectsScope&) = delete;

private:
    SlowEffects previous_;
};
// And the faces in the frame (media/Faces.h), for Face Refinement.
struct FaceBox;
const std::vector<FaceBox>* currentFaces();
class FaceScope {
public:
    explicit FaceScope(std::shared_ptr<const std::vector<FaceBox>> faces);
    ~FaceScope();
    FaceScope(const FaceScope&) = delete;
    FaceScope& operator=(const FaceScope&) = delete;

private:
    std::shared_ptr<const std::vector<FaceBox>> previous_;
};
// Redact Faces follows the faces its analysis found (media/FaceTracks.h) within the media seconds analysed; outside
// them, or with no analysis, it covers the faces found in the frame itself, so needs them detected.
bool redactNeedsLiveFaces(const Effect& e, double sourceSeconds);
// The media the frame comes from (its id) and whether it was reframed from 360° first, for the duration of a scope:
// an analysis is used only on the media it was made from (not on footage its effect was pasted onto, or replaced
// with) and not on a reframed view of it.
class TrackedSourceScope {
public:
    TrackedSourceScope(uint64_t media, bool reframed);
    ~TrackedSourceScope();
    TrackedSourceScope(const TrackedSourceScope&) = delete;
    TrackedSourceScope& operator=(const TrackedSourceScope&) = delete;

private:
    uint64_t previousMedia_;
    bool previousReframed_;
};
// The clip's surface track (its first Surface Tracker's, render/SurfaceTrack.h) for the duration of a scope, with the
// media time of each of the clip's frames: what masks that bend with the surface and paint that follows it read.
struct SurfaceTrack;
struct SurfaceContext {
    std::shared_ptr<const SurfaceTrack> track;
    std::function<double(FrameTime clipFrame)> mediaSeconds;
    // The picture as it was before a Surface Tracker held the surface still, for one further on to put it back on.
    mutable std::shared_ptr<const Image> heldPlate;
};
const SurfaceContext* currentSurface();
class SurfaceScope {
public:
    explicit SurfaceScope(SurfaceContext context);
    ~SurfaceScope();
    SurfaceScope(const SurfaceScope&) = delete;
    SurfaceScope& operator=(const SurfaceScope&) = delete;

private:
    SurfaceContext previous_;
};
// The outline of an effect's ellipse, rectangle or drawn mask at clip frame t, in pixels of a w x h frame (none for
// any other mask).
std::vector<std::array<double, 2>> maskOutline(const Effect& e, FrameTime t, int w, int h);
// The colour space the frame being worked on is in (the sequence's, set by the compositor for each sequence it draws),
// for effects that work in light rather than in code values (the HDR Palette); Rec.709 when none is set.
struct ColorSpace;
// An ASC CDL effect applied to the pixels as they are (the compositor's for a CDL made in the media's own space).
void applyCdlValues(const Effect& e, FrameTime t, Image& img);
// The ACES gamut compression effect on pixels in `space` (the compositor's on the media as it comes, in the media's space).
void applyAcesGamutCompress(const Effect& e, FrameTime t, const ColorSpace& space, Image& img);
// A CDL or LUT made on the camera's picture: its Apply In a scene-referred space (a camera log, ACEScct, ACES2065-1);
// null for any other effect. The compositor runs those first in a clip's chain on the media as it comes, in that space,
// before the input transform renders it for display (as an AMF's looks are).
const ColorSpace* sourceLookSpace(const Effect& e);
void applySourceLook(const Effect& e, FrameTime t, const ColorSpace& media, Image& img);
const ColorSpace& currentWorkingSpace();
class WorkingSpaceScope {
public:
    explicit WorkingSpaceScope(const ColorSpace* space);
    ~WorkingSpaceScope();
    WorkingSpaceScope(const WorkingSpaceScope&) = delete;
    WorkingSpaceScope& operator=(const WorkingSpaceScope&) = delete;

private:
    const ColorSpace* previous_;
};
// A soft matte's edge moved out by `expand` px (re-edged by distance, softened over
// `feather`), or else blurred by `blur` px; unchanged when both are zero.
void refineMatte(std::vector<float>& matte, int w, int h, double expand, double feather, double blur);

// The effect's mask (Effects.h maskInfo) over `img` as one value in 0..1 per
// pixel; the qualifier reads the colours of `img`, the depth qualifier
// currentDepth() (selecting nothing without one), an object mask the frame
// of the media at `sourceSeconds`. Empty when there is no mask.
std::vector<float> effectMatte(const Effect& e, FrameTime t, const Image& img, double pixelScale, double sourceSeconds = -1);

// Mask edges. Holes and gaps narrower than twice `px` filled (a closing, beyond the frame counting as outside; what
// it adds feathered by `feather` px, as the shape is, and the shape's own edge kept); and, within `radius` px of the
// edge and across a soft edge, each pixel's alpha taken from where `guide`'s colour lies between the object's and the
// background's colours nearby (a colour-line matte), mixed in by `amount` (0..1), so the edge follows the picture's.
void consolidateMatte(std::vector<float>& matte, int w, int h, double px, double feather = 1);
void refineMatteEdges(std::vector<float>& matte, const Image& guide, double radius, double amount);

// Euclidean distance (px) from each pixel to the nearest pixel where seed is
// non-zero (exact; Felzenszwalb–Huttenlocher). Very large where there is no seed.
std::vector<float> distanceTransform(const std::vector<uint8_t>& seed, int w, int h);

// An object mask's matte at w x h from its logits (ObjectMask.h grid): a
// sub-pixel edge, moved out by `expand` px and softened over `feather` px.
// A matte from a field positive inside and crossing zero at the edge (feather and expansion in pixels).
std::vector<float> fieldMatte(const std::vector<float>& field, int w, int h, double feather, double expand);
// How much of each pixel a polygon (pixel coordinates, non-zero winding) covers, 0 to 1.
std::vector<float> polygonCoverage(const std::vector<std::pair<double, double>>& poly, int w, int h);
std::vector<float> objectMatte(const std::vector<float>& logits, int w, int h, double feather, double expand);

// 3D LUT loaded from a .cube file.
struct Lut3D {
    int size = 0;
    float domainMin[3] = {0, 0, 0};
    float domainMax[3] = {1, 1, 1};
    std::vector<float> data;  // size^3 * 3, red varies fastest
    bool is1D = false;
    void apply(float& r, float& g, float& b) const;
};
std::shared_ptr<const Lut3D> loadCubeLut(const std::string& path, std::string* error = nullptr);
// A .cube or CLF file's transform on one colour, as the LUT effect applies it; null (with the reason) when it cannot be
// read.
std::function<void(float[3])> lutTransform(const std::string& path, std::string* error = nullptr);
// The space a CDL or LUT effect's "Apply In" names (null: the working space).
const ColorSpace* effectApplySpace(const Effect& e);
// Drops a cached .cube so the next load reads the file again (after it was rewritten).
void forgetCubeLut(const std::string& path);

// Monotone cubic curve through "x,y x,y ..." points, sampled into `n` entries.
std::vector<float> buildCurve(const std::string& points, int n = 1024);
// A Hue Curves curve: points "x,y" with x in 0..1 (hue, or luma/saturation) and
// y in 0..1, 0.5 meaning no change; smooth through the points without overshoot,
// wrapping round when `cyclic` (hue), flat past the ends otherwise. No points:
// 0.5 throughout. n + 1 samples over 0..1.
std::vector<float> buildFlatCurve(const std::string& points, int n = 360, bool cyclic = true);

// Composites `src` over `dst` (same size) with a blend mode and opacity.
void blendOnto(Image& dst, const Image& src, const std::string& mode, float opacity);
// A blend mode's index ("normal" 0, "add", "multiply", "screen", "overlay"...), and one channel of it: `source` blended
// onto `backdrop` (straight, not premultiplied).
int blendModeIndex(const std::string& mode);
float blendModeChannel(int mode, float backdrop, float source);

// Mixes outgoing `a` and incoming `b` (either may be empty = transparent) for
// transition progress u in [0,1]; result has the given size.
Image transitionMix(const std::string& type, const Effect& params, const Image& a, const Image& b, double u, int w,
                    int h);

// Box-blur approximation of a Gaussian, separable; radius in pixels.
void gaussianBlur(Image& img, double radius, bool horizontal = true, bool vertical = true);

// Builds a primary colour correction that neutralises the image's colour
// cast (grey world) and stretches its levels to 0.5 % / 99.5 % percentiles.
Effect autoColorCorrection(const Image& img, Id effectId);
// Builds a primary colour correction that makes `img` look like `reference`
// (shot matching): per channel, lift and gain put its shadows and highlights
// (5th and 95th percentiles) on the reference's and gamma its mid-tones
// (medians). The pictures need not show the same thing, only similar light.
Effect colorMatchCorrection(const Image& img, const Image& reference, Id effectId);
// The colour of a green or blue screen in a picture: the cleaner half of the pixels where green (or blue, whichever
// covers more) clearly leads the other two, as a median. False if neither screen colour covers 2 % of the picture.
bool estimateScreenColor(const Image& img, double rgb[3]);

// Flattens premultiplied RGBA over an opaque colour.
void flattenOver(Image& img, float r, float g, float b);

}  // namespace montage
