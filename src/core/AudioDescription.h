// Montage — audio description (described video, the AD track broadcasters and streamers ask for): a describer's lines
// spoken in the gaps between the dialogue, telling what is seen. The gaps where lines fit and how many words each
// holds, and the sequence's description track (a hidden caption track, "Audio Description", whose cues are the lines
// to speak). The lines are voiced with the speech generator onto an AD track as clips of the role "Description", the
// programme dips under them, and exports can add a described stream (render/Exporter.h). Premiere, Final Cut,
// Resolve and Media Composer have no description tools.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Model.h"

namespace montage {

constexpr const char* kDescriptionRole = "Description";
constexpr const char* kDescriptionTrackName = "Audio Description";
// The clip volume lane (dB, clip frames) that dips the programme under the descriptions: separate from the clip's
// own volume, and heard only while the role Description is (so not in the mix without descriptions).
constexpr const char* kDescriptionDuckParam = "ad_duck_db";

struct DescriptionGap {
    FrameTime start = 0, end = 0;  // end exclusive
    FrameTime length() const { return end - start; }
    bool operator==(const DescriptionGap&) const = default;
};

// The gaps between speech (spans in timeline seconds, sorted) within frames [from, to), kept `margin` seconds clear
// of the speech at both ends so a description never runs into a line, and at least `minSeconds` long.
std::vector<DescriptionGap> descriptionGaps(const std::vector<std::pair<double, double>>& speech, double fps, FrameTime from,
                                            FrameTime to, double minSeconds = 2.0, double margin = 0.3);

// Words in a description, and the seconds it takes to say at `wordsPerMinute` (160 is a describer's usual pace).
int descriptionWords(const std::string& text);
double descriptionSeconds(const std::string& text, double wordsPerMinute = 160);

// How a description fits `room` seconds: the seconds it needs, the speed it would take (1 when it fits as it is),
// and whether it fits at up to `maxSpeed` (a little faster is fine; much faster is not).
struct DescriptionFit {
    double needed = 0, room = 0, speed = 1;
    bool fits = true;
    int overWords = 0;  // words to cut for it to fit at normal pace (0 when it does)
};
DescriptionFit descriptionFit(const std::string& text, double room, double wordsPerMinute = 160, double maxSpeed = 1.25);

// The sequence's description track: the caption track named "Audio Description" (-1 when there is none), and that
// track made (hidden: not shown or burned in as subtitles) when missing.
int findDescriptionTrack(const Sequence& s);
int descriptionTrack(Project& p, Sequence& s, const std::string& language = "en");

// Puts a description on the description track from `start` to `end` (frames): one that began earlier is cut short
// at `start`, others it overlaps are replaced; empty text removes the description at `start`. Returns false when
// there is no room (end <= start).
bool setDescription(Project& p, Sequence& s, FrameTime start, FrameTime end, const std::string& text);

// Whether the sequence has described clips (clips of the role "Description").
bool hasDescriptionClips(const Sequence& s);

}  // namespace montage
