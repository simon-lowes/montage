// Montage — numbered image sequences (VFX plates and renders, animation, time-lapse): shot_1001.exr to shot_1048.exr
// as one clip, as Premiere, Resolve and Avid bring them in. A sequence's media path is FFmpeg's pattern for its
// files with its frame rate and first number after a separator no file name holds, so everything that opens media
// by path (decoders, thumbnails, the frame cache) opens it like a movie: "dir/shot_%04d.exr\x1e" "24,1:1001-1048"
// (no "/" after the separator, so path functions see one file name).
#pragma once

#include <string>

#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

struct ImageSequence {
    std::string pattern;  // the files, with the number as %d or %0Nd
    int first = 0, last = 0;
    Rational fps{24, 1};
    int frames() const { return last - first + 1; }
};

bool isImageSequencePath(const std::string& path);
bool parseImageSequencePath(const std::string& path, ImageSequence& out);
std::string imageSequencePath(const ImageSequence& s);
// The file holding frame `n` (in the sequence's own numbering).
std::string imageSequenceFrame(const ImageSequence& s, int n);
// "shot_[1001-1048].exr".
std::string imageSequenceName(const ImageSequence& s);
// The file a media path is on disk: itself, or a sequence's first frame.
std::string mediaFileOnDisk(const std::string& path);

// The unbroken numbered run `file` belongs to: the same folder, the same name either side of the number and the
// same way of writing it (padded to a width or not), from `file`'s number down and up while the files are there.
// False for a file with no number or alone. A gap ends the run.
bool detectImageSequence(const std::string& file, ImageSequence& out);
// Formats rendered as frames (EXR, DPX, PNG, TIFF, TGA, BMP), grouped into sequences on import; camera stills
// (JPEG, HEIC...) are grouped only when asked.
bool isFrameFormat(const std::string& file);
// A frame rate as typed: whole numbers exact, the NTSC rates (23.976, 29.97, 59.94...) as n/1001.
Rational rateFor(double fps);

namespace edit {

// Interpret Footage on an image sequence: its frames played at `fps`. Clips keep starting on the same frame and keep
// their length on the timeline, shortened where the sequence no longer reaches.
Result setImageSequenceRate(Project& p, Id media, Rational fps);

}  // namespace edit

}  // namespace montage
