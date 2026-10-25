// Montage — what a music file says about itself, for the cue sheet (core/Paperwork.h): its title, composer,
// publisher, performing-rights society, library, catalogue number, ISRC and ISWC from its tags (ID3, Vorbis comments,
// MP4 and RIFF INFO through FFmpeg; a WAV's BWF description and iXML USER and ASWG fields), else the sound library's
// index; and whether it is sung, from Separate Stems' vocal stem.
#pragma once

#include <map>
#include <string>

#include "core/Model.h"
#include "core/Paperwork.h"

namespace montage {

// The cue fields (core/Paperwork.h kCueFields) a file's tags give; missing ones are left out.
std::map<std::string, std::string> musicTags(const std::string& path);
// The same for a media item (its file, then the sound library's entry for it): a MusicTagReader.
std::map<std::string, std::string> musicTagsFor(const MediaItem& m);

// Whether a cue is sung: the share of its sound in the vocal stem (the separation of up to `seconds` from the middle
// of its first clip by the music model installed: two stems, else four or five). False with a reason when the stem
// model is missing or the media cannot be read.
bool cueIsVocal(const Project& p, const Sequence& seq, const MusicCue& cue, bool& vocal, double* vocalShare = nullptr,
                double seconds = 30, std::string* error = nullptr);

}  // namespace montage
