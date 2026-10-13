// Montage — head-related impulse responses from SOFA files (AES69, the SimpleFreeFieldHRIR convention), for hearing
// ambisonic sound binaurally (core/Ambisonics.h): a measured head (by default SADIE II's Neumann KU100, downloaded on
// first use; Apache-2.0) or a personal set. Read with libmysofa when Montage is built with it.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/Ambisonics.h"
#include "media/ModelFiles.h"

namespace montage {

// Whether SOFA files can be read (Montage built with libmysofa).
bool sofaSupported();

// The HRIRs a SOFA file holds at binauralDirections() (each the nearest measured), resampled to `sampleRate`, the
// longest 512 taps at 48 kHz. A zip holding SOFA files is read too (its 48 kHz one when it has several). Null with the
// reason when it cannot be read.
std::shared_ptr<const Hrirs> readSofa(const std::string& path, double sampleRate, std::string* error = nullptr);

// SADIE II's KU100 dummy head (University of York; Apache-2.0): the HRIRs in SOFA, in their zip.
const ModelPack& sadieKu100Pack();

// How ambisonic sound is heard binaurally (monitoring, and stereo mixes decoding a field binaurally): "" the spherical
// head model, "ku100" SADIE II's KU100 (when its pack is installed), or a SOFA file's path. Process-wide.
void setBinauralHrtf(const std::string& choice);
std::string binauralHrtf();
// Changes each time the choice does (renderers made before then are out of date).
uint64_t binauralHrtfGeneration();
// The HRIRs for the choice at a rate (read once and kept); null for the spherical head, or when they cannot be read
// (`error` says why, and the spherical head is used). Without `wait` (playback), a set not read yet is read in the
// background and null returned meanwhile; the generation changes when it is in.
std::shared_ptr<const Hrirs> binauralHrirs(double sampleRate, bool wait = true, std::string* error = nullptr);

}  // namespace montage
