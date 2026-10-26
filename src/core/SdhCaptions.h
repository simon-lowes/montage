// Montage — captions for the deaf and hard of hearing (SDH): the dialogue's captions with what else the audience
// needs to hear written in. Sounds that matter get a cue in brackets ("[Door slams]"), sung lines are marked with ♪,
// a change of speaker is named where the frame does not show who speaks, and captions move to the top of the frame
// where they would cover text in the picture. The cueing rules are here; render/SdhDraft.h finds the sounds (by
// listening), the songs, the faces and the text, and drafts a track with them.
#pragma once

#include <string>
#include <vector>

#include "Captions.h"
#include "Model.h"

namespace montage {

// How sounds and speakers are written, after the house styles of the main delivery specifications:
//  - Brackets: sentence case in square brackets ("[Door slams]", "[Anna] Over here!"), as streaming services ask;
//  - Capitals: capitals in square brackets and speakers named before a colon ("[DOOR SLAMS]", "ANNA: Over here!"),
//    as US broadcast captions are;
//  - Colours: sounds in capitals without brackets ("DOOR SLAMS") and each main speaker in a colour of their own
//    (the track's own text colour, then yellow, cyan and green) rather than named, as UK broadcast subtitles are.
enum SdhStyle : int { kSdhBrackets = 0, kSdhCapitals = 1, kSdhColours = 2 };
const char* sdhStyleName(SdhStyle s);  // "brackets", "capitals", "colours"
bool parseSdhStyle(const std::string& name, SdhStyle& out);

// A sound heard on the timeline and what it is, in sentence case ("Door slams"), timeline seconds.
struct SdhSound {
    double start = 0, end = 0;
    std::string label;
    float score = 1;  // how sure the listener was
};

// A dialogue caption, who says it ("" unknown), how many faces the frame shows when it starts (-1: not looked at)
// and whether it is sung.
struct SdhLine {
    Caption caption;
    std::string speaker;
    int faces = -1;
    bool sung = false;
};

// Text in the picture (read off it), timeline seconds, the box as fractions of the frame.
struct SdhText {
    double start = 0, end = 0;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

struct SdhOptions {
    SdhStyle style = kSdhBrackets;
    double minSeconds = 5.0 / 6.0;  // a sound cue stays up at least this long (20 frames at 24 fps)
    double maxSeconds = 7.0;        // and at most this long
    int gapFrames = 2;              // kept clear between a sound cue and the captions around it
    double mergeSeconds = 2.0;      // the same sound again within this is one cue
    bool speakerLabels = true;
    bool lyrics = true;
    int lineChars = 42;             // for captions that gain a name or ♪, and for sound cues
    int maxLines = 2;
    CaptionStyle look;              // where the track shows captions (for what they would cover)
    double aspect = 16.0 / 9.0;     // the frame's width over its height
};

// What the cueing did.
struct SdhSummary {
    int sounds = 0;    // sound cues
    int lyrics = 0;    // captions marked as sung
    int speakers = 0;  // captions with the speaker named (or coloured)
    int raised = 0;    // captions moved to the top
};

// A sound's cue in the style ("[Door slams]", "[DOOR SLAMS]", "DOOR SLAMS").
std::string sdhSoundText(const std::string& label, SdhStyle style);
// A speaker's name as it leads a caption ("[Anna] ", "ANNA: "); "" in the colours style.
std::string sdhSpeakerPrefix(const std::string& name, SdhStyle style);
// The colours speakers are given in the colours style, most lines first ("" for white, the track's own colour).
const std::vector<std::string>& sdhSpeakerColours();

// The lines where a speaker label may be needed: where a known speaker takes over from someone else (or speaks
// first). Only there are faces looked for.
std::vector<size_t> sdhSpeakerChanges(const std::vector<SdhLine>& lines);

// The SDH captions, in order:
//  - the dialogue's captions keep their timing; sung ones get ♪ at their start and end and italics; at a change of
//    speaker the speaker is named when the frame shows no face or several (or was not looked at); in the colours
//    style every line of a speaker is in their colour instead (from the fifth speaker on, who have no colour, they
//    are named in the track's colour where a label is called for);
//  - each sound becomes a cue in the style, the same sound again within mergeSeconds joining it; a cue goes only
//    where no caption is (gapFrames clear of them), starting when the sound does or when the caption over it ends,
//    held at least minSeconds (into the time after the sound if need be, never over the next caption) and at most
//    maxSeconds; a sound with no room for that is left out;
//  - a caption that would cover text in the picture (bottom captions over text low in the frame) moves to the top.
std::vector<Caption> sdhCaptions(const std::vector<SdhLine>& dialogue, const std::vector<SdhSound>& sounds,
                                 const std::vector<SdhText>& text, Rational fps, const SdhOptions& options = {},
                                 SdhSummary* summary = nullptr);

// Moves the bottom captions that would cover text in the picture (in time and place) to the top. Returns how many.
int sdhRaiseOverText(std::vector<Caption>& captions, const std::vector<SdhText>& text, Rational fps, const SdhOptions& options = {});
// Where a caption's block sits in the frame (fractions), as the viewer draws it with options.look.
bool sdhCaptionArea(const Caption& c, const SdhOptions& options, double& x0, double& y0, double& x1, double& y1);

// The dialogue's captions from the sequence's transcripts (captionsFromTranscripts), each with its speaker.
std::vector<SdhLine> sdhDialogue(const Project& p, const Sequence& seq, const CaptionRules& rules = {});

// The descriptions sounds are captioned with: the project's own list, or 388 ready-made ones. Each is a
// caption in sentence case, optionally followed by '|' and how the sound is described to the listener ("Door slams|a
// door slamming shut"; without one, "the sound of" and the caption).
const std::vector<std::string>& defaultSoundCaptions();
std::vector<std::string> soundCaptionsFor(const Project& p);
struct SoundCaption {
    std::string caption, prompt;
};
SoundCaption parseSoundCaption(const std::string& entry);

}  // namespace montage
