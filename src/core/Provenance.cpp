#include "Provenance.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "ColorGroups.h"
#include "Effects.h"

namespace montage {

bool Provenance::generated() const {
    return std::any_of(ai.begin(), ai.end(), [](const AiUse& u) { return u.role == AiRole::Generated; });
}

std::string iptcSourceType(AiRole role) {
    switch (role) {
    case AiRole::Generated: return "http://cv.iptc.org/newscodes/digitalsourcetype/compositeWithTrainedAlgorithmicMedia";
    case AiRole::Enhanced: return "http://cv.iptc.org/newscodes/digitalsourcetype/algorithmicallyEnhanced";
    case AiRole::Assisted: break;
    }
    return {};
}

namespace {

std::function<bool(const std::string&)>& optionalModelCheck() {
    static std::function<bool(const std::string&)> check;
    return check;
}

bool optionalModelThere(const std::string& model) { return !optionalModelCheck() || optionalModelCheck()(model); }

}  // namespace

void setOptionalModelCheck(std::function<bool(const std::string& model)> check) { optionalModelCheck() = std::move(check); }

namespace {

void add(Provenance& out, AiUse use) {
    if (std::find(out.ai.begin(), out.ai.end(), use) == out.ai.end()) out.ai.push_back(std::move(use));
}

void effectUses(Provenance& out, const Effect& e) {
    if (!e.enabled) return;
    const std::string& t = e.type;
    if (t == "object_removal") add(out, {"Object Removal", "LaMa", AiRole::Generated});
    else if (t == "super_scale") add(out, {"Super Scale", "Real-ESRGAN", AiRole::Enhanced});
    else if (t == "motion_deblur") add(out, {"Motion Deblur", "NAFNet", AiRole::Enhanced});
    else if (t == "face_refine") add(out, {"Face Refinement", "YuNet", AiRole::Enhanced});
    else if (t == "blemish_remover") add(out, {"Blemish Remover", "YuNet", AiRole::Enhanced});
    else if (t == "face_shape") add(out, {"Face Shape", "YuNet, MediaPipe Face Mesh V2", AiRole::Enhanced});
    else if (t == "look_at_lens") add(out, {"Look at Lens", "YuNet, MediaPipe Face Mesh V2", AiRole::Enhanced});
    else if (t == "lip_sync") add(out, {"Lip Sync", "MuseTalk 1.5, whisper-tiny, sd-vae-ft-mse, YuNet, MediaPipe Face Mesh V2", AiRole::Generated});
    else if (t == "relight") add(out, {"Relight", "Depth Anything V2", AiRole::Enhanced});
    else if (t == "enhance_speech") add(out, {"Enhance Speech", "DeepFilterNet3", AiRole::Enhanced});
    else if (t == "voice_isolate") add(out, {"Voice Isolation", "RNNoise", AiRole::Enhanced});
    else if (t == "redact_faces") add(out, {"Redact Faces", "YuNet", AiRole::Assisted});
    else if (t == "remove_background" || t == "behind_people") add(out, {"People Matte", "MODNet", AiRole::Assisted});
    else if (t == "depth_blur" || t == "depth_fog" || t == "depth_map") add(out, {"Depth", "Depth Anything V2", AiRole::Assisted});
    else if (t == "sky_replace" && e.p("use_depth", 0, 1) > 0.5 && optionalModelThere("depth")) add(out, {"Sky Replacement", "Depth Anything V2", AiRole::Assisted});
    if (t == "depth_blur" && std::lround(e.p("follow", 0)) == 2) add(out, {"Focus on Faces", "YuNet", AiRole::Assisted});
    // Masks: an object picked out, people, or depth.
    const long s = std::lround(e.p("mask.shape", 0));
    if (s == 3 && e.object) add(out, {"Object Mask", "EdgeTAM", AiRole::Assisted});
    if (s == 4) add(out, {"People Matte", "MODNet", AiRole::Assisted});
    if (e.p("mask.depth", 0) > 0.5) add(out, {"Depth", "Depth Anything V2", AiRole::Assisted});
}

void mediaUses(Provenance& out, const MediaItem& m) {
    auto has = [&](const char* k) { return std::find(m.keywords.begin(), m.keywords.end(), k) != m.keywords.end(); };
    const auto note = m.metadata.find("comment");
    const std::string said = note == m.metadata.end() ? std::string() : note->second;
    if (has("Stem"))  // (one part of a mix, split by a model; its note says which)
        add(out, {"Stem Separation", said.find("with Bandit v2") != std::string::npos ? "Bandit v2" : "Spleeter", AiRole::Enhanced});
    if (!has("AI voice")) return;
    if (said.find(" converted to a voice") != std::string::npos) add(out, {"Voice Conversion", "Chatterbox", AiRole::Generated});
    else if (said.find("cloned") != std::string::npos) add(out, {"Cloned Voice", "Chatterbox Multilingual", AiRole::Generated});
    else add(out, {"Generated Speech", "Kokoro", AiRole::Generated});
}

void walk(Provenance& out, const Project& p, const Sequence& s, std::set<Id>& seen, FrameTime from, FrameTime to) {
    if (!seen.insert(s.id).second) return;  // (a sequence nested in itself)
    for (const auto* tracks : {&s.videoTracks, &s.audioTracks})
        for (const Track& t : *tracks) {
            for (const Clip& c : t.clips) {
                // (`to` is exclusive, as an export's out point)
                if (!c.enabled || (from >= 0 && c.end() <= from) || (to >= 0 && c.start >= to)) continue;
                for (const Effect* e : gradeChain(s, c)) effectUses(out, *e);  // (its colour group's effects too)
                if (!c.timing.empty() && std::lround(c.timing.p("sampling", 0)) == 3) add(out, {"AI Frames", "RIFE", AiRole::Generated});
                const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
                if (!m) continue;
                if (m->kind == MediaKind::Sequence) {
                    for (const Sequence& nested : p.sequences)
                        if (nested.id == m->sequenceId) walk(out, p, nested, seen, -1, -1);
                    continue;
                }
                mediaUses(out, *m);
                if (!m->path.empty() && std::find(out.sources.begin(), out.sources.end(), m->path) == out.sources.end())
                    out.sources.push_back(m->path);
            }
            for (const Effect& e : t.effects) effectUses(out, e);
        }
}

}  // namespace

Provenance sequenceProvenance(const Project& p, const Sequence& s, FrameTime from, FrameTime to) {
    Provenance out;
    std::set<Id> seen;
    walk(out, p, s, seen, from, to);
    return out;
}

}  // namespace montage
