// Montage — the "model3d" generator: a 3D model (media/Model3D.h) placed in the shot and drawn (render/ModelRender.h)
// through either its own camera (in front of the frame as 3D titles are, sizes in pixels) or the camera solved from
// footage on another track by the 3D Camera Tracker (media/CameraSolve.h; sizes in the solved scene's units, the
// ground at y = 0), lit by a sun, a point or a spot light, by an ambient light or by a latitude-longitude picture on
// another track, casting shadows onto a ground that shows them alone, and playing one of its animations.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"
#include "render/ModelRender.h"

namespace montage {

// What the clip's model is drawn with at sequence frame t, on a w x h picture of the sequence's frame scaled by
// `scale` (output pixels a sequence pixel): the scene (in the world) and the camera. `trackLayer(track)` gives that
// video track's picture at t in the sequence's working space (for image-based light), empty when it has none. False
// with `error` when the model cannot be read or the solved camera is missing at t.
bool modelClipScene(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, int w, int h, double scale,
                    const std::function<Image(int track)>& trackLayer, ModelScene& scene, ModelCamera& camera,
                    std::shared_ptr<const Model>& model, std::string* error = nullptr);

// The clip's picture (premultiplied, in Rec.709 as other generators draw) and the depth of what is drawn (in front of
// the camera: pixels for its own camera, the scene's units for a solved one; 0 where nothing), w x h.
Image renderModelClip(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, int w, int h, double scale,
                      const std::function<Image(int track)>& trackLayer, std::vector<float>* depth = nullptr,
                      std::string* error = nullptr);

// The video track whose solved camera the clip uses at t (-1: none): its "camera_track" (1-based), else the nearest
// track below it whose clip at t carries a solve.
int modelCameraTrack(const Sequence& seq, size_t track, const Clip& c, FrameTime t);
// The track lighting it (-1: none).
int modelLightTrack(const Sequence& seq, size_t track, const Clip& c, FrameTime t);

}  // namespace montage
