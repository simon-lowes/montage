// Montage — drawing Gaussian splats (media/Splats.h), from the equations of Kerbl et al. (SIGGRAPH 2023) and the EWA
// splatting of Zwicker et al. (2002): each splat's 3D covariance is carried into the camera and projected to the
// picture through the projection's Jacobian at its centre, blurred by 0.3 px² so that none falls between pixels (its
// opacity made up for the blur when it was trained that way), and coloured by its spherical harmonics for the
// direction it is seen from. The splats are sorted by depth in 16 x 16 pixel tiles and blended front to back, each
// tile on its own, so the picture is the same however many threads draw it. Colours are the values the splats were
// trained on (display-referred, as their photographs were), blended as the training blended them.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "media/Image.h"
#include "media/Model3D.h"
#include "media/Splats.h"
#include "render/ModelRender.h"

namespace montage {

// Splat i placed in the world by `place` (as a model's matrix places it) and seen by `camera`: where its centre falls
// (pixels), its covariance there (pixels², before the blur) and its depth in front of the camera. False when its centre
// is not in front of the camera.
struct ProjectedSplat {
    double x = 0, y = 0;
    double xx = 0, xy = 0, yy = 0;
    double depth = 0;
};
bool projectSplat(const SplatCloud& cloud, size_t i, const Mat4& place, const ModelCamera& camera, ProjectedSplat& out);

// What a render did, when asked: the splats there were to draw (kept by the options), those whose shape and colour
// were worked out (the rest were told to be off the picture from their centre and largest size alone), and those
// that reached the picture.
struct SplatRenderStats {
    size_t offered = 0, projected = 0, drawn = 0;
};

struct SplatRenderOptions {
    int threads = 0;  // 0: the shared workers; otherwise this many threads (1: this one alone)
    // Only the splats whose centres are in this box (in the cloud's own space) are drawn.
    bool crop = false;
    std::array<double, 3> cropLo{0, 0, 0}, cropHi{0, 0, 0};
    float opacity = 1;
    // Only the splats with a 1 here are drawn (one byte a splat, media/Splats.h splatKeep); null: all.
    std::shared_ptr<const std::vector<uint8_t>> keep;
    // Splats that cannot reach the picture are passed over before their shape and colour are worked out. The picture
    // is the same either way (off only to compare).
    bool cullToView = true;
    // Each splat's centre alone, as one pixel of its colour, the nearest in front (a quick look at a heavy capture).
    bool points = false;
    SplatRenderStats* stats = nullptr;
};

// The splats of `cloud` (those `keep` keeps, or all) whose centres, placed by `place` and seen by `camera`, fall inside
// the closed outline `polygon` (picture pixels, even-odd), in front of the camera: one byte a splat.
std::vector<uint8_t> splatsInOutline(const SplatCloud& cloud, const Mat4& place, const ModelCamera& camera,
                                     const std::vector<std::array<double, 2>>& polygon, const std::vector<uint8_t>* keep = nullptr);
// The same for a brush: within `radius` pixels of the stroke through `points`.
std::vector<uint8_t> splatsUnderBrush(const SplatCloud& cloud, const Mat4& place, const ModelCamera& camera,
                                      const std::vector<std::array<double, 2>>& points, double radius, const std::vector<uint8_t>* keep = nullptr);

// The cloud through the camera, w x h, premultiplied. `depth` (w x h): the depth in front of the camera where the
// splats in front have covered half the pixel (else their weighted mean), 0 where nothing is drawn.
Image renderSplats(const SplatCloud& cloud, const Mat4& place, const ModelCamera& camera, int w, int h,
                   std::vector<float>* depth = nullptr, const SplatRenderOptions& options = {});

// The splats renderSplats would blend (the options as it takes them; `depth` aside), worked out here with its
// arithmetic, for a renderer that sorts and blends them itself (the GPU compositor): each one's depth in front of the
// camera, and 12 floats a splat, in the cloud's order. Each splat: its centre on the picture (pixels), how far from it
// it is seen (pixels), its opacity, the inverse of its covariance there (xx, xy, yy) and the power below which a pixel
// is left out (fainter than 1/255), and its colour; it is blended at a pixel's centre, nearest first (equal depths in
// the cloud's order), as alpha = min(0.99, opacity exp(power)) for power = -(xx dx² + yy dy²) / 2 - xy dx dy (dx, dy
// from the pixel's centre to the splat's), the blend stopping where less than 1e-4 of the pixel is left uncovered; a
// linear cloud's colour is then brought to display values under its coverage, and the whole is the layer's opacity
// of that. With `points`, those whose centres fall on the picture: their centres and their colours as they are drawn
// (display values, the layer's opacity in), each pixel taking the nearest. Returns how many.
// `tiles` (Gaussians only) takes the 16-pixel tiles each one reaches, as the CPU lists it in them: the first and last
// across, then down (4 a splat).
size_t splatsForBlending(const SplatCloud& cloud, const Mat4& place, const ModelCamera& camera, int w, int h, const SplatRenderOptions& options,
                         std::vector<float>& records, std::vector<float>& depths, std::vector<int32_t>* tiles = nullptr);

}  // namespace montage
