// Montage — checking and fixing subtitles as a whole, as subtitle editors do
// (Subtitle Edit, Resolve 21.1's subtitle editor): reading speed, line
// length and timing against the limits a delivery asks for (by default the
// Netflix Timed Text Style Guide's), timing against shot changes, a timing
// fix, shifting, syncing to two points, and find and replace.
#pragma once

#include <string>
#include <vector>

#include "Captions.h"
#include "Model.h"

namespace montage {

struct CaptionLimits {
    double maxCps = 20;             // characters a second (spaces and punctuation count, line breaks do not)
    int maxLineChars = 42;
    int maxLines = 2;
    double minSeconds = 5.0 / 6.0;  // 20 frames at 24 fps
    double maxSeconds = 7.0;
    int minGapFrames = 2;           // between one caption and the next
    double chainSeconds = 0.5;      // gaps shorter than this close up to the minimum gap when fixing
    // Shot changes (Netflix's half-second rule): no caption starts or ends this close to one, except starting on it
    // or ending minGapFrames before it. 0: shot changes are not looked at.
    double shotZoneSeconds = 0.5;
};

enum CaptionIssue : unsigned {
    kCaptionTooFast = 1,       // more characters a second than maxCps
    kCaptionLineTooLong = 2,   // a line longer than maxLineChars
    kCaptionTooManyLines = 4,
    kCaptionTooShort = 8,      // on screen less than minSeconds
    kCaptionTooLong = 16,      // on screen more than maxSeconds
    kCaptionGapTooSmall = 32,  // the next caption follows within minGapFrames
    kCaptionNearShotChange = 64,     // starts or ends within the zone of a shot change (but not on it, or the gap before it)
    kCaptionCrossesShotChange = 128,  // stays up across a shot change the speech may not (known from word times: it does not)
};

// The zone round a shot change, in frames: half a second is 12 frames at 23.976, 24 and 25 fps, 15 at 29.97 and 30,
// 30 at 59.94 and 60 (halves round down). 0 when the limits do not look at shot changes.
int shotChangeZone(Rational fps, const CaptionLimits& limits = {});
// Where the sequence's shot changes are: its cuts (the picture's clip edges on tracks shown, not under a picture on a
// track above, not a through edit or a transition, not titles or other generators) and its shot-change markers
// (Marker::shotChange: scene detection's on a clip's source, where the clip shows them now; those given on the
// sequence), sorted, after its first frame.
std::vector<FrameTime> captionShotChanges(const Sequence& seq);

// Characters a second for one caption.
double captionCps(const Caption& c, Rational fps);
// For each caption, the CaptionIssue flags it breaks (0 for none).
// With `shotChanges` (sorted frames), captions are also checked against them.
std::vector<unsigned> checkCaptions(const std::vector<Caption>& captions, Rational fps, const CaptionLimits& limits = {},
                                    const std::vector<FrameTime>& shotChanges = {});
// What is wrong with a caption, in words ("23 characters a second (20 at most)"), one issue a line.
std::string describeCaptionIssues(unsigned issues, const Caption& c, Rational fps, const CaptionLimits& limits = {});

// Fixes what timing can: each caption ends at least minGapFrames before the
// next, short or fast ones stay up longer into the time after them (not past
// maxSeconds or the next caption), and gaps shorter than chainSeconds close
// up to the minimum gap. Captions never start later or move. Returns how many
// changed.
// With `shotChanges` (sorted frames): a caption starting within the zone after a shot change starts on it; one ending
// within the zone before a shot change ends minGapFrames before it, and so does one ending on it or within the zone
// after it (or one its word times show the speech has finished by it) where it is still long enough to read; and
// captions are not lengthened across one. Captions starting within the zone before a shot change, and ones that would
// be too short, are left for the check to report.
int fixCaptionTiming(std::vector<Caption>& captions, Rational fps, const CaptionLimits& limits = {},
                     const std::vector<FrameTime>& shotChanges = {});

// Moves the captions at `indices` (all when empty) by `delta` frames, not
// before frame 0. False if nothing moved.
bool shiftCaptions(std::vector<Caption>& captions, const std::vector<size_t>& indices, FrameTime delta);
// Retimes every caption so the one at `fromA` lands on `toA` and the one at
// `fromB` on `toB`, stretching linearly in between (subtitles made for another
// frame rate or cut). False if fromA == fromB.
bool syncCaptions(std::vector<Caption>& captions, FrameTime fromA, FrameTime toA, FrameTime fromB, FrameTime toB);
// Replaces `find` in the captions at `indices` (all when empty), as a whole
// word when asked. Returns how many were replaced.
int replaceInCaptions(std::vector<Caption>& captions, const std::vector<size_t>& indices, const std::string& find,
                      const std::string& replace, bool caseSensitive = false, bool wholeWords = false);

// Puts the chosen captions (all when none are chosen) in a place: up or down
// (`vertical`, or -1 to keep each one's) and lined up (`align`, or -1 to keep).
// False if none moved.
bool placeCaptions(std::vector<Caption>& captions, const std::vector<size_t>& indices, int vertical, int align);

// Captions on screen while a title sits in the lower part of the frame (a lower
// third, a crawl, a title placed low) move to the top, as subtitle style guides
// ask, keeping their alignment. Returns how many moved.
int raiseCaptionsOverTitles(std::vector<Caption>& captions, const Sequence& seq);

}  // namespace montage
