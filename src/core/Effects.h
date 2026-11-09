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
// PictureInput: what an OpenFX plugin's extra input shows (render/Ofx.h inputKey): "" nothing, "track:<id>" a video track,
// "media:<id>" a bin item, "mask" the clip's own mask.
enum class StringKind { Text, MultilineText, File, Font, Choice, Curve, Dynamic, HueCurve, LevelCurve, ColorWarp, Track, PictureInput };

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
// animation has, named "ta<k>.<name>" (effectParams lists them after the title's own). A 3D title's ("title3d") pick
// letters the same way and set how deep they are extruded.
std::vector<int> textAnimators(const Effect& e);
std::vector<ParamInfo> textAnimatorParams(int k, const std::string& type = "title");
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

// The Parametric EQ's bands (render/AudioFx.h ParametricEq), up to kEqMaxBands, added and removed per effect. Each is
// the params "<band>_on", "_type" (eqBandTypes), "_hz", "_db", "_q" (the three keyframeable, each on its own) and
// "_slope" (a cut's, eqSlopes); a band is there when any of them is. Bands are "b1" to "b16", and "low" and "high":
// the low and high shelves of an EQ made before bands could be added (shelves unless their type says otherwise), kept
// so that older projects, presets, automation and Match Voice play as they did. They run low, b1..b16, high.
constexpr int kEqMaxBands = 16;
enum EqBandType { kEqBell, kEqLowShelf, kEqHighShelf, kEqLowCut, kEqHighCut, kEqNotch, kEqBandPass, kEqTilt };
const std::vector<std::string>& eqBandTypes();  // their names, by EqBandType
const std::vector<std::string>& eqSlopes();     // a cut's slope: 12, 24, 36 or 48 dB an octave
bool eqTypeHasGain(int type);                   // a bell, a shelf or a tilt (not a cut, notch or band-pass)
bool eqTypeHasSlope(int type);                  // a cut
double eqDefaultQ(int type);                    // 1 for a bell, notch or band-pass; 0.707 (no overshoot) otherwise
// The most Q a type takes: 2 for a shelf or tilt (a +6 dB shelf then bumps to under +9 dB beside its corner), 30 otherwise.
double eqMaxQ(int type);
// Whether `name` is a band's param ("b4_hz"), and which band and field.
bool isEqBandParam(const std::string& name, std::string* band = nullptr, std::string* field = nullptr);
std::vector<std::string> eqBands(const Effect& e);  // the bands it has, in the order they run
int eqBandType(const Effect& e, const std::string& band);
// A band's place among all the names, low 0, b1 to b16 1 to 16, high 17 (-1: not a band's): it stays the same band's
// as others are added and removed, so playback keeps each band's filter to itself.
int eqBandSlot(const std::string& band);
std::string eqBandLabel(const std::string& band);  // "Band 4", "Low Band"
double eqDefaultHz(const std::string& band);       // a band's frequency when it has none (as the five-band EQ had)
// A band's settings, its Q's range that of `type` (-1: the band's own when it has none). Their names are made from
// eqBandLabel; for translators they are listed with "%1" in place of the band's number ("Band %1 (Hz)", which
// app/Language.h engine() fills in).
std::vector<ParamInfo> eqBandParams(const std::string& band, int type = -1);
// Adds a band named for the first free "b<k>", its params set (q 0: the type's own); "" when it has kEqMaxBands.
std::string addEqBand(Effect& e, int type, double hz, double db = 0, double q = 0);
void removeEqBand(Effect& e, const std::string& band);
// Changes a band's type; a Q left at the old type's own becomes the new type's.
void setEqBandType(Effect& e, const std::string& band, int type);

// Builds an Effect of the given type populated with default values.
Effect makeEffect(Project& p, const std::string& type);
Effect makeEffect(const std::string& type, Id id);

// Blend modes supported by the compositor.
const std::vector<std::string>& blendModes();
// A blend mode as the interface names it ("soft_light" → "Soft light").
std::string blendModeLabel(const std::string& mode);

}  // namespace montage
