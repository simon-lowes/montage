// Montage — checking and fixing subtitles as a whole, as subtitle editors do
// (Subtitle Edit, Resolve 21.1's subtitle editor): reading speed, line
// length and timing against the limits a delivery asks for (by default the
// Netflix Timed Text Style Guide's), a timing fix, shifting, syncing to two
// points, and find and replace.
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
};

enum CaptionIssue : unsigned {
    kCaptionTooFast = 1,       // more characters a second than maxCps
    kCaptionLineTooLong = 2,   // a line longer than maxLineChars
    kCaptionTooManyLines = 4,
    kCaptionTooShort = 8,      // on screen less than minSeconds
    kCaptionTooLong = 16,      // on screen more than maxSeconds
    kCaptionGapTooSmall = 32,  // the next caption follows within minGapFrames
};

// Characters a second for one caption.
double captionCps(const Caption& c, Rational fps);
// For each caption, the CaptionIssue flags it breaks (0 for none).
std::vector<unsigned> checkCaptions(const std::vector<Caption>& captions, Rational fps, const CaptionLimits& limits = {});
// What is wrong with a caption, in words ("23 characters a second (20 at most)"), one issue a line.
std::string describeCaptionIssues(unsigned issues, const Caption& c, Rational fps, const CaptionLimits& limits = {});

// Fixes what timing can: each caption ends at least minGapFrames before the
// next, short or fast ones stay up longer into the time after them (not past
// maxSeconds or the next caption), and gaps shorter than chainSeconds close
// up to the minimum gap. Captions never start later or move. Returns how many
// changed.
int fixCaptionTiming(std::vector<Caption>& captions, Rational fps, const CaptionLimits& limits = {});

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
