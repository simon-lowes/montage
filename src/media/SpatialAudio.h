// Montage — spatial audio metadata: Google's Spatial Audio box (SA3D) in an MP4 or MOV file's audio sample entry,
// which tells YouTube and 360° players that the track's channels are an ambisonic field (AmbiX: ACN channel order,
// SN3D normalisation) rather than speakers. FFmpeg reads it (the stream's layout becomes ambisonic) but does not
// write it, so Montage adds it to the files it exports.
#pragma once

#include <string>

namespace montage {

// Adds an SA3D box of `order` (1 = first order, four channels) to each audio track of an MP4/MOV file with that many
// channels and none yet (none such: nothing to do). The media data stay where they are (chunk offsets are moved when
// the movie header comes first). A movie header at the end is rewritten after it, the old one left as free space only
// once the new one is written, so nothing is copied and a failure leaves the file as it was; otherwise the file is
// written again beside itself and swapped in.
bool writeSpatialAudioBox(const std::string& path, int order, std::string* error = nullptr);

// The ambisonic order the first audio track with an SA3D box declares, or 0 when none has one (or the file is not
// MP4/MOV).
int readSpatialAudioBox(const std::string& path);

}  // namespace montage
