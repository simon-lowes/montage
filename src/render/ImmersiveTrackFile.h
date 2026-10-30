// Montage — IMF immersive sound track files read back: IAB (SMPTE ST 2067-201), Metadata-Guided Audio with S-ADM
// (ST 2067-203) and ADM audio (ST 2067-204), each as the scene it carries (render/Adm.h: a bed of speakers and objects
// that move, each on a channel) and the sound of those channels, so a composition opens with a track for each speaker
// and each object, and the media pool plays a channel of such a file as it plays any other (FFmpeg reads none of
// IAB's or MGA's sound).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Adm.h"

namespace montage {

struct ImmersiveTrackInfo {
    enum class Kind { Iab, Mga, Adm } kind = Kind::Adm;
    int channels = 0;      // IAB: the first frame's bed channels, then its objects
    int sampleRate = 48000;
    int64_t samples = 0;   // per channel
};
// What the track file is: false (quickly, from its header) when it is none of those.
bool immersiveTrackInfo(const std::string& mxf, ImmersiveTrackInfo& info, std::string* error = nullptr);

// Its scene: the bed and objects on its channels, times in samples from its start. IAB's objects are where its pan
// sub-blocks put them; its bed channels are the speakers of a cinema's (L, C, R, the side and rear surrounds, the
// top surrounds, LFE).
bool immersiveTrackScene(const std::string& mxf, AdmScene& out, std::string* error = nullptr, std::vector<std::string>* warnings = nullptr);

// Its channels' sound from sample `first`, interleaved 24-bit samples a stretch at a time (`sink` returns false to stop).
bool readImmersiveTrack(const std::string& mxf, int64_t first, const std::function<bool(const int32_t* samples, size_t frames)>& sink,
                        std::string* error = nullptr);

}  // namespace montage
