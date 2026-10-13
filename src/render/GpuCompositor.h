#pragma once

// The GPU compositor (Phase 6): a frame's layers drawn on the GPU through Qt's RHI (Metal on macOS, Direct3D 11 on
// Windows, OpenGL elsewhere), with the same arithmetic as the CPU renderer (render/Compositor, render/Processing), so
// a frame comes out the same within float rounding either way. render/Compositor builds a Frame (what to draw) and
// falls back to the CPU for whatever the GPU cannot draw; this runs it.
//
// Built where Qt is 6.6 or later (MONTAGE_WITH_GPU); elsewhere available() says why not. MONTAGE_GPU in the
// environment picks the backend (auto, off, opengl, metal, d3d11, d3d12; software for WARP), MONTAGE_GPU_LOG=1 prints
// why frames fall back.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "media/Image.h"

class QImage;

namespace montage {

struct Lut3D;

namespace gpu {

// Sets the GPU up (the GUI thread, once: OpenGL needs a surface made there). False, with why, when there is none.
bool initialize(std::string* why = nullptr);
bool available(std::string* why = nullptr);
std::string deviceName();  // e.g. "OpenGL 4.5, llvmpipe (LLVM 20.1.2, 256 bits)"
int maxTextureSize();      // the widest picture it takes (0 without a GPU)
void shutdown();           // (before the application goes; initialize() again is allowed)

// One colour step on a layer at its source size (render/Processing's colour effects, as they run there).
struct ColorOp {
    // (Mark keeps the picture as it is, for a Mix later to mix what the steps between made with it, through its mask:
    // a grade node's mask and mix. A grade graph's other nodes keep pictures and keys in numbered slots: Store keeps
    // the picture in `slot`, Load takes it up again, and Pass draws gpucolor.frag's `passMode` from the slots `in`
    // (its source, table and mask; -1 for none) into the slot `out`, leaving the picture as it is.)
    enum Kind { Lut, ColorCorrect, Curves, HueCurves, Cdl, Invert, BlackWhite, HueSat, Levels, Mark, Mix, Store, Load, Pass } kind = Lut;
    int slot = 0;                // Store, Load
    int passMode = 0;            // Pass
    int in[3] = {-1, -1, -1};    // Pass
    int out = -1;                // Pass
    std::shared_ptr<const Lut3D> lut;  // Lut
    bool clampFirst = false;           // Lut: the colour clamped to 0..1 first (a LUT baked into another space)
    float strength = 1;                // Lut
    float params[32] = {};             // ColorCorrect: see gpucolor.frag; Curves and HueCurves: [0] mix
    std::vector<float> table;          // RGBA rows of tableWidth (curves one a channel; Color Correct's tone in red)
    int tableWidth = 0;
    // Through the effect's mask: its drawn shape (the layer's size; empty for none) and gpucolor.frag's p[13..15]
    // (shape, qualifier, inverted, opacity; hue centre, half width, softness; saturation and luma ranges).
    bool masked = false;
    std::vector<float> mask;
    float maskParams[12] = {};
};

// Where a layer goes on the canvas (render/Compositor's Geometry, worked out on the CPU).
struct Placement {
    bool full = false;  // the picture is the canvas's size already, drawn as it is
    float scale = 1, opacity = 1;
    bool axis = true;
    float cx = 0, cy = 0, cosr = 1, sinr = 0;  // centre (sequence pixels), rotation
    float sx = 1, sy = 1, au = 0, av = 0;      // scale; the media point at the centre
    float u0 = 0, u1 = 0, v0 = 0, v1 = 0;      // crop (media pixels)
    float kx = 1, ky = 1, px = 1, py = 1;      // image pixels per media pixel; output pixels per media pixel
};

struct Layer {
    Frame16Ptr frame;                    // a decoded frame (premultiplied on the GPU) ...
    std::shared_ptr<const Lut3D> input;  // ... and its input transform, if any
    Image image;                         // or a picture made on the CPU (premultiplied, working space)
    std::vector<ColorOp> ops;
    Placement place;
};

struct Item {
    Layer a;
    std::optional<Layer> b;  // a cross dissolve's second clip
    float dissolve = -1;     // its position 0..1 (negative: one layer)
    int blendMode = 0;       // render/Processing's blendModeIndex
};

struct Frame {
    int width = 0, height = 0;
    std::vector<Item> items;  // bottom first
    bool flatten = true;      // over black
    std::shared_ptr<const Lut3D> display;  // the display transform, if any
};

// Draws the frame into `out` (premultiplied float) or `out8` (8-bit RGBA, as toRgba8 gives). False when the GPU
// could not, so the caller renders on the CPU.
bool execute(const Frame& frame, Image* out, QImage* out8 = nullptr);

struct Stats {
    long gpuFrames = 0, cpuFrames = 0;  // frames asked of the GPU path: drawn there, or fallen back
    long gpuLayers = 0, cpuLayers = 0;  // their layers: drawn from the source, or made on the CPU and uploaded
    std::string lastFallback;
};
Stats stats();
void resetStats();
void countFallback(const std::string& why);  // (render/Compositor's bookkeeping)
void countLayer(bool onGpu);

}  // namespace gpu
}  // namespace montage
