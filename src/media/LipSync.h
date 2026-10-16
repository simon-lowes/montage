// Montage — lip sync: a face's mouth and jaw drawn again to speak other sound (a dub, regenerated words), with MuseTalk
// 1.5 (Tencent Music Entertainment, code and weights MIT) on ONNX Runtime. MuseTalk draws the lower half of a 256 x 256
// crop of the face in one pass of a UNet over the crop's latents (sd-vae-ft-mse's, MIT), guided by whisper-tiny's
// encoding of the sound around the frame (Apache-2.0). The graphs are src/media/lipsync-*.onnx
// (scripts/gen-lipsync-graphs.py), their weights read from the three authors' files where they lie. The face's crop and
// the region blended back come from its mesh (media/FaceMesh.h). A frame takes about 4 s on four cores, so the
// effect is shown once rendered (Render In to Out) and in deliveries.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "FaceMesh.h"
#include "core/Model.h"
#include "Image.h"
#include "ModelFiles.h"

namespace montage {

// The three files (3.9 GB), downloaded on first use; $MONTAGE_LIPSYNC_MODEL names another folder,
// $MONTAGE_LIPSYNC_MODEL_URL a mirror.
const ModelPack& lipSyncModel();
bool lipSyncAvailable();  // built with ONNX Runtime

constexpr int kLipSyncRate = 16000;          // the sound, mono
constexpr int kLipSyncSegment = 30 * 16000;  // whisper hears 30 s at a time
constexpr int kLipSyncFeatureRate = 50;      // its encoding, a frame every 20 ms
constexpr int kLipSyncLayers = 5, kLipSyncWidth = 384;
constexpr int kLipSyncTokens = 50;           // what a video frame is given: ten encoding frames of five layers

// whisper-tiny's encoding of 30 s of sound (kLipSyncSegment samples, shorter is padded with silence): 1500 frames of
// kLipSyncLayers x kLipSyncWidth (its input and each layer's output).
bool lipSyncEncodeSound(const std::vector<float>& segment, std::vector<float>& features, std::string* error = nullptr);

// What video frame `index` (from the sound's start, at `fps`) is given, as MuseTalk's audio processor cuts it: the
// ten encoding frames from two video frames' worth before it, each with its five layers (kLipSyncTokens x
// kLipSyncWidth). `feature(j)` gives encoding frame j (kLipSyncLayers x kLipSyncWidth), or nullptr for silence (before
// the start).
std::vector<float> lipSyncWindow(int64_t index, double fps, const std::function<const float*(int64_t)>& feature);

// The 30 s stretches of sound (counted from the sound's start, each kLipSyncSegment samples) that video frame `index`'s
// window draws on: first..last.
void lipSyncSegments(int64_t index, double fps, int64_t& first, int64_t& last);

// The audio track a Lip Sync's Sound From (a track id, "" for every track) names in `seq`: 1-based, 0 for every track,
// -1 for a track no longer there.
int lipSyncTrack(const Sequence& seq, const std::string& id);

// The box MuseTalk crops a face to (pixels, x1 and y1 exclusive), from its mesh: across the jaw, from as far above the
// nose's bridge as the chin is below it down to a little under the chin.
struct LipSyncBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
};
LipSyncBox lipSyncBox(const FaceMesh& face, int width, int height);

// Where the drawn mouth replaces the picture, 0..1 over the picture: the face below the nose (its outline from the mesh,
// widened under the chin for an opening jaw), inside the box, feathered.
std::vector<float> lipSyncMask(const FaceMesh& face, const LipSyncBox& box, int width, int height);

// `img` (premultiplied, display-referred) with `face`'s mouth drawn to speak `window` (lipSyncWindow), mixed over the
// original by `amount` (0..1). False without the model or on a failure (`img` unchanged).
bool lipSyncFace(Image& img, const FaceMesh& face, const std::vector<float>& window, double amount, std::string* error = nullptr,
                 const std::atomic<bool>* cancel = nullptr);

}  // namespace montage
