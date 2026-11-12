// Montage — what a field recorder writes into its WAV files: Broadcast Wave's
// bext chunk (when the take began, as samples since midnight, and a
// description that recorders fill with "sSCENE=12A" lines) and the iXML chunk
// (project, scene, take, tape, notes, circled takes, the timecode rate and
// each channel's name). FFmpeg reads only part of the first, so the chunks
// are read here, before and after the sound (RIFF, RF64 and BW64).
#pragma once

#include <cstdint>
#include <map>
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
    std::string description;              // bext's description as written (a recorder's lines, a library's words)
    // iXML's USER and ASWG fields (what sound libraries write: DESCRIPTION, CATEGORY, KEYWORDS, LIBRARY...), by
    // upper-case name.
    std::map<std::string, std::string> user;
    bool any() const { return startSeconds >= 0 || !scene.empty() || !take.empty() || !tape.empty() || !trackNames.empty(); }
};

// False if the file is not a WAV or carries neither chunk.
bool readFieldRecording(const std::string& path, FieldRecording& out);

// Gives a media item what the recording says: its start timecode (when it has
// none) and the logging fields it has not got: scene, take, tape, project,
// comment (the sound recordist's note), channel names and circled takes.
void applyFieldRecording(const FieldRecording& f, MediaItem& m);

// What a WAV Montage writes is stamped with: when its first sample was in the source, in seconds since midnight
// (written as Broadcast Wave's time reference: samples at the file's rate, to the nearest), and the take's logging
// fields.
struct BroadcastStamp {
    double seconds = 0;
    Rational timecodeRate{25, 1};
    bool dropFrame = false;
    std::string description, project, scene, take, tape, note;
};
// Writes (or replaces) the file's bext chunk (version 1, the time reference and description) and iXML chunk (the
// same time as TIMESTAMP_SAMPLES_SINCE_MIDNIGHT with its sample rate, the timecode rate, and the fields), before its
// sound; RIFF and RF64 both. The sound is copied untouched, through a file beside it.
bool writeBroadcastStamp(const std::string& path, const BroadcastStamp& stamp, std::string* error = nullptr);

}  // namespace montage
