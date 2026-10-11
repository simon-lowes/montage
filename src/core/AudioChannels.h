// Montage — source audio channels (Premiere's Modify > Audio Channels, Resolve's Clip Attributes > Audio, Final
// Cut's audio configuration): which of a file's channels a clip plays, and splitting a clip into a clip per channel
// (a lav and a boom recorded on one camera, a field recorder's polyphonic WAV, an MXF with a mono stream per channel).
#pragma once

#include <string>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

// Every audio channel in the media, across its audio streams (0 without sound).
int sourceChannelCount(const MediaItem& m);
// What each channel is called: the recorder's track names when it wrote them (Boom, Lav 1...), else "Channel n".
std::vector<std::string> sourceChannelNames(const MediaItem& m);
// A short label for channels a clip plays: "Boom", "Ch 1+2", "" for the default mix.
std::string channelsLabel(const MediaItem& m, const std::vector<int>& channels);

// MediaItem::audioChannelMode values: how new clips of the media take its channels.
inline constexpr const char* kChannelsMix = "";        // the main stream mixed to stereo (the default)
inline constexpr const char* kChannelsMono = "mono";   // a mono clip per channel, on tracks below each other
inline constexpr const char* kChannelsPairs = "pairs";  // a stereo clip per pair of channels (1+2, 3+4...)

namespace edit {

// Plays `channels` (0-based, each once, of sourceChannelCount) in the clip, or the default mix when empty. A
// picture clip gives them to the sound clips linked to it.
Result setClipChannels(Project& p, Sequence& s, Id clip, const std::vector<int>& channels);
// One clip per channel (`pairs`: per pair of channels) of the sound clip (or the sound linked to a picture clip):
// the clip keeps the first, copies on the audio tracks below it (the first with room, new ones when none) take the
// others, all linked together with the clip's picture and named for their channels. The copies are in `created`.
Result splitAudioChannels(Project& p, Sequence& s, Id clip, bool pairs = false);

}  // namespace edit

}  // namespace montage
