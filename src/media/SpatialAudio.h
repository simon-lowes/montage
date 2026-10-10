// Montage — spatial audio metadata: Google's Spatial Audio box (SA3D) in an MP4 or MOV file's audio sample entry,
// which tells YouTube and 360° players that the track's channels are an ambisonic field (AmbiX: ACN channel order,
// SN3D normalisation) rather than speakers. FFmpeg reads it (the stream's layout becomes ambisonic) but does not
// write it, so Montage adds it to the files it exports.
#pragma once

#include <string>

namespace montage {

// Adds an SA3D box of `order` (1 = first order, four channels) to the first audio track of an MP4/MOV file, unless it
// has one. The file's media data stay where they are (chunk offsets are moved when the movie header comes first).
bool writeSpatialAudioBox(const std::string& path, int order, std::string* error = nullptr);

// The ambisonic order the first audio track's SA3D box declares, or 0 when there is none (or the file is not MP4/MOV).
int readSpatialAudioBox(const std::string& path);

}  // namespace montage
