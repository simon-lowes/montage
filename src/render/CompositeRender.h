// Montage — a clip's compositing node graph rendered (core/CompositeGraph.h): each node after those feeding it, on
// pictures the size of the sequence's frame (at the preview scale), through the same renderer as the timeline (a clip
// or track input is drawn by the compositor as its track would draw it; effects, keys and grades run as on a clip, in
// the values each is made for when the sequence is graded scene-referred). The output is the clip's layer.
//
// Each node's result is kept in memory by a key made from everything that decides it (its settings and effects, the
// clip-local frame, what its inputs' keys are, and for an input what it shows, as the render cache keys a clip's
// layer), so changing one node renders again only that node and those downstream of it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "Compositor.h"
#include "core/Model.h"
#include "media/DeepImage.h"
#include "media/Image.h"

namespace montage {

// A move of a picture the frame's size, as a clip's transform places its media (sequence pixels; the anchor is where
// in the picture lands at the position, as offsets from its centre).
struct CanvasMove {
    double sx = 1, sy = 1, rot = 0;  // scale, rotation (radians)
    double px = 0, py = 0;           // position offset from the frame's centre
    double ax = 0, ay = 0;           // anchor offset from the picture's centre
    double opacity = 1;
};

// What the compositor does for the graph (render/Compositor.cpp makes one for each clip's graph).
struct CompositeHost {
    // A clip's layer at the frame being drawn, as its track would draw it (a graph's own clip without its graph);
    // empty when it has no picture. `displayReferred` as clipLayer tells it.
    std::function<Image(const Clip&, bool* displayReferred)> layer;
    // What video track `track` shows at the frame (a transition's mix while one runs).
    std::function<Image(int track, bool* displayReferred)> track;
    // Whether a clip's picture is deep (a deep OpenEXR, Make Deep), and it as deep samples on the frame; a 3D model
    // always gives one, at its own depths.
    std::function<bool(const Clip&)> isDeep;
    std::function<DeepFrame(const Clip&)> deep;
    // A picture moved by a Transform's settings at clip frame `lt`, or by `move`.
    std::function<Image(const Image&, const Effect& transform, FrameTime lt)> transform;
    std::function<Image(const Image&, const CanvasMove& move)> move;
    // Where clip `c`'s transform puts it at sequence frame `t` (its anchor's position, its scale and rotation); false when
    // it has no picture.
    std::function<bool(const Clip& c, FrameTime t, CanvasMove& out)> placement;
    // `layer` blended onto `canvas` by `mode` at `opacity` (graded scene-referred, as the compositor mixes layers).
    std::function<void(Image& canvas, const Image& layer, const std::string& mode, float opacity, bool displayReferred)> blend;
    // Whether clip `id`'s graph is being drawn now (its picture is then drawn without it, so a track input showing it
    // differs from the same input elsewhere).
    std::function<bool(Id)> drawing;
    // A picture as the frame's display shows it (graded scene-referred; else as it is), for mattes and the models.
    std::function<Image(const Image&)> shown;
};

// Clip `c`'s graph at sequence frame `t` (its picture as its layer; empty when the graph gives nothing).
Image renderCompositeGraph(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, const RenderOptions& o,
                           const CompositeHost& host, bool* displayReferred = nullptr);

// How many node results were rendered and how many found kept, since the start or the last clear; and how many times
// the node with owner id `node` was rendered.
struct CompositeCacheStats {
    uint64_t rendered = 0, reused = 0;
};
CompositeCacheStats compositeCacheStats();
uint64_t compositeNodeRenders(Id node);
// Every kept result gone, and the counts back to zero.
void clearCompositeCache();

}  // namespace montage
