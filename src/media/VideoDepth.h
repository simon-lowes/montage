// Montage — steady depth for video: Video Depth Anything Small (Sili Chen et al., ByteDance, Apache-2.0) on ONNX
// Runtime. A still's depth (media/DepthMap.h) is worked out frame by frame and shimmers on video; this model looks at
// 32 frames at a time, so a surface keeps its depth as the camera moves. A clip's depth is analysed once and kept on
// disk; the depth effects (Lens Blur (Depth), Depth Fog, Depth Map, Relight, depth masks, Sky Replacement) use it
// where it reaches, and the still model elsewhere.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "DepthMap.h"
#include "ModelFiles.h"

namespace montage {

// The authors' checkpoint (116 MB, video_depth_anything_vits.pth), downloaded on first use; the network's graphs
// without weights are compiled in (scripts/gen-vda-graph.py). $MONTAGE_VIDEO_DEPTH_MODEL names another folder,
// $MONTAGE_VIDEO_DEPTH_MODEL_URL a mirror.
const ModelPack& videoDepthModel();
bool videoDepthAvailable();  // built with ONNX Runtime

// Depth for `count` consecutive frames (`frameAt(i, img)` gives frame i, premultiplied RGBA, display-referred), steady
// from one to the next, run as its authors run it: windows of 32 frames, each starting with ten frames of the last
// (its first frame and every twelfth, and the last eight), its depth fitted to the last window's on the first two
// (scale and shift) and blended into it over the eight. `sink(i, depth)` is given each frame's disparity in order
// (nearer is larger, not yet stretched); `lo` and `hi` come back as the whole run's 1st and 99th percentiles, to
// stretch every frame alike. The short side runs at `size` (a multiple of 14; 518 as trained, less for frames wider
// than 16:9). `progress` (0..1) may return false to stop.
bool estimateVideoDepth(int count, const std::function<bool(int, Image&)>& frameAt,
                        const std::function<bool(int, const DepthMap&)>& sink, float& lo, float& hi, int size = 518,
                        const std::function<bool(double)>& progress = {}, std::string* error = nullptr);

// ---- A clip's depth, analysed and kept --------------------------------------------------------------------------
// Analyses media seconds `start`..`end` of the video at `path` (every frame, up to 30 a second) and keeps the result
// in the cache folder (beside the render cache), where analysedDepth finds it. A file has one analysis: analysed
// again, the new one covers what the old did too (so other clips of it keep theirs), stretched over the whole of it.
// False if cancelled (`progress` returning false) or on failure.
bool analyzeVideoDepth(const std::string& path, double start, double end, int size = 518,
                       const std::function<bool(double)>& progress = {}, std::string* error = nullptr);

// What was analysed for the file as it is now (the same size and modification time): its media range (the first and
// last frames analysed, `step` seconds apart; the depth reaches a step beyond either end), or false.
bool analysedDepthRange(const std::string& path, double& start, double& end, double* step = nullptr);
// What tells this analysis of the file from another ("" none), for keys of what was made from it.
std::string analysedDepthStamp(const std::string& path);

// The analysed depth of the frame shown at media time `seconds` (0 far, 1 near, stretched over the whole analysis),
// between the two analysed frames either side where not every frame was analysed; null where the analysis does not reach (more than a frame beyond either end) or there
// is none. The last few are kept in memory.
std::shared_ptr<const DepthMap> analysedDepth(const std::string& path, double seconds);

// A frame's depth for the effects that use it: the analysis's at media time `seconds` of `path` where there is one,
// else the still model's on `img` (cachedDepth) when it is installed; null when neither. `path` "" (a generator, a
// nested sequence, a reframed view) always takes the still model's.
std::shared_ptr<const DepthMap> frameDepth(const Image& img, const std::string& path, double seconds);

// The folder kept analyses are in ($MONTAGE_DEPTH_CACHE, or "depth" in the app's cache folder).
std::string videoDepthCacheDirectory();

}  // namespace montage
