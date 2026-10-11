// Montage — what went into a sequence, for its Content Credentials (media/ContentCredentials.h): the media files it
// plays (through nested sequences) and each use of an AI model in it, sorted by what the model did: made new content
// (cloned or generated speech, voice conversion, object removal, AI frame interpolation), changed what was there (Super
// Scale, Face Refinement, Relight, speech enhancement...) or only helped the edit (object and people masks, depth,
// finding faces to redact).
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace montage {

enum class AiRole {
    Generated,  // new content made by a model (IPTC compositeWithTrainedAlgorithmicMedia)
    Enhanced,   // existing content changed by a model (IPTC algorithmicallyEnhanced)
    Assisted,   // a model chose where an edit applies; the content is the editor's
};

struct AiUse {
    std::string feature;  // "Object Removal"
    std::string model;    // "LaMa"
    AiRole role = AiRole::Assisted;
    bool operator==(const AiUse&) const = default;
};

struct Provenance {
    std::vector<AiUse> ai;             // each once, in the order first met
    std::vector<std::string> sources;  // the media files played, each once
    bool generated() const;            // any AiRole::Generated
};

// What the enabled clips of the sequence between `from` and `to` (timeline frames, -1: its start or end), and the
// sequences nested in them, use.
Provenance sequenceProvenance(const Project& p, const Sequence& s, FrameTime from = -1, FrameTime to = -1);

// The IPTC digital source type for a role ("" for Assisted).
std::string iptcSourceType(AiRole role);

}  // namespace montage
