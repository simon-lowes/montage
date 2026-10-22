// Montage — film counts: feet and frames for the common gauges and pulldowns, and the numbers printed along a negative
// and a workprint that a cut list names shots by. A KeyKode key number is the film maker's and stock's code, the
// roll's prefix and a count printed every foot of 35 mm (every half foot of 16 mm), read by the scanner into each
// DPX or Cineon frame's film header; an ink number is a code and count a foot printed on the workprint and the
// synced sound. A frame is named by the number before it and how far past it it is ("KJ 23 1234 5678+05").
//
// Feet and frames count perforations: 35 mm and 65 mm film have 64 a foot, 16 mm 40. A 4-perf 35 mm frame is 4 (16
// frames a foot), a 3-perf one 3 (64 frames in 3 feet), a 2-perf one 2, a 16 mm frame 1 and a 65 mm 5-perf frame 5.
// Where a foot is not a whole number of frames, the perforation a frame starts on is given too ("123+05.1").
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Model.h"

namespace montage {

struct FilmGauge {
    const char* id;    // "35mm-4perf", "35mm-3perf", "35mm-2perf", "16mm", "65mm-5perf"
    const char* name;  // "35 mm 4-perf (16 frames a foot)"
    int perfsPerFrame;
    int perfsPerFoot;
    int perfsPerKey;  // between key numbers: a foot of 35 mm (64), 120 perforations of 65 mm, half a foot of 16 mm (20)
};
const std::vector<FilmGauge>& filmGauges();
// The gauge named `id`; 35 mm 4-perf for any other.
const FilmGauge& filmGauge(const std::string& id);

// Feet and frames of frame `frame` counted from 0+00 ("-0+03" before it).
std::string formatFeetFrames(int64_t frame, const FilmGauge& g);
// "123+05", "123+05.1", "-2+03", "123" (feet alone); false for anything else, or a frame past the foot.
bool parseFeetFrames(const std::string& text, const FilmGauge& g, int64_t& frame);

struct FilmNumber {
    std::string prefix;  // "KJ 23 1234" (a key number) or "123A" (an ink number)
    int64_t count = 0;
    int perfs = 0;       // past the count (0 .. the spacing - 1)
    bool ink = false;    // an ink number: a count a foot (key numbers: the gauge's perfsPerKey)
    bool valid() const { return !prefix.empty(); }
    bool operator==(const FilmNumber&) const = default;
};
int filmNumberSpacing(const FilmNumber& n, const FilmGauge& g);  // perforations between counts
// "KJ 23 1234 5678+05" (the count four digits or more; the frames past it, and the perforation where the gauge needs).
std::string formatFilmNumber(const FilmNumber& n, const FilmGauge& g);
// The prefix, then the count and "+frames" (".perf" too); key numbers' prefixes as printed ("KJ 23 1234",
// "KJ231234") or as a DPX header has them.
bool parseFilmNumber(const std::string& text, const FilmGauge& g, FilmNumber& out, bool ink = false);
// `frames` frames on (or back) along the roll.
FilmNumber advanceFilmNumber(FilmNumber n, int64_t frames, const FilmGauge& g);
// Frames from `a` to `b` on one roll (the same prefix); nullopt across rolls.
std::optional<int64_t> filmNumberFrames(const FilmNumber& a, const FilmNumber& b, const FilmGauge& g);

// The gauge a media item's key and ink numbers count its frames in: its own ("film_gauge", worked out from a scan's
// headers), else `fallback` (the sequence's).
const FilmGauge& mediaFilmGauge(const MediaItem& m, const FilmGauge& fallback);
// The key number ("key_number") or ink number ("ink_number") of frame `frame` of a media item (its own frames, from
// its first), from its first frame's in its metadata, in gauge `g` (mediaFilmGauge).
std::optional<FilmNumber> mediaFilmNumber(const MediaItem& m, bool ink, int64_t frame, const FilmGauge& g);
// The same as text in the media's gauge ("" where it has none).
std::string mediaFilmNumberText(const MediaItem& m, bool ink, int64_t frame, const FilmGauge& fallback);
// The frame of media `m` (its own frames, from its first) clip `c` of sequence `s` shows at timeline frame `t`; outside
// the clip, the handle it would show.
int64_t clipMediaFrame(const Clip& c, FrameTime t, const Sequence& s, const MediaItem& m);

// What a DPX or Cineon file's film header says: its KeyKode (manufacturer and film type codes, prefix, count and
// perforation offset), format ("Academy", "Full Aperture"...), frame rate, and the frame's position in its scan.
struct FilmScanHeader {
    bool hasKeyKode = false;
    FilmNumber key;  // perfs: the header's perforation offset
    std::string format, frameId, slate;
    double frameRate = 0;
    int64_t framePosition = -1;
};
bool readFilmScanHeader(const std::string& path, FilmScanHeader& out);
// The gauge of a scan: the one in which frame `next` follows `first` (their key numbers that gauge's frame apart);
// with no next frame, 16 mm where the format says so, else 35 mm 4-perf where the perforation offset is a whole number
// of 4-perf frames, else 3-perf.
const FilmGauge& filmScanGauge(const FilmScanHeader& first, const FilmScanHeader* next = nullptr);
// The key number as Montage keeps it ("key_number"), its frames past the count by gauge `g`.
std::string filmScanKeyNumber(const FilmScanHeader& h, const FilmGauge& g);

}  // namespace montage
