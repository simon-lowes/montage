// Montage — drawing Gaussian splats (media/Splats.h), from the equations of Kerbl et al. (SIGGRAPH 2023) and the EWA
// splatting of Zwicker et al. (2002): each splat's 3D covariance is carried into the camera and projected to the
// picture through the projection's Jacobian at its centre, blurred by 0.3 px² so that none falls between pixels (its
// opacity made up for the blur when it was trained that way), and coloured by its spherical harmonics for the
// direction it is seen from. The splats are sorted by depth in 16 x 16 pixel tiles and blended front to back, each
// tile on its own, so the picture is the same however many threads draw it. Colours are the values the splats were
// trained on (display-referred, as their photographs were), blended as the training blended them.
#pragma once

#include <array>
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

struct SplatRenderOptions {
    int threads = 0;  // 0: the shared workers; otherwise this many threads (1: this one alone)
    // Only the splats whose centres are in this box (in the cloud's own space) are drawn.
    bool crop = false;
    std::array<double, 3> cropLo{0, 0, 0}, cropHi{0, 0, 0};
    float opacity = 1;
};

// The cloud through the camera, w x h, premultiplied. `depth` (w x h): the depth in front of the camera where the
// splats in front have covered half the pixel (else their weighted mean), 0 where nothing is drawn.
Image renderSplats(const SplatCloud& cloud, const Mat4& place, const ModelCamera& camera, int w, int h,
                   std::vector<float>* depth = nullptr, const SplatRenderOptions& options = {});

}  // namespace montage
