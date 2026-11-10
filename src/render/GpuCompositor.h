#pragma once

// The GPU compositor (Phase 6): a frame's layers drawn on the GPU through Qt's RHI (Metal on macOS, Direct3D 11 on
// Windows, OpenGL elsewhere, or Vulkan where OpenGL can't start), with the same arithmetic as the CPU renderer
// (render/Compositor, render/Processing), so a frame comes out the same within float rounding either way.
// render/Compositor builds a Frame (what to draw) and falls back to the CPU for whatever the GPU cannot draw; this runs
// it.
//
// Built where Qt is 6.6 or later (MONTAGE_WITH_GPU); elsewhere available() says why not. MONTAGE_GPU in the
// environment picks the backend (auto, off, opengl, vulkan, metal, d3d11, d3d12; software for WARP or lavapipe),
// MONTAGE_GPU_LOG=1 prints why frames fall back and how each layer was drawn.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "media/Image.h"

class QImage;

namespace montage {

struct Lut3D;
struct ShapeRaster;

namespace gpu {

// Sets the GPU up (the GUI thread, once: OpenGL needs a surface made there). False, with why, when there is none.
bool initialize(std::string* why = nullptr);
bool available(std::string* why = nullptr);
std::string deviceName();  // e.g. "OpenGL 4.5, llvmpipe (LLVM 20.1.2, 256 bits)"
int maxTextureSize();      // the widest picture it takes (0 without a GPU)
bool takesPlanes();        // it converts decoded Y′CbCr planes itself (Frame16::planes)
bool takesShaders(const std::string& key = {});  // it compiles shader steps (ShaderStep) while running (that one, too)
void shutdown();           // (before the application goes; initialize() again is allowed)

// A colour step written in GLSL at run time (render/Ocio's OpenColorIO transforms, from OCIO's own GPU shaders):
// GLSL 4.40 declaring its textures from binding 2 up, in `textures`' order, and `vec4 function(vec4)` on straight
// colour. Compiled once a key.
struct ShaderStep {
    std::string key;
    std::string source, function;
    struct Texture {
        int width = 0, height = 1, depth = 0;  // (depth: a 3D texture's)
        int channels = 3;                      // 1 (red) or 3, `values` holding that many a texel
        bool linear = true;                    // sampled between texels
        std::vector<float> values;
    };
    std::vector<Texture> textures;
    // Values set while it runs (OCIO's dynamic properties): floats each `uniforms[i]` names, the source reading them as
    // montageValues[i / 4][i % 4] (the Params block), given by the ColorOp drawing it (ColorOp::values).
    std::vector<std::string> uniforms;
};

// One colour step on a layer at its source size (render/Processing's colour effects, as they run there).
struct ColorOp {
    // (Mark keeps the picture as it is, for a Mix later to mix what the steps between made with it, through its mask:
    // a grade node's mask and mix. A grade graph's other nodes keep pictures and keys in numbered slots: Store keeps
    // the picture in `slot`, Load takes it up again, and Pass draws gpucolor.frag's `passMode` from the slots `in`
    // (its source, table and mask; -1 for none) into the slot `out`, leaving the picture as it is.)
    // (ChromaKey, LumaKey and ScreenKey change the alpha too: render/Processing's keyers, gpucolor.frag's 18 to 20;
    // a ScreenKey whose params[10] or [11] are set has its matte shrunk or grown and blurred by passes of its own, and
    // one with a clean plate, made on the CPU, reads it from `plate`.)
    // (Shader runs `shader` on the picture, without a mask.)
    // (Filter runs gpufilter.frag's `passes` in turn, each on the picture the one before made, with the picture the
    // filter began with beside it: the blurs, Sharpen, Glow and the other effects that read beyond their own pixel. A
    // filter with a mask is marked before and mixed after, as a grade node is.)
    enum Kind { Lut, ColorCorrect, Curves, HueCurves, Cdl, Invert, BlackWhite, HueSat, Levels, Mark, Mix, Store, Load, Pass, ChromaKey, LumaKey, ScreenKey, Shader, Filter } kind = Lut;
    struct FilterPass {
        int mode = 0;           // gpufilter.frag's
        float params[60] = {};  // its p[1..15]
        std::vector<float> table;  // RGBA rows of tableWidth (its `table`, as the mode says), or none
        std::shared_ptr<const std::vector<float>> sharedTable;  // (or kept elsewhere, not copied: read in `table`'s place)
        int tableWidth = 0;
        int width = 0, height = 0;  // what it makes (0: the layer's size)
        int save = -1;              // the filter's slot it keeps what it makes in, for a later pass
        int extra = -1, tableSlot = -1;  // slots read as gpufilter.frag's `extra`, and as its `table` instead of `table` above
    };
    std::vector<FilterPass> passes;  // Filter
    std::shared_ptr<const ShaderStep> shader;  // Shader
    std::vector<float> values;                 // Shader: its uniforms' values
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
    std::vector<float> plate;  // ScreenKey: its clean plate (RGBA, the layer's size), or none
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
    bool canvas = false;                 // an adjustment layer's: the canvas as drawn so far, at the canvas's size ...
    Frame16Ptr frame;                    // or a decoded frame, RGBA or Y′CbCr planes (premultiplied on the GPU) ...
    std::shared_ptr<const Lut3D> input;  // ... and its input transform, if any
    Image image;                         // or a picture made on the CPU (premultiplied, working space)
    std::shared_ptr<const ShapeRaster> shapes;  // or a Shape layer's paints, drawn here (gpufilter.frag's 56) at their size
    std::vector<ColorOp> ops;
    Placement place;
};

struct Item {
    Layer a;
    std::optional<Layer> b;  // a transition's second clip
    float dissolve = -1;     // its position 0..1 (negative: one layer)
    // A transition other than a cross dissolve: gpumix.frag's kind and its settings (render/Processing's
    // gpuTransitionParams), both clips placed on the canvas first. (-1: a cross dissolve, mixed as they are placed.)
    int transition = -1;
    float transitionParams[44] = {};
    // (Smooth Cut, 18: its flow, a grid of moves in RGBA rows of transitionTableWidth, read as gpumix.frag's table)
    std::vector<float> transitionTable;
    int transitionTableWidth = 0;
    // A track matte: the clip seen through another track's picture (placed at the canvas's size), by its alpha or
    // luma, or the reverse (render/Compositor's applyTrackMatte).
    std::optional<Layer> matte;
    bool matteLuma = false, matteReverse = false;
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
    long filterPasses = 0, mixPasses = 0;  // passes of filters (gpufilter.frag) and of transitions and mattes (gpumix.frag)
    long shaderBuilds = 0;                 // shader steps built (ShaderStep, once a key), since resetStats
    long shapeLayers = 0;                  // Shape layers drawn on the GPU (Layer::shapes)
    std::string lastFallback;
    std::string lastCpuLayer;  // the last layer made on the CPU, and why (as MONTAGE_GPU_LOG says it)
    // Hardware-decoded frames (render/GpuHardware): drawn from the decoding device's memory (hwGpuCopies of them
    // copied plane by plane on the GPU first, a device copy, no read-back), and copied back to memory (media/HwAccel's
    // hwFrameCopies: after decoding, for the CPU, or as the GPU could not map them), since resetStats.
    long hwMapped = 0, hwGpuCopies = 0, hwReadBacks = 0;
    std::string lastHwFallback;  // why the last hardware frame the GPU could not map was copied back
};
Stats stats();
void resetStats();
// The decoding devices whose frames the GPU takes as they are ("vulkan, vaapi"; empty for none).
std::string mappedHardware();
// Tests: the device lost (simulated: the next frame finds it so and sets it up again), and hardware frames refused
// (each copied back, as a mapping that fails is).
void loseDeviceForTesting();
void refuseHardwareFramesForTesting(bool refuse);
void countFallback(const std::string& why);  // (render/Compositor's bookkeeping)
void countLayer(bool onGpu, const std::string& how = {});  // (and with MONTAGE_GPU_LOG, says how it was drawn)

}  // namespace gpu
}  // namespace montage
