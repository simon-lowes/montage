// Montage — media handed over as Media Composer keeps its own: MXF OP-Atom files, one for each track (VC-3 picture,
// DNxHD or DNxHR, and mono 24-bit PCM sound), in a numbered folder of a drive's "Avid MediaFiles/MXF". Media Composer
// indexes such folders when it looks over its drives, so an AAF whose file mobs carry the files' package IDs
// (render/AafExport.h) opens linked to them, with no linking to camera files or importing. FFmpeg's OP-Atom muxer
// writes the files and their package IDs are read back for the AAF.
#pragma once

#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct AudioBuffer;

// The picture codings offered: "dnxhr_lb", "dnxhr_sq", "dnxhr_hq", "dnxhr_hqx" (10-bit), "dnxhr_444" (10-bit 4:4:4),
// and DNxHD at 1920 x 1080 only, "dnxhd_lb" (36 to 90 Mb/s by frame rate), "dnxhd_sq" (115 to 290), "dnxhd_hq" (175 to
// 440), 8-bit.
struct AvidCodec {
    const char* id;
    const char* name;
};
const std::vector<AvidCodec>& avidCodecs();
// Why `codec` cannot code a w x h picture at `fps` ("" when it can).
std::string avidCodecProblem(const std::string& codec, int width, int height, double fps);
// FFmpeg's settings for it: the dnxhd encoder's profile, the pixel format and the bit rate (0 = the profile's own).
void avidCodecSettings(const std::string& codec, double fps, std::string& profile, std::string& pixFmt, int64_t& bitrate);

// The next free numbered folder (1, 2, ...) in "Avid MediaFiles/MXF" on `drive`, which must exist (or in `drive` itself
// when it is such an "Avid MediaFiles" or MXF folder), made, with the folders above it; those it made are added to
// `made`. "" with `error` if it cannot be made.
std::string avidMediaFolder(const std::string& drive, std::string* error = nullptr, std::vector<std::string>* made = nullptr);

struct OpAtomFile {
    bool opAtom = false;  // its operational pattern is OP-Atom
    bool picture = false;
    std::string codec;                         // FFmpeg's name: "dnxhd", "pcm_s24le"
    std::string profile;                       // FFmpeg's: "DNXHR SQ", "DNXHD"
    std::string materialPackage, filePackage;  // as AAF mob IDs
    std::string tapePackage;                   // the tape the file package is from, when it names one
    Rational editRate{25, 1};                  // the picture's rate, or the sound's sample rate
    int64_t length = 0;                        // in edit units
    int width = 0, height = 0;
    std::string timecode, name, reel;
};
// What an MXF file is, as an AAF needs to know to point at it.
bool readOpAtom(const std::string& path, OpAtomFile& out, std::string* error = nullptr);

// Channel `channel` of `buf` (at its sample rate) as a mono 24-bit PCM OP-Atom file whose timecode starts at `timecode`
// (at `rate`, "" = 00:00:00:00), its clip and tape named.
bool writeOpAtomSound(const std::string& path, const AudioBuffer& buf, int channel, Rational rate, const std::string& timecode,
                      const std::string& clipName, const std::string& reel, std::string* error = nullptr);

}  // namespace montage
