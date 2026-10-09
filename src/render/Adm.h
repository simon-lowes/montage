// Montage — immersive masters as ADM BWF: a BW64 file (ITU-R BS.2088) carrying the Audio Definition Model (BS.2076)
// in its axml chunk and the channel assignment in its chna chunk, the interchange format of Dolby Atmos and the EBU's
// renderer (DaVinci Resolve's Fairlight and Pro Tools import and export it; Premiere and Final Cut do not).
//
// The bed is the mix of every track that is not an object, in the sequence's layout (stereo, 5.1, 7.1 or an immersive
// one with overhead speakers), named by the common definitions of BS.2094 so any ADM tool knows the speakers (7.1.2,
// which those definitions lack in Dolby's form, has a pack of its own made of their channels). Each audio track marked
// as an object (SurroundPan::object) becomes an object of its own: its sound after its fader, as one channel, with its
// position (x left to right, y back to front, z up) in the ADM's cartesian coordinates. 48 kHz, 24 bits.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct AdmSettings {
    std::string title;
    bool inOut = false;  // only In to Out (both included)
};

struct AdmResult {
    std::string path;
    int bedChannels = 0;
    int objects = 0;
    int64_t samples = 0;  // per channel, at 48 kHz
    std::string bedPack;  // the bed's audioPackFormat ID
};

// The audio tracks a master would write as objects, in order: those marked as objects that are not muted.
std::vector<int> admObjectTracks(const Sequence& s);

// The bed's pack for a layout (BS.2094 common definitions, or Montage's own for 7.1.2) and each speaker's channel
// format, in the layout's channel order.
std::string admBedPack(const std::string& layout);
std::vector<std::string> admBedChannels(const std::string& layout);

// Writes the master to `path` (.wav). `progress` (0..1) may return false to stop (the file is removed).
bool exportAdmBwf(const Project& p, const Sequence& s, const AdmSettings& settings, const std::string& path, AdmResult* result = nullptr,
                  const std::function<bool(double)>& progress = {}, std::string* error = nullptr);

// What a BW64 file holds: its channels, sample rate, bits and frames, its chna entries (track UID, track format,
// pack) and its axml text; false when it is not a readable BW64/RIFF WAVE file.
struct BwfInfo {
    int channels = 0, sampleRate = 0, bits = 0;
    int64_t frames = 0;
    struct Track {
        int index = 0;
        std::string uid, trackFormat, pack;
    };
    std::vector<Track> chna;
    std::string axml;
};
bool readBwfInfo(const std::string& path, BwfInfo& info, std::string* error = nullptr);

}  // namespace montage
