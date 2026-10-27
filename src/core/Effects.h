// Montage — effect, generator and transition catalogue.
//
// Each effect type is described once here (its parameters, ranges and
// defaults). The renderer implements the processing; the UI builds the
// inspector from these descriptions, so adding a parameter here is enough to
// expose it everywhere.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Model.h"

namespace montage {

enum class EffectCategory { VideoFilter, AudioFilter, Generator, VideoTransition, AudioTransition, Fixed };

enum class ParamKind {
    Number,   // plain slider
    Angle,    // degrees
    Percent,  // shown as %
    Bool,     // 0 / 1 checkbox
    Choice,   // integer index into choices
    Color,    // three params "<name>.r", "<name>.g", "<name>.b" in 0..1
};

struct ParamInfo {
    std::string name;
    std::string label;
    ParamKind kind = ParamKind::Number;
    double min = 0;
    double max = 1;
    double def = 0;
    double step = 0.01;
    std::vector<std::string> choices;  // for Choice
    // Default colour for Color params.
    double defG = 0, defB = 0;
    bool keyframeable = true;
};

// Dynamic: an editable list whose entries depend on the effect's other
// settings (the colour spaces of the chosen OCIO config, for example).
// Curve: a tone curve (in -> out). HueCurve and LevelCurve: a change against
// hue (wrapping round) or against luma/saturation, 0.5 meaning none.
// Track: an audio track of the sequence by its id (a sidechain key), "" = none (or what choices[0] names).
enum class StringKind { Text, MultilineText, File, Font, Choice, Curve, Dynamic, HueCurve, LevelCurve, ColorWarp, Track };

struct StringParamInfo {
    std::string name;
    std::string label;
    StringKind kind = StringKind::Text;
    std::string def;
    std::vector<std::string> choices;
    std::string fileFilter;  // File: the open dialog's filter, e.g. "LUT files (*.cube)"
};

struct EffectInfo {
    std::string type;
    std::string displayName;
    EffectCategory category = EffectCategory::VideoFilter;
    std::string group;  // UI grouping, e.g. "Color", "Blur & Sharpen", "Keying"
    std::vector<ParamInfo> params;
    std::vector<StringParamInfo> strings;
    // Not offered in effect lists (e.g. "plugin", added per installed plugin).
    bool hidden = false;
};

const std::vector<EffectInfo>& effectCatalog();

// The HDR Palette's zones, darkest first: the dark three reach every tone below their range, the bright three every
// tone above it (stops from mid grey), each fading out over its falloff.
struct HdrZone {
    const char* name;   // parameter prefix: "black", "dark", "shadow", "light", "highlight", "specular"
    const char* label;
    double range, falloff;
    bool dark;
};
const std::vector<HdrZone>& hdrZones();
const EffectInfo* findEffectInfo(const std::string& type);

// Ready-made titles (lower thirds, call-outs...): listed as generators of
// their own, they make a "title" generator with these settings.
struct TitleTemplate {
    const char* id;
    const char* name;
    std::vector<std::pair<const char*, double>> params;
    std::vector<std::pair<const char*, const char*>> strings;
};
const std::vector<TitleTemplate>& titleTemplates();
const TitleTemplate* findTitleTemplate(const std::string& id);
// Ready-made looks for other generators (Particles' sparks, snow, rain...): listed as generators of their own, they
// make a `base` generator with these settings.
struct GeneratorPreset {
    const char* id;
    const char* name;
    const char* base;
    std::vector<std::pair<const char*, double>> params;
    std::vector<std::pair<const char*, const char*>> strings;
};
const std::vector<GeneratorPreset>& generatorPresets();
const GeneratorPreset* findGeneratorPreset(const std::string& id);
std::vector<const EffectInfo*> effectsInCategory(EffectCategory c);

// The parameters the inspector shows for an effect: the catalogue entry's,
// or for "plugin" effects those recorded when the plugin was added
// (keys "param.<id>", described by strings "meta.<id>").
std::vector<ParamInfo> effectParams(const Effect& e);
std::string pluginParamMeta(const ParamInfo& p);
// A title's text animations (render/Titles.h): the k of each "ta<k>.by" param it has, in order, and the params an
// animation has, named "ta<k>.<name>" (effectParams lists them after the title's own).
std::vector<int> textAnimators(const Effect& e);
std::vector<ParamInfo> textAnimatorParams(int k);
// Adds text animation k to a title with its params at their defaults (a new k past the last when 0); returns k.
int addTextAnimator(Effect& e, int k = 0);

// A Mesh Warp's pins (render/MeshWarp.h): the k of each "pin<k>.kind" param it has, in order; a pin's kind (0
// Position, 1 Stiff, 2 Bend, 3 Overlap); and the params a pin of that kind has, named "pin<k>.<name>" (effectParams
// lists them after the effect's own). Positions are fractions of the frame: x and y where the pin sits on the picture at
// rest, to_x and to_y where a Position pin moves it.
std::vector<int> meshPins(const Effect& e);
int meshPinKind(const Effect& e, int k);
std::vector<ParamInfo> meshPinParams(int k, int kind);
// Adds a pin of `kind` at (x, y) (past the last; its params at their defaults, a Position pin where it sits); returns k.
int addMeshPin(Effect& e, int kind, double x, double y);
void removeMeshPin(Effect& e, int k);
// A Grid Warp's points: "g<r>_<c>" (r 0..rows, c 0..cols), each with params ".dx" and ".dy", how far it is moved (as
// fractions of the frame); gridWarpParams lists them for its rows and columns.
std::string gridWarpPoint(int r, int c);
std::vector<ParamInfo> gridWarpParams(const Effect& e);

// The mask every video filter can have (params "mask.*", absent = no mask):
// an ellipse or rectangle in the clip's source frame (centre and size as
// fractions of the frame, feather and expansion in sequence pixels), a drawn
// Bézier path in that box (shape 5, core/MaskPath.h), a gradient across it
// (shape 6), the people in the picture (shape 4, media/Matting.h), an HSL
// qualifier that selects colours, and/or a depth qualifier that selects a
// range of distances. The filter is applied through it.
const EffectInfo& maskInfo();
bool supportsMask(const std::string& effectType);
bool hasMask(const Effect& e, FrameTime t);
// The effect works from the picture's depth (a depth effect, or a depth qualifier).
bool needsDepth(const Effect& e, FrameTime t);
// The effect works from the faces in the picture (Face Refinement, Blemish Remover, Redact Faces).
bool needsFaces(const Effect& e);
// The effect works from the people in the picture (Remove Background, or a People mask).
bool needsPersonMatte(const Effect& e, FrameTime t);
// The effect is masked by Cryptomatte ID mattes (mask.shape 7): its strings mask.crypto_layer (the Cryptomatte, ""
// the file's first) and mask.crypto_ids (the objects picked, as 8-digit hex ids, media/Exr.h).
bool needsCryptomatte(const Effect& e, FrameTime t);

// Builds an Effect of the given type populated with default values.
Effect makeEffect(Project& p, const std::string& type);
Effect makeEffect(const std::string& type, Id id);

// Blend modes supported by the compositor.
const std::vector<std::string>& blendModes();

}  // namespace montage
