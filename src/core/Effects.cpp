#include "Effects.h"

#include <algorithm>
#include <cctype>
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
                  choice("sampling", "Frame Sampling", {"Nearest Frame", "Frame Blending", "Optical Flow", "AI Frames (RIFE)"}, 0),
                  // Sound played faster or slower keeps its pitch (audio/TimeStretch) rather than rising or falling.
                  boolean("maintain_pitch", "Maintain Audio Pitch", false)},
                 {}});
    c.push_back({"volume", "Volume", EffectCategory::Fixed, "Fixed",
                 {num("gain_db", "Gain (dB)", -60, 24, 0, 0.1), num("pan", "Pan", -1, 1, 0, 0.01)},
                 {}});

    // ---- Video filters ----------------------------------------------------
    // A colour chart's match (render/ColorChart.h): a matrix and offset in linear light, from Clip › Match Colour Chart (its
    // string "space": the colour space of the picture it was measured in).
    c.push_back({"chart_match", "Chart Match", EffectCategory::VideoFilter, "Color",
                 {num("amount", "Amount (%)", 0, 100, 100, 1), num("rr", "Red from Red", -16, 16, 1, 0.0001), num("rg", "Red from Green", -16, 16, 0, 0.0001),
                  num("rb", "Red from Blue", -16, 16, 0, 0.0001), num("gr", "Green from Red", -16, 16, 0, 0.0001), num("gg", "Green from Green", -16, 16, 1, 0.0001),
                  num("gb", "Green from Blue", -16, 16, 0, 0.0001), num("br", "Blue from Red", -16, 16, 0, 0.0001), num("bg", "Blue from Green", -16, 16, 0, 0.0001),
                  num("bb", "Blue from Blue", -16, 16, 1, 0.0001), num("offset_r", "Red Offset", -1, 1, 0, 0.00001), num("offset_g", "Green Offset", -1, 1, 0, 0.00001),
                  num("offset_b", "Blue Offset", -1, 1, 0, 0.00001)},
                 {}});
    // A grade node's own settings (core/GradeGraph.h): how much of the node shows, its key input's part in its matte,
    // and (as every video filter) its window, qualifier and depth qualifier, which limit the node as a whole.
    c.push_back({"grade_node", "Node", EffectCategory::VideoFilter, "Color",
                 {pct("mix", "Key Output Gain (Mix)", 0, 100, 100), pct("key_gain", "Key Input Gain", 0, 100, 100),
                  boolean("key_invert", "Invert Key Input"),
                  choice("key_mode", "Key Input", {"Multiply with the node's matte", "Add to it", "Subtract from it", "Replace it"}, 0)},
                 {}});
    c.back().hidden = true;
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
                     // Printer lights, as a timer sets them on a film printer: each channel's exposure in points of
                     // `printer_point` log exposure (0.025 by default, about a twelfth of a stop), more light more colour.
                     num("printer_r", "Printer Light R (points)", -100, 100, 0, 1),
                     num("printer_g", "Printer Light G (points)", -100, 100, 0, 1),
                     num("printer_b", "Printer Light B (points)", -100, 100, 0, 1),
                     num("printer_point", "Printer Point (log E)", 0.005, 0.1, 0.025, 0.001),
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
    // Colour Warper (Resolve's): a hue-by-saturation mesh whose points pull colours to others (core/ColorWarp.h).
    c.push_back({"color_warper", "Colour Warper", EffectCategory::VideoFilter, "Color",
                 {pct("mix", "Mix", 0, 100, 100), boolean("preserve_luma", "Preserve Luminance")},
                 {str("mesh", "Hue / Saturation", StringKind::ColorWarp, "")}});
    // HDR Palette (Resolve's HDR wheels): exposure, colour and saturation by zones of brightness measured in stops from
    // mid grey, in the sequence's light (SDR or HDR alike); each zone reaches below (the dark three) or above (the bright
    // three) its range with a soft falloff.
    {
        std::vector<ParamInfo> ps{num("exposure", "Global Exposure (stops)", -4, 4, 0),
                                  num("saturation", "Global Saturation", 0, 2, 1),
                                  num("global_r", "Global R", -1, 1, 0, 0.005),
                                  num("global_g", "Global G", -1, 1, 0, 0.005),
                                  num("global_b", "Global B", -1, 1, 0, 0.005),
                                  num("contrast", "Contrast", 0.25, 3, 1),
                                  num("pivot", "Contrast Pivot (stops)", -4, 4, 0),
                                  num("black_offset", "Black Offset", -0.05, 0.05, 0, 0.0005),
                                  pct("mix", "Mix", 0, 100, 100)};
        for (const HdrZone& z : hdrZones()) {
            const std::string n = z.name, l = z.label;
            ps.push_back(num(n + "_exposure", l + " Exposure (stops)", -4, 4, 0));
            ps.push_back(num(n + "_saturation", l + " Saturation", 0, 2, 1));
            for (const char* ch : {"r", "g", "b"})
                ps.push_back(num(n + "_" + ch, l + " " + char(std::toupper(ch[0])), -1, 1, 0, 0.005));
            ps.push_back(num(n + "_range", l + (z.dark ? " Up To (stops)" : " From (stops)"), -8, 8, z.range, 0.05));
            ps.push_back(num(n + "_falloff", l + " Falloff (stops)", 0.25, 6, z.falloff, 0.05));
        }
        c.push_back({"hdr_palette", "HDR Palette", EffectCategory::VideoFilter, "Color", ps, {}});
    }
    c.push_back({"hue_sat", "Hue / Saturation / Lightness", EffectCategory::VideoFilter, "Color",
                 {angle("hue", "Hue Shift"), num("saturation", "Saturation", 0, 3, 1),
                  num("lightness", "Lightness", -1, 1, 0), num("vibrance", "Vibrance", -1, 1, 0)},
                 {}});
    {
        // RGB Mixer: each output channel a mix of the three input channels (a 3x3 matrix on the values as they are),
        // for crosstalk between channels as film and some looks have it.
        EffectInfo mixer{"rgb_mixer", "RGB Mixer", EffectCategory::VideoFilter, "Color", {}, {}};
        const char* names[3] = {"Red", "Green", "Blue"};
        for (int o = 0; o < 3; ++o)
            for (int i = 0; i < 3; ++i)
                mixer.params.push_back(num(std::string(1, "rgb"[o]) + "." + "rgb"[i], std::string(names[o]) + " Output: " + names[i], -2, 3,
                                           o == i ? 1 : 0, 0.001));
        c.push_back(mixer);
    }
    {
        // A .cube LUT or a Common LUT Format file (render/Clf.h, as ACES looks come), applied in the working space or
        // in the space it was made for (a camera log, ACEScct, ACES2065-1).
        std::vector<std::string> spaces{"Working space"};
        for (const auto& cs : colorSpaces()) spaces.push_back(cs.label);
        c.push_back({"lut", "LUT (.cube, .clf)", EffectCategory::VideoFilter, "Color",
                     {pct("strength", "Strength", 0, 100, 100)},
                     {str("path", "LUT File", StringKind::File), str("space", "Apply In", StringKind::Choice, "Working space")}});
        c.back().strings[0].fileFilter = "LUT files (*.cube *.clf *.ctf)";
        c.back().strings[1].choices = spaces;
    }
    {
        // ASC CDL (core/Cdl.h): slope, offset and power per channel and saturation, as dailies carry it, applied in
        // the working space or in the space it was made in (a camera log, ACEScct).
        EffectInfo cdl{"cdl", "ASC CDL", EffectCategory::VideoFilter, "Color", {}, {}};
        for (const char* part : {"slope", "offset", "power"})
            for (const char* ch : {"r", "g", "b"}) {
                const std::string P = part, C = ch;
                const std::string label = std::string(1, char(std::toupper(P[0]))) + P.substr(1) + " " + char(std::toupper(C[0]));
                if (P == "offset") cdl.params.push_back(num(P + "." + C, label, -1, 1, 0, 0.001));
                else cdl.params.push_back(num(P + "." + C, label, P == "power" ? 0.1 : 0, 4, 1, 0.001));
            }
        cdl.params.push_back(num("saturation", "Saturation", 0, 4, 1, 0.001));
        std::vector<std::string> spaces{"Working space"};
        for (const auto& cs : colorSpaces()) spaces.push_back(cs.label);
        cdl.strings = {str("space", "Apply In", StringKind::Choice, "Working space"), str("id", "CDL ID", StringKind::Text),
                       str("description", "Description", StringKind::Text)};
        cdl.strings[0].choices = spaces;
        c.push_back(cdl);
    }
    // ACES Reference Gamut Compression (render/Gamut.h): colours a camera saw outside AP1 brought smoothly inside it. On a
    // clip of media it works on the media's colours as they come, before the input transform clips them to the working
    // gamut; elsewhere (adjustment layers, nodes, LUT export) in the working space.
    c.push_back({"aces_gamut_compress", "ACES Gamut Compress", EffectCategory::VideoFilter, "Color",
                 {choice("direction", "Direction", {"Forward", "Inverse"}, 0)},
                 {}});
    {
        // Hue Vectors (render/ColorSlice.h; as Resolve's ColorSlice): hue shift, saturation and density by hue vector in a
        // log encoding, saturation subtractive as film's or additive (brightness kept), highlights protected.
        EffectInfo slice{"color_slice", "Hue Vectors", EffectCategory::VideoFilter, "Color",
                         {num("density", "Density", -1, 1, 0), pct("depth", "Depth", 0, 200, 100), num("saturation", "Saturation", 0, 2, 1),
                          boolean("subtractive", "Subtractive Saturation", true), pct("protect_highlights", "Protect Highlights", 0, 100, 50),
                          pct("mix", "Mix", 0, 100, 100)},
                         {}};
        auto vector = [&](const std::string& n, const std::string& l) {
            slice.params.push_back(num(n + "_hue", l + " Hue (°)", -60, 60, 0, 0.5));
            slice.params.push_back(num(n + "_saturation", l + " Saturation", 0, 2, 1));
            slice.params.push_back(num(n + "_density", l + " Density", -1, 1, 0));
        };
        for (const char* v : {"red", "yellow", "green", "cyan", "blue", "magenta"}) {
            std::string l = v;
            l[0] = char(std::toupper(l[0]));
            vector(v, l);
        }
        vector("skin", "Skin");
        slice.params.push_back(num("skin_centre", "Skin Centre (°)", 0, 90, 25, 0.5));
        slice.params.push_back(num("skin_width", "Skin Width (°)", 5, 60, 25, 0.5));
        c.push_back(slice);
    }
    {
        // Colour Space Transform: one clip from any space into another (render/ColorSpace.h).
        std::vector<std::string> spaces;
        for (const auto& cs : colorSpaces()) spaces.push_back(cs.label);
        EffectInfo cst{"color_space_transform", "Colour Space Transform", EffectCategory::VideoFilter, "Color", {},
                       {str("from", "From", StringKind::Choice, colorSpaces().front().label),
                        str("to", "To", StringKind::Choice, colorSpaces().front().label),
                        str("rendering", "Rendering", StringKind::Choice, "Display Rendering")}};
        cst.strings[0].choices = cst.strings[1].choices = spaces;
        // Display Rendering: camera footage into a display space looks as it would on screen (convertColor). None: the
        // code values converted exactly and nothing more, as a LUT's "Apply In" goes into its space and back.
        cst.strings[2].choices = {"Display Rendering", "None"};
        c.push_back(cst);
    }
    // Stabilize: the analysed camera path (strings["motion"], written by the
    // Inspector's Analyze) smoothed; render/Processing.cpp moves each frame.
    // Camera Gyro turns each frame by the camera's own motion data instead
    // (strings["gyro"], media/MotionData.h), with the horizon levelled from gravity.
    c.push_back({"stabilize", "Stabilize", EffectCategory::VideoFilter, "Transform",
                 {num("smoothness", "Smoothness (s)", 0, 10, 1.5, 0.1),
                  choice("method", "Method", {"Position", "Position & Scale", "Position, Scale & Rotation", "Camera Gyro"}, 2),
                  choice("framing", "Framing", {"Zoom to Fill", "Show Edges"}, 0), pct("extra_zoom", "Extra Zoom", 0, 50, 0),
                  pct("rolling_shutter", "Rolling Shutter", 0, 100, 0), pct("horizon", "Horizon Lock (Camera Gyro)", 0, 100, 0),
                  num("gyro_offset", "Gyro Offset (ms)", -500, 500, 0, 1)},
                 {}});
    for (auto& pi : c.back().params) pi.keyframeable = false;
    // 3D Camera Tracker (Fusion's Camera Tracker, After Effects' 3D Camera Tracker): the camera that filmed the clip,
    // solved from its footage (media/CameraSolve.h) and kept in strings["solve"]; the picture is untouched. Its tracked
    // points show in the viewer, and titles are placed in the scene from the Inspector (render/ClipAnalysis.h).
    c.push_back({"camera_track", "3D Camera Tracker", EffectCategory::VideoFilter, "Transform",
                 {boolean("show_points", "Show Tracked Points", true), pct("place_x", "Place At X", 0, 100, 50),
                  pct("place_y", "Place At Y", 0, 100, 70), pct("place_size", "Title Width (% of its distance)", 1, 300, 40),
                  boolean("place_upright", "Stand Upright"), num("place_turn", "Turn (degrees)", -180, 180, 0, 1)},
                 {}});
    for (auto& pi : c.back().params) pi.keyframeable = false;
    // Rolling Shutter Repair (Premiere's, Final Cut's): the same analysis; each row is moved back to where the
    // picture was mid-frame, from the camera's speed and how long the sensor takes to read out (a share of a frame).
    c.push_back({"rolling_shutter", "Rolling Shutter Repair", EffectCategory::VideoFilter, "Transform",
                 {pct("readout", "Readout (% of a frame)", 0, 100, 50), choice("framing", "Framing", {"Zoom to Fill", "Show Edges"}, 0)},
                 {}});
    for (auto& pi : c.back().params) pi.keyframeable = false;
    // Reframe 360° (GoPro's Reframe, Insta360 Studio's keyframes, Premiere's VR Projection): a flat view out of
    // equirectangular 360° footage, aimed and zoomed with keys, or the whole sphere as a little planet or a tunnel.
    // The clip takes the sequence's shape (render/Spherical.h).
    c.push_back({"reframe_360", "Reframe 360°", EffectCategory::VideoFilter, "Transform",
                 {num("yaw", "Pan (°)", -360, 360, 0, 0.1), num("pitch", "Tilt (°)", -90, 90, 0, 0.1),
                  num("roll", "Roll (°)", -180, 180, 0, 0.1), num("fov", "Field of View (°)", 20, 330, 100, 0.1),
                  choice("projection", "Projection", {"Flat", "Little Planet", "Tunnel"}, 0)},
                 {}});
    // Stereo 3D (Premiere's and Resolve's stereoscopic convergence): in a stereoscopic sequence, a clip's place in
    // depth, by moving its eyes apart (positive into the screen, negative out of it, as a % of the width), and its eyes
    // swapped when stereo footage was shot or packed the other way round (render/Stereo.h).
    c.push_back({"stereo_3d", "Stereo 3D", EffectCategory::VideoFilter, "Transform",
                 {num("depth", "Depth (% of width)", -10, 10, 0, 0.05), choice("swap_eyes", "Eyes", {"As Shot", "Swapped"}, 0)},
                 {}});
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
    // A screen keyer in the manner of Keylight and Ultra Key: how much the screen's colour dominates a pixel, against the
    // screen's own dominance, makes the matte (cleaner edges and hair than a colour distance); then matte levels,
    // shrink or grow, soften, despill with the lost brightness put back, and the edges desaturated.
    c.push_back({"screen_key", "Keyer (Green / Blue Screen)", EffectCategory::VideoFilter, "Keying",
                 {color("key", "Screen Colour", 0.1, 0.75, 0.2), num("balance", "Screen Balance", 0, 1, 0.5),
                  num("gain", "Screen Gain", 0.5, 2, 1.1), num("clip_black", "Clip Black", 0, 1, 0.05), num("clip_white", "Clip White", 0, 1, 1),
                  num("shrink", "Shrink / Grow (px)", -10, 10, 0, 0.5), num("soften", "Soften (px)", 0, 20, 0, 0.5),
                  pct("despill", "Despill", 0, 100, 100), pct("restore", "Restore Brightness", 0, 100, 50),
                  pct("edge_desat", "Edge Desaturate", 0, 100, 0), choice("view", "View", {"Composite", "Matte", "Status"}, 0)},
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
    // Deep compositing (media/DeepImage.h; handled when tracks are composited): deep OpenEXR clips, and pictures made
    // deep, are merged by depth whatever their tracks' order and flattened in where the lowest of them is.
    c.push_back({"make_deep", "Make Deep from Depth Map", EffectCategory::VideoFilter, "Deep",
                 {choice("source", "Depth", {"Depth Map Track", "Estimated from the Picture", "Constant"}, 0),
                  num("track", "Depth Map Track (V number, 0: the one above)", 0, 99, 0, 1), boolean("hide", "Hide the Depth Map", true),
                  choice("map", "Depth Map", {"White Is Near", "White Is Far", "Values Are Depth"}, 0),
                  num("near", "Nearest Depth", 0, 100000, 1), num("far", "Farthest Depth", 0, 100000, 10),
                  num("depth", "Constant Depth", 0, 100000, 5), num("thickness", "Thickness", 0, 100000, 0)},
                 {}});
    c.push_back({"deep_holdout", "Hold Out by Depth", EffectCategory::VideoFilter, "Deep",
                 {num("track", "Holdout Track (V number, 0: the one above)", 0, 99, 0, 1), boolean("hide", "Hide the Holdout")},
                 {}});
    c.push_back({"deep_grade", "Colour Correct by Depth", EffectCategory::VideoFilter, "Deep",
                 {num("near", "From Depth", 0, 100000, 0), num("far", "To Depth", 0, 100000, 10), num("soft", "Softness", 0, 100000, 0),
                  num("exposure", "Exposure (stops)", -8, 8, 0), color("gain", "Gain", 1, 1, 1), pct("saturation", "Saturation", 0, 200, 100)},
                 {}});
    c.push_back({"deep_slice", "Depth Slice", EffectCategory::VideoFilter, "Deep",
                 {num("near", "From Depth", 0, 100000, 0), num("far", "To Depth", 0, 100000, 10)},
                 {}});
    c.push_back({"flatten_deep", "Flatten Deep", EffectCategory::VideoFilter, "Deep", {}, {}});
    // The sky found, keyed and replaced by another track's picture or a gradient (handled when tracks are composited,
    // render/SkyReplace.h). Its strings["motion"] holds the camera motion an analysis measured, to move the sky with.
    c.push_back({"sky_replace", "Sky Replacement", EffectCategory::VideoFilter, "Keying",
                 {choice("source", "Sky", {"Track", "Gradient"}, 0), num("track", "Sky Track (V number, 0: the one above)", 0, 99, 0, 1),
                  boolean("hide", "Hide the Sky Track", true), color("zenith", "Gradient Top", 0.16, 0.36, 0.72),
                  color("horizon", "Gradient Horizon", 0.72, 0.82, 0.92), pct("sensitivity", "Sensitivity", 0, 100, 50),
                  pct("lowest", "Lowest Sky (% of height)", 5, 100, 80), boolean("use_depth", "Use Depth", true),
                  pct("refine", "Refine Edges", 0, 100, 70), num("shift", "Edge Shift (px)", -20, 20, 0, 0.5),
                  num("soften", "Soften (px)", 0, 40, 1, 0.5), boolean("steady", "Steady Through the Shot", true),
                  choice("follow", "Follow Camera", {"Off", "Analysed Camera Motion", "3D Camera Track"}, 1),
                  pct("follow_amount", "Follow Amount", 0, 200, 100), num("sky_exposure", "Sky Exposure (stops)", -4, 4, 0),
                  num("sky_blur", "Sky Defocus (px)", 0, 50, 0, 0.5), pct("match", "Match Foreground", 0, 100, 30),
                  pct("wrap", "Light Wrap", 0, 100, 20), num("wrap_width", "Light Wrap Width (px)", 1, 100, 20, 0.5),
                  choice("view", "View", {"Composite", "Sky Matte"}, 0)},
                 {}});
    c.push_back({"luma_key", "Luma Key", EffectCategory::VideoFilter, "Keying",
                 {num("threshold", "Threshold", 0, 1, 0.1), num("softness", "Softness", 0, 1, 0.05),
                  boolean("invert", "Key Out Brights")},
                 {}});
    // What is under its mask (a shape, a picked object, people) painted out and filled (LaMa).
    c.push_back({"object_removal", "Object Removal", EffectCategory::VideoFilter, "Refine",
                 {num("grow", "Grow Mask (px)", 0, 50, 4, 0.5)},
                 {}});
    // A network trained on frames worked on by hand (media/LearnedEffect.h; Clip › Learn Effect from Examples), run on
    // the picture: what was done to those frames, done to these.
    c.push_back({"learned_effect", "Learned Effect", EffectCategory::VideoFilter, "Refine",
                 {pct("amount", "Amount", 0, 100, 100)},
                 {str("network", "Network File", StringKind::File)}});
    c.back().strings[0].fileFilter = "Learned effects (*.mlnet)";
    // The faces in the frame (YuNet, found again each frame) touched up within a soft mask.
    c.push_back({"face_refine", "Face Refinement", EffectCategory::VideoFilter, "Refine",
                 {pct("smooth", "Smooth Skin", 0, 100, 40), pct("lighten", "Lighten Face", 0, 100, 0),
                  pct("eyes_bright", "Brighten Eyes", 0, 100, 20), pct("eyes_sharp", "Sharpen Eyes", 0, 100, 30),
                  boolean("show", "Show Face Mask")},
                 {}});
    // Faces reshaped by moving the face mesh's points (render/FaceShape.h).
    c.push_back({"face_shape", "Face Shape", EffectCategory::VideoFilter, "Refine",
                 {pct("face_width", "Face Width", -100, 100, 0), pct("jaw", "Jaw", -100, 100, 0), pct("chin", "Chin Length", -100, 100, 0),
                  pct("forehead", "Forehead Height", -100, 100, 0), pct("nose_width", "Nose Width", -100, 100, 0),
                  pct("nose_length", "Nose Length", -100, 100, 0), pct("eye_size", "Eye Size", -100, 100, 0),
                  pct("eye_distance", "Eye Distance", -100, 100, 0), pct("mouth_width", "Mouth Width", -100, 100, 0),
                  pct("mouth_height", "Lip Height", -100, 100, 0), boolean("show", "Show Face Mesh")},
                 {}});
    // Each open eye's iris turned to the lens, for a presenter reading a script near it (render/FaceShape.h).
    c.push_back({"look_at_lens", "Look at Lens", EffectCategory::VideoFilter, "Refine",
                 {pct("amount", "Amount", 0, 100, 100), num("limit", "Largest Gaze Corrected (degrees)", 5, 40, 20, 1),
                  boolean("show", "Show Irises Found")},
                 {}});
    // The largest face's mouth and jaw drawn again to speak an audio track's sound (a dub), with MuseTalk (media/LipSync.h).
    {
        StringParamInfo sound = str("track", "Sound From", StringKind::Track);
        sound.choices = {"Every Track"};  // (shown for "": every audio track)
        c.push_back({"lip_sync", "Lip Sync", EffectCategory::VideoFilter, "Refine", {pct("amount", "Amount", 0, 100, 100)}, {sound}});
    }
    // Spots, pimples and small marks on faces found and healed from the skin round them.
    c.push_back({"blemish_remover", "Blemish Remover", EffectCategory::VideoFilter, "Refine",
                 {pct("amount", "Amount", 0, 100, 100), pct("sensitivity", "Sensitivity", 0, 100, 50),
                  pct("size", "Largest Spot (% of face)", 1, 15, 6), boolean("show", "Show Spots Found")},
                 {}});
    // Faces hidden: every face found in the clip (or everyone but the people left showing in Clip > Redact Faces,
    // which follows them from frame to frame) blurred, pixelated or covered.
    c.push_back({"redact_faces", "Redact Faces", EffectCategory::VideoFilter, "Privacy",
                 {choice("style", "Style", {"Blur", "Pixelate", "Solid"}), pct("strength", "Strength", 0, 100, 70),
                  choice("shape", "Shape", {"Ellipse", "Rectangle"}), pct("expand", "Expand", 0, 150, 30),
                  pct("feather", "Feather", 0, 100, 15), num("hold", "Hold Lost Faces (frames)", 0, 60, 12, 1),
                  color("color", "Solid Colour", 0, 0, 0), boolean("show", "Show Tracked Faces")},
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
    // Local Contrast (render/ColorSlice.h; as Resolve's Mid-tone Detail and Texture Pop): local contrast in brightness only,
    // mid-tone contrast through an edge-preserving filter, fine texture above a noise floor; Detail Scale scales both.
    c.push_back({"texture_pop", "Local Contrast", EffectCategory::VideoFilter, "Color",
                 {pct("midtone_detail", "Mid-tone Contrast", -100, 100, 0), pct("texture_pop", "Fine Texture", 0, 200, 0),
                  num("scale", "Detail Scale", 0.25, 4, 1), pct("noise_floor", "Noise Floor", 0, 100, 10)},
                 {}});
    c.push_back({"sharpen", "Sharpen", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {num("amount", "Amount", 0, 5, 1), num("radius", "Radius (px)", 0.5, 20, 1.5, 0.1)},
                 {}});
    // Undoes a slight focus miss or lens softness by deconvolution (render/Deconvolve).
    c.push_back({"channel_blur", "Channel Blur", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {num("red", "Red Blurriness", 0, 200, 0, 0.5), num("green", "Green Blurriness", 0, 200, 0, 0.5),
                  num("blue", "Blue Blurriness", 0, 200, 0, 0.5), num("alpha", "Alpha Blurriness", 0, 200, 0, 0.5),
                  choice("dimensions", "Blur Dimensions", {"Horizontal and Vertical", "Horizontal", "Vertical"}, 0)},
                 {}});
    c.push_back({"noise", "Noise", EffectCategory::VideoFilter, "Stylize",
                 {pct("amount", "Amount of Noise", 0, 100, 20), boolean("color", "Color Noise", true), boolean("clip", "Clip Result Values", true)},
                 {}});
    c.push_back({"focus_repair", "Focus Repair", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {num("blur", "Blur to Undo (px)", 0.3, 8, 1.5, 0.1), num("iterations", "Iterations", 1, 100, 20, 1),
                  pct("strength", "Strength", 0, 100, 100), num("noise", "Noise Threshold (%)", 0, 10, 1, 0.1)},
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
    // NAFNet on the source frames: the smear of camera or subject motion taken out (media/MotionDeblur.h).
    c.push_back({"motion_deblur", "Motion Deblur", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {pct("strength", "Strength", 0, 100, 100)},
                 {}});
    // Depth (Depth Anything V2 on the clip's source frame): 0 is the farthest part of the picture, 100 the nearest.
    // Lens Blur (render/LensBlur.h): focus on a depth, on a point (keys rack between the points' depths) or on the
    // nearest face; the blur is its radius at f/2.8, scaled by the F-Stop.
    c.push_back({"depth_blur", "Lens Blur (Depth)", EffectCategory::VideoFilter, "Depth",
                 {choice("follow", "Focus On", {"Focus Depth", "Focus Point", "Nearest Face"}, 0),
                  pct("focus", "Focus Depth", 0, 100, 80), num("point_x", "Focus Point X", 0, 1, 0.5, 0.005),
                  num("point_y", "Focus Point Y", 0, 1, 0.5, 0.005), pct("range", "In Focus Range", 0, 100, 10),
                  pct("falloff", "Falloff", 1, 100, 30), num("radius", "Blur (px)", 0, 100, 12, 0.5),
                  num("fstop", "F-Stop", 0.7, 22, 2.8, 0.1), boolean("near", "Blur Nearer Than Focus", true),
                  num("blades", "Aperture Blades (0 Round, 3 to 12)", 0, 12, 0, 1), angle("blade_rotation", "Blade Rotation", 0),
                  num("squeeze", "Anamorphic Squeeze", 1, 2, 1, 0.05), pct("cat_eye", "Cat's Eye Toward the Edges", 0, 100, 0),
                  pct("highlights", "Bokeh Highlights", 0, 100, 0), pct("threshold", "Highlight Threshold", 5, 100, 80),
                  pct("breathing", "Focus Breathing", 0, 100, 0)},
                 {}});
    for (auto& pi : c.back().params)
        if (pi.name == "blades") pi.keyframeable = false;
    c.push_back({"depth_fog", "Depth Fog", EffectCategory::VideoFilter, "Depth",
                 {color("color", "Color", 0.78, 0.82, 0.88), pct("amount", "Amount", 0, 100, 70), pct("start", "Starts At Depth", 0, 100, 60),
                  num("curve", "Thickening", 0.2, 5, 1.5, 0.05), pct("height", "Thinning With Height", 0, 100, 0),
                  pct("horizon", "Horizon (From the Top)", 0, 100, 50)},
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
    // A legaliser (Premiere's Video Limiter, Final Cut's Broadcast Safe): see render/QualityCheck.h broadcastSafe.
    c.push_back({"broadcast_safe", "Broadcast Safe", EffectCategory::VideoFilter, "Color",
                 {choice("limits", "Limits", {"EBU R103 (-5 to 105 %)", "Strict (0 to 100 %)"}, 0), pct("knee", "Soft Knee", 0, 20, 5),
                  boolean("highlight", "Highlight Unsafe Areas", false)},
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
    // Grain Match (render/GrainMatch.h): grain made to a measured profile (Clip > Measure Grain, MCP montage_grain) or
    // one of Film Look's gauges, by the picture's own light; Replace Grain takes the plate's own out first (with Video
    // Noise Reduction's defaults on the source frames, unless the clip has that effect itself).
    c.push_back({"grain_match", "Grain Match", EffectCategory::VideoFilter, "Stylize",
                 {choice("source", "Grain", {"Measured", "65mm", "35mm", "16mm", "Super 8"}, 0),
                  choice("mode", "Mode", {"Add Grain", "Replace Grain"}, 0), pct("amount", "Amount", 0, 200, 100),
                  pct("size", "Size", 25, 400, 100), pct("saturation", "Colour", 0, 200, 100), num("seed", "Seed", 0, 9999, 0, 1)},
                 {str("profile", "Measured Profile", StringKind::Text)}});
    c.push_back({"directional_blur", "Directional Blur", EffectCategory::VideoFilter, "Blur & Sharpen",
                 {num("length", "Length (px)", 0, 300, 20, 0.5), angle("angle", "Direction", 0)},
                 {}});
    c.push_back({"chromatic_aberration", "Chromatic Aberration", EffectCategory::VideoFilter, "Stylize",
                 {num("amount", "Amount (px)", -30, 30, 3, 0.1)},
                 {}});
    // Premiere 26.3's magnifying lens: render/VideoFx.h magnify.
    c.push_back({"magnify", "Magnify", EffectCategory::VideoFilter, "Distort",
                 {choice("shape", "Shape", {"Circle", "Square"}, 0), num("center_x", "Center X", -0.5, 1.5, 0.5, 0.001),
                  num("center_y", "Center Y", -0.5, 1.5, 0.5, 0.001), pct("size", "Size (% of height)", 1, 200, 40),
                  pct("magnification", "Magnification", 100, 1000, 200), num("feather", "Feather (px)", 0, 200, 0, 0.5),
                  num("border", "Border (px)", 0, 50, 2, 0.5), color("border_color", "Border Color", 1, 1, 1), pct("opacity", "Opacity", 0, 100, 100)},
                 {}});
    c.push_back({"lens_distortion", "Lens Distortion", EffectCategory::VideoFilter, "Distort",
                 {num("amount", "Barrel / Pincushion", -100, 100, 0, 0.5)},
                 {}});
    // Lens Correction (Premiere's Lens Distortion presets, Resolve's Lens Correction, Lightroom's profiles): a lens's
    // profile from the lensfun database (strings["profile"], its <lens> as written by core/LensProfile lensXml, with
    // strings["camera"] and ["lens"] naming them, chosen in the Correct Lens dialog) undoes its distortion, colour
    // fringes and vignetting and makes a fisheye rectilinear; render/VideoFx.cpp lensCorrection.
    c.push_back({"lens_correction", "Lens Correction", EffectCategory::VideoFilter, "Distort",
                 {num("focal", "Focal Length (mm)", 0, 2000, 0, 0.1), num("aperture", "Aperture (f/)", 0, 64, 0, 0.1),
                  num("distance", "Focus Distance (m)", 0.1, 1000, 1000, 0.1),
                  num("camera_crop", "Camera Crop Factor", 0, 20, 0, 0.001),
                  num("recording_crop", "Recording Crop", 1, 10, 1, 0.01),
                  boolean("distortion", "Distortion", true), boolean("tca", "Chromatic Aberration", true),
                  boolean("vignetting", "Vignetting", true), boolean("rectilinear", "Fisheye to Rectilinear", true),
                  choice("fit", "Framing", {"Fill Frame", "Show Whole Picture", "None"}, 0), pct("scale", "Scale", 25, 400, 100)},
                 {}});
    for (auto& pi : c.back().params) pi.keyframeable = pi.name == "focal" || pi.name == "aperture" || pi.name == "distance" || pi.name == "scale";
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
                  num("makeup_db", "Makeup (dB)", 0, 30, 0, 0.1), num("key_hpf_hz", "Key High-Pass (Hz)", 0, 500, 0, 1)},
                 {str("sidechain", "Sidechain", StringKind::Track)}});
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
    // The missing top of band-limited speech rebuilt (media/BandwidthExtension.h); the cut-off 0 is found.
    c.push_back({"bandwidth_extension", "Bandwidth Extension", EffectCategory::AudioFilter, "Restoration",
                 {pct("amount", "Amount", 0, 100, 100), num("cutoff_hz", "Cut-off (Hz, 0 = auto)", 0, 24000, 0, 50)},
                 {}});
#ifndef MONTAGE_WITH_ONNXRUNTIME
    c.back().hidden = true;  // needs ONNX Runtime
#endif
    c.push_back({"declick", "De-Click", EffectCategory::AudioFilter, "Restoration",
                 {pct("sensitivity", "Sensitivity", 0, 100, 50), num("max_ms", "Longest Click (ms)", 0.1, 10, 2, 0.1)},
                 {}});
    // Dialogue repair on the clip's whole source audio (audio/AudioRepair.h).
    c.push_back({"declip", "De-Clip", EffectCategory::AudioFilter, "Restoration",
                 {num("clip_level_db", "Clip Level (dBFS, 0 = auto)", -30, 0, 0, 0.1)},
                 {}});
    c.push_back({"deplosive", "De-Plosive", EffectCategory::AudioFilter, "Restoration",
                 {pct("sensitivity", "Sensitivity", 0, 100, 50), num("reduction_db", "Max Reduction (dB)", 0, 60, 24, 0.5)},
                 {}});
    c.push_back({"dewind", "De-Wind", EffectCategory::AudioFilter, "Restoration",
                 {pct("amount", "Amount", 0, 100, 100), num("max_reduction_db", "Max Reduction (dB)", 0, 60, 24, 0.5)},
                 {}});
    // On the clip's whole source audio (audio/SpeechCleanup.h dereverb).
    c.push_back({"dereverb", "De-Reverb", EffectCategory::AudioFilter, "Restoration",
                 {pct("amount", "Amount", 0, 100, 80), num("reverb_time", "Room Reverb Time (s, 0 = auto)", 0, 5, 0, 0.05),
                  num("max_reduction_db", "Max Reduction (dB)", 0, 40, 18, 0.5)},
                 {}});
    // Boxes of time and frequency healed or turned down on the clip's whole source audio (audio/SpectralRepair.h),
    // drawn in the Spectral Repair editor (Clip menu) or set over MCP; kept in the "regions" string.
    c.push_back({"spectral_repair", "Spectral Repair", EffectCategory::AudioFilter, "Restoration", {}, {}});
    c.back().hidden = true;
    // A take fitted to the guide's timing (audio/DialogueAlign.h): the "warp" string (result seconds to the file's,
    // for the take named by "media"), made by Align to Guide in the ADR panel or over MCP.
    c.push_back({"dialogue_align", "Dialogue Alignment", EffectCategory::AudioFilter, "Restoration",
                 {num("invert", "Invert Polarity", 0, 1, 0, 1)}, {}});
    c.back().hidden = true;
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
                  num("release_ms", "Release (ms)", 5, 2000, 150, 1), num("key_hpf_hz", "Key High-Pass (Hz)", 0, 500, 0, 1)},
                 {str("sidechain", "Sidechain", StringKind::Track)}});
    // Stereo spread over a surround or ambisonic sequence's speakers (audio/Upmix.h), on a track; elsewhere (a clip, a
    // bus, a stereo sequence) the sound stays stereo, delayed as much as the upmix would have and made up for.
    c.push_back({"upmix", "Upmix", EffectCategory::AudioFilter, "Surround",
                 {pct("centre_width", "Centre Width", 0, 100, 0), pct("surround_depth", "Surround Depth", 0, 100, 50),
                  pct("height", "Height", 0, 100, 30), num("delay_ms", "Surround Delay (ms)", 0, 30, 10, 0.1),
                  boolean("lfe", "Copy Lows to LFE", false), boolean("dialogue_centre", "Dialogue in Centre", true)},
                 {}});
    c.back().params[3].keyframeable = false;
    c.push_back({"reverb", "Reverb", EffectCategory::AudioFilter, "Time",
                 {pct("size", "Room Size", 0, 100, 50), pct("damping", "Damping", 0, 100, 50), pct("width", "Width", 0, 100, 100),
                  pct("mix", "Mix", 0, 100, 25)},
                 {}});
    // A room by convolution (audio/RoomResponse.h): one made to measure from its decay time in each octave, or an
    // impulse-response file (a made room stands in if the file cannot be read). Dialogue Match adds it to put ADR in
    // the production take's room. The room's shape (decays, pre-delay) is built once, not keyed; its levels can be.
    c.push_back({"convolution_reverb", "Convolution Reverb", EffectCategory::AudioFilter, "Time",
                 {num("wet_db", "Room Level (dB)", -60, 12, -12, 0.1), num("dry_db", "Direct Level (dB)", -60, 12, 0, 0.1),
                  num("predelay_ms", "Pre-Delay (ms)", 0, 200, 12, 1), num("rt_125", "Decay at 125 Hz (s)", 0.1, 10, 0.6, 0.01),
                  num("rt_250", "Decay at 250 Hz (s)", 0.1, 10, 0.6, 0.01), num("rt_500", "Decay at 500 Hz (s)", 0.1, 10, 0.6, 0.01),
                  num("rt_1k", "Decay at 1 kHz (s)", 0.1, 10, 0.6, 0.01), num("rt_2k", "Decay at 2 kHz (s)", 0.1, 10, 0.6, 0.01),
                  num("rt_4k", "Decay at 4 kHz (s)", 0.1, 10, 0.5, 0.01), num("rt_8k", "Decay at 8 kHz (s)", 0.1, 10, 0.4, 0.01)},
                 {str("ir", "Impulse Response", StringKind::File)}});
    c.back().strings[0].fileFilter = "Impulse responses (*.wav *.aif *.aiff *.flac);;All files (*)";
    for (auto& pi : c.back().params)
        if (pi.name != "wet_db" && pi.name != "dry_db") pi.keyframeable = false;
    c.push_back({"channels", "Channel Tools", EffectCategory::AudioFilter, "Channels",
                 {choice("mode", "Channels", {"Stereo", "Mono (Sum)", "Left to Both", "Right to Both", "Swap Left and Right"}, 0),
                  boolean("invert_l", "Invert Left Polarity"), boolean("invert_r", "Invert Right Polarity")},
                 {}});
    // Ambisonic sound (core/Ambisonics.h): the field turned (as a view of it would turn), following the linked
    // picture's Reframe 360° view while asked, and heard binaurally (headphones) or as stereo (speakers).
    c.push_back({"ambisonics", "Ambisonics", EffectCategory::AudioFilter, "Channels",
                 {num("yaw", "Turn (degrees right)", -180, 180, 0, 0.5), num("pitch", "Tilt (degrees up)", -90, 90, 0, 0.5),
                  num("roll", "Roll (degrees clockwise)", -180, 180, 0, 0.5),
                  choice("follow_view", "Follow the 360° View", {"Off", "On"}, 1),
                  choice("decode", "Heard As", {"Binaural (headphones)", "Stereo (speakers)"}, 0)},
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
        // An ARA plug-in on the clip's whole sound (audio/AraHost.h): the musical context it is given.
        EffectInfo ara{"ara", "ARA Plug-in", EffectCategory::AudioFilter, "Plugins",
                       {num("tempo_bpm", "Tempo (BPM)", 10, 1000, 120, 0.01), num("beats_per_bar", "Beats per Bar", 1, 32, 4, 1)},
                       {}};
        ara.hidden = true;
        for (ParamInfo& pi : ara.params) pi.keyframeable = false;
        c.push_back(ara);
    }
    {
        // An OpenFX video plugin; see render/Ofx.h.
        EffectInfo ofx{"ofx", "OpenFX Plugin", EffectCategory::VideoFilter, "OpenFX", {}, {}};
        ofx.hidden = true;
        c.push_back(ofx);
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
        c.push_back({"lens_flare", "Lens Flare", EffectCategory::VideoFilter, "Stylize",
                     {num("pos_x", "Light X", -0.5, 1.5, 0.3, 0.001), num("pos_y", "Light Y", -0.5, 1.5, 0.3, 0.001),
                      pct("brightness", "Brightness", 0, 300, 100), choice("lens", "Lens", {"50-300mm Zoom", "35mm Prime", "105mm Prime", "Anamorphic"}, 0),
                      color("color", "Colour", 1, 0.9, 0.75), pct("ghosts", "Ghosts", 0, 200, 100)},
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
        // Restoration (render/Restoration.h). Dirt Removal and Dust Spot Removal use the frames either side, so they run where
        // the source frame is read; Dust Spot Removal's spots and Paint's strokes are kept as text ("spots", "strokes"), marked
        // on the Program monitor. Patch Fill copies its mask's area from the same shape moved by Source X and Y.
        c.push_back({"dirt_removal", "Dirt Removal", EffectCategory::VideoFilter, "Repair",
                     {pct("sensitivity", "Sensitivity", 0, 100, 50), num("size", "Largest Speck (px)", 1, 100, 12, 1),
                      boolean("show", "Show Detected")},
                     {}});
        c.push_back({"dust_spots", "Dust Spot Removal", EffectCategory::VideoFilter, "Repair",
                     {pct("sensitivity", "Sensitivity", 0, 100, 50), num("brush", "Brush Size (px)", 2, 400, 24, 1),
                      boolean("show", "Show Detected")},
                     {}});
        c.push_back({"scratch_removal", "Scratch Removal", EffectCategory::VideoFilter, "Repair",
                     {pct("sensitivity", "Sensitivity", 0, 100, 50), num("width", "Widest Scratch (px)", 1, 32, 4, 1),
                      choice("polarity", "Scratches", {"Dark and Light", "Dark", "Light"}, 0),
                      boolean("follow", "Follow Slanting Scratches", true), boolean("show", "Show Detected")},
                     {}});
        c.push_back({"patch_fill", "Patch Fill", EffectCategory::VideoFilter, "Repair",
                     {num("source_x", "Source X (of the width)", -1, 1, 0.15, 0.001), num("source_y", "Source Y (of the height)", -1, 1, 0, 0.001),
                      choice("blend", "Blend", {"Seamless", "Copy"}, 0)},
                     {}});
        c.push_back({"paint", "Paint (Clone and Heal)", EffectCategory::VideoFilter, "Repair",
                     {choice("tool", "Brush", {"Clone", "Heal"}, 0), num("brush", "Brush Size (px)", 2, 400, 30, 1),
                      pct("softness", "Brush Softness", 0, 100, 50), choice("span", "Strokes Last", {"This Frame", "Whole Clip"}, 0),
                      boolean("track", "Strokes Follow the Mask Track"), num("source_x", "Source X (of the width)", -1, 1, 0.05, 0.001),
                      num("source_y", "Source Y (of the height)", -1, 1, 0, 0.001)},
                     {}});
        for (size_t i = c.size() - 5; i < c.size(); ++i)
            for (auto& pi : c[i].params)
                if (pi.name != "sensitivity")
                    pi.keyframeable = c[i].type == "patch_fill" && (pi.name == "source_x" || pi.name == "source_y");
        // Frame Repair (render/FrameRepair.h): the source frames listed as damaged ("frames" as text, from Clip › Repair
        // Damaged Frames… or MCP) rebuilt where the source frame is read, before anything else runs on it.
        c.push_back({"frame_repair", "Frame Repair", EffectCategory::VideoFilter, "Repair", {boolean("show", "Show Repaired")}, {}});
        c.back().params.back().keyframeable = false;
        // Film scans (render/FilmScan.h), run where the source frame is read: Steady on Perforations first (holding the
        // holes where they were on the frame its reference was taken from, "reference" as text), then Film Negative (its
        // base found in each frame's rebate unless set), then Faded Colour Restore (from the frames either side).
        c.push_back({"film_negative", "Film Negative", EffectCategory::VideoFilter, "Film",
                     {choice("scan", "Scan Encoding", {"The Picture's Own Curve", "Linear", "Printing Density (10-bit Log)"}, 0),
                      choice("base_from", "Film Base", {"Found in the Rebate", "As Set Below"}, 0),
                      num("base_r", "Film Base R", 0, 1, 0.9, 0.001), num("base_g", "Film Base G", 0, 1, 0.55, 0.001),
                      num("base_b", "Film Base B", 0, 1, 0.35, 0.001), num("gamma", "Negative Gamma", 0.3, 1.2, 0.6, 0.01),
                      num("exposure", "Print Exposure (stops)", -4, 4, 0, 0.05)},
                     {}});
        c.push_back({"fade_restore", "Faded Colour Restore", EffectCategory::VideoFilter, "Film",
                     {pct("strength", "Strength", 0, 100, 100), num("frames", "Frames Either Side", 0, 24, 12, 1)},
                     {}});
        c.push_back({"perf_lock", "Steady on Perforations", EffectCategory::VideoFilter, "Film",
                     {choice("side", "Perforations", {"Left", "Right"}, 0), pct("rebate", "Rebate Width", 2, 30, 12),
                      boolean("show", "Show Detected")},
                     {}});
        for (size_t i = c.size() - 3; i < c.size(); ++i)
            for (auto& pi : c[i].params) pi.keyframeable = pi.name == "exposure" || pi.name == "strength";
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
    // An OGraf graphic (render/OGraf.h): an HTML graphic in the EBU's open format, rendered in a headless browser. Its
    // strings: "manifest" (the .ograf.json), "data" (JSON, edited in the Inspector's fields from its schema) and
    // "actions" (JSON, replacing the usual play in, steps and out).
    c.push_back({"ograf", "OGraf Graphic", EffectCategory::Generator, "Titles",
                 {num("out", "Play Out Before the End (s, 0 none)", 0, 30, 1, 0.1),
                  num("advance", "Next Step Every (s, 0 never)", 0, 60, 0, 0.1)},
                 {str("manifest", "Graphic", StringKind::File)}});
    c.back().strings[0].fileFilter = "OGraf graphics (*.ograf.json)";
    for (auto& pi : c.back().params) pi.keyframeable = false;
    c.back().hidden = true;  // (added with Add OGraf Graphic, which picks the graphic first)
    // Its effects, opacity and blend mode apply to everything on the tracks below it.
    c.push_back({"adjustment", "Adjustment Layer", EffectCategory::Generator, "Generators", {}, {}});
    // Audio Visualiser (render/AudioReactive): an audio track's sound drawn each frame, for audiograms and music videos.
    c.push_back({"audio_viz", "Audio Visualiser", EffectCategory::Generator, "Generators",
                 {num("track", "Audio Track (0 = all)", 0, 32, 1, 1),
                  choice("style", "Style", {"Bars", "Mirrored Bars", "Waveform", "Circle"}, 0),
                  num("bars", "Bars", 4, 128, 32, 1),
                  color("color", "Colour", 1, 1, 1),
                  pct("width", "Width", 5, 100, 80),
                  pct("height", "Height", 5, 100, 30),
                  pct("center_y", "Centre Height", 0, 100, 70),
                  num("sensitivity", "Sensitivity (dB)", -24, 24, 0, 0.5),
                  num("window", "Response (ms)", 20, 250, 60, 1),
                  pct("opacity", "Opacity", 0, 100, 100)},
                 {}});
    // Particles (render/Particles.h): positions in pixels from the frame's centre (y down, z towards the viewer).
    {
        EffectInfo particles{
            "particles", "Particles", EffectCategory::Generator, "Generators",
            {
                // Where they are born.
                choice("emitter", "Emitter", {"Point", "Line", "Box", "Sphere", "Title Text", "Picture (Alpha)", "Picture (Brightness)"}, 0),
                boolean("edge_only", "Only From the Edge"),
                num("emit_x", "Emitter X", -8000, 8000, 0, 1), num("emit_y", "Emitter Y", -8000, 8000, 200, 1),
                num("emit_z", "Emitter Z (3D)", -8000, 8000, 0, 1),
                num("emit_w", "Emitter Width (px)", 0, 16000, 400, 1), num("emit_h", "Emitter Height (px)", 0, 16000, 200, 1),
                num("emit_d", "Emitter Depth (px, 3D)", 0, 16000, 0, 1), angle("emit_angle", "Emitter Angle", 0),
                num("text_size", "Title Text Size (px)", 4, 2000, 160, 1), boolean("text_bold", "Title Text Bold", true),
                // How many, for how long, which way.
                num("rate", "Birth Rate (per second)", 0, 100000, 60, 1), num("burst", "Burst at the Start", 0, 200000, 0, 1),
                num("life", "Life (s)", 0.01, 120, 2, 0.01), pct("life_random", "Life Variation", 0, 100, 25),
                num("speed", "Speed (px/s)", 0, 20000, 250, 1), pct("speed_random", "Speed Variation", 0, 100, 25),
                angle("direction", "Direction (0 up, 90 right)", 0), angle("elevation", "Elevation (3D, towards the viewer)", 0),
                num("spread", "Spread (°)", 0, 180, 25, 0.5), boolean("outward", "Away From the Emitter's Centre"),
                // What moves them.
                num("gravity", "Gravity (px/s²)", -20000, 20000, 300, 1), num("wind_x", "Wind X (px/s²)", -20000, 20000, 0, 1),
                num("wind_y", "Wind Y (px/s²)", -20000, 20000, 0, 1), num("wind_z", "Wind Z (px/s², 3D)", -20000, 20000, 0, 1),
                num("drag", "Air Resistance (per s)", 0, 50, 0, 0.01),
                num("attract", "Attractor Strength (px/s², negative repels)", -100000, 100000, 0, 1),
                num("attract_x", "Attractor X", -8000, 8000, 0, 1), num("attract_y", "Attractor Y", -8000, 8000, 0, 1),
                num("attract_z", "Attractor Z (3D)", -8000, 8000, 0, 1), num("attract_radius", "Attractor Reach (px)", 1, 8000, 200, 1),
                num("turbulence", "Turbulence (px/s)", 0, 10000, 0, 1), num("turbulence_scale", "Turbulence Size (px)", 1, 8000, 250, 1),
                num("turbulence_evolve", "Turbulence Change (per s)", 0, 20, 0.3, 0.01),
                // What they hit.
                boolean("floor", "Floor"), pct("floor_y", "Floor Height (from the top)", -100, 300, 100),
                boolean("edges", "Bounce Off the Frame's Sides and Top"), pct("bounce", "Bounce", 0, 100, 50), pct("friction", "Friction", 0, 100, 20),
                // How they look.
                choice("look", "Look", {"Dots", "Soft Discs", "Streaks", "Squares", "Sprites"}, 1),
                num("size", "Size (px)", 0, 2000, 12, 0.1), pct("size_random", "Size Variation", 0, 100, 30),
                pct("size_end", "Size at Death", 0, 2000, 100),
                color("color", "Colour at Birth", 1, 0.9, 0.6), color("color_end", "Colour at Death", 1, 0.35, 0.1),
                num("hue_random", "Hue Variation (°)", 0, 180, 0, 0.5), pct("opacity", "Opacity", 0, 100, 100),
                pct("fade_in", "Fade In (of life)", 0, 100, 10), pct("fade_out", "Fade Out (of life)", 0, 100, 30),
                angle("rotation", "Rotation", 0), num("rotation_random", "Rotation Variation (°)", 0, 180, 0, 0.5),
                num("spin", "Spin (°/s)", -7200, 7200, 0, 1), pct("spin_random", "Spin Variation", 0, 200, 0),
                num("streak", "Streak Length (frames of motion)", 0, 10, 0.5, 0.05),
                choice("blend", "Blend", {"Normal", "Add (glow where they overlap)"}, 0),
                // 3D, through the 3D titles' lens.
                choice("mode", "Space", {"Flat (2D)", "3D"}, 0), num("fov", "Lens (field of view °, 3D)", 5, 120, 35, 0.5),
                num("cam_tilt", "Camera Tilt (°, 3D)", -90, 90, 0, 0.5), num("cam_turn", "Camera Turn (°, 3D)", -180, 180, 0, 0.5),
                // The simulation.
                num("preroll", "Already Running For (s)", 0, 120, 0, 0.1), num("seed", "Seed", 0, 1000000, 1, 1),
                num("steps", "Steps per Frame", 1, 32, 4, 1), num("max_particles", "Most Particles", 1, 200000, 20000, 1),
            },
            {str("text", "Title Text", StringKind::MultilineText, "Montage"), str("font", "Title Font", StringKind::Font, "Sans Serif"),
             str("picture", "Emitter Picture", StringKind::File), str("sprite", "Sprite (Picture or Clip)", StringKind::File)}};
        for (const char* fixed : {"text_size", "text_bold", "burst", "preroll", "seed", "steps", "max_particles"})
            for (ParamInfo& pi : particles.params)
                if (pi.name == fixed) pi.keyframeable = false;
        particles.strings[2].fileFilter = particles.strings[3].fileFilter =
            "Pictures and clips (*.png *.jpg *.jpeg *.tif *.tiff *.webp *.exr *.gif *.mov *.mp4 *.mkv *.webm);;All files (*)";
        c.push_back(particles);
        // Ready-made looks: Particles with these settings (makeEffect gives type "particles").
        for (const GeneratorPreset& pre : generatorPresets()) {
            EffectInfo e = particles;
            e.type = pre.id;
            e.displayName = pre.name;
            c.push_back(e);
        }
    }
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
                             // Text coming on a letter, word or line at a time (CapCut's and Descript's text animations).
                             choice("text_anim", "Text Animation", {"None", "Rise", "Fade", "Pop", "Drop", "Wave", "Scramble"}, 0),
                             choice("text_anim_by", "Animate By", {"Letter", "Word", "Line"}, 0),
                             num("text_anim_dur", "Across (s)", 0.1, 10, 1.0, 0.05),
                             boolean("text_anim_out", "Animate Out Too"),
                             // Credits: the whole block moves through the frame over the clip's length, at a
                             // steady speed after easing in and before easing out.
                             choice("motion", "Roll / Crawl", {"Still", "Roll Up", "Crawl Left", "Crawl Right"}, 0),
                             boolean("start_off", "Start Off Screen", true),
                             boolean("end_off", "End Off Screen", true),
                             num("motion_ease", "Ease In and Out (s)", 0, 10, 0, 0.1),
                             // Text set along a path: a circle about Position (radius), a wave through it (a quarter
                             // of the radius high) or SVG path data from it, starting at Offset (% of its length).
                             choice("path_shape", "Text on a Path", {"None", "Circle", "Wave", "Custom (SVG Path Data)"}, 0),
                             num("path_radius", "Path Radius (px)", 4, 4000, 300, 1),
                             num("path_wavelength", "Wavelength (px)", 4, 8000, 600, 1),
                             pct("path_offset", "Path Offset", -200, 200, 0),
                             boolean("path_upright", "Keep Letters Upright"),
                             boolean("path_reverse", "Flip Path"),
                         },
                         {str("text", "Text", StringKind::MultilineText, "Title"),
                          str("font", "Font", StringKind::Font, "Sans Serif"),
                          str("path_data", "Path (SVG Data)", StringKind::Text)}};
        c.push_back(title);
        // Extruded 3D text (render/Title3D.h): turned in space, lit, seen through a perspective camera.
        EffectInfo title3d{"title3d", "3D Title", EffectCategory::Generator, "Titles",
                           {
                               num("size", "Font Size (px)", 4, 1000, 120, 1),
                               color("color", "Face", 0.95, 0.95, 0.95),
                               color("side_color", "Sides", 0.45, 0.5, 0.6),
                               num("depth", "Extrusion (px)", 0, 1000, 40, 0.5),
                               num("rot_x", "Tilt (X°)", -360, 360, 10, 0.5),
                               num("rot_y", "Turn (Y°)", -360, 360, -20, 0.5),
                               num("rot_z", "Roll (Z°)", -360, 360, 0, 0.5),
                               num("pos_x", "Position X", -4000, 4000, 0, 1),
                               num("pos_y", "Position Y", -4000, 4000, 0, 1),
                               pct("scale", "Scale", 1, 2000, 100),
                               num("fov", "Lens (field of view °)", 5, 120, 35, 0.5),
                               num("light_angle", "Light Direction (°)", -180, 180, -40, 1),
                               num("light_height", "Light Height (°)", -90, 90, 40, 1),
                               pct("ambient", "Ambient Light", 0, 100, 35),
                               pct("specular", "Shine", 0, 100, 40),
                               num("tracking", "Tracking", -50, 200, 0, 0.5),
                               num("line_spacing", "Line Spacing", 0.5, 3, 1.1),
                               boolean("bold", "Bold", true),
                               boolean("italic", "Italic"),
                               choice("align", "Alignment", {"Left", "Center", "Right"}, 1),
                               pct("opacity", "Opacity", 0, 100, 100),
                               choice("anim_in", "Animate In", {"None", "Fade", "Spin", "Flip Up", "Swing In", "Zoom From Far"}, 0),
                               num("anim_in_dur", "In Duration (s)", 0.05, 10, 0.8, 0.05),
                               choice("anim_out", "Animate Out", {"None", "Fade", "Spin", "Flip Up", "Swing In", "Zoom From Far"}, 0),
                               num("anim_out_dur", "Out Duration (s)", 0.05, 10, 0.8, 0.05),
                           },
                           {str("text", "Text", StringKind::MultilineText, "3D Title"),
                            str("font", "Font", StringKind::Font, "Sans Serif")}};
        c.push_back(title3d);
        // A glTF or OBJ model in the shot (render/ModelClip.h): through its own camera (pixels) or the camera the 3D
        // Camera Tracker solved on another track (the solved scene's units, its ground at height 0).
        c.push_back({"model3d", "3D Model", EffectCategory::Generator, "Titles",
                     {
                         choice("camera", "Camera", {"Its Own", "Solved by the 3D Camera Tracker"}, 0),
                         num("camera_track", "Solved Footage Track (0: nearest below)", 0, 99, 0, 1),
                         num("fov", "Lens (field of view °)", 5, 120, 35, 0.5),
                         num("size", "Size (px, own camera)", 1, 20000, 400, 1),
                         pct("scale", "Scale (solved camera, of its own units)", 0.01, 100000, 100),
                         boolean("stand", "Stand It on the Ground (solved camera)", true),
                         num("pos_x", "Position X", -100000, 100000, 0, 0.01),
                         num("pos_y", "Position Y (up)", -100000, 100000, 0, 0.01),
                         num("pos_z", "Position Z (towards the camera)", -100000, 100000, 0, 0.01),
                         num("rot_y", "Turn (Y°)", -360, 360, -25, 0.5),
                         num("rot_x", "Tilt (X°)", -360, 360, 0, 0.5),
                         num("rot_z", "Roll (Z°)", -360, 360, 0, 0.5),
                         num("animation", "Animation (0: none)", 0, 999, 1, 1),
                         pct("anim_speed", "Animation Speed", -1000, 1000, 100),
                         num("anim_offset", "Animation Offset (s)", -3600, 3600, 0, 0.01),
                         boolean("loop", "Loop the Animation", true),
                         choice("light_kind", "Light", {"Sun", "Point", "Spot"}, 0),
                         num("light_angle", "Light Direction (°)", -180, 180, -40, 1),
                         num("light_height", "Light Height (°)", -90, 90, 45, 1),
                         num("light_distance", "Light Distance (model sizes)", 0.1, 100, 3, 0.1),
                         num("spot_angle", "Spot Angle (°)", 1, 85, 30, 0.5),
                         pct("light_strength", "Light Strength", 0, 1000, 100),
                         color("light_color", "Light Colour", 1, 1, 1),
                         boolean("shadows", "Cast Shadows (sun and spot)", true),
                         pct("ambient", "Ambient Light", 0, 1000, 30),
                         num("env_track", "Image Light Track (0: none)", 0, 99, 0, 1),
                         num("env_rotation", "Image Light Rotation (°)", -360, 360, 0, 1),
                         pct("env_strength", "Image Light Strength", 0, 1000, 100),
                         boolean("env_hide", "Hide the Image Light Track", true),
                         boolean("ground", "Shadows on the Ground", true),
                         pct("ground_strength", "Ground Shadow Strength", 0, 100, 80),
                         boolean("depth_merge", "Hide Behind Nearer Things (Merge by Depth)", false),
                         pct("opacity", "Opacity", 0, 100, 100),
                     },
                     {str("file", "Model (glTF, GLB, OBJ)", StringKind::File)}});
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
        m.params = {choice("mask.shape", "Shape", {"None", "Ellipse", "Rectangle", "Object", "People", "Bézier", "Gradient", "Cryptomatte"}, 0),
                    num("mask.x", "Center X", -0.5, 1.5, 0.5, 0.001),
                    num("mask.y", "Center Y", -0.5, 1.5, 0.5, 0.001),
                    num("mask.w", "Width", 0.001, 2, 0.4, 0.001),
                    num("mask.h", "Height", 0.001, 2, 0.4, 0.001),
                    angle("mask.rotation", "Rotation"),
                    num("mask.feather", "Feather (px)", 0, 500, 20, 0.5),
                    num("mask.expansion", "Expansion (px)", -500, 500, 0, 0.5),
                    num("mask.consolidate", "Consolidate (px)", 0, 200, 0, 0.5),
                    pct("mask.refine", "Refine Edges", 0, 100, 0),
                    num("mask.refine_radius", "Refine Radius (px)", 1, 64, 8, 0.5),
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

bool needsFaces(const Effect& e) {
    return e.enabled && (e.type == "face_refine" || e.type == "blemish_remover" || e.type == "redact_faces" || e.type == "face_shape" ||
                         e.type == "look_at_lens" || e.type == "lip_sync" ||
                         (e.type == "depth_blur" && std::lround(e.p("follow", 0, 0)) == 2));
}

bool needsPersonMatte(const Effect& e, FrameTime t) {
    if (!e.enabled) return false;
    return e.type == "remove_background" || std::lround(e.p("mask.shape", t)) == 4;
}

bool needsCryptomatte(const Effect& e, FrameTime t) {
    return e.enabled && std::lround(e.p("mask.shape", t)) == 7;
}

bool needsDepth(const Effect& e, FrameTime t) {
    if (!e.enabled) return false;
    return e.type == "depth_blur" || e.type == "depth_fog" || e.type == "depth_map" || e.type == "relight" || e.p("mask.depth", t) > 0.5;
}

const std::vector<HdrZone>& hdrZones() {
    static const std::vector<HdrZone> zones{{"black", "Black", -4, 2, true},          {"dark", "Dark", -2.5, 1.5, true},
                                            {"shadow", "Shadow", -1, 1.5, true},      {"light", "Light", 1, 1.5, false},
                                            {"highlight", "Highlight", 2, 1.5, false}, {"specular", "Specular", 3.5, 1.5, false}};
    return zones;
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
    std::string meta = p.label + "\t" + std::to_string(p.min) + "\t" + std::to_string(p.max) + "\t" + std::to_string(p.def) + "\t" +
                       (p.step >= 1 ? "1" : "0");
    // Beyond the number: how it is shown (a choice, a switch, a colour, an angle), as OpenFX parameters need.
    if (p.kind != ParamKind::Number || !p.keyframeable) {
        std::string choices;
        for (const std::string& c : p.choices) choices += (choices.empty() ? "" : "|") + c;
        meta += "\t" + std::to_string(int(p.kind)) + "\t" + choices + "\t" + std::to_string(p.defG) + "\t" + std::to_string(p.defB) +
                "\t" + (p.keyframeable ? "1" : "0");
    }
    return meta;
}

namespace {

std::vector<std::string> metaFields(const std::string& meta) {
    std::vector<std::string> f;
    size_t start = 0;
    for (size_t tab; (tab = meta.find('\t', start)) != std::string::npos; start = tab + 1) f.push_back(meta.substr(start, tab - start));
    f.push_back(meta.substr(start));
    return f;
}

void applyMeta(ParamInfo& p, const std::vector<std::string>& f) {
    if (f.size() < 5) return;
    p.label = f[0];
    p.min = std::atof(f[1].c_str());
    p.max = std::atof(f[2].c_str());
    p.def = std::atof(f[3].c_str());
    p.step = f[4] == "1" ? 1.0 : (p.max - p.min) / 1000.0;
    if (f.size() >= 10) {
        p.kind = ParamKind(std::clamp(std::atoi(f[5].c_str()), 0, int(ParamKind::Color)));
        p.choices.clear();
        for (size_t start = 0; !f[6].empty() && start <= f[6].size();) {
            const size_t bar = f[6].find('|', start);
            p.choices.push_back(f[6].substr(start, bar == std::string::npos ? std::string::npos : bar - start));
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
        p.defG = std::atof(f[7].c_str());
        p.defB = std::atof(f[8].c_str());
        p.keyframeable = f[9] == "1";
        if (p.kind == ParamKind::Choice || p.kind == ParamKind::Bool) p.step = 1;
    }
}

}  // namespace

std::vector<int> textAnimators(const Effect& e) {
    std::vector<int> out;
    for (const auto& [key, param] : e.params) {
        if (key.size() < 6 || key.compare(0, 2, "ta") != 0 || key.compare(key.size() - 3, 3, ".by") != 0) continue;
        const std::string n = key.substr(2, key.size() - 5);
        if (n.empty() || n.size() > 4 || !std::all_of(n.begin(), n.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        out.push_back(std::stoi(n));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<ParamInfo> textAnimatorParams(int k) {
    const std::string pre = "ta" + std::to_string(k) + ".";
    ParamInfo rotation = angle(pre + "rotation", "Rotation");
    rotation.min = -720, rotation.max = 720;
    return {
        choice(pre + "by", "Animate", {"Letters", "Letters (Not Spaces)", "Words", "Lines"}),
        choice(pre + "shape", "Shape", {"Square", "Ramp Up", "Ramp Down", "Triangle", "Round", "Smooth"}),
        pct(pre + "start", "Start", 0, 100, 0),
        pct(pre + "end", "End", 0, 100, 100),
        pct(pre + "offset", "Offset", -200, 200, 0),
        pct(pre + "ease", "Ease", 0, 100, 0),
        pct(pre + "amount", "Amount", -100, 100, 100),
        // The range swept through the text over a time from the clip's start (In) or to its end (Out, the In played
        // backwards), or the whole selection let go of or taken up together: an animation without keyframes.
        choice(pre + "sweep", "Sweep", {"None (Offset Only)", "In Across", "Out Across", "In Together", "Out Together"}),
        num(pre + "sweep_dur", "Sweep Duration (s)", 0.05, 10, 1.0, 0.05),
        pct(pre + "random", "Randomness", 0, 100, 0),
        boolean(pre + "shuffle", "Random Order"),
        num(pre + "seed", "Random Seed", 0, 10000, 0, 1),
        num(pre + "x", "Position X", -4000, 4000, 0, 1),
        num(pre + "y", "Position Y", -4000, 4000, 0, 1),
        pct(pre + "scale", "Scale", 0, 1000, 100),
        rotation,
        pct(pre + "opacity", "Opacity", 0, 100, 100),
        num(pre + "blur", "Blur (px)", 0, 200, 0, 0.5),
        num(pre + "tracking", "Tracking", -200, 500, 0, 0.5),
        color(pre + "fill", "Fill", 1, 1, 1),
        pct(pre + "fill_mix", "Fill Amount", 0, 100, 0),
        color(pre + "outline_color", "Outline Color", 0, 0, 0),
        pct(pre + "outline_mix", "Outline Color Amount", 0, 100, 0),
    };
}

int addTextAnimator(Effect& e, int k) {
    if (k <= 0) {
        const std::vector<int> have = textAnimators(e);
        k = have.empty() ? 1 : have.back() + 1;
    }
    for (const ParamInfo& pi : textAnimatorParams(k)) {
        if (pi.kind == ParamKind::Color) {
            e.params.try_emplace(pi.name + ".r", Param(pi.def));
            e.params.try_emplace(pi.name + ".g", Param(pi.defG));
            e.params.try_emplace(pi.name + ".b", Param(pi.defB));
        } else {
            e.params.try_emplace(pi.name, Param(pi.def));
        }
    }
    return k;
}

std::vector<ParamInfo> effectParams(const Effect& e) {
    if (e.type != "plugin" && e.type != "ofx") {
        const EffectInfo* info = findEffectInfo(e.type);
        std::vector<ParamInfo> out = info ? info->params : std::vector<ParamInfo>{};
        if (e.type == "title")
            for (int k : textAnimators(e))
                for (ParamInfo& pi : textAnimatorParams(k)) out.push_back(std::move(pi));
        return out;
    }
    std::vector<ParamInfo> out;
    for (const auto& [key, param] : e.params) {
        if (key.rfind("param.", 0) != 0) continue;
        ParamInfo p;
        p.name = key;
        p.label = key.substr(6);
        auto meta = e.strings.find("meta." + key.substr(6));
        if (meta == e.strings.end() && key.size() > 8 && key[key.size() - 2] == '.') {
            // A colour's channels: one row, named for the colour, at its red channel.
            const std::string stem = key.substr(6, key.size() - 8);
            auto colour = e.strings.find("meta." + stem);
            if (colour != e.strings.end()) {
                ParamInfo c;
                applyMeta(c, metaFields(colour->second));
                if (c.kind == ParamKind::Color) {
                    if (key.back() != 'r') continue;
                    c.name = "param." + stem;
                    out.push_back(c);
                    continue;
                }
            }
        }
        if (meta != e.strings.end()) applyMeta(p, metaFields(meta->second));
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
        // text_anim 0 None, 1 Rise, 2 Fade, 3 Pop, 4 Drop, 5 Wave, 6 Scramble; text_anim_by 0 Letter, 1 Word, 2 Line.
        {"title_cascade", "Cascade", {{"size", 110}, {"shadow", 4}, {"text_anim", 1}, {"text_anim_dur", 1.0}, {"text_anim_out", 1}}, {{"text", "Cascade"}}},
        {"title_pop_words", "Pop Words",
         {{"size", 96}, {"shadow", 0}, {"outline", 6}, {"text_anim", 3}, {"text_anim_by", 1}, {"text_anim_dur", 0.8}},
         {{"text", "Every word pops"}}},
        {"title_drop", "Drop In", {{"size", 120}, {"shadow", 6}, {"text_anim", 4}, {"text_anim_dur", 1.2}}, {{"text", "Drop In"}}},
        {"title_wave", "Wave", {{"size", 110}, {"shadow", 3}, {"text_anim", 5}}, {{"text", "Making waves"}}},
        {"title_decode", "Decode", {{"size", 96}, {"bold", 1}, {"text_anim", 6}, {"text_anim_dur", 1.2}}, {{"text", "ACCESS GRANTED"}, {"font", "Monospace"}}},
        // Text animations (ta<k>.*, core/Effects.h textAnimatorParams): by 0 Letters, 1 Letters (Not Spaces), 2 Words,
        // 3 Lines; shape 0 Square, 1 Ramp Up, 2 Ramp Down, 3 Triangle, 4 Round, 5 Smooth; sweep 0 None, 1 In Across,
        // 2 Out Across, 3 In Together, 4 Out Together.
        {"title_blur_in", "Blur In",
         {{"size", 110}, {"shadow", 0}, {"ta1.by", 0}, {"ta1.shape", 1}, {"ta1.end", 40}, {"ta1.sweep", 1}, {"ta1.sweep_dur", 1.2},
          {"ta1.blur", 18}, {"ta1.opacity", 0}, {"ta1.scale", 130}},
         {{"text", "Blur In"}}},
        {"title_tracking_out", "Tracking Out",
         {{"size", 100}, {"shadow", 0}, {"anim_in", 1}, {"anim_in_dur", 0.5}, {"ta1.by", 0}, {"ta1.sweep", 4}, {"ta1.sweep_dur", 1.0},
          {"ta1.tracking", 40}, {"ta1.opacity", 0}},
         {{"text", "TRACKING OUT"}}},
        {"title_word_by_word", "Word by Word",
         {{"size", 90}, {"shadow", 3}, {"ta1.by", 2}, {"ta1.shape", 1}, {"ta1.end", 1}, {"ta1.sweep", 1}, {"ta1.sweep_dur", 1.5},
          {"ta1.opacity", 0}, {"ta1.scale", 70}, {"ta1.y", 20}},
         {{"text", "One word at a time"}}},
        {"title_circle", "Text on a Circle",
         {{"size", 56}, {"shadow", 2}, {"path_shape", 1}, {"path_radius", 220}, {"ta1.by", 0}, {"ta1.shape", 1}, {"ta1.end", 10},
          {"ta1.sweep", 1}, {"ta1.sweep_dur", 1.5}, {"ta1.opacity", 0}, {"ta1.y", -30}},
         {{"text", "ALL THE WAY AROUND"}}},
        {"title_end_card", "End Card",
         {{"size", 110}, {"sub_style", 1}, {"sub_scale", 50}, {"anim_in", 1}, {"anim_in_dur", 0.8}, {"anim_out", 1}, {"anim_out_dur", 0.8}},
         {{"text", "Thanks for watching\nSee you next time"}}},
        // motion 0 Still, 1 Roll Up, 2 Crawl Left, 3 Crawl Right.
        {"title_credits", "Rolling Credits",
         {{"size", 52}, {"bold", 0}, {"line_spacing", 1.45}, {"shadow", 2}, {"motion", 1}},
         {{"text", "Directed by\nName Surname\n\nEdited by\nName Surname\n\nMusic by\nName Surname\n\nThank you for watching"}}},
        {"title_crawl", "News Crawl",
         {{"size", 44}, {"anchor", 2}, {"box_opacity", 80}, {"box_padding", 14}, {"shadow", 0}, {"motion", 2}},
         {{"text", "Your headline here   \u2022   The next story follows   \u2022   And one more"}}},
    };
    return list;
}

const std::vector<GeneratorPreset>& generatorPresets() {
    // Choices: emitter 0 Point, 1 Line, 2 Box, 3 Sphere, 4 Title Text, 5 Picture (Alpha), 6 Picture (Brightness);
    // look 0 Dots, 1 Soft Discs, 2 Streaks, 3 Squares, 4 Sprites; blend 0 Normal, 1 Add; mode 0 Flat, 1 3D.
    static const std::vector<GeneratorPreset> list = {
        {"particles_sparks", "Sparks", "particles",
         {{"emit_y", 250}, {"rate", 300}, {"life", 1.2}, {"life_random", 50}, {"speed", 750}, {"speed_random", 40}, {"spread", 35},
          {"gravity", 1400}, {"drag", 0.4}, {"floor", 1}, {"floor_y", 95}, {"bounce", 35}, {"friction", 30}, {"look", 2},
          {"size", 2.5}, {"size_random", 40}, {"streak", 1}, {"color.r", 1}, {"color.g", 0.85}, {"color.b", 0.45},
          {"color_end.r", 1}, {"color_end.g", 0.25}, {"color_end.b", 0.05}, {"fade_in", 0}, {"fade_out", 40}, {"blend", 1}},
         {}},
        {"particles_snow", "Snow", "particles",
         {{"emitter", 1}, {"emit_y", -620}, {"emit_w", 2400}, {"rate", 70}, {"life", 14}, {"speed", 40}, {"speed_random", 50},
          {"direction", 180}, {"spread", 25}, {"gravity", 40}, {"drag", 0.35}, {"turbulence", 22}, {"turbulence_scale", 400},
          {"turbulence_evolve", 0.15}, {"size", 7}, {"size_random", 60}, {"color.r", 1}, {"color.g", 1}, {"color.b", 1},
          {"color_end.r", 0.92}, {"color_end.g", 0.95}, {"color_end.b", 1}, {"opacity", 90}, {"fade_in", 3}, {"fade_out", 8},
          {"preroll", 12}},
         {}},
        {"particles_rain", "Rain", "particles",
         {{"emitter", 1}, {"emit_y", -640}, {"emit_w", 2600}, {"rate", 900}, {"life", 1.1}, {"life_random", 10}, {"speed", 1500},
          {"speed_random", 15}, {"direction", 188}, {"spread", 2}, {"gravity", 2000}, {"look", 2}, {"size", 1.2},
          {"size_random", 30}, {"streak", 1.5}, {"color.r", 0.75}, {"color.g", 0.8}, {"color.b", 0.9}, {"color_end.r", 0.75},
          {"color_end.g", 0.8}, {"color_end.b", 0.9}, {"opacity", 45}, {"fade_in", 0}, {"fade_out", 0}, {"preroll", 2}},
         {}},
        {"particles_dust", "Floating Dust", "particles",
         {{"emitter", 2}, {"emit_y", 0}, {"emit_w", 2200}, {"emit_h", 1300}, {"emit_d", 900}, {"mode", 1}, {"rate", 70},
          {"life", 8}, {"life_random", 40}, {"speed", 6}, {"spread", 180}, {"gravity", 0}, {"turbulence", 22},
          {"turbulence_scale", 260}, {"turbulence_evolve", 0.08}, {"size", 6}, {"size_random", 70}, {"color.r", 1},
          {"color.g", 0.95}, {"color.b", 0.85}, {"color_end.r", 1}, {"color_end.g", 0.95}, {"color_end.b", 0.85},
          {"opacity", 75}, {"fade_in", 30}, {"fade_out", 30}, {"preroll", 8}},
         {}},
        {"particles_smoke", "Smoke Puffs", "particles",
         {{"emit_y", 320}, {"emit_w", 40}, {"emit_h", 40}, {"emitter", 3}, {"rate", 14}, {"life", 4}, {"life_random", 30}, {"speed", 90},
          {"spread", 18}, {"gravity", -35}, {"drag", 0.45}, {"turbulence", 60}, {"turbulence_scale", 220}, {"size", 70},
          {"size_random", 30}, {"size_end", 420}, {"color.r", 0.8}, {"color.g", 0.8}, {"color.b", 0.8}, {"color_end.r", 0.55},
          {"color_end.g", 0.55}, {"color_end.b", 0.55}, {"opacity", 28}, {"fade_in", 15}, {"fade_out", 60}},
         {}},
        {"particles_confetti", "Confetti", "particles",
         {{"emitter", 1}, {"emit_y", -600}, {"emit_w", 2000}, {"burst", 250}, {"rate", 60}, {"life", 7}, {"speed", 160},
          {"direction", 180}, {"spread", 45}, {"gravity", 260}, {"drag", 1.1}, {"turbulence", 90}, {"turbulence_scale", 200},
          {"look", 3}, {"size", 10}, {"size_random", 35}, {"color.r", 1}, {"color.g", 0.3}, {"color.b", 0.35},
          {"color_end.r", 1}, {"color_end.g", 0.3}, {"color_end.b", 0.35}, {"hue_random", 180}, {"fade_in", 0}, {"fade_out", 10},
          {"rotation_random", 180}, {"spin", 300}, {"spin_random", 150}},
         {}},
        {"particles_burst", "Firework Burst", "particles",
         {{"emit_y", -150}, {"burst", 400}, {"rate", 0}, {"outward", 1}, {"life", 2.4}, {"life_random", 30}, {"speed", 420},
          {"speed_random", 25}, {"spread", 0}, {"gravity", 160}, {"drag", 1}, {"look", 2}, {"size", 2.5}, {"streak", 1.2},
          {"color.r", 1}, {"color.g", 0.9}, {"color.b", 0.6}, {"color_end.r", 0.9}, {"color_end.g", 0.2}, {"color_end.b", 0.5},
          {"fade_in", 0}, {"fade_out", 60}, {"blend", 1}},
         {}},
        {"particles_title_sparkle", "Title Sparkle", "particles",
         {{"emitter", 4}, {"edge_only", 1}, {"emit_y", 0}, {"rate", 900}, {"life", 0.9}, {"life_random", 40}, {"speed", 12},
          {"spread", 180}, {"gravity", -20}, {"size", 4}, {"size_random", 60}, {"color.r", 1}, {"color.g", 0.92},
          {"color.b", 0.6}, {"color_end.r", 1}, {"color_end.g", 0.7}, {"color_end.b", 0.3}, {"fade_in", 20}, {"fade_out", 50},
          {"blend", 1}},
         {{"text", "Montage"}}},
    };
    return list;
}

const GeneratorPreset* findGeneratorPreset(const std::string& id) {
    for (const GeneratorPreset& p : generatorPresets())
        if (id == p.id) return &p;
    return nullptr;
}

const TitleTemplate* findTitleTemplate(const std::string& id) {
    for (const TitleTemplate& t : titleTemplates())
        if (id == t.id) return &t;
    return nullptr;
}

Effect makeEffect(const std::string& type, Id id) {
    if (const GeneratorPreset* pre = findGeneratorPreset(type)) {
        Effect e = makeEffect(pre->base, id);
        for (const auto& [name, value] : pre->params) e.params[name] = Param(value);
        for (const auto& [name, value] : pre->strings) e.strings[name] = value;
        return e;
    }
    if (const TitleTemplate* tpl = findTitleTemplate(type)) {
        Effect e = makeEffect("title", id);
        for (const auto& [name, value] : tpl->params) e.params[name] = Param(value);
        for (const auto& [name, value] : tpl->strings) e.strings[name] = value;
        for (int k : textAnimators(e)) addTextAnimator(e, k);  // the rest of a text animation's settings
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
