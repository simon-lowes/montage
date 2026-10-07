// Montage — word-timed transcripts of media (speech to text), and the
// caption formats made from them.
#pragma once

#include <string>
#include <vector>

namespace montage {

struct TranscriptWord {
    double start = 0;  // seconds, media time
    double end = 0;
    std::string text;  // without surrounding spaces
    float probability = 1;
    bool operator==(const TranscriptWord&) const = default;
};

struct TranscriptSegment {
    double start = 0;
    double end = 0;
    std::string text;
    std::vector<TranscriptWord> words;
    int speaker = -1;  // -1 = unknown
    bool operator==(const TranscriptSegment&) const = default;
};

struct Transcript {
    std::string language;  // ISO 639-1, e.g. "en"
    std::string model;     // the model that produced it, e.g. "base.en"
    std::vector<TranscriptSegment> segments;
    std::vector<std::string> speakerNames;  // by speaker number; empty or missing = "Speaker N"

    bool empty() const { return segments.empty(); }
    std::string text() const;  // the whole transcript as plain text
    size_t wordCount() const;
    bool operator==(const Transcript&) const = default;
};

// Who speaks when (from media/Diarizer.h), in seconds of media time.
struct SpeakerTurn {
    double start = 0;
    double end = 0;
    int speaker = 0;
    bool operator==(const SpeakerTurn&) const = default;
};
// Labels the transcript with its speakers: each word goes to the person
// speaking over most of it (or the nearest turn within a second), a lone
// word between two of someone else's is theirs, and segments are split
// where the speaker changes.
void applySpeakers(Transcript& t, const std::vector<SpeakerTurn>& turns);
// The speaker's name, "Speaker 1" etc. unless it was renamed; "" for -1.
std::string speakerName(const Transcript& t, int speaker);
int speakerCount(const Transcript& t);  // highest speaker number + 1

std::string transcriptToJson(const Transcript& t);
bool transcriptFromJson(const std::string& json, Transcript& out, std::string* error = nullptr);

// Caption cues: transcript text split into readable lines, at most
// `maxChars` characters and `maxSeconds` long, breaking at word boundaries
// and where the speaker changes.
struct Cue {
    double start = 0;
    double end = 0;
    std::string text;
    std::string voice;  // who speaks it, when the transcript knows (WebVTT <v>)
};
std::vector<Cue> transcriptCues(const Transcript& t, int maxChars = 42, double maxSeconds = 6.0);

// SubRip and WebVTT, with times shifted by `offset` seconds.
std::string cuesToSrt(const std::vector<Cue>& cues, double offset = 0);
std::string cuesToVtt(const std::vector<Cue>& cues, double offset = 0);

// The transcript as a file of the given type: "srt", "vtt", "txt" or "json"
// (empty for anything else).
std::string transcriptAs(const Transcript& t, const std::string& format, double offset = 0);

// Media times where `phrase` is spoken (case- and punctuation-insensitive,
// whole words), as (start, end) pairs.
std::vector<std::pair<double, double>> findPhrase(const Transcript& t, const std::string& phrase);

}  // namespace montage
