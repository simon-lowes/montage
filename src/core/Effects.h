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
enum class StringKind { Text, MultilineText, File, Font, Choice, Curve, Dynamic };

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
std::vector<const EffectInfo*> effectsInCategory(EffectCategory c);

// The parameters the inspector shows for an effect: the catalogue entry's,
// or for "plugin" effects those recorded when the plugin was added
// (keys "param.<id>", described by strings "meta.<id>").
std::vector<ParamInfo> effectParams(const Effect& e);
std::string pluginParamMeta(const ParamInfo& p);

// The mask every video filter can have (params "mask.*", absent = no mask):
// an ellipse or rectangle in the clip's source frame (centre and size as
// fractions of the frame, feather and expansion in sequence pixels) and/or
// an HSL qualifier that selects colours. The filter is applied through it.
const EffectInfo& maskInfo();
bool supportsMask(const std::string& effectType);
bool hasMask(const Effect& e, FrameTime t);

// Builds an Effect of the given type populated with default values.
Effect makeEffect(Project& p, const std::string& type);
Effect makeEffect(const std::string& type, Id id);

// Blend modes supported by the compositor.
const std::vector<std::string>& blendModes();

}  // namespace montage
