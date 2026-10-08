#include "Effects.h"

#include <algorithm>
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
                  choice("sampling", "Frame Sampling", {"Nearest Frame", "Frame Blending", "Optical Flow"}, 0)},
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
                 },
                 {}});
    c.push_back({"curves", "Curves", EffectCategory::VideoFilter, "Color",
                 {pct("mix", "Mix", 0, 100, 100)},
                 {str("master", "Master (Y)", StringKind::Curve, "0,0 1,1"),
                  str("red", "Red", StringKind::Curve, "0,0 1,1"),
                  str("green", "Green", StringKind::Curve, "0,0 1,1"),
                  str("blue", "Blue", StringKind::Curve, "0,0 1,1")}});
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
    c.push_back({"luma_key", "Luma Key", EffectCategory::VideoFilter, "Keying",
                 {num("threshold", "Threshold", 0, 1, 0.1), num("softness", "Softness", 0, 1, 0.05),
                  boolean("invert", "Key Out Brights")},
                 {}});
    c.push_back({"gaussian_blur", "Gaussian Blur", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {num("radius", "Radius (px)", 0, 250, 10, 0.5),
                  choice("direction", "Direction", {"Both", "Horizontal", "Vertical"})},
                 {}});
    c.push_back({"sharpen", "Sharpen", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {num("amount", "Amount", 0, 5, 1), num("radius", "Radius (px)", 0.5, 20, 1.5, 0.1)},
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
    c.push_back({"parametric_eq", "Parametric EQ", EffectCategory::AudioFilter, "EQ",
                 {num("low_hz", "Low Shelf (Hz)", 20, 1000, 100, 1), num("low_db", "Low Shelf (dB)", -24, 24, 0, 0.1),
                  num("b1_hz", "Band 1 (Hz)", 20, 20000, 250, 1), num("b1_db", "Band 1 (dB)", -24, 24, 0, 0.1),
                  num("b1_q", "Band 1 Q", 0.1, 20, 1), num("b2_hz", "Band 2 (Hz)", 20, 20000, 1000, 1),
                  num("b2_db", "Band 2 (dB)", -24, 24, 0, 0.1), num("b2_q", "Band 2 Q", 0.1, 20, 1),
                  num("b3_hz", "Band 3 (Hz)", 20, 20000, 4000, 1), num("b3_db", "Band 3 (dB)", -24, 24, 0, 0.1),
                  num("b3_q", "Band 3 Q", 0.1, 20, 1), num("high_hz", "High Shelf (Hz)", 1000, 20000, 10000, 1),
                  num("high_db", "High Shelf (dB)", -24, 24, 0, 0.1), num("output_db", "Output (dB)", -24, 24, 0, 0.1)},
                 {}});
    c.push_back({"deesser", "De-Esser", EffectCategory::AudioFilter, "Dynamics",
                 {num("hz", "Frequency (Hz)", 2000, 12000, 6000, 10), num("threshold_db", "Threshold (dB)", -60, 0, -30, 0.1),
                  num("reduction_db", "Max Reduction (dB)", 0, 24, 10, 0.1)},
                 {}});
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

    // ---- Generators -------------------------------------------------------
    c.push_back({"color", "Color Matte", EffectCategory::Generator, "Generators",
                 {color("color", "Color", 0.1, 0.1, 0.1), pct("alpha", "Alpha", 0, 100, 100)},
                 {}});
    c.push_back({"gradient", "Gradient", EffectCategory::Generator, "Generators",
                 {color("color_a", "Start Color", 0.05, 0.1, 0.3), color("color_b", "End Color", 0.6, 0.2, 0.4),
                  angle("angle", "Angle", 90), choice("shape", "Shape", {"Linear", "Radial"})},
                 {}});
    c.push_back({"bars", "Color Bars", EffectCategory::Generator, "Generators", {}, {}});
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
                         },
                         {str("text", "Text", StringKind::MultilineText, "Title"),
                          str("font", "Font", StringKind::Font, "Sans Serif")}};
        c.push_back(title);
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
    c.push_back({"crossfade", "Crossfade (Equal Power)", EffectCategory::AudioTransition, "Crossfade", {}, {}});
    c.push_back({"crossfade_linear", "Crossfade (Constant Gain)", EffectCategory::AudioTransition, "Crossfade", {}, {}});

    return c;
}

}  // namespace

const EffectInfo& maskInfo() {
    static const EffectInfo info = [] {
        EffectInfo m{"mask", "Mask", EffectCategory::Fixed, "Mask", {}, {}, true};
        m.params = {choice("mask.shape", "Shape", {"None", "Ellipse", "Rectangle", "Object"}, 0),
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
                    boolean("mask.show", "Show Mask")};
        return m;
    }();
    return info;
}

bool supportsMask(const std::string& effectType) {
    const EffectInfo* info = findEffectInfo(effectType);
    return info && info->category == EffectCategory::VideoFilter;
}

bool hasMask(const Effect& e, FrameTime t) { return e.p("mask.shape", t) > 0.5 || e.p("mask.qualify", t) > 0.5; }

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

Effect makeEffect(const std::string& type, Id id) {
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
