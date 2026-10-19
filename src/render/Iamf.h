// Montage — immersive sound as IAMF (the Alliance for Open Media's Immersive Audio Model and Formats, v1.1), the format
// YouTube takes immersive and ambisonic uploads in: an MP4 whose audio track is IAMF (or the bare IAMF stream).
//
// A surround or immersive sequence (5.1, 7.1, 5.1.2, 5.1.4, 7.1.2, 7.1.4) becomes a channel-based audio element in its
// own loudspeaker layout, its objects rendered into the bed as the mix pans them; an ambisonic sequence (first to third
// order, AmbiX) a scene-based one, each component a substream (sixteen at third order, within IAMF's simple profile).
// One mix presentation plays it, rendered binaurally on headphones (or as stereo), and carries the loudness it measures
// at (BS.1770 integrated loudness, sample peak and true peak) in the element's own layout and in stereo, so players
// level it as delivery loudness does. FLAC by default (lossless; YouTube re-encodes it), or 24-bit PCM. 48 kHz.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct IamfSettings {
    std::string codec = "flac";  // "flac" or "lpcm"
    bool inOut = false;          // only In to Out (both included)
    bool binaural = true;        // on headphones: rendered binaurally (else as stereo)
    std::string title;           // the mix presentation's name
};

// One loudness the mix presentation carries: for a layout ("stereo", "7.1.4"...), in LUFS and dBFS / dBTP.
struct IamfLoudness {
    std::string layout;
    double integrated = 0, digitalPeak = 0, truePeak = 0;
};

struct IamfResult {
    std::string path, layout, codec;
    int channels = 0, substreams = 0, coupled = 0;
    int64_t samples = 0;  // per channel, at 48 kHz
    int frames = 0;       // temporal units
    std::vector<IamfLoudness> loudness;
};

// The IAMF loudspeaker layout of a sequence's layout (2 for 5.1 ... 7 for 7.1.4; 1 stereo), -1 for an ambisonic or
// unknown one; and its sound system in mix presentations (BS.2051's A, B, C...: 0, 1, 2...), -1 when it has none.
int iamfLoudspeakerLayout(const std::string& layout);
int iamfSoundSystem(const std::string& layout);
// For each substream, the layout's channels in it (two for a coupled pair), in IAMF's order: the pairs first.
std::vector<std::vector<int>> iamfSubstreams(const std::string& layout);

// Folds a layout's channels to stereo as IAMF's reference renderer (libiamf) plays them: a bed by IAMF's downmix
// matrices, an ambisonic field (to third order) by its decoders.
void iamfDownmixToStereo(const std::string& layout, const float* in, int frames, float* out);

// Writes the sequence's sound to `path`: an MP4 (.mp4, .m4a) or the bare stream (.iamf). Returning false from
// `progress` stops it and removes the file.
bool exportIamf(const Project& p, const Sequence& s, const IamfSettings& settings, const std::string& path,
                IamfResult* result = nullptr, const std::function<bool(double)>& progress = {}, std::string* error = nullptr);

// An IAMF file's descriptors as read back (an MP4's iacb box, or the bare stream's first OBUs) and its frames counted.
struct IamfInfo {
    int primaryProfile = -1, additionalProfile = -1;
    std::string codec;  // the codec config's four characters ("fLaC", "ipcm", "Opus", "mp4a")
    int samplesPerFrame = 0, sampleRate = 0, bitDepth = 0;
    bool sceneBased = false;
    int loudspeakerLayout = -1;  // a channel-based element's
    int ambisonicChannels = 0;   // a scene-based element's
    int substreams = 0, coupled = 0;
    int headphonesMode = -1;  // 0 stereo, 1 binaural
    std::string title;
    std::vector<int> loudnessSoundSystems;  // per loudness: its sound system (-2 binaural)
    std::vector<IamfLoudness> loudness;     // the same order (layout names Montage's where known)
    int frames = 0;                         // temporal units (an MP4's samples)
    int64_t trimmedAtEnd = 0;
};
bool readIamfInfo(const std::string& path, IamfInfo& out, std::string* error = nullptr);

}  // namespace montage
