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
    double outlineR = 0, outlineG = 0, outlineB = 0;  // outline colour
    double shadow = 0;            // drop shadow offset, fraction of the text height; 0 = none
    double shadowOpacity = 0.6;
    bool allCaps = false;         // shown in capitals (the text itself is unchanged)
    bool operator==(const CaptionStyle&) const = default;
};

// Ready-made caption looks (Descript's and CapCut's caption styles): from the
// classic boxed subtitle to bold social-video captions with the spoken word
// popping. A look sets the whole style.
struct CaptionLook {
    std::string id, name;
    CaptionStyle style;
};
const std::vector<CaptionLook>& captionLooks();
const CaptionLook* findCaptionLook(const std::string& id);

// Where a caption sits when not where the track's style puts every caption
// (bottom centre): at the top (clear of lower thirds and other text in the
// picture, as subtitle style guides ask) or in the middle, lined up left or
// right (to show who speaks).
enum CaptionVertical : int { kCaptionBottom = 0, kCaptionTop = 1, kCaptionMiddle = 2 };
enum CaptionAlign : int { kCaptionCentre = 0, kCaptionLeft = 1, kCaptionRight = 2 };

struct Caption {
    FrameTime start = 0;  // timeline frames; end is exclusive
    FrameTime end = 0;
    std::string text;     // lines separated by '\n'
    // When each word of the text is said, as fractions of the caption's length
    // (so moves and retimes keep them); empty, or not one per word, when unknown.
    std::vector<double> wordTimes;
    int vertical = kCaptionBottom;
    int align = kCaptionCentre;
    bool operator==(const Caption&) const = default;
};

// A caption's place as a keypad digit, as ASS's \an tag and SubRip's {\an} write
// it (1-3 bottom, 4-6 middle, 7-9 top; left, centre, right), and back (anything
// but 1-9 is 2, bottom centre).
int captionKeypad(const Caption& c);
void setCaptionKeypad(Caption& c, int keypad);
// The keypad digit for an ASS/SSA alignment: `legacy` for SSA's numbering (1-3
// bottom, 5-7 top, 9-11 middle). 0 for none.
int keypadFromAss(int alignment, bool legacy);
// The place in words ("bottom", "top left", "middle right"), and a place from
// words like those ("top-left", "center", "right"); false for words it does not know.
std::string captionPlaceName(const Caption& c);
bool parseCaptionPlace(const std::string& words, int& vertical, int& align);

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

// Reads any of the formats below, told apart by their content: SubRip,
// WebVTT, Scenarist SCC, TTML / IMSC / DFXP, EBU STL and ASS / SSA. Markup is removed.
bool parseSubtitles(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error = nullptr);

// Scenarist SCC: CEA-608 pop-on captions on channel 1 at 29.97 fps drop frame,
// bottom rows, up to 32 characters per row.
std::string captionsToScc(const std::vector<Caption>& captions, Rational fps);
// Reads SCC: CEA-608 channel 1 pop-on, paint-on and roll-up captions (each
// roll-up line becomes a caption, up until the next one).
bool parseScc(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error = nullptr);

// TTML in the IMSC 1.1 Text profile (Netflix, broadcasters; DFXP is the same
// family): times as media clock time, lines as <br/>, the style's colours.
std::string captionsToTtml(const std::vector<Caption>& captions, Rational fps, const std::string& language = "en",
                           const CaptionStyle& style = {});
// Reads TTML: clock times (with frames at ttp:frameRate) and offsets (h, m, s,
// ms, f, t), times nested in body and div, spans and <br/>.
bool parseTtml(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error = nullptr);

// EBU Tech 3264 STL, the European broadcast subtitle file: binary, a GSI block
// and a 128-byte TTI block per subtitle, at 25 fps (STL25.01) or 30
// (STL30.01, for every other rate), Latin text (ISO 6937) as centred
// double-height teletext lines at the bottom.
std::string captionsToStl(const std::vector<Caption>& captions, Rational fps, const std::string& language = "en",
                          const std::string& title = {});
// Reads STL (times from its start-of-programme timecode on, comments skipped).
bool parseStl(const std::string& data, Rational fps, std::vector<Caption>& out, std::string* error = nullptr);

// Advanced SubStation Alpha: the track's style (font, size, colours, box or
// outline, height) at the frame size, so players draw it as the viewer does.
std::string captionsToAss(const std::vector<Caption>& captions, Rational fps, const CaptionStyle& style, int width, int height);
bool parseAss(const std::string& text, Rational fps, std::vector<Caption>& out, std::string* error = nullptr);

// A caption track in the format a file extension names: srt, vtt, scc, ttml
// (also xml, dfxp), stl, ass (also ssa). Empty for an unknown extension.
bool captionFormatKnown(const std::string& extension);
std::string exportCaptions(const CaptionTrack& track, const Sequence& seq, const std::string& extension);

}  // namespace montage
