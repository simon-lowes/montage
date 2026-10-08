// Montage — caption (subtitle) tracks: timed text on a sequence, generated
// from transcripts or imported, shown in the viewer, burned in or embedded
// on export, and written as SubRip, WebVTT or Scenarist SCC (CEA-608).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace montage {

struct Project;
struct Sequence;
struct Rational;
using FrameTime = int64_t;
using Id = uint64_t;

struct CaptionStyle {
    std::string font = "Sans Serif";
    double size = 0.05;            // text height, fraction of the frame height
    bool bold = false;
    double textR = 1, textG = 1, textB = 1;
    double boxOpacity = 0.75;      // background box, 0 = none
    double boxR = 0, boxG = 0, boxB = 0;
    double outline = 0;            // outline width, fraction of the text height
    double position = 0.92;        // bottom of the caption block, fraction of the frame height
    // Word animation, for captions on social video: 0 none, 1 word by word (each
    // appears as it is said), 2 highlight (the word being said in the highlight
    // colour), 3 pop (the same, a little larger), 4 one word (only the word being
    // said, large).
    int animation = 0;
    double hiR = 1, hiG = 0.84, hiB = 0.1;  // highlight colour
    bool operator==(const CaptionStyle&) const = default;
};

struct Caption {
    FrameTime start = 0;  // timeline frames; end is exclusive
    FrameTime end = 0;
    std::string text;     // lines separated by '\n'
    // When each word of the text is said, as fractions of the caption's length
    // (so moves and retimes keep them); empty, or not one per word, when unknown.
    std::vector<double> wordTimes;
    bool operator==(const Caption&) const = default;
};

struct CaptionTrack {
    Id id = 0;
    std::string name = "Subtitles";
    std::string language = "en";  // ISO 639-1
    bool visible = true;          // shown in the viewer, burned in when exporting with burn-in
    CaptionStyle style;
    std::vector<Caption> captions;  // sorted by start, never overlapping
    bool operator==(const CaptionTrack&) const = default;
};

// The track the viewer shows and exports burn in or embed: the given one, or
// the first visible track when id is 0. nullptr if there is none.
const CaptionTrack* captionTrackFor(const Sequence& seq, Id id = 0);

// The caption on screen at frame t, or nullptr.
const Caption* captionAt(const CaptionTrack& track, FrameTime t);
// Index of the caption at or after t (captions.size() if none).
size_t captionIndexAt(const CaptionTrack& track, FrameTime t);

// When each word of the caption is said (fractions of its length): its word
// times, or else spread over it by the words' lengths.
std::vector<double> captionWordStarts(const Caption& c);
// The word being said at frame t (-1 before the caption, the last word after it).
int captionWordAt(const Caption& c, FrameTime t);

// Sorts, drops empty or zero-length captions and trims overlaps.
void normalizeCaptions(std::vector<Caption>& captions);

// Breaks text into at most `maxLines` lines of about `lineChars` characters,
// balancing the line lengths.
std::string wrapCaptionText(const std::string& text, int lineChars = 42, int maxLines = 2);

// A translated copy of a track: the same timings and style, each caption's
// text replaced (in order) and re-wrapped, word timings dropped (the words
// are new). Named "<name> (<languageName>)".
CaptionTrack translatedTrack(const CaptionTrack& source, const std::vector<std::string>& texts, Id id, const std::string& language,
                             const std::string& languageName);
// Each caption's text as one line, for translating.
std::vector<std::string> captionTexts(const CaptionTrack& track);

struct CaptionRules {
    int lineChars = 42;        // characters per line
    int maxLines = 2;
    double maxSeconds = 7.0;   // longest caption
    double minSeconds = 0.8;   // shortest caption (extended into the following gap)
};

// Captions for the sequence from the transcripts of the media its clips use:
// each word is placed where its clip plays it (following trims, speed and
// track mutes) and the words are grouped into readable captions.
std::vector<Caption> captionsFromTranscripts(const Project& p, const Sequence& seq, const CaptionRules& rules = {});

// SubRip / WebVTT text for the captions (times from the sequence frame rate).
std::string captionsToSrt(const std::vector<Caption>& captions, Rational fps);
std::string captionsToVtt(const std::vector<Caption>& captions, Rational fps);

// Reads SubRip or WebVTT (detected from the text). Markup is removed.
bool parseSubtitles(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error = nullptr);

// Scenarist SCC: CEA-608 pop-on captions on channel 1 at 29.97 fps drop frame,
// bottom rows, up to 32 characters per row.
std::string captionsToScc(const std::vector<Caption>& captions, Rational fps);

}  // namespace montage
