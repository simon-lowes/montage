#include "Effects.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "render/ColorSpace.h"

namespace montage {

namespace {

ParamInfo num(std::string name, std::string label, double min, double max, double def, double step = 0.01) {
    ParamInfo p;
    p.name = std::move(name);
    p.label = std::move(label);
    p.kind = ParamKind::Number;
    p.min = min;
    p.max = max;
    p.def = def;
    p.step = step;
    return p;
}

ParamInfo pct(std::string name, std::string label, double min, double max, double def) {
    ParamInfo p = num(std::move(name), std::move(label), min, max, def, 0.1);
    p.kind = ParamKind::Percent;
    return p;
}

ParamInfo angle(std::string name, std::string label, double def = 0) {
    ParamInfo p = num(std::move(name), std::move(label), -360, 360, def, 0.1);
    p.kind = ParamKind::Angle;
    return p;
}

ParamInfo boolean(std::string name, std::string label, bool def = false) {
    ParamInfo p = num(std::move(name), std::move(label), 0, 1, def ? 1 : 0, 1);
    p.kind = ParamKind::Bool;
    p.keyframeable = false;
    return p;
}

ParamInfo choice(std::string name, std::string label, std::vector<std::string> choices, int def = 0) {
    ParamInfo p = num(std::move(name), std::move(label), 0, double(choices.size() - 1), def, 1);
    p.kind = ParamKind::Choice;
    p.choices = std::move(choices);
    p.keyframeable = false;
    return p;
}

ParamInfo color(std::string name, std::string label, double r, double g, double b) {
    ParamInfo p = num(std::move(name), std::move(label), 0, 1, r);
    p.kind = ParamKind::Color;
    p.defG = g;
    p.defB = b;
    return p;
}

StringParamInfo str(std::string name, std::string label, StringKind kind, std::string def = {}) {
    StringParamInfo s;
    s.name = std::move(name);
    s.label = std::move(label);
    s.kind = kind;
    s.def = std::move(def);
    return s;
}

std::vector<EffectInfo> buildCatalog() {
    std::vector<EffectInfo> c;

    // ---- Fixed attributes -------------------------------------------------
    c.push_back({"transform", "Transform", EffectCategory::Fixed, "Fixed",
                 {
                     num("pos_x", "Position X", -8000, 8000, 0, 1),
                     num("pos_y", "Position Y", -8000, 8000, 0, 1),
                     pct("scale", "Scale", 0, 1000, 100),
                     pct("scale_x", "Scale Width", 0, 1000, 100),
                     pct("scale_y", "Scale Height", 0, 1000, 100),
                     angle("rotation", "Rotation"),
                     num("anchor_x", "Anchor X", -8000, 8000, 0, 1),
                     num("anchor_y", "Anchor Y", -8000, 8000, 0, 1),
                     pct("crop_left", "Crop Left", 0, 100, 0),
                     pct("crop_right", "Crop Right", 0, 100, 0),
                     pct("crop_top", "Crop Top", 0, 100, 0),
                     pct("crop_bottom", "Crop Bottom", 0, 100, 0),
                     pct("opacity", "Opacity", 0, 100, 100),
                     boolean("flip_h", "Flip Horizontal"),
                     boolean("flip_v", "Flip Vertical"),
                     choice("fit", "Fit", {"Fit", "Fill", "Stretch", "None (1:1)"}, 0),
                 },
                 {}});
    // Time Remapping: a keyframeable speed curve within the clip's length, and
    // how in-between source frames are made in slow motion.
    c.push_back({"time", "Time Remapping", EffectCategory::Fixed, "Fixed",
                 {pct("speed", "Speed", 0, 1000, 100),  // 0 holds the frame
                  choice("sampling", "Frame Sampling", {"Nearest Frame", "Frame Blending", "Optical Flow", "AI Frames (RIFE)"}, 0)},
                 {}});
    c.push_back({"volume", "Volume", EffectCategory::Fixed, "Fixed",
                 {num("gain_db", "Gain (dB)", -60, 24, 0, 0.1), num("pan", "Pan", -1, 1, 0, 0.01)},
                 {}});

    // ---- Video filters ----------------------------------------------------
    c.push_back({"color_correct", "Color Correct (Primaries)", EffectCategory::VideoFilter, "Color",
                 {
                     num("exposure", "Exposure (stops)", -5, 5, 0),
                     num("contrast", "Contrast", 0, 3, 1),
                     num("pivot", "Contrast Pivot", 0, 1, 0.435),
                     num("saturation", "Saturation", 0, 3, 1),
                     num("temperature", "Temperature", -100, 100, 0, 0.5),
                     num("tint", "Tint", -100, 100, 0, 0.5),
                     num("lift", "Lift (Master)", -1, 1, 0, 0.005),
                     num("lift_r", "Lift R", -1, 1, 0, 0.005),
                     num("lift_g", "Lift G", -1, 1, 0, 0.005),
                     num("lift_b", "Lift B", -1, 1, 0, 0.005),
                     num("gamma", "Gamma (Master)", -1, 1, 0, 0.005),
                     num("gamma_r", "Gamma R", -1, 1, 0, 0.005),
                     num("gamma_g", "Gamma G", -1, 1, 0, 0.005),
                     num("gamma_b", "Gamma B", -1, 1, 0, 0.005),
                     num("gain", "Gain (Master)", 0, 4, 1, 0.005),
                     num("gain_r", "Gain R", 0, 4, 1, 0.005),
                     num("gain_g", "Gain G", 0, 4, 1, 0.005),
                     num("gain_b", "Gain B", 0, 4, 1, 0.005),
                     num("offset", "Offset", -1, 1, 0, 0.005),
                     // Tone, as in Lumetri's Basic Correction: each moves its own part of the range.
                     num("highlights", "Highlights", -100, 100, 0, 0.5),
                     num("shadows", "Shadows", -100, 100, 0, 0.5),
                     num("whites", "Whites", -100, 100, 0, 0.5),
                     num("blacks", "Blacks", -100, 100, 0, 0.5),
                     num("vibrance", "Vibrance", -100, 100, 0, 0.5),
                 },
                 {}});
    c.push_back({"curves", "Curves", EffectCategory::VideoFilter, "Color",
                 {pct("mix", "Mix", 0, 100, 100)},
                 {str("master", "Master (Y)", StringKind::Curve, "0,0 1,1"),
                  str("red", "Red", StringKind::Curve, "0,0 1,1"),
                  str("green", "Green", StringKind::Curve, "0,0 1,1"),
                  str("blue", "Blue", StringKind::Curve, "0,0 1,1")}});
    // Secondary grading without masks: change colours by their hue, brightness or saturation.
    {
        auto curve = [](const char* n, const char* label, StringKind k) { return str(n, label, k, ""); };
        c.push_back({"hue_curves", "Hue Curves", EffectCategory::VideoFilter, "Color",
                     {pct("mix", "Mix", 0, 100, 100)},
                     {curve("hue_hue", "Hue vs Hue", StringKind::HueCurve), curve("hue_sat", "Hue vs Saturation", StringKind::HueCurve),
                      curve("hue_luma", "Hue vs Luma", StringKind::HueCurve), curve("luma_sat", "Luma vs Saturation", StringKind::LevelCurve),
                      curve("sat_sat", "Saturation vs Saturation", StringKind::LevelCurve)}});
    }
    c.push_back({"hue_sat", "Hue / Saturation / Lightness", EffectCategory::VideoFilter, "Color",
                 {angle("hue", "Hue Shift"), num("saturation", "Saturation", 0, 3, 1),
                  num("lightness", "Lightness", -1, 1, 0), num("vibrance", "Vibrance", -1, 1, 0)},
                 {}});
    c.push_back({"lut", "LUT (.cube)", EffectCategory::VideoFilter, "Color",
                 {pct("strength", "Strength", 0, 100, 100)},
                 {str("path", "LUT File", StringKind::File)}});
    c.back().strings[0].fileFilter = "LUT files (*.cube)";
    {
        // Colour Space Transform: one clip from any space into another (render/ColorSpace.h).
        std::vector<std::string> spaces;
        for (const auto& cs : colorSpaces()) spaces.push_back(cs.label);
        EffectInfo cst{"color_space_transform", "Colour Space Transform", EffectCategory::VideoFilter, "Color", {},
                       {str("from", "From", StringKind::Choice, colorSpaces().front().label),
                        str("to", "To", StringKind::Choice, colorSpaces().front().label)}};
        cst.strings[0].choices = cst.strings[1].choices = spaces;
        c.push_back(cst);
    }
    // Stabilize: the analysed camera path (strings["motion"], written by the
    // Inspector's Analyze) smoothed; render/Processing.cpp moves each frame.
    c.push_back({"stabilize", "Stabilize", EffectCategory::VideoFilter, "Transform",
                 {num("smoothness", "Smoothness (s)", 0, 10, 1.5, 0.1),
                  choice("method", "Method", {"Position", "Position & Scale", "Position, Scale & Rotation"}, 2),
                  choice("framing", "Framing", {"Zoom to Fill", "Show Edges"}, 0), pct("extra_zoom", "Extra Zoom", 0, 50, 0)},
                 {}});
    for (auto& pi : c.back().params) pi.keyframeable = false;
    {
        // OpenColorIO transform from a config file, a built-in config or $OCIO (render/Ocio.h).
        EffectInfo ocio{"ocio", "OpenColorIO Transform", EffectCategory::VideoFilter, "Color",
                        {boolean("inverse", "Inverse")},
                        {str("config", "Config", StringKind::File), str("mode", "Mode", StringKind::Choice, "Colour Space"),
                         str("src", "Input Space", StringKind::Dynamic), str("dst", "Output Space", StringKind::Dynamic),
                         str("display", "Display", StringKind::Dynamic), str("view", "View", StringKind::Dynamic),
                         str("look", "Look", StringKind::Dynamic)}};
        ocio.strings[0].fileFilter = "OpenColorIO configs (*.ocio);;All files (*)";
        ocio.strings[1].choices = {"Colour Space", "Display / View"};
#ifndef MONTAGE_WITH_OCIO
        ocio.hidden = true;  // needs OpenColorIO
#endif
        c.push_back(ocio);
    }
    c.push_back({"black_white", "Black & White", EffectCategory::VideoFilter, "Color",
                 {pct("amount", "Amount", 0, 100, 100), color("tint", "Tint", 1, 1, 1)},
                 {}});
    c.push_back({"invert", "Invert", EffectCategory::VideoFilter, "Color", {pct("amount", "Amount", 0, 100, 100)}, {}});
    c.push_back({"chroma_key", "Chroma Key", EffectCategory::VideoFilter, "Keying",
                 {color("key", "Key Color", 0, 1, 0), num("tolerance", "Tolerance", 0, 1, 0.25),
                  num("softness", "Edge Softness", 0, 1, 0.1), num("spill", "Spill Suppression", 0, 1, 0.6),
                  num("choke", "Choke", -1, 1, 0), boolean("show_matte", "Show Matte")},
                 {}});
    // A title or graphic behind the people in the shot beneath it (handled when tracks are composited).
    c.push_back({"behind_people", "Behind People", EffectCategory::VideoFilter, "Keying",
                 {pct("amount", "Amount", 0, 100, 100), num("shift", "Edge Shift (px)", -20, 20, 1, 0.5),
                  num("soften", "Soften (px)", 0, 40, 1, 0.5)},
                 {}});
    // Shows the clip through another track's picture (handled when tracks are composited).
    c.push_back({"track_matte", "Track Matte Key", EffectCategory::VideoFilter, "Keying",
                 {num("track", "Matte Track (V number, 0: the one above)", 0, 99, 0, 1),
                  choice("composite", "Composite Using", {"Matte Alpha", "Matte Luma"}, 0), boolean("reverse", "Reverse"),
                  boolean("hide", "Hide the Matte", true)},
                 {}});
    c.push_back({"luma_key", "Luma Key", EffectCategory::VideoFilter, "Keying",
                 {num("threshold", "Threshold", 0, 1, 0.1), num("softness", "Softness", 0, 1, 0.05),
                  boolean("invert", "Key Out Brights")},
                 {}});
    // What is under its mask (a shape, a picked object, people) painted out and filled (LaMa).
    c.push_back({"object_removal", "Object Removal", EffectCategory::VideoFilter, "Refine",
                 {num("grow", "Grow Mask (px)", 0, 50, 4, 0.5)},
                 {}});
    // The faces in the frame (YuNet, found again each frame) touched up within a soft mask.
    c.push_back({"face_refine", "Face Refinement", EffectCategory::VideoFilter, "Refine",
                 {pct("smooth", "Smooth Skin", 0, 100, 40), pct("lighten", "Lighten Face", 0, 100, 0),
                  pct("eyes_bright", "Brighten Eyes", 0, 100, 20), pct("eyes_sharp", "Sharpen Eyes", 0, 100, 30),
                  boolean("show", "Show Face Mask")},
                 {}});
    // People cut out of their background (MODNet on the clip's source frame), keeping hair and soft edges.
    c.push_back({"remove_background", "Remove Background", EffectCategory::VideoFilter, "Keying",
                 {choice("keep", "Keep", {"People", "Background"}), num("shift", "Edge Shift (px)", -20, 20, 0, 0.5),
                  num("soften", "Soften (px)", 0, 20, 0, 0.5)},
                 {}});
    c.push_back({"gaussian_blur", "Gaussian Blur", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {num("radius", "Radius (px)", 0, 250, 10, 0.5),
                  choice("direction", "Direction", {"Both", "Horizontal", "Vertical"})},
                 {}});
    c.push_back({"sharpen", "Sharpen", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {num("amount", "Amount", 0, 5, 1), num("radius", "Radius (px)", 0.5, 20, 1.5, 0.1)},
                 {}});
    // Runs on the source frames before the clip's other effects (it needs the frames either side).
    c.push_back({"video_denoise", "Video Noise Reduction", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {num("frames", "Temporal Frames Each Side", 0, 3, 2, 1), boolean("motion", "Motion Compensation", true),
                  num("temporal", "Temporal Strength", 0, 3, 1, 0.05), num("luma", "Spatial Luma", 0, 1, 0.25, 0.01),
                  num("chroma", "Spatial Chroma", 0, 1, 0.6, 0.01), num("noise", "Noise Level % (0 = auto)", 0, 20, 0, 0.1),
                  num("blend", "Blend Original", 0, 1, 0, 0.01)},
                 {}});
    // Real-ESRGAN on the source frames when the clip is shown larger than it was shot (scaled up, or in a
    // bigger sequence); nothing happens at its own size or smaller.
    c.push_back({"super_scale", "Super Scale", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {pct("strength", "Strength", 0, 100, 100)},
                 {}});
    // Depth (Depth Anything V2 on the clip's source frame): 0 is the farthest part of the picture, 100 the nearest.
    c.push_back({"depth_blur", "Lens Blur (Depth)", EffectCategory::VideoFilter, "Depth",
                 {pct("focus", "Focus Depth", 0, 100, 80), pct("range", "In Focus Range", 0, 100, 10),
                  pct("falloff", "Falloff", 1, 100, 30), num("radius", "Blur (px)", 0, 100, 12, 0.5),
                  boolean("near", "Blur Nearer Than Focus", true)},
                 {}});
    c.push_back({"depth_fog", "Depth Fog", EffectCategory::VideoFilter, "Depth",
                 {color("color", "Color", 0.78, 0.82, 0.88), pct("amount", "Amount", 0, 100, 70), pct("start", "Starts At Depth", 0, 100, 60),
                  num("curve", "Thickening", 0.2, 5, 1.5, 0.05)},
                 {}});
    c.push_back({"relight", "Relight", EffectCategory::VideoFilter, "Depth",
                 {angle("direction", "Light Direction", 135), num("elevation", "Light Height", 0, 90, 35, 0.5),
                  color("color", "Light Color", 1.0, 0.95, 0.85), pct("intensity", "Intensity", 0, 300, 100),
                  pct("shadows", "Shadows", 0, 100, 50), num("relief", "Relief", 0, 20, 3, 0.1),
                  num("smoothness", "Surface Smoothing (px)", 0, 50, 8, 0.5), pct("reach", "Reach", 0, 100, 100),
                  boolean("normals", "Show Surface Directions")},
                 {}});
    c.push_back({"depth_map", "Depth Map", EffectCategory::VideoFilter, "Depth",
                 {boolean("invert", "Far Is White"), pct("mix", "Mix", 0, 100, 100)},
                 {}});
    c.push_back({"levels", "Levels", EffectCategory::VideoFilter, "Color",
                 {num("in_black", "Input Black", 0, 1, 0, 0.005), num("in_white", "Input White", 0, 1, 1, 0.005),
                  num("gamma", "Gamma", 0.1, 10, 1, 0.01), num("out_black", "Output Black", 0, 1, 0, 0.005),
                  num("out_white", "Output White", 0, 1, 1, 0.005)},
                 {}});
    c.push_back({"glow", "Glow", EffectCategory::VideoFilter, "Stylize",
                 {num("threshold", "Threshold", 0, 1, 0.7, 0.005), num("radius", "Radius (px)", 0, 200, 20, 0.5),
                  num("intensity", "Intensity", 0, 5, 1, 0.01)},
                 {}});
    // Film's marks in one effect (Resolve's Film Look Creator); the gauge sets the grain's size and the weave.
    c.push_back({"film_look", "Film Look", EffectCategory::VideoFilter, "Stylize",
                 {choice("gauge", "Gauge", {"65mm", "35mm", "16mm", "Super 8"}, 1), pct("halation", "Halation", 0, 100, 30),
                  pct("bloom", "Bloom", 0, 100, 20), pct("grain", "Grain", 0, 100, 30), pct("weave", "Gate Weave", 0, 100, 20),
                  pct("vignette", "Vignette", 0, 100, 25), pct("softness", "Softness", 0, 100, 10), pct("flicker", "Flicker", 0, 100, 0),
                  pct("aberration", "Chromatic Aberration", 0, 100, 0), pct("fade", "Fade (Lifted Blacks)", 0, 100, 10)},
                 {}});
    c.push_back({"film_grain", "Film Grain", EffectCategory::VideoFilter, "Stylize",
                 {num("amount", "Amount", 0, 1, 0.15, 0.005), num("size", "Grain Size (px)", 0.5, 8, 1.5, 0.1),
                  boolean("color", "Colour Grain")},
                 {}});
    c.push_back({"directional_blur", "Directional Blur", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {num("length", "Length (px)", 0, 300, 20, 0.5), angle("angle", "Direction", 0)},
                 {}});
    c.push_back({"chromatic_aberration", "Chromatic Aberration", EffectCategory::VideoFilter, "Stylize",
                 {num("amount", "Amount (px)", -30, 30, 3, 0.1)},
                 {}});
    c.push_back({"lens_distortion", "Lens Distortion", EffectCategory::VideoFilter, "Distort",
                 {num("amount", "Barrel / Pincushion", -100, 100, 0, 0.5)},
                 {}});
    // Corners as fractions of the frame, keyframeable for screen replacements.
    c.push_back({"corner_pin", "Corner Pin", EffectCategory::VideoFilter, "Distort",
                 {num("tl_x", "Top Left X", -1, 2, 0, 0.001), num("tl_y", "Top Left Y", -1, 2, 0, 0.001),
                  num("tr_x", "Top Right X", -1, 2, 1, 0.001), num("tr_y", "Top Right Y", -1, 2, 0, 0.001),
                  num("br_x", "Bottom Right X", -1, 2, 1, 0.001), num("br_y", "Bottom Right Y", -1, 2, 1, 0.001),
                  num("bl_x", "Bottom Left X", -1, 2, 0, 0.001), num("bl_y", "Bottom Left Y", -1, 2, 1, 0.001)},
                 {}});
    c.push_back({"letterbox", "Letterbox", EffectCategory::VideoFilter, "Stylize",
                 {choice("aspect", "Aspect", {"2.39:1", "2:1", "1.85:1", "4:3", "1:1", "9:16"}, 0),
                  pct("opacity", "Opacity", 0, 100, 100)},
                 {}});
    c.push_back({"posterize", "Posterize", EffectCategory::VideoFilter, "Stylize",
                 {num("levels", "Levels", 2, 64, 6, 1)},
                 {}});
    c.push_back({"vignette", "Vignette", EffectCategory::VideoFilter, "Stylize",
                 {num("amount", "Amount", 0, 1, 0.5), num("size", "Size", 0, 1.5, 0.75),
                  num("softness", "Softness", 0.01, 1, 0.5)},
                 {}});
    c.push_back({"mosaic", "Mosaic", EffectCategory::VideoFilter, "Stylize",
                 {num("size", "Block Size (px)", 1, 200, 16, 1)},
                 {}});
    c.push_back({"mirror", "Mirror", EffectCategory::VideoFilter, "Distort",
                 {choice("mode", "Mode", {"Left onto Right", "Right onto Left", "Top onto Bottom", "Bottom onto Top"})},
                 {}});
    c.push_back({"drop_shadow", "Drop Shadow", EffectCategory::VideoFilter, "Stylize",
                 {num("distance", "Distance (px)", 0, 500, 10, 1), angle("angle", "Direction", 135),
                  num("softness", "Softness (px)", 0, 100, 8, 0.5), pct("opacity", "Opacity", 0, 100, 60),
                  color("color", "Color", 0, 0, 0)},
                 {}});

    // ---- Audio filters ----------------------------------------------------
    c.push_back({"eq3", "3-Band EQ", EffectCategory::AudioFilter, "EQ",
                 {num("low_db", "Low (dB)", -24, 24, 0, 0.1), num("low_hz", "Low Freq (Hz)", 20, 1000, 200, 1),
                  num("mid_db", "Mid (dB)", -24, 24, 0, 0.1), num("mid_hz", "Mid Freq (Hz)", 200, 8000, 1000, 1),
                  num("mid_q", "Mid Q", 0.1, 10, 0.9), num("high_db", "High (dB)", -24, 24, 0, 0.1),
                  num("high_hz", "High Freq (Hz)", 1000, 20000, 5000, 1)},
                 {}});
    c.push_back({"compressor", "Compressor", EffectCategory::AudioFilter, "Dynamics",
                 {num("threshold_db", "Threshold (dB)", -60, 0, -18, 0.1), num("ratio", "Ratio", 1, 20, 4, 0.1),
                  num("attack_ms", "Attack (ms)", 0.1, 200, 10, 0.1), num("release_ms", "Release (ms)", 5, 2000, 120, 1),
                  num("makeup_db", "Makeup (dB)", 0, 30, 0, 0.1)},
                 {}});
    c.push_back({"limiter", "Limiter", EffectCategory::AudioFilter, "Dynamics",
                 {num("ceiling_db", "Ceiling (dB)", -20, 0, -1, 0.1), num("release_ms", "Release (ms)", 5, 1000, 60, 1)},
                 {}});
    // Restoration: processed on the clip's whole source audio (audio/SpeechCleanup.h).
    c.push_back({"denoise", "Noise Reduction", EffectCategory::AudioFilter, "Restoration",
                 {num("reduction_db", "Reduction (dB)", 0, 40, 15, 0.5), pct("sensitivity", "Sensitivity", 0, 100, 50)},
                 {}});
    c.push_back({"voice_isolate", "Voice Isolation", EffectCategory::AudioFilter, "Restoration",
                 {pct("amount", "Amount", 0, 100, 100)},
                 {}});
#ifndef MONTAGE_WITH_RNNOISE
    c.back().hidden = true;  // needs the RNNoise model
#endif
    c.push_back({"enhance_speech", "Enhance Speech", EffectCategory::AudioFilter, "Restoration",
                 {choice("keep", "Keep", {"Speech", "Everything but Speech"}, 0), pct("amount", "Amount", 0, 100, 100),
                  num("max_reduction_db", "Max Reduction (dB)", 3, 100, 100, 1)},
                 {}});
#ifndef MONTAGE_WITH_ONNXRUNTIME
    c.back().hidden = true;  // needs ONNX Runtime
#endif
    c.push_back({"declick", "De-Click", EffectCategory::AudioFilter, "Restoration",
                 {pct("sensitivity", "Sensitivity", 0, 100, 50), num("max_ms", "Longest Click (ms)", 0.1, 10, 2, 0.1)},
                 {}});
    c.push_back({"dehum", "De-Hum", EffectCategory::AudioFilter, "Restoration",
                 {choice("mains", "Mains", {"50 Hz", "60 Hz"}, 0), num("harmonics", "Harmonics", 1, 16, 6, 1),
                  num("reduction_db", "Reduction (dB)", 0, 60, 30, 0.5), num("width_hz", "Notch Width (Hz)", 0.5, 10, 2, 0.1)},
                 {}});
    c.push_back({"parametric_eq", "Parametric EQ", EffectCategory::AudioFilter, "EQ",
                 {num("low_hz", "Low Shelf (Hz)", 20, 1000, 100, 1), num("low_db", "Low Shelf (dB)", -24, 24, 0, 0.1),
                  num("b1_hz", "Band 1 (Hz)", 20, 20000, 250, 1), num("b1_db", "Band 1 (dB)", -24, 24, 0, 0.1),
                  num("b1_q", "Band 1 Q", 0.1, 20, 1), num("b2_hz", "Band 2 (Hz)", 20, 20000, 1000, 1),
                  num("b2_db", "Band 2 (dB)", -24, 24, 0, 0.1), num("b2_q", "Band 2 Q", 0.1, 20, 1),
                  num("b3_hz", "Band 3 (Hz)", 20, 20000, 4000, 1), num("b3_db", "Band 3 (dB)", -24, 24, 0, 0.1),
                  num("b3_q", "Band 3 Q", 0.1, 20, 1), num("high_hz", "High Shelf (Hz)", 1000, 20000, 10000, 1),
                  num("high_db", "High Shelf (dB)", -24, 24, 0, 0.1), num("output_db", "Output (dB)", -24, 24, 0, 0.1)},
                 {}});
    c.push_back({"multiband", "Multiband Compressor", EffectCategory::AudioFilter, "Dynamics",
                 {num("low_hz", "Low / Mid Split (Hz)", 40, 2000, 200, 1), num("high_hz", "Mid / High Split (Hz)", 500, 16000, 2500, 1),
                  num("low_threshold_db", "Low Threshold (dB)", -60, 0, -24, 0.1), num("low_ratio", "Low Ratio", 1, 20, 3, 0.1),
                  num("low_gain_db", "Low Gain (dB)", -24, 24, 0, 0.1), num("mid_threshold_db", "Mid Threshold (dB)", -60, 0, -24, 0.1),
                  num("mid_ratio", "Mid Ratio", 1, 20, 2.5, 0.1), num("mid_gain_db", "Mid Gain (dB)", -24, 24, 0, 0.1),
                  num("high_threshold_db", "High Threshold (dB)", -60, 0, -24, 0.1), num("high_ratio", "High Ratio", 1, 20, 3, 0.1),
                  num("high_gain_db", "High Gain (dB)", -24, 24, 0, 0.1), num("attack_ms", "Attack (ms)", 0.1, 200, 10, 0.1),
                  num("release_ms", "Release (ms)", 5, 2000, 150, 1), num("output_db", "Output (dB)", -24, 24, 0, 0.1)},
                 {}});
    c.push_back({"deesser", "De-Esser", EffectCategory::AudioFilter, "Dynamics",
                 {num("hz", "Frequency (Hz)", 2000, 12000, 6000, 10), num("threshold_db", "Threshold (dB)", -60, 0, -30, 0.1),
                  num("reduction_db", "Max Reduction (dB)", 0, 24, 10, 0.1)},
                 {}});
    // Added from the transcript (Bleep Words); the stretches are in source seconds.
    {
        EffectInfo bleep{"bleep", "Bleep", EffectCategory::AudioFilter, "Restoration",
                         {choice("mode", "Cover With", {"Tone", "Silence"}, 0), num("frequency", "Tone (Hz)", 200, 4000, 1000, 1),
                          num("level", "Tone Level (dB)", -40, 0, -12, 0.5)},
                         {str("ranges", "Bleeped (source seconds)", StringKind::Text)}};
        bleep.hidden = true;
        c.push_back(bleep);
    }
    c.push_back({"gate", "Noise Gate", EffectCategory::AudioFilter, "Dynamics",
                 {num("threshold_db", "Threshold (dB)", -80, 0, -45, 0.1), num("range_db", "Range (dB)", -80, 0, -40, 0.1),
                  num("attack_ms", "Attack (ms)", 0.1, 50, 1, 0.1), num("hold_ms", "Hold (ms)", 0, 500, 50, 1),
                  num("release_ms", "Release (ms)", 5, 2000, 150, 1)},
                 {}});
    c.push_back({"reverb", "Reverb", EffectCategory::AudioFilter, "Time",
                 {pct("size", "Room Size", 0, 100, 50), pct("damping", "Damping", 0, 100, 50), pct("width", "Width", 0, 100, 100),
                  pct("mix", "Mix", 0, 100, 25)},
                 {}});
    c.push_back({"channels", "Channel Tools", EffectCategory::AudioFilter, "Channels",
                 {choice("mode", "Channels", {"Stereo", "Mono (Sum)", "Left to Both", "Right to Both", "Swap Left and Right"}, 0),
                  boolean("invert_l", "Invert Left Polarity"), boolean("invert_r", "Invert Right Polarity")},
                 {}});
    c.push_back({"stereo_width", "Stereo Width", EffectCategory::AudioFilter, "Channels",
                 {pct("width", "Width", 0, 200, 100), num("bass_mono_hz", "Mono Bass Below (Hz)", 0, 500, 0, 1)},
                 {}});
    c.push_back({"chorus", "Chorus", EffectCategory::AudioFilter, "Modulation",
                 {num("rate", "Rate (Hz)", 0.02, 5, 0.8, 0.01), num("depth_ms", "Depth (ms)", 0, 10, 3, 0.1),
                  num("delay_ms", "Delay (ms)", 5, 40, 15, 0.1), pct("spread", "Stereo Spread", 0, 100, 100), pct("mix", "Mix", 0, 100, 50)},
                 {}});
    c.push_back({"flanger", "Flanger", EffectCategory::AudioFilter, "Modulation",
                 {num("rate", "Rate (Hz)", 0.02, 5, 0.25, 0.01), num("depth_ms", "Depth (ms)", 0, 5, 2, 0.05),
                  num("delay_ms", "Delay (ms)", 0.1, 10, 1, 0.05), pct("feedback", "Feedback", -95, 95, 50),
                  pct("spread", "Stereo Spread", 0, 100, 50), pct("mix", "Mix", 0, 100, 50)},
                 {}});
    c.push_back({"phaser", "Phaser", EffectCategory::AudioFilter, "Modulation",
                 {choice("stages", "Stages", {"4", "6", "8", "12"}, 0), num("low_hz", "Low (Hz)", 20, 5000, 300, 1),
                  num("high_hz", "High (Hz)", 100, 16000, 3000, 1), num("rate", "Rate (Hz)", 0.02, 5, 0.4, 0.01),
                  pct("feedback", "Feedback", -95, 95, 30), pct("spread", "Stereo Spread", 0, 100, 50), pct("mix", "Mix", 0, 100, 50)},
                 {}});
    c.push_back({"tremolo", "Tremolo / Auto-Pan", EffectCategory::AudioFilter, "Modulation",
                 {choice("mode", "Mode", {"Tremolo", "Auto-Pan"}, 0), choice("shape", "Shape", {"Sine", "Triangle", "Square"}, 0),
                  num("rate", "Rate (Hz)", 0.05, 20, 5, 0.01), pct("depth", "Depth", 0, 100, 50)},
                 {}});
    c.push_back({"saturation", "Saturation", EffectCategory::AudioFilter, "Distortion",
                 {choice("type", "Type", {"Tape", "Tube", "Hard Clip"}, 0), num("drive_db", "Drive (dB)", 0, 36, 6, 0.1),
                  pct("tone", "Tone", -100, 100, 0), pct("mix", "Mix", 0, 100, 100), num("output_db", "Output (dB)", -24, 24, 0, 0.1)},
                 {}});
    // Processed on the clip's whole source audio (audio/AudioRepair.h): no latency, and a fine shift.
    c.push_back({"pitch_shift", "Pitch Shift", EffectCategory::AudioFilter, "Pitch",
                 {num("semitones", "Semitones", -24, 24, 0, 1), num("cents", "Fine (cents)", -100, 100, 0, 1)},
                 {}});
    c.push_back({"highpass", "High-Pass Filter", EffectCategory::AudioFilter, "EQ",
                 {num("hz", "Cutoff (Hz)", 20, 2000, 80, 1)}, {}});
    c.push_back({"lowpass", "Low-Pass Filter", EffectCategory::AudioFilter, "EQ",
                 {num("hz", "Cutoff (Hz)", 200, 20000, 12000, 1)}, {}});
    c.push_back({"delay", "Delay / Echo", EffectCategory::AudioFilter, "Time",
                 {num("time_ms", "Time (ms)", 1, 2000, 300, 1), num("feedback", "Feedback", 0, 0.95, 0.35),
                  pct("mix", "Mix", 0, 100, 30)},
                 {}});
    {
        // A third-party audio plugin (CLAP/VST3/AU/LV2); see audio/PluginEffect.h.
        EffectInfo plugin{"plugin", "Audio Plugin", EffectCategory::AudioFilter, "Plugins", {}, {}};
        plugin.hidden = true;
        c.push_back(plugin);
    }

    // Distort and stylize (render/StyleFx.h). Sizes in source pixels; centres as offsets from the middle.
    {
        auto centre = [](std::vector<ParamInfo> ps) {
            ps.push_back(num("center_x", "Center X (px)", -8000, 8000, 0, 1));
            ps.push_back(num("center_y", "Center Y (px)", -8000, 8000, 0, 1));
            return ps;
        };
        c.push_back({"wave_warp", "Wave Warp", EffectCategory::VideoFilter, "Distort",
                     {choice("shape", "Wave Type", {"Sine", "Triangle", "Square"}, 0), num("height", "Wave Height (px)", -500, 500, 10, 0.5),
                      num("width", "Wave Width (px)", 1, 4000, 40, 1), angle("direction", "Direction", 0), angle("phase", "Phase", 0),
                      num("speed", "Speed (° per frame)", -180, 180, 0, 0.5)},
                     {}});
        c.push_back({"twirl", "Twirl", EffectCategory::VideoFilter, "Distort",
                     centre({angle("angle", "Angle", 90), pct("radius", "Radius", 1, 100, 50)}), {}});
        c.push_back({"spherize", "Spherize", EffectCategory::VideoFilter, "Distort",
                     centre({pct("amount", "Amount (Bulge + / Pinch -)", -100, 100, 50), pct("radius", "Radius", 1, 100, 50)}), {}});
        c.push_back({"ripple", "Ripple", EffectCategory::VideoFilter, "Distort",
                     centre({num("amplitude", "Amplitude (px)", 0, 200, 8, 0.5), num("wavelength", "Wavelength (px)", 2, 2000, 40, 1),
                             num("speed", "Speed (° per frame)", -180, 180, 20, 0.5), pct("radius", "Radius", 1, 100, 100)}),
                     {}});
        c.push_back({"turbulent_displace", "Turbulent Displace", EffectCategory::VideoFilter, "Distort",
                     {num("amount", "Amount (px)", 0, 500, 20, 0.5), num("size", "Size (px)", 2, 2000, 100, 1),
                      num("complexity", "Complexity", 1, 8, 3, 1), num("evolution", "Evolution (°)", -36000, 36000, 0, 1),
                      num("speed", "Evolution Speed (° per frame)", -90, 90, 0, 0.1), num("seed", "Random Seed", 0, 9999, 0, 1)},
                     {}});
        c.push_back({"motion_tile", "Motion Tile", EffectCategory::VideoFilter, "Distort",
                     {pct("tile_width", "Tile Width", 1, 100, 100), pct("tile_height", "Tile Height", 1, 100, 100),
                      num("offset_x", "Offset X (px)", -8000, 8000, 0, 1), num("offset_y", "Offset Y (px)", -8000, 8000, 0, 1),
                      boolean("mirror", "Mirror Edges")},
                     {}});
        c.push_back({"find_edges", "Find Edges", EffectCategory::VideoFilter, "Stylize",
                     {boolean("invert", "Invert", true), pct("blend", "Blend With Original", 0, 100, 0)}, {}});
        c.push_back({"emboss", "Emboss", EffectCategory::VideoFilter, "Stylize",
                     {angle("direction", "Direction", 135), num("relief", "Relief (px)", 0.25, 50, 2, 0.25), pct("contrast", "Contrast", 0, 500, 100),
                      pct("blend", "Blend With Original", 0, 100, 0)},
                     {}});
        c.push_back({"halftone", "Halftone", EffectCategory::VideoFilter, "Stylize",
                     {num("dot_size", "Dot Size (px)", 2, 200, 8, 0.5), angle("angle", "Angle", 45),
                      choice("mode", "Ink", {"Black", "Colour (CMY)"}, 0)},
                     {}});
        c.push_back({"duotone", "Duotone", EffectCategory::VideoFilter, "Stylize",
                     {color("shadows", "Shadows", 0.08, 0.1, 0.35), color("highlights", "Highlights", 1, 0.85, 0.6), pct("mix", "Mix", 0, 100, 100)},
                     {}});
        c.push_back({"vhs", "VHS", EffectCategory::VideoFilter, "Stylize",
                     {pct("amount", "Amount", 0, 100, 100), num("bleed", "Colour Bleed (px)", 0, 40, 4, 0.5), pct("noise", "Noise", 0, 100, 30),
                      pct("scanlines", "Scanlines", 0, 100, 30), pct("tracking", "Tracking Errors", 0, 100, 30)},
                     {}});
        // Holds each frame for a few (handled where the clip's frame is chosen).
        c.push_back({"stop_motion", "Stop Motion", EffectCategory::VideoFilter, "Stylize",
                     {num("hold", "Hold Each Frame For (frames)", 1, 30, 3, 1)}, {}});
        // From the frames either side (render/TemporalFx.h; worked out where the source frame is read).
        c.push_back({"motion_blur", "Motion Blur", EffectCategory::VideoFilter, "Blur & Sharpen",
                     {num("shutter", "Shutter Angle (°)", 0, 720, 180, 1)}, {}});
        c.push_back({"deflicker", "Deflicker", EffectCategory::VideoFilter, "Repair",
                     {num("frames", "Frames Either Side", 1, 12, 3, 1), choice("area", "Even Out", {"Whole Frame", "Each Area (16 x 9)"}, 1),
                      pct("strength", "Strength", 0, 100, 100)},
                     {}});
        c.push_back({"tilt_shift", "Tilt-Shift Blur", EffectCategory::VideoFilter, "Blur & Sharpen",
                     {pct("focus", "Focus Position", 0, 100, 50), pct("width", "Focus Width", 0, 100, 20), num("blur", "Blur (px)", 0, 200, 12, 0.5),
                      angle("angle", "Angle", 0), pct("saturation", "Saturation", 0, 200, 120)},
                     {}});
        c.push_back({"camera_shake", "Camera Shake", EffectCategory::VideoFilter, "Transform",
                     {num("amplitude", "Amplitude (px)", 0, 500, 10, 0.5), num("rotation", "Rotation (°)", 0, 45, 1, 0.1),
                      pct("speed", "Speed", 0, 100, 50), boolean("zoom", "Zoom to Hide Edges", true), num("seed", "Random Seed", 0, 9999, 0, 1)},
                     {}});
    }

    // ---- Generators -------------------------------------------------------
    c.push_back({"color", "Color Matte", EffectCategory::Generator, "Generators",
                 {color("color", "Color", 0.1, 0.1, 0.1), pct("alpha", "Alpha", 0, 100, 100)},
                 {}});
    c.push_back({"gradient", "Gradient", EffectCategory::Generator, "Generators",
                 {color("color_a", "Start Color", 0.05, 0.1, 0.3), color("color_b", "End Color", 0.6, 0.2, 0.4),
                  angle("angle", "Angle", 90), choice("shape", "Shape", {"Linear", "Radial"})},
                 {}});
    c.push_back({"bars", "Color Bars", EffectCategory::Generator, "Generators", {}, {}});
    // Shape layers: every value keyframeable; Trim End from 0 to 100 % draws the outline on.
    c.push_back({"shape", "Shape", EffectCategory::Generator, "Generators",
                 {choice("shape", "Shape", {"Rectangle", "Ellipse", "Polygon", "Star", "Line", "Arrow"}, 0),
                  num("width", "Width (px)", 1, 8000, 400, 1), num("height", "Height (px)", 1, 8000, 300, 1),
                  num("pos_x", "Position X", -8000, 8000, 0, 1), num("pos_y", "Position Y", -8000, 8000, 0, 1),
                  angle("rotation", "Rotation", 0), num("roundness", "Corner Roundness (px)", 0, 4000, 0, 0.5),
                  num("points", "Sides / Points", 3, 64, 5, 1), pct("inner", "Star Inner Radius", 1, 100, 45),
                  boolean("fill", "Fill", true), color("fill_color", "Fill Color", 0.95, 0.75, 0.1),
                  pct("fill_opacity", "Fill Opacity", 0, 100, 100),
                  choice("gradient", "Fill Gradient", {"None", "Linear", "Radial"}, 0),
                  color("fill_color2", "Gradient End Color", 0.9, 0.2, 0.4), angle("gradient_angle", "Gradient Angle", 0),
                  num("stroke", "Stroke Width (px)", 0, 500, 0, 0.5), color("stroke_color", "Stroke Color", 1, 1, 1),
                  pct("stroke_opacity", "Stroke Opacity", 0, 100, 100),
                  choice("join", "Corners and Ends", {"Round", "Sharp", "Bevelled"}, 0),
                  num("dash", "Dash (px, 0 solid)", 0, 2000, 0, 0.5), num("gap", "Dash Gap (px)", 0, 2000, 10, 0.5),
                  pct("trim_start", "Trim Start", 0, 100, 0), pct("trim_end", "Trim End", 0, 100, 100),
                  angle("trim_offset", "Trim Offset", 0), pct("opacity", "Opacity", 0, 100, 100)},
                 {}});
    // Its effects, opacity and blend mode apply to everything on the tracks below it.
    c.push_back({"adjustment", "Adjustment Layer", EffectCategory::Generator, "Generators", {}, {}});
    {
        EffectInfo title{"title", "Title", EffectCategory::Generator, "Titles",
                         {
                             num("size", "Font Size (px)", 4, 1000, 96, 1),
                             color("color", "Fill", 1, 1, 1),
                             num("pos_x", "Position X", -4000, 4000, 0, 1),
                             num("pos_y", "Position Y", -4000, 4000, 0, 1),
                             num("tracking", "Tracking", -50, 200, 0, 0.5),
                             num("line_spacing", "Line Spacing", 0.5, 3, 1.15),
                             boolean("bold", "Bold", true),
                             boolean("italic", "Italic"),
                             choice("align", "Alignment", {"Left", "Center", "Right"}, 1),
                             num("outline", "Outline Width (px)", 0, 50, 0, 0.5),
                             color("outline_color", "Outline Color", 0, 0, 0),
                             num("shadow", "Shadow Distance (px)", 0, 100, 4, 0.5),
                             pct("shadow_opacity", "Shadow Opacity", 0, 100, 50),
                             pct("box_opacity", "Background Box", 0, 100, 0),
                             color("box_color", "Box Color", 0, 0, 0),
                             num("box_padding", "Box Padding (px)", 0, 200, 24, 1),
                             pct("opacity", "Opacity", 0, 100, 100),
                             // Where it sits: Free is Position from the centre; the others keep it inside
                             // the title-safe area at any frame size, Position then nudging it.
                             choice("anchor", "Placement", {"Free", "Lower Left", "Lower Centre", "Lower Right", "Upper Left", "Upper Right"}, 0),
                             // Lines after the first in their own size and colour (a name and a role).
                             boolean("sub_style", "Style Lines After the First"),
                             pct("sub_scale", "Their Size", 20, 200, 60),
                             color("sub_color", "Their Color", 0.85, 0.85, 0.85),
                             choice("bar", "Accent Bar", {"None", "Left", "Below"}, 0),
                             color("bar_color", "Bar Color", 1, 0.75, 0.1),
                             num("bar_width", "Bar Width (px)", 1, 60, 8, 0.5),
                             choice("anim_in", "Animate In", {"None", "Fade", "Slide Up", "Slide Down", "Slide Left", "Slide Right", "Pop", "Typewriter", "Wipe"}, 0),
                             num("anim_in_dur", "In Duration (s)", 0.05, 10, 0.5, 0.05),
                             choice("anim_out", "Animate Out", {"None", "Fade", "Slide Up", "Slide Down", "Slide Left", "Slide Right", "Pop", "Typewriter", "Wipe"}, 0),
                             num("anim_out_dur", "Out Duration (s)", 0.05, 10, 0.5, 0.05),
                         },
                         {str("text", "Text", StringKind::MultilineText, "Title"),
                          str("font", "Font", StringKind::Font, "Sans Serif")}};
        c.push_back(title);
        // Ready-made titles: a Title with its settings filled in (makeEffect gives type "title").
        for (const TitleTemplate& tpl : titleTemplates()) {
            EffectInfo t = title;
            t.type = tpl.id;
            t.displayName = tpl.name;
            c.push_back(t);
        }
    }

    // ---- Transitions ------------------------------------------------------
    c.push_back({"cross_dissolve", "Cross Dissolve", EffectCategory::VideoTransition, "Dissolve", {}, {}});
    c.push_back({"dip_to_black", "Dip to Black", EffectCategory::VideoTransition, "Dissolve", {}, {}});
    c.push_back({"dip_to_white", "Dip to White", EffectCategory::VideoTransition, "Dissolve", {}, {}});
    // Hides a jump cut (an interview with a pause taken out): the outgoing
    // frame morphs into the incoming one along the optical flow between them.
    c.push_back({"smooth_cut", "Smooth Cut", EffectCategory::VideoTransition, "Dissolve", {}, {}});
    c.push_back({"wipe", "Wipe", EffectCategory::VideoTransition, "Wipe",
                 {angle("angle", "Angle", 0), num("softness", "Softness", 0, 1, 0.05)}, {}});
    c.push_back({"push", "Push", EffectCategory::VideoTransition, "Slide",
                 {choice("direction", "Direction", {"Left", "Right", "Up", "Down"})}, {}});
    c.push_back({"slide", "Slide", EffectCategory::VideoTransition, "Slide",
                 {choice("direction", "Direction", {"Left", "Right", "Up", "Down"})}, {}});
    c.push_back({"iris", "Iris Round", EffectCategory::VideoTransition, "Iris",
                 {num("softness", "Softness", 0, 1, 0.05)}, {}});
    c.push_back({"zoom", "Cross Zoom", EffectCategory::VideoTransition, "Zoom",
                 {num("strength", "Strength", 0, 4, 1)}, {}});
    // Creator transitions: fast moves smeared along their motion, and stylised cuts.
    c.push_back({"whip_pan", "Whip Pan", EffectCategory::VideoTransition, "Motion",
                 {choice("direction", "Direction", {"Left", "Right", "Up", "Down"}), num("strength", "Blur", 0, 2, 1, 0.05)}, {}});
    c.push_back({"zoom_blur", "Zoom Blur", EffectCategory::VideoTransition, "Motion", {num("strength", "Strength", 0, 3, 1, 0.05)}, {}});
    c.push_back({"spin", "Spin", EffectCategory::VideoTransition, "Motion",
                 {num("turn", "Turn (degrees)", -720, 720, 180, 1), num("strength", "Blur", 0, 2, 1, 0.05)}, {}});
    c.push_back({"glitch", "Glitch", EffectCategory::VideoTransition, "Stylize", {num("strength", "Strength", 0, 2, 1, 0.05)}, {}});
    c.push_back({"light_leak", "Light Leak", EffectCategory::VideoTransition, "Stylize",
                 {color("color", "Color", 1.0, 0.55, 0.2), num("strength", "Strength", 0, 3, 1, 0.05)}, {}});
    c.push_back({"luma_wipe", "Luma Wipe", EffectCategory::VideoTransition, "Wipe",
                 {num("softness", "Softness", 0, 1, 0.1), boolean("invert", "Brights First")}, {}});
    c.push_back({"clock_wipe", "Clock Wipe", EffectCategory::VideoTransition, "Wipe", {num("softness", "Softness", 0, 1, 0.03)}, {}});
    // Premiere's Shape Dissolve: B grows inside shapes (one, or a grid of them) until it covers A.
    c.push_back({"shape_wipe", "Shape Wipe", EffectCategory::VideoTransition, "Wipe",
                 {choice("shape", "Shape", {"Circle", "Diamond", "Square", "Star", "Heart", "Cross"}, 0), num("count", "Shapes Across", 1, 20, 1, 1),
                  angle("angle", "Rotation", 0), num("softness", "Softness", 0, 1, 0.05)},
                 {}});
    // In perspective: A and B on two faces of a turning cube, or the two sides of a card.
    c.push_back({"cube", "3D Cube", EffectCategory::VideoTransition, "3D",
                 {choice("direction", "Direction", {"Left", "Right", "Up", "Down"})}, {}});
    c.push_back({"flip", "3D Flip", EffectCategory::VideoTransition, "3D",
                 {choice("direction", "Direction", {"Horizontal", "Vertical"})}, {}});
    c.push_back({"crossfade", "Crossfade (Equal Power)", EffectCategory::AudioTransition, "Crossfade", {}, {}});
    c.push_back({"crossfade_linear", "Crossfade (Constant Gain)", EffectCategory::AudioTransition, "Crossfade", {}, {}});

    return c;
}

}  // namespace

const EffectInfo& maskInfo() {
    static const EffectInfo info = [] {
        EffectInfo m{"mask", "Mask", EffectCategory::Fixed, "Mask", {}, {}, true};
        m.params = {choice("mask.shape", "Shape", {"None", "Ellipse", "Rectangle", "Object", "People"}, 0),
                    num("mask.x", "Center X", -0.5, 1.5, 0.5, 0.001),
                    num("mask.y", "Center Y", -0.5, 1.5, 0.5, 0.001),
                    num("mask.w", "Width", 0.001, 2, 0.4, 0.001),
                    num("mask.h", "Height", 0.001, 2, 0.4, 0.001),
                    angle("mask.rotation", "Rotation"),
                    num("mask.feather", "Feather (px)", 0, 500, 20, 0.5),
                    num("mask.expansion", "Expansion (px)", -500, 500, 0, 0.5),
                    pct("mask.opacity", "Mask Opacity", 0, 100, 100),
                    boolean("mask.invert", "Invert"),
                    boolean("mask.qualify", "HSL Qualifier"),
                    num("mask.hue", "Hue Center", 0, 360, 0, 0.5),
                    num("mask.hue_width", "Hue Width", 1, 360, 60, 0.5),
                    pct("mask.sat_low", "Saturation Low", 0, 100, 15),
                    pct("mask.sat_high", "Saturation High", 0, 100, 100),
                    pct("mask.lum_low", "Luma Low", 0, 100, 5),
                    pct("mask.lum_high", "Luma High", 0, 100, 100),
                    pct("mask.softness", "Softness", 0, 100, 20),
                    boolean("mask.depth", "Depth Qualifier"),
                    pct("mask.depth_low", "Depth From (far)", 0, 100, 50),
                    pct("mask.depth_high", "Depth To (near)", 0, 100, 100),
                    pct("mask.depth_soft", "Depth Softness", 0, 100, 10),
                    boolean("mask.show", "Show Mask")};
        return m;
    }();
    return info;
}

bool supportsMask(const std::string& effectType) {
    const EffectInfo* info = findEffectInfo(effectType);
    return info && info->category == EffectCategory::VideoFilter;
}

bool hasMask(const Effect& e, FrameTime t) {
    return e.p("mask.shape", t) > 0.5 || e.p("mask.qualify", t) > 0.5 || e.p("mask.depth", t) > 0.5;
}

bool needsFaces(const Effect& e) { return e.enabled && e.type == "face_refine"; }

bool needsPersonMatte(const Effect& e, FrameTime t) {
    if (!e.enabled) return false;
    return e.type == "remove_background" || std::lround(e.p("mask.shape", t)) == 4;
}

bool needsDepth(const Effect& e, FrameTime t) {
    if (!e.enabled) return false;
    return e.type == "depth_blur" || e.type == "depth_fog" || e.type == "depth_map" || e.type == "relight" || e.p("mask.depth", t) > 0.5;
}

const std::vector<EffectInfo>& effectCatalog() {
    static const std::vector<EffectInfo> catalog = buildCatalog();
    return catalog;
}

const EffectInfo* findEffectInfo(const std::string& type) {
    for (const auto& e : effectCatalog())
        if (e.type == type) return &e;
    return nullptr;
}

std::vector<const EffectInfo*> effectsInCategory(EffectCategory c) {
    std::vector<const EffectInfo*> out;
    for (const auto& e : effectCatalog())
        if (e.category == c && !e.hidden) out.push_back(&e);
    return out;
}

std::string pluginParamMeta(const ParamInfo& p) {
    return p.label + "\t" + std::to_string(p.min) + "\t" + std::to_string(p.max) + "\t" + std::to_string(p.def) + "\t" +
           (p.step >= 1 ? "1" : "0");
}

std::vector<ParamInfo> effectParams(const Effect& e) {
    if (e.type != "plugin") {
        const EffectInfo* info = findEffectInfo(e.type);
        return info ? info->params : std::vector<ParamInfo>{};
    }
    std::vector<ParamInfo> out;
    for (const auto& [key, param] : e.params) {
        if (key.rfind("param.", 0) != 0) continue;
        ParamInfo p;
        p.name = key;
        p.label = key.substr(6);
        auto meta = e.strings.find("meta." + key.substr(6));
        if (meta != e.strings.end()) {
            std::vector<std::string> f;
            size_t start = 0;
            for (size_t tab; (tab = meta->second.find('\t', start)) != std::string::npos; start = tab + 1)
                f.push_back(meta->second.substr(start, tab - start));
            f.push_back(meta->second.substr(start));
            if (f.size() >= 5) {
                p.label = f[0];
                p.min = std::atof(f[1].c_str());
                p.max = std::atof(f[2].c_str());
                p.def = std::atof(f[3].c_str());
                p.step = f[4] == "1" ? 1.0 : (p.max - p.min) / 1000.0;
            }
        }
        out.push_back(p);
    }
    return out;
}

const std::vector<TitleTemplate>& titleTemplates() {
    // Choices: anchor 0 Free, 1 Lower Left, 2 Lower Centre, 3 Lower Right, 4 Upper Left, 5 Upper Right;
    // bar 0 None, 1 Left, 2 Below; animations 0 None, 1 Fade, 2 Slide Up, 3 Slide Down, 4 Slide Left,
    // 5 Slide Right, 6 Pop, 7 Typewriter, 8 Wipe.
    static const std::vector<TitleTemplate> list = {
        {"title_lower_third", "Lower Third",
         {{"size", 60}, {"align", 0}, {"anchor", 1}, {"sub_style", 1}, {"sub_scale", 60}, {"bar", 1}, {"shadow", 3},
          {"shadow_opacity", 60}, {"anim_in", 5}, {"anim_in_dur", 0.5}, {"anim_out", 1}, {"anim_out_dur", 0.4}},
         {{"text", "Name Surname\nRole or place"}}},
        {"title_lower_third_box", "Lower Third (Box)",
         {{"size", 54}, {"align", 0}, {"anchor", 1}, {"sub_style", 1}, {"sub_scale", 62}, {"box_opacity", 72},
          {"box_padding", 22}, {"shadow", 0}, {"anim_in", 8}, {"anim_in_dur", 0.45}, {"anim_out", 8}, {"anim_out_dur", 0.35}},
         {{"text", "Name Surname\nRole or place"}}},
        {"title_centred", "Centred Title",
         {{"size", 128}, {"shadow", 6}, {"anim_in", 6}, {"anim_in_dur", 0.45}, {"anim_out", 1}, {"anim_out_dur", 0.5}},
         {{"text", "Title"}}},
        {"title_chapter", "Chapter Heading",
         {{"size", 96}, {"sub_style", 1}, {"sub_scale", 45}, {"bar", 2}, {"bar_width", 6}, {"anim_in", 2}, {"anim_in_dur", 0.6},
          {"anim_out", 1}, {"anim_out_dur", 0.5}},
         {{"text", "Chapter One\nWhere it begins"}}},
        {"title_callout", "Call-out",
         {{"size", 64}, {"color.r", 0.05}, {"color.g", 0.05}, {"color.b", 0.05}, {"box_opacity", 100}, {"box_color.r", 1},
          {"box_color.g", 0.82}, {"box_color.b", 0.15}, {"box_padding", 20}, {"shadow", 0}, {"anchor", 5}, {"anim_in", 6},
          {"anim_in_dur", 0.35}, {"anim_out", 6}, {"anim_out_dur", 0.3}},
         {{"text", "Look at this!"}}},
        {"title_typewriter", "Typewriter",
         {{"size", 72}, {"bold", 0}, {"shadow", 2}, {"anim_in", 7}, {"anim_in_dur", 1.5}, {"anim_out", 1}, {"anim_out_dur", 0.5}},
         {{"text", "Once upon a time..."}, {"font", "Monospace"}}},
        {"title_end_card", "End Card",
         {{"size", 110}, {"sub_style", 1}, {"sub_scale", 50}, {"anim_in", 1}, {"anim_in_dur", 0.8}, {"anim_out", 1}, {"anim_out_dur", 0.8}},
         {{"text", "Thanks for watching\nSee you next time"}}},
    };
    return list;
}

const TitleTemplate* findTitleTemplate(const std::string& id) {
    for (const TitleTemplate& t : titleTemplates())
        if (id == t.id) return &t;
    return nullptr;
}

Effect makeEffect(const std::string& type, Id id) {
    if (const TitleTemplate* tpl = findTitleTemplate(type)) {
        Effect e = makeEffect("title", id);
        for (const auto& [name, value] : tpl->params) e.params[name] = Param(value);
        for (const auto& [name, value] : tpl->strings) e.strings[name] = value;
        return e;
    }
    Effect e;
    e.id = id;
    e.type = type;
    if (const EffectInfo* info = findEffectInfo(type)) {
        for (const auto& p : info->params) {
            if (p.kind == ParamKind::Color) {
                e.params[p.name + ".r"] = Param(p.def);
                e.params[p.name + ".g"] = Param(p.defG);
                e.params[p.name + ".b"] = Param(p.defB);
            } else {
                e.params[p.name] = Param(p.def);
            }
        }
        for (const auto& s : info->strings) e.strings[s.name] = s.def;
    }
    return e;
}

Effect makeEffect(Project& p, const std::string& type) { return makeEffect(type, p.newId()); }

const std::vector<std::string>& blendModes() {
    static const std::vector<std::string> modes = {"normal",  "add",     "multiply", "screen",     "overlay",
                                                   "darken",  "lighten", "difference", "soft_light", "hard_light",
                                                   "color_dodge", "color_burn", "subtract"};
    return modes;
}

}  // namespace montage
