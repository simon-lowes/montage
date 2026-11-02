// Montage — the "model3d" generator: a 3D model (media/Model3D.h) placed in the shot and drawn (render/ModelRender.h)
// through either its own camera (in front of the frame as 3D titles are, sizes in pixels) or the camera solved from
// footage on another track by the 3D Camera Tracker (media/CameraSolve.h; sizes in the solved scene's units, the
// ground at y = 0) or a camera in a USD or glTF file (the model's own, or another file named for the camera, as the
// file places everything), lit by a sun, a point or a spot light or by the file's own lights, by an ambient light or
// by a latitude-longitude picture on another track (or the file's dome light), casting shadows onto a ground that
// shows them alone, and playing one of its animations. Or Gaussian splats (media/Splats.h, a captured scene, a
// numbered run of files played as an animation) placed and seen the same ways, cropped to a box and drawn by
// render/SplatRender.h; their own light is in their colours.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/CameraSolve.h"
#include "media/Image.h"
#include "render/ModelRender.h"
#include "render/SplatRender.h"

namespace montage {

// What the clip's model is drawn with at sequence frame t, on a w x h picture of the sequence's frame scaled by
// `scale` (output pixels a sequence pixel): the scene (in the world) and the camera. `trackLayer(track)` gives that
// video track's picture at t in the sequence's working space (for image-based light), empty when it has none. False
// with `error` when the model cannot be read or the solved camera is missing at t.
// `trackLayerAt(track, at)` gives a track's picture at another frame (a projected picture held at one; without it,
// the picture at t is projected).
bool modelClipScene(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, int w, int h, double scale,
                    const std::function<Image(int track)>& trackLayer, ModelScene& scene, ModelCamera& camera,
                    std::shared_ptr<const Model>& model, std::string* error = nullptr,
                    const std::function<Image(int track, FrameTime at)>& trackLayerAt = {});

// The same for a clip of Gaussian splats: the cloud showing at t, where it is placed, the camera and how it is drawn.
bool splatClipScene(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, double scale,
                    std::shared_ptr<const SplatCloud>& cloud, Mat4& place, ModelCamera& camera, SplatRenderOptions& options,
                    std::string* error = nullptr);

// The clip's picture (premultiplied, in Rec.709 as other generators draw) and the depth of what is drawn (in front of
// the camera: pixels for its own camera, the scene's units for a solved or a file's one; 0 where nothing), w x h.
Image renderModelClip(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, int w, int h, double scale,
                      const std::function<Image(int track)>& trackLayer, std::vector<float>* depth = nullptr,
                      std::string* error = nullptr, const std::function<Image(int track, FrameTime at)>& trackLayerAt = {});

// The video track whose solved camera the clip uses at t (-1: none): its "camera_track" (1-based), else the nearest
// track below it whose clip at t carries a solve.
int modelCameraTrack(const Sequence& seq, size_t track, const Clip& c, FrameTime t);
// The Camera clip ("camera3d") a scene is seen through at t: the one on video track `track` (0-based), else the one on
// the highest track with one at t (null: none). Its camera at t on the sequence's picture scaled by `scale`: where its
// Position and Turn put it from where the frame (at z = 0, a unit a pixel, y up) fills its view, or aimed at a point.
const Clip* sceneCameraClip(const Sequence& seq, FrameTime t, int track = -1);
void sceneCamera(const Project& p, const Sequence& seq, const Clip& camera, FrameTime t, double scale, ModelCamera& cam);
// The camera that an effect choosing one as models do ("camera": its own with "fov", the solved one, a file's or the
// scene's; "camera_track", "camera_index", "camera_file") on clip c sees through at t.
bool effectCamera(const Project& p, const Sequence& seq, const Clip& c, const Effect& g, FrameTime t, double scale, ModelCamera& cam,
                  std::string* error = nullptr);
// The track lighting it (-1: none).
int modelLightTrack(const Sequence& seq, size_t track, const Clip& c, FrameTime t);
// The track whose picture covers its surface ("texture_track"; -1: none).
int modelTextureTrack(const Sequence& seq, size_t track, const Clip& c, FrameTime t);
// The track whose picture is projected onto it through a camera ("proj_track"; -1: none), and the sequence frame it is
// taken at (t, or the frame held).
int modelProjectionTrack(const Sequence& seq, size_t track, const Clip& c, FrameTime t);
FrameTime modelProjectionFrame(const Clip& c, FrameTime t);

// A shape made in place of a file's model (1 plane, the frame's shape a unit tall, facing +z; 2 box; 3 sphere; 4
// cylinder, each a unit across), in one material; a picture's top left at uv (0, 0), the right way round from outside.
std::shared_ptr<const Model> shapeModel(int shape, double aspect, const std::array<float, 4>& color, float metal, float rough);
// A track's picture (premultiplied, the sequence's colour; graded scene-referred, the linear light layers are mixed in)
// as a texture: linear light, Rec. 709 primaries, premultiplied, at most `largest` pixels across (averaged down).
std::shared_ptr<const ModelTexture> pictureTexture(const Image& layer, const Sequence& seq, int largest = 2048);
// The light an effect's settings make at its frame `lt` (light_kind 0 a sun, 1 a point, 2 a spot; light_angle and
// light_height, light_distance times `size` from `mid`, light_color, light_strength, spot_angle, shadows), and the
// ambient light (`ambient`).
ModelLight settingsLight(const Effect& g, FrameTime lt, const std::array<double, 3>& mid, double size, float* ambient = nullptr);

// The models placed in the scene solved on `footage` (on video track `track`): each model clip seen through that
// solve, as it stands at its first frame, one mesh a material, then each 3D layer's card seen through it (for
// writeSolveUsda).
std::vector<SolvedSceneMesh> solvedSceneModels(const Project& p, const Sequence& seq, size_t track, const Clip& footage);

}  // namespace montage
