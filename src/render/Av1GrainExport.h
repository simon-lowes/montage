// Montage — Carry Grain as Parameters: an AV1 export whose grain is taken out of the picture before it is encoded and
// written beside it as AV1 film grain parameters, a set for each shot, so the decoder puts it back (render/Av1FilmGrain.h,
// render/Av1Headers.h). Each shot's grain is measured as Clip › Measure Grain measures it (or given), the picture is
// cleaned as Replace Grain cleans it (Video Noise Reduction, the grain's own strength at each light), the cleaned
// sequence is exported as any other, and the file's frame headers are then written again with each shot's parameters
// and a new seed every frame. Any AV1 encoder serves: the parameters are Montage's own writing, not the encoder's.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "render/Av1FilmGrain.h"
#include "render/Exporter.h"
#include "render/GrainMatch.h"

namespace montage {

// One shot of such an export: its frames (from the export's first), the grain it carries (apply false: none), where
// that came from ("measured", "given" or "" for none) and why there is none where there is not.
struct Av1GrainShot {
    FrameTime first = 0, end = 0;
    GrainProfile profile;
    Av1FilmGrain grain;  // (its seed is the shot's first frame's; each frame has its own)
    std::string source, note;
};
struct Av1GrainReport {
    std::vector<Av1GrainShot> shots;
    int64_t framesWithGrain = 0, framesByReference = 0, bytesAdded = 0;
};

// Whether ExportSettings::filmGrain asks for grain carried as parameters ("measure", or a grain profile's text as
// grainProfileText writes it), and whether it is well formed ("", "off", "measure" or a profile). False with `error`
// for anything else.
bool exportCarriesGrain(const ExportSettings& s);
bool checkFilmGrainSetting(const std::string& v, std::string* error = nullptr);

// The shots of an export of [in, out) (sequence frames: the cuts and shot changes, as per-shot ladders find them) and
// each one's grain in the sequence's working space: measured from the topmost clip of media at the shot's middle
// (`filmGrain` "measure"), or the profile `filmGrain` holds. Their `grain` is not yet set.
std::vector<Av1GrainShot> av1GrainShots(const Project& p, const Sequence& seq, FrameTime in, FrameTime out, const std::string& filmGrain,
                                        const std::atomic<bool>* cancel = nullptr, const std::function<void(double)>& progress = {});
// Each shot's grain fitted to AV1's parameters, for a coded picture `target` in `delivery` (the profiles being in
// `working`).
void fitAv1GrainShots(std::vector<Av1GrainShot>& shots, const ColorSpace& working, const ColorSpace& delivery, const Av1GrainTarget& target);

// The export as exportSequence makes it, with its grain carried as parameters (s.filmGrain): see above. `report`, when
// given, gets each shot's grain and what was written.
bool exportCarryingGrain(const Project& p, const Sequence& seq, const ExportSettings& s, const ExportProgress& progress,
                         const std::atomic<bool>* cancel, std::string* error, std::string* encoderUsed = nullptr,
                         LightLevels* light = nullptr, bool* keptUnsigned = nullptr, Av1GrainReport* report = nullptr);

// An AV1 file's frame headers written again, in place, with the film grain parameters `grain` gives for each frame
// shown (counted from the first; apply false: none), its other streams and the rest of its AV1 copied as they are.
bool carryAv1Grain(const std::string& path, const std::function<Av1FilmGrain(int64_t frame)>& grain, std::string* error = nullptr,
                   Av1GrainReport* report = nullptr);

}  // namespace montage
