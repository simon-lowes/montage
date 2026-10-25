// Montage — delivery paperwork from the cut: the music cue sheet a broadcaster and the performing-rights societies
// are sent, the dialogue list (and the as-broadcast script) a distributor asks for, and the combined continuity and
// spotting list (CCSL) of a theatrical delivery. Each comes from what the project already knows: clip roles and the
// media's music tags, the transcript and its speakers, the media's scene logging and shot descriptions, the faces
// found in the footage, the caption tracks and the sequence's film gauge. Each is a table (core/Xlsx.h's sheet, so it
// goes to XLSX, CSV or, through render/PaperworkOutput.h, a PDF).
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/Model.h"
#include "core/Xlsx.h"

namespace montage {

// ---- Music cue sheet -------------------------------------------------------------------------------------------------

// The media's own cue sheet fields (MediaItem::metadata, edited in the Cue Sheet panel), each over the file's tag.
inline constexpr const char* kCueFields[] = {"title", "composer", "publisher", "society", "library", "catalogue", "isrc", "iswc"};
std::string cueMetadataKey(const std::string& field);  // "cue.composer"
// How it is used: "background" or "visual"; "instrumental" or "vocal"; "main title", "end title" or "".
inline constexpr const char* kCueUse = "cue.use";
inline constexpr const char* kCueVocal = "cue.vocal";
inline constexpr const char* kCueTheme = "cue.theme";
inline constexpr const char* kCueMusic = "cue.music";  // "yes" or "no": music whatever its role or tags say

struct MusicCue {
    int number = 0;                // from 1, in order of their first frame
    Id mediaId = 0;
    std::string file;              // the media's name
    FrameTime in = 0, out = 0;     // sequence frames, [in, out)
    FrameTime duration() const { return out - in; }
    int clips = 0;                 // the clips it is made of (cut into, or running across picture cuts)
    std::map<std::string, std::string> fields;  // kCueFields' values ("" where neither the media nor its tags say)
    std::string use = "background", vocal, theme;
};

// The file's own tags for a media item, by cue field name (media/MusicTags.h reads them); none by default.
using MusicTagReader = std::function<std::map<std::string, std::string>(const MediaItem&)>;

// The music heard in the cut: clips with the Music role (or whose media says it is music, or whose untagged role has
// music tags: a composer, a publisher, an ISRC or an ISWC), on audio tracks that are heard. A media item's clips one
// after another (gaps under a second bridged, clips on several tracks as one) are one cue; the cues in order.
std::vector<MusicCue> musicCues(const Project& p, const Sequence& seq, const MusicTagReader& tags = {});

// ---- Dialogue list and as-broadcast script ---------------------------------------------------------------------------

struct DialogueEntry {
    enum class Kind { Scene, Line, OnScreen };
    Kind kind = Kind::Line;
    FrameTime in = 0, out = 0;   // sequence frames, [in, out)
    std::string speaker;         // a line's
    std::string text;            // a line's words; a scene's heading ("SCENE 12"); on-screen text
    std::string description;     // a scene's: the shot description where it starts, if the media has one
};

struct DialogueOptions {
    double pause = 1.0;        // seconds of silence that end a line (a change of speaker always does)
    double longest = 15.0;     // a line longer than this is ended at its next sentence
};

// On-screen text read from the picture (render/PaperworkOutput.h readOnScreenText), to be listed with the dialogue.
struct OnScreenText {
    FrameTime in = 0, out = 0;
    std::string text;
};

// The transcript of the cut by speaker, a line at a time, with a heading wherever the scene changes (the logged scene
// of the picture on top) and any on-screen text, in order.
std::vector<DialogueEntry> dialogueList(const Project& p, const Sequence& seq, const DialogueOptions& o = {},
                                        const std::vector<OnScreenText>& onScreen = {});

// ---- CCSL -------------------------------------------------------------------------------------------------------------

struct CcslShot {
    int number = 0;
    FrameTime in = 0, out = 0;   // sequence frames, [in, out)
    Id clipId = 0, mediaId = 0;
    std::string name;            // the clip's
    std::string footageIn, footageOut, length;  // feet and frames of the sequence's gauge: its first and last frame, and how long
    std::string shotSize;        // "CU", "MS", "WS"; "" where the media's faces were never looked for
    float faceHeight = 0;        // the largest face's height in the shot (a fraction of the frame), 0 for none
    std::string description;     // the shot description (Describe Shots) where it starts
    std::vector<DialogueEntry> dialogue;  // the lines that start in it
    std::vector<Caption> subtitles;       // the first caption track's captions that start in it
};

// What the largest face's height (a fraction of the frame) makes a shot: a close-up from 0.3, a medium shot from
// 0.12, else wide.
std::string shotSizeFor(float faceHeight);

// The shots of the picture as seen (the top enabled video clip at each frame), each with its footage, size,
// dialogue and subtitles.
std::vector<CcslShot> ccsl(const Project& p, const Sequence& seq, const std::string& gauge = "");

// ---- As tables --------------------------------------------------------------------------------------------------------

XlsxSheet cueSheetTable(const std::vector<MusicCue>& cues, const Sequence& seq);
XlsxSheet dialogueTable(const std::vector<DialogueEntry>& list, const Sequence& seq);
XlsxSheet ccslTable(const std::vector<CcslShot>& shots, const Sequence& seq);
// A table as CSV (RFC 4180: fields quoted where they need it, CRLF line ends).
std::string tableCsv(const XlsxSheet& t);
// The as-broadcast script as text: each scene's heading, then each speaker's name over their line, with its timecode.
std::string asBroadcastScript(const std::vector<DialogueEntry>& list, const Sequence& seq);

}  // namespace montage
