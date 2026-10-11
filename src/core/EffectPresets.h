// Montage — effect presets (Premiere's effect presets, Resolve's PowerGrades): a clip's effects saved under a name,
// to put on other clips and in other projects. Keyframes keep their place in the clip.
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace montage {

struct EffectPreset {
    std::string name;
    bool video = true;            // video effects (else audio effects)
    std::vector<Effect> effects;  // in order
};

// A preset file (".montagepreset", JSON).
std::string presetToJson(const EffectPreset& p);
bool presetFromJson(const std::string& json, EffectPreset& out, std::string* error = nullptr);
// Adds the preset's effects after the clip's own, with new ids; how many were added.
int applyPreset(Project& p, Clip& c, const EffectPreset& preset);

}  // namespace montage
