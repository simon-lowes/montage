// Montage — what a field recorder writes into its WAV files: Broadcast Wave's
// bext chunk (when the take began, as samples since midnight, and a
// description that recorders fill with "sSCENE=12A" lines) and the iXML chunk
// (project, scene, take, tape, notes, circled takes, the timecode rate and
// each channel's name). FFmpeg reads only part of the first, so the chunks
// are read here, before and after the sound (RIFF, RF64 and BW64).
#pragma once

#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct FieldRecording {
    double startSeconds = -1;      // when the take began, seconds since midnight; -1 = not stamped
    Rational timecodeRate{0, 1};   // the production's timecode rate, if given
    bool dropFrame = false;
    std::string project, scene, take, tape, note;
    bool circled = false;
    std::vector<std::string> trackNames;  // in channel order
    bool any() const { return startSeconds >= 0 || !scene.empty() || !take.empty() || !tape.empty() || !trackNames.empty(); }
};

// False if the file is not a WAV or carries neither chunk.
bool readFieldRecording(const std::string& path, FieldRecording& out);

// Gives a media item what the recording says: its start timecode (when it has
// none) and the logging fields it has not got: scene, take, tape, project,
// comment (the sound recordist's note), channel names and circled takes.
void applyFieldRecording(const FieldRecording& f, MediaItem& m);

}  // namespace montage
